#include "cutline/engine/mask_tracker.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/layout.hpp"
#include "cutline/core/mask_path.hpp"
#include "cutline/core/query.hpp"
#include "cutline/engine/frame_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace cutline::engine {
namespace {

constexpr double kDegrees = 180.0 / std::numbers::pi;

/// How wide the clip is drawn for tracking. Wide enough that a corner is a
/// few pixels across in anything filmed, and small enough that drawing and
/// reading a frame back costs less than following it.
constexpr double kTrackingWidth = 960.0;

[[nodiscard]] track::Point turned(double x, double y, double degrees) {
  const double r = degrees / kDegrees;
  const double c = std::cos(r);
  const double s = std::sin(r);
  return {c * x - s * y, s * x + c * y};
}

/// The project as tracking sees it: the one clip, alone, as its effects would
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
  track.id = "tracking";
  track.kind = core::TrackKind::Video;
  track.clips = {std::move(only)};
  sequence.tracks = {std::move(track)};
  return out;
}

}  // namespace

std::vector<track::Point> mask_outline(const core::Mask& mask, double width, double height) {
  const track::Point centre{mask.x * width, mask.y * height};
  std::vector<track::Point> local;
  if (mask.shape == core::MaskShape::Ellipse) {
    constexpr int kSides = 48;
    for (int i = 0; i < kSides; ++i) {
      const double a = 2.0 * std::numbers::pi * i / kSides;
      local.push_back({mask.width * width * std::cos(a), mask.height * height * std::sin(a)});
    }
  } else if (mask.shape == core::MaskShape::Rectangle) {
    const double w = mask.width * width;
    const double h = mask.height * height;
    local = {{-w, -h}, {w, -h}, {w, h}, {-w, h}};
  } else if (mask.shape == core::MaskShape::Path) {
    for (const core::MaskPoint& point : core::flatten_mask_path(mask.points)) {
      local.push_back({point.x * width, point.y * height});
    }
  }
  std::vector<track::Point> out;
  out.reserve(local.size());
  for (const track::Point& p : local) {
    const track::Point t = turned(p.x, p.y, mask.rotation);
    out.push_back({centre.x + t.x, centre.y + t.y});
  }
  return out;
}

track::Motion motion_for(core::MaskTracking tracking) noexcept {
  switch (tracking) {
    case core::MaskTracking::Position: return track::Motion::Translation;
    case core::MaskTracking::PositionRotation: return track::Motion::TranslationRotation;
    default: return track::Motion::Similarity;
  }
}

core::Mask moved_mask(const core::Mask& mask, const track::Similarity& motion, double width,
                      double height) {
  core::Mask out = mask;
  const track::Point centre = motion.apply({mask.x * width, mask.y * height});
  out.x = centre.x / width;
  out.y = centre.y / height;
  if (mask.tracking != core::MaskTracking::Position) out.rotation += motion.rotation * kDegrees;
  if (mask.tracking == core::MaskTracking::PositionScaleRotation) {
    const double s = motion.scale;
    out.width *= s;
    out.height *= s;
    for (core::MaskPoint& point : out.points) {
      point.x *= s;
      point.y *= s;
      point.in_x *= s;
      point.in_y *= s;
      point.out_x *= s;
      point.out_y *= s;
    }
  }
  return out;
}

std::expected<MaskTrackResult, std::string> track_mask(std::shared_ptr<gpu::Device> device,
                                                       const core::Project& project,
                                                       const MaskTrackRequest& request,
                                                       const MaskTrackProgress& progress) {
  const core::Clip* clip = core::find_clip(project, request.clip_id);
  if (clip == nullptr) return std::unexpected("the clip is not in the sequence");
  if (request.effect >= clip->effects.size()) return std::unexpected("the effect is not there");
  const std::vector<core::ClipEffect> resolved = core::resolved_effects(*clip, request.from);
  const core::Mask start = resolved[request.effect].mask;
  if (!start.usable()) return std::unexpected("the effect has no mask to track");

  // The clip's frame, at a size worth tracking at.
  const auto media = std::ranges::find(project.media, clip->media_id, &core::Media::id);
  const core::Size frame = core::natural_size(
      media == project.media.end() ? nullptr : &*media,
      static_cast<double>(project.sequence().canvas_w),
      static_cast<double>(project.sequence().canvas_h));
  if (frame.width <= 0.0 || frame.height <= 0.0) return std::unexpected("the clip has no picture");
  const double shrink = std::min(1.0, kTrackingWidth / frame.width);
  const int width = std::max(16, static_cast<int>(std::lround(frame.width * shrink)));
  const int height = std::max(16, static_cast<int>(std::lround(frame.height * shrink)));

  const core::Project only = alone(project, *clip, width, height);
  auto renderer = FrameRenderer::create(std::move(device), width, height);
  if (!renderer.has_value()) return std::unexpected(renderer.error());
  const auto picture = [&](double t) -> std::expected<track::Pyramid, std::string> {
    if (auto drawn = (*renderer)->render(only, clip->start + t); !drawn.has_value()) {
      return std::unexpected(drawn.error());
    }
    auto image = (*renderer)->read_back();
    if (!image.has_value()) return std::unexpected(image.error());
    return track::Pyramid(track::grey_of_rgba(image->pixels, image->width, image->height));
  };

  const double fps = project.sequence().fps > 0.0 ? project.sequence().fps : 30.0;
  const double step = (request.backward ? -1.0 : 1.0) / fps;
  const double length = core::clip_duration(*clip);
  // Every frame from the one after the start to the clip's last.
  int total = 0;
  for (double t = request.from + step; t >= -1e-6 && t < length - 0.5 / fps; t += step) {
    ++total;
    if (request.one_frame) break;
  }

  MaskTrackResult result;
  result.frames.push_back(core::TrackedMask{.t = request.from, .mask = start});
  auto before = picture(request.from);
  if (!before.has_value()) return std::unexpected(before.error());
  core::Mask mask = start;
  for (int done = 0; done < total; ++done) {
    const double t = request.from + step * (done + 1);
    auto after = picture(t);
    if (!after.has_value()) return std::unexpected(after.error());
    const track::Point origin{mask.x * width, mask.y * height};
    const std::optional<track::Similarity> motion =
        track::track_region(*before, *after, mask_outline(mask, width, height),
                            motion_for(mask.tracking), origin);
    if (!motion.has_value()) {
      result.stopped = "there was too little inside the mask to follow";
      break;
    }
    mask = moved_mask(mask, *motion, width, height);
    result.frames.push_back(core::TrackedMask{.t = t, .mask = mask});
    before = std::move(after);
    if (progress && !progress(done + 1, total)) {
      result.stopped = "it was stopped";
      break;
    }
  }
  return result;
}

}  // namespace cutline::engine
