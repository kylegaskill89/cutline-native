#include "cutline/editor/monitor_binding.hpp"

#include "cutline/core/layout.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/editor/inspector.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace cutline::editor {
namespace {

[[nodiscard]] const core::Media* media_of(const core::Project& project,
                                          const core::Clip& clip) noexcept {
  const auto found = std::ranges::find(project.media, clip.media_id, &core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// The clip's aspect-fit size as a fraction of the canvas, which is what a
/// scale of 1 covers. Zero on either axis means there is nothing to scale.
[[nodiscard]] core::Size natural_fraction(const core::Project& project,
                                          const core::Clip& clip) noexcept {
  const auto canvas_w = static_cast<double>(project.canvas_w);
  const auto canvas_h = static_cast<double>(project.canvas_h);
  if (canvas_w <= 0.0 || canvas_h <= 0.0) return {};

  // A title is measured by the text layer, which is nowhere near here, so its
  // stored dimensions are what there is. That makes the handles on a title
  // approximate until it has been laid out — better than no handles, and the
  // box is drawn from the same numbers the drag writes back, so the gesture is
  // consistent with itself either way.
  const core::Size natural =
      core::natural_size(media_of(project, clip), canvas_w, canvas_h);
  return {natural.width / canvas_w, natural.height / canvas_h};
}

/// The anchor-to-centre offset in the units the overlay is in: canvas
/// fractions, where `core::anchor_offset` works in canvas pixels because a
/// rotation is only a rotation in square units. Converted here, once, rather
/// than at each of the two places that need it.
[[nodiscard]] core::Offset anchor_shift(const core::Project& project,
                                        const core::Transform& transform,
                                        core::Size drawn_fraction) noexcept {
  const auto canvas_w = static_cast<double>(project.canvas_w);
  const auto canvas_h = static_cast<double>(project.canvas_h);
  if (canvas_w <= 0.0 || canvas_h <= 0.0) return {};

  const core::Offset px = core::anchor_offset(
      transform, {drawn_fraction.width * canvas_w, drawn_fraction.height * canvas_h});
  return {.dx = px.dx / canvas_w, .dy = px.dy / canvas_h};
}

}  // namespace

std::optional<ui::MonitorBox> monitor_box(const core::Project& project,
                                          std::string_view clip_id, double t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || clip->kind != core::TrackKind::Video) return std::nullopt;

  const core::Media* media = media_of(project, *clip);
  if (media == nullptr || !media->has_video) return std::nullopt;

  // An adjustment layer contributes a filter rather than a picture, so there is
  // no picture to place. It still sets `has_video`, which is why this is a
  // separate check and not an accident waiting to happen.
  if (media->is_adjustment) return std::nullopt;

  // Only while it is the thing on screen. A selected clip the playhead has run
  // past is not in the picture, and handles drawn over whatever *is* would
  // move something the eye cannot see move.
  if (t < clip->start || t >= clip->start + core::clip_duration(*clip)) return std::nullopt;

  const core::Size natural = natural_fraction(project, *clip);
  if (natural.width <= 0.0 || natural.height <= 0.0) return std::nullopt;

  const core::Transform transform = core::animated_transform(*clip, t - clip->start);
  const core::Size drawn{natural.width * transform.scale_x,
                         natural.height * transform.scale_y};
  const core::Offset shift = anchor_shift(project, transform, drawn);
  return ui::MonitorBox{
      // The box is drawn about its centre, and position names the anchor. The
      // same conversion the compositor makes, so the handles land on the
      // picture rather than beside it whenever an anchor has been moved.
      .x = transform.x + shift.dx,
      .y = transform.y + shift.dy,
      .width = drawn.width,
      .height = drawn.height,
      .rotation = transform.rotation,
  };
}

namespace {

/// The layer's box on the canvas, in fractions, or nothing when it has none.
///
/// Not `monitor_box`: that refuses an adjustment layer, which draws no picture
/// of its own but can perfectly well carry a masked effect.
[[nodiscard]] std::optional<ui::MonitorBox> mask_frame(const core::Project& project,
                                                       const core::Clip& clip, double t) {
  const core::Size natural = natural_fraction(project, clip);
  if (natural.width <= 0.0 || natural.height <= 0.0) return std::nullopt;

  const core::Transform transform = core::animated_transform(clip, t - clip.start);
  const core::Size drawn{natural.width * transform.scale_x, natural.height * transform.scale_y};
  const core::Offset shift = anchor_shift(project, transform, drawn);
  return ui::MonitorBox{
      .x = transform.x + shift.dx,
      .y = transform.y + shift.dy,
      .width = drawn.width,
      .height = drawn.height,
      .rotation = transform.rotation,
  };
}

/// Turns an offset by `degrees`, clockwise, in a *square* space.
///
/// Canvas fractions are not square, so a turn has to be done in a space where
/// they are: the offsets below are in fractions of the layer, which is square
/// with respect to itself, and the anisotropy comes back with the multiply.
[[nodiscard]] std::pair<double, double> turn(double x, double y, double degrees) {
  const double radians = degrees * std::numbers::pi / 180.0;
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  return {x * c - y * s, x * s + y * c};
}

}  // namespace

std::vector<MaskOverlayRef> mask_overlays(const core::Project& project,
                                          std::string_view clip_id, double t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return {};

  const std::optional<ui::MonitorBox> frame = mask_frame(project, *clip, t);
  if (!frame.has_value()) return {};

  std::vector<MaskOverlayRef> out;
  for (std::size_t i = 0; i < clip->effects.size(); ++i) {
    const core::Mask& mask = clip->effects[i].mask;
    if (!mask.active() || !clip->effects[i].enabled) continue;

    // The mask's centre as an offset from the layer's, in layer fractions,
    // turned with the layer and scaled by its drawn size.
    const auto [dx, dy] =
        turn((mask.x - 0.5) * frame->width, (mask.y - 0.5) * frame->height, frame->rotation);

    out.push_back(MaskOverlayRef{
        .effect = i,
        .overlay = ui::MaskOverlay{
            .shape = static_cast<int>(mask.shape),
            .x = frame->x + dx,
            .y = frame->y + dy,
            .width = mask.width * frame->width,
            .height = mask.height * frame->height,
            .rotation = mask.rotation + frame->rotation,
        }});
  }
  return out;
}

core::Project apply_mask_overlay(core::Project project, std::string_view clip_id,
                                 std::size_t effect, const ui::MaskOverlay& overlay, double t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || effect >= clip->effects.size()) return project;

  const std::optional<ui::MonitorBox> frame = mask_frame(project, *clip, t);
  if (!frame.has_value()) return project;
  if (frame->width <= 0.0 || frame->height <= 0.0) return project;

  // Straight back the way it came: the offset from the layer's centre, turned
  // the other way, divided by the layer's drawn size.
  const auto [dx, dy] = turn(overlay.x - frame->x, overlay.y - frame->y, -frame->rotation);

  core::Mask mask = clip->effects[effect].mask;
  mask.x = dx / frame->width + 0.5;
  mask.y = dy / frame->height + 0.5;
  mask.width = overlay.width / frame->width;
  mask.height = overlay.height / frame->height;
  mask.rotation = overlay.rotation - frame->rotation;
  return core::set_effect_mask(std::move(project), clip_id, effect, std::move(mask));
}

core::Project apply_monitor_box(core::Project project, std::string_view clip_id,
                                const ui::MonitorBox& box, double t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  const core::Size natural = natural_fraction(project, *clip);
  if (natural.width <= 0.0 || natural.height <= 0.0) return project;

  const double local_t = t - clip->start;

  // In the units the inspector's rows are in, because `set_clip_parameter` is
  // what divides them back out. Going through it rather than around it is the
  // point: a drag on the monitor and a drag on a number have to be the same
  // edit, keyframes and all.
  //
  // Position is in **pixels of the canvas** and scale is in percent, which is
  // exactly the split the rows show. The box's own coordinates are canvas
  // fractions either way.
  constexpr double kPercent = 100.0;
  const auto canvas = [&project](int extent) {
    return extent > 0 ? static_cast<double>(extent) : 1.0;
  };

  // The box gives a centre; position stores where the anchor goes. Taking the
  // offset back off is what makes the two agree — and it is computed from the
  // box's *own* rotation and size, the ones the drag has just produced, so
  // whatever the overlay showed while the button was down is what gets stored.
  //
  // One consequence worth naming: the overlay turns a layer about the box's
  // centre, and the number in the inspector turns it about the anchor. For a
  // layer left at the default anchor those are the same point and there is
  // nothing to tell apart. For one whose anchor has been moved they are not,
  // and the handle behaves as the handle looks — the corner you are dragging
  // goes where you drag it — while the anchor slides along the canvas to suit.
  // Premiere pivots the handle about the anchor as well; that is a change to
  // the overlay rather than to this, and it is not made here.
  const core::Transform current = core::animated_transform(*clip, local_t);
  const core::Offset shift = anchor_shift(
      project, core::Transform{.rotation = box.rotation,
                               .anchor_x = current.anchor_x,
                               .anchor_y = current.anchor_y},
      {box.width, box.height});

  project = set_clip_parameter(std::move(project), clip_id, ClipParam::X,
                               (box.x - shift.dx) * canvas(project.canvas_w), local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::Y,
                               (box.y - shift.dy) * canvas(project.canvas_h), local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::ScaleX,
                               box.width / natural.width * kPercent, local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::ScaleY,
                               box.height / natural.height * kPercent, local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::Rotation, box.rotation,
                               local_t);
  return project;
}

}  // namespace cutline::editor
