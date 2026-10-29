#include "cutline/engine/morph_analysis.hpp"

#include "cutline/core/query.hpp"
#include "cutline/engine/clip_pictures.hpp"
#include "cutline/render/morph_cut.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::engine {
namespace {

/// One side, as long as the transition draws it: the clip with the handle it
/// borrows, starting where that handle begins.
struct Side {
  core::Clip clip;
  std::unique_ptr<ClipPictures> pictures;

  /// The picture at timeline `t`, held at whichever end the source runs out.
  [[nodiscard]] std::expected<track::Image, std::string> at(double t, double frame) const {
    const double last = std::max(0.0, core::clip_duration(clip) - frame / 2.0);
    return pictures->at(std::clamp(t - clip.start, 0.0, last));
  }
};

[[nodiscard]] core::Clip lengthened(const core::VideoSeg& seg, bool incoming) {
  core::Clip clip = *seg.clip;
  const double before = core::clip_duration(clip);
  clip.source_in = seg.source_in;
  clip.source_out = seg.source_out;
  // The incoming side's handle comes before it, so it starts that much
  // earlier; the outgoing side's comes after and moves nothing.
  if (incoming) clip.start -= core::clip_duration(clip) - before;
  return clip;
}

/// `image` drawn again at another size, for two sides whose frames differ.
[[nodiscard]] track::Image resized(const track::Image& image, int width, int height) {
  if (image.width == width && image.height == height) return image;
  track::Image out{width, height,
                   std::vector<float>(static_cast<std::size_t>(width) * static_cast<std::size_t>(height))};
  const double sx = static_cast<double>(image.width) / width;
  const double sy = static_cast<double>(image.height) / height;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      out.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                 static_cast<std::size_t>(x)] =
          image.sample((x + 0.5) * sx - 0.5, (y + 0.5) * sy - 0.5);
    }
  }
  return out;
}

}  // namespace

std::expected<track::MorphAnalysis, std::string> analyse_morph_cut(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const std::string& clip_id, const AnalysisProgress& progress) {
  const std::optional<render::MorphJoin> join = render::morph_cut_of(project, clip_id);
  if (!join.has_value()) return std::unexpected("the clip has no Morph Cut");

  // Small: what is looked for is how things moved, and a head that has moved
  // shows it at a few hundred pixels across as well as at four thousand.
  Side out{.clip = lengthened(join->out, false)};
  Side in{.clip = lengthened(join->in, true)};
  for (Side* side : {&out, &in}) {
    auto pictures = ClipPictures::create(device, project, side->clip, 320.0);
    if (!pictures.has_value()) return std::unexpected(pictures.error());
    side->pictures = std::move(*pictures);
  }

  const double fps = project.sequence().fps > 0.0 ? project.sequence().fps : 30.0;
  const int total = render::morph_frames(project, join->start, join->end);
  track::MorphAnalysis analysis{.fps = fps};
  analysis.frames.reserve(static_cast<std::size_t>(total));
  for (int k = 0; k < total; ++k) {
    const double t = render::morph_frame_time(project, join->start, k);
    auto from = out.at(t, 1.0 / fps);
    if (!from.has_value()) return std::unexpected(from.error());
    auto to = in.at(t, 1.0 / fps);
    if (!to.has_value()) return std::unexpected(to.error());
    analysis.frames.push_back(
        track::dense_flow(*from, resized(*to, from->width, from->height)));
    if (progress && !progress(k + 1, total)) return std::unexpected("it was stopped");
  }
  return analysis;
}

}  // namespace cutline::engine
