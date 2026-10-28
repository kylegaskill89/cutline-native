#include "cutline/engine/mask_tracker.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/mask_path.hpp"
#include "cutline/core/query.hpp"
#include "cutline/engine/clip_pictures.hpp"

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

  auto pictures = ClipPictures::create(std::move(device), project, *clip, kTrackingWidth);
  if (!pictures.has_value()) return std::unexpected(pictures.error());
  const int width = (*pictures)->width();
  const int height = (*pictures)->height();
  const auto picture = [&](double t) -> std::expected<track::Pyramid, std::string> {
    auto image = (*pictures)->at(t);
    if (!image.has_value()) return std::unexpected(image.error());
    return track::Pyramid(*image);
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
        track::align_region(*before, *after, mask_outline(mask, width, height),
                            motion_for(mask.tracking), origin);
    if (!motion.has_value()) {
      result.stopped = "what was inside the mask could not be found on the next frame";
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
