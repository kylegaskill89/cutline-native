#include "cutline/render/warp_stabilizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <list>
#include <map>
#include <mutex>
#include <system_error>

namespace cutline::render {
namespace {

[[nodiscard]] double param(const core::ClipEffect& effect, std::string_view key, double fallback) {
  const auto found = effect.params.find(std::string(key));
  if (found == effect.params.end() || !std::isfinite(found->second)) return fallback;
  return found->second;
}

std::mutex folder_lock;
std::filesystem::path chosen_folder;

/// FNV-1a over everything that decides what the analysis would find.
struct Fingerprint {
  std::uint64_t hash = 14695981039346656037ull;
  void add(std::string_view text) {
    for (const char c : text) {
      hash ^= static_cast<unsigned char>(c);
      hash *= 1099511628211ull;
    }
    add_byte(0);
  }
  void add(double value) { add(std::format("{:.9g}", value)); }
  void add_byte(unsigned char b) {
    hash ^= b;
    hash *= 1099511628211ull;
  }
};

/// Analyses and the stabilisations worked out from them, a few of each, the
/// most recently used kept.
struct Cache {
  std::mutex lock;
  struct Entry {
    std::string key;
    track::StabilizeSettings settings;
    std::shared_ptr<const track::Stabilization> result;
  };
  std::list<Entry> stabilizations;
  std::map<std::string, std::shared_ptr<const track::Analysis>> analyses;
};

Cache& cache() {
  static Cache instance;
  return instance;
}

}  // namespace

std::filesystem::path analysis_folder() {
  const std::scoped_lock lock(folder_lock);
  if (!chosen_folder.empty()) return chosen_folder;
  // Beside the preview files, under local rather than roaming data: an
  // analysis can always be made again, and has no business following anyone
  // from machine to machine.
  if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') {
    return std::filesystem::path(local) / "Cutline" / "Analysis";
  }
  return std::filesystem::temp_directory_path() / "Cutline" / "Analysis";
}

void set_analysis_folder(std::filesystem::path folder) {
  const std::scoped_lock lock(folder_lock);
  chosen_folder = std::move(folder);
}

std::string analysis_key(const core::Project& project, const core::Clip& clip,
                         const core::ClipEffect& effect) {
  const auto media = std::ranges::find(project.media, clip.media_id, &core::Media::id);
  if (media == project.media.end()) return {};
  Fingerprint f;
  if (!media->path.empty()) {
    // The file, and which version of it.
    f.add(media->path);
    std::error_code error;
    const auto size = std::filesystem::file_size(media->path, error);
    f.add(error ? 0.0 : static_cast<double>(size));
    const auto written = std::filesystem::last_write_time(media->path, error);
    f.add(error ? 0.0 : static_cast<double>(written.time_since_epoch().count()));
  } else {
    // Something made rather than read: a nest or a graphic, known by what it
    // is in this project.
    f.add(media->id);
    f.add(media->sequence_id);
  }
  f.add(clip.source_in);
  f.add(clip.source_out);
  f.add(clip.speed);
  f.add(clip.reverse ? 1.0 : 0.0);
  for (const core::Keyframe& k : clip.keyframes[static_cast<std::size_t>(core::AnimProp::Speed)]) {
    f.add(k.t);
    f.add(k.v);
  }
  f.add(project.sequence().fps);
  f.add(param(effect, "detailed", 0.0) >= 0.5 ? 1.0 : 0.0);
  return std::format("{:016x}", f.hash);
}

std::filesystem::path analysis_path(const std::string& key) {
  return analysis_folder() / (key + ".cwsa");
}

bool has_analysis(const std::string& key) {
  std::error_code error;
  return !key.empty() && std::filesystem::exists(analysis_path(key), error);
}

track::StabilizeSettings stabilize_settings(const core::ClipEffect& effect) {
  const auto choice = [&](std::string_view key, double fallback, int most) {
    return std::clamp(static_cast<int>(std::lround(param(effect, key, fallback))), 0, most);
  };
  return track::StabilizeSettings{
      .result = static_cast<track::StabilizeResult>(choice("result", 0.0, 1)),
      .smoothness = std::max(0.0, param(effect, "smoothness", 50.0)) / 100.0,
      .method = static_cast<track::StabilizeMethod>(choice("method", 3.0, 3)),
      .preserve_scale = param(effect, "preserve_scale", 0.0) >= 0.5,
      .framing = static_cast<track::Framing>(choice("framing", 2.0, 3)),
      .maximum_scale = std::max(1.0, param(effect, "maximum_scale", 150.0) / 100.0),
      .safe_margin = std::clamp(param(effect, "safe_margin", 0.0) / 100.0, 0.0, 0.45),
      .additional_scale = std::max(0.01, param(effect, "additional_scale", 100.0) / 100.0),
      .rolling_shutter = static_cast<track::RollingShutter>(choice("rolling_shutter", 0.0, 1)),
      .crop_less_smooth_more = std::clamp(param(effect, "crop_smooth", 50.0) / 100.0, 0.0, 1.0)};
}

std::shared_ptr<const track::Stabilization> stabilization(const std::string& key,
                                                          const track::StabilizeSettings& settings,
                                                          bool wait) {
  Cache& c = cache();
  std::shared_ptr<const track::Analysis> analysis;
  {
    const std::scoped_lock lock(c.lock);
    for (auto it = c.stabilizations.begin(); it != c.stabilizations.end(); ++it) {
      if (it->key == key && it->settings == settings) {
        c.stabilizations.splice(c.stabilizations.begin(), c.stabilizations, it);
        return c.stabilizations.front().result;
      }
    }
    if (!wait) return nullptr;
    if (const auto found = c.analyses.find(key); found != c.analyses.end()) analysis = found->second;
  }
  if (analysis == nullptr) {
    auto loaded = track::load_analysis(analysis_path(key));
    if (!loaded.has_value()) return nullptr;
    analysis = std::make_shared<const track::Analysis>(std::move(*loaded));
  }
  // Worked out without the lock held: it takes a moment for a long clip, and
  // a second clip should not wait behind it.
  auto result = std::make_shared<const track::Stabilization>(track::stabilize(*analysis, settings));
  const std::scoped_lock lock(c.lock);
  c.analyses[key] = analysis;
  if (c.analyses.size() > 8) c.analyses.erase(c.analyses.begin());
  c.stabilizations.push_front({key, settings, result});
  if (c.stabilizations.size() > 16) c.stabilizations.pop_back();
  return result;
}

void plan_warp_stabilizer(const core::Project& project, const core::Clip& clip,
                          const core::ClipEffect& effect, double local_t,
                          std::vector<EffectPass>& passes) {
  const std::string key = analysis_key(project, clip, effect);
  if (!has_analysis(key)) return;
  const std::shared_ptr<const track::Stabilization> result =
      stabilization(key, stabilize_settings(effect));
  if (result == nullptr || result->frames.empty()) return;

  const double fps = project.sequence().fps > 0.0 ? project.sequence().fps : 30.0;
  const auto frame = static_cast<std::size_t>(std::clamp<long long>(
      std::llround(local_t * fps), 0, static_cast<long long>(result->frames.size()) - 1));
  const track::WarpGrid& grid = result->frames[frame];

  EffectPass pass;
  pass.kind = EffectPassKind::WarpStabilizer;
  std::vector<float>& data = pass.data;
  data = {static_cast<float>(grid.columns), static_cast<float>(grid.rows),
          static_cast<float>(result->visible), 0.0f};
  data.reserve(kWarpHeader + grid.source.size() * 2);
  for (const track::Point& p : grid.source) {
    data.push_back(static_cast<float>(p.x));
    data.push_back(static_cast<float>(p.y));
  }
  passes.push_back(std::move(pass));
}

}  // namespace cutline::render
