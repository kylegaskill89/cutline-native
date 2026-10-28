#include "cutline/engine/clip_pictures.hpp"

#include "cutline/core/layout.hpp"
#include "cutline/engine/frame_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::engine {
namespace {

/// The project as analysis sees it: the one clip, alone, as its effects would
/// receive it, filling a canvas the shape of its frame.
[[nodiscard]] core::Project alone(const core::Project& project, const core::Clip& clip,
                                  int width, int height) {
  core::Project out = project;
  core::Sequence& sequence = out.sequence();
  sequence.canvas_w = width;
  sequence.canvas_h = height;
  core::Clip only = clip;
  only.effects.clear();
  only.transform = core::Transform{};
  only.opacity = 1.0;
  only.fade_in = 0.0;
  only.fade_out = 0.0;
  only.blend = core::BlendMode::Normal;
  only.disabled = false;
  only.transition_out.reset();
  // Motion's animation goes with Motion. Speed's stays: it decides which
  // frame of the source is the clip's picture at a moment.
  for (std::size_t i = 0; i < only.keyframes.size(); ++i) {
    if (i != static_cast<std::size_t>(core::AnimProp::Speed)) only.keyframes[i].clear();
  }
  core::Track track;
  track.id = "analysis";
  track.kind = core::TrackKind::Video;
  track.clips = {std::move(only)};
  sequence.tracks = {std::move(track)};
  return out;
}

}  // namespace

ClipPictures::~ClipPictures() = default;

std::expected<std::unique_ptr<ClipPictures>, std::string> ClipPictures::create(
    std::shared_ptr<gpu::Device> device, const core::Project& project, const core::Clip& clip,
    double width) {
  const auto media = std::ranges::find(project.media, clip.media_id, &core::Media::id);
  const core::Size frame = core::natural_size(
      media == project.media.end() ? nullptr : &*media,
      static_cast<double>(project.sequence().canvas_w),
      static_cast<double>(project.sequence().canvas_h));
  if (frame.width <= 0.0 || frame.height <= 0.0) return std::unexpected("the clip has no picture");
  const double shrink = std::min(1.0, width / frame.width);

  std::unique_ptr<ClipPictures> out(new ClipPictures());
  out->width_ = std::max(16, static_cast<int>(std::lround(frame.width * shrink)));
  out->height_ = std::max(16, static_cast<int>(std::lround(frame.height * shrink)));
  out->alone_ = alone(project, clip, out->width_, out->height_);
  out->start_ = clip.start;
  auto renderer = FrameRenderer::create(std::move(device), out->width_, out->height_);
  if (!renderer.has_value()) return std::unexpected(renderer.error());
  out->renderer_ = std::move(*renderer);
  return out;
}

std::expected<track::Image, std::string> ClipPictures::at(double t) {
  if (auto drawn = renderer_->render(alone_, start_ + t); !drawn.has_value()) {
    return std::unexpected(drawn.error());
  }
  auto image = renderer_->read_back();
  if (!image.has_value()) return std::unexpected(image.error());
  return track::grey_of_rgba(image->pixels, image->width, image->height);
}

}  // namespace cutline::engine
