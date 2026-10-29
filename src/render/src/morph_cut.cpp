#include "cutline/render/morph_cut.hpp"

#include "cutline/render/plan.hpp"
#include "cutline/render/warp_stabilizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <system_error>

namespace cutline::render {
namespace {

/// Analyses read from their files, with when each file was written. A file
/// analysed again, or gone, is read afresh.
struct Cache {
  std::mutex lock;
  struct Read {
    std::filesystem::file_time_type written;
    std::shared_ptr<const track::MorphAnalysis> analysis;
  };
  std::map<std::filesystem::path, Read> files;
};

Cache& cache() {
  static Cache instance;
  return instance;
}

[[nodiscard]] double fps_of(const core::Project& project) {
  return project.sequence().fps > 0.0 ? project.sequence().fps : 30.0;
}

}  // namespace

std::vector<MorphJoin> morph_cuts(const core::Project& project) {
  std::vector<MorphJoin> out;
  const auto durations = media_durations(project);
  for (const core::Track& track : project.sequence().tracks) {
    if (track.kind != core::TrackKind::Video) continue;
    const std::vector<core::VideoSeg> segs = core::resolve_video_segments(track, durations);
    for (const core::VideoSeg& seg : segs) {
      if (!seg.morph.has_value() || seg.morph->incoming) continue;
      const auto partner = std::ranges::find_if(segs, [&](const core::VideoSeg& other) {
        return other.morph.has_value() && other.morph->incoming && other.clip == seg.morph->in;
      });
      if (partner == segs.end()) continue;
      out.push_back(MorphJoin{.out = seg,
                              .in = *partner,
                              .start = seg.morph->start,
                              .end = seg.morph->end,
                              .key = morph_key(project, *seg.morph)});
    }
  }
  return out;
}

std::optional<MorphJoin> morph_cut_of(const core::Project& project, std::string_view clip_id) {
  for (MorphJoin& join : morph_cuts(project)) {
    if (join.out.clip != nullptr && join.out.clip->id == clip_id) return std::move(join);
  }
  return std::nullopt;
}

std::string morph_key(const core::Project& project, const core::MorphSide& side) {
  if (side.out == nullptr || side.in == nullptr) return {};
  // Each side's source, part and speed, as Warp Stabilizer names them, and the
  // window, which decides which frames either side are looked at.
  const std::string out = analysis_key(project, *side.out, core::ClipEffect{});
  const std::string in = analysis_key(project, *side.in, core::ClipEffect{});
  if (out.empty() || in.empty()) return {};
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : std::format("{}|{}|{:.6f}", out, in, side.end - side.start)) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 1099511628211ull;
  }
  return std::format("{:016x}", hash);
}

std::filesystem::path morph_analysis_path(const std::string& key) {
  return analysis_folder() / (key + ".cmca");
}

bool has_morph_analysis(const std::string& key) {
  std::error_code error;
  return !key.empty() && std::filesystem::exists(morph_analysis_path(key), error);
}

int morph_frames(const core::Project& project, double start, double end) {
  return std::max(1, static_cast<int>(std::ceil((end - start) * fps_of(project) - 1e-6)));
}

double morph_frame_time(const core::Project& project, double start, int frame) {
  return start + (frame + 0.5) / fps_of(project);
}

std::shared_ptr<const track::MorphAnalysis> morph_analysis(const std::string& key) {
  if (key.empty()) return nullptr;
  const std::filesystem::path file = morph_analysis_path(key);
  std::error_code error;
  const auto written = std::filesystem::last_write_time(file, error);
  Cache& c = cache();
  if (error) {
    const std::scoped_lock lock(c.lock);
    c.files.erase(file);
    return nullptr;
  }
  {
    const std::scoped_lock lock(c.lock);
    if (const auto found = c.files.find(file);
        found != c.files.end() && found->second.written == written) {
      return found->second.analysis;
    }
  }
  auto loaded = track::load_morph(file);
  if (!loaded.has_value() || loaded->frames.empty()) return nullptr;
  auto analysis = std::make_shared<const track::MorphAnalysis>(std::move(*loaded));
  const std::scoped_lock lock(c.lock);
  c.files[file] = {written, analysis};
  return analysis;
}

std::optional<EffectPass> plan_morph_cut(const core::Project& project, const core::VideoSeg& seg,
                                         double t) {
  if (!seg.morph.has_value()) return std::nullopt;
  const core::MorphSide& side = *seg.morph;
  const double span = side.end - side.start;
  if (span <= 0.0) return std::nullopt;
  const std::shared_ptr<const track::MorphAnalysis> analysis =
      morph_analysis(morph_key(project, side));
  if (analysis == nullptr) return std::nullopt;

  const auto last = static_cast<long long>(analysis->frames.size()) - 1;
  const auto frame = static_cast<std::size_t>(
      std::clamp<long long>(static_cast<long long>(std::floor((t - side.start) * analysis->fps)),
                            0, last));
  const track::FlowField& field = analysis->frames[frame];
  // How far through: where the dissolve is, so the bending and the blending
  // keep step.
  const double p = std::clamp((t - side.start) / span, 0.0, 1.0);

  EffectPass pass;
  pass.kind = EffectPassKind::WarpStabilizer;
  std::vector<float>& data = pass.data;
  // Warp Stabilizer's header with all of the frame showing and nothing to
  // synthesise: a plain mesh.
  data.assign(kWarpHeader, 0.0f);
  data[0] = static_cast<float>(field.columns);
  data[1] = static_cast<float>(field.rows);
  data[2] = 1.0f;
  data.reserve(kWarpHeader + field.moves.size() * 2);
  for (int j = 0; j <= field.rows; ++j) {
    for (int i = 0; i <= field.columns; ++i) {
      const track::Point move =
          field.moves[static_cast<std::size_t>(j) * static_cast<std::size_t>(field.columns + 1) +
                      static_cast<std::size_t>(i)];
      const double x = static_cast<double>(i) / field.columns;
      const double y = static_cast<double>(j) / field.rows;
      // A point of the outgoing picture is a fraction p of the way to where
      // it is going, so here shows what was a fraction p back along the move;
      // the incoming picture shows what is the rest of the way ahead. Kept on
      // the frame, where the edge carries outward rather than leaving a gap.
      const double k = side.incoming ? 1.0 - p : -p;
      data.push_back(static_cast<float>(std::clamp(x + k * move.x, 0.0, 1.0)));
      data.push_back(static_cast<float>(std::clamp(y + k * move.y, 0.0, 1.0)));
    }
  }
  return pass;
}

}  // namespace cutline::render
