#include "cutline/editor/monitor_binding.hpp"

#include "cutline/editor/effects_binding.hpp"
#include "cutline/editor/graphics.hpp"

#include "cutline/core/layout.hpp"
#include "cutline/core/mask_path.hpp"
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
  const auto canvas_w = static_cast<double>(project.sequence().canvas_w);
  const auto canvas_h = static_cast<double>(project.sequence().canvas_h);
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
  const auto canvas_w = static_cast<double>(project.sequence().canvas_w);
  const auto canvas_h = static_cast<double>(project.sequence().canvas_h);
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

  // Resolved, so an animated mask is drawn where it is *now* rather than where
  // it was stored. The shape on the picture and the shape the renderer uses
  // have to be the same shape, or the outline stops meaning anything the moment
  // one of its numbers is animated.
  const std::vector<core::ClipEffect> effects =
      core::resolved_effects(*clip, t - clip->start);

  std::vector<MaskOverlayRef> out;
  for (std::size_t i = 0; i < effects.size(); ++i) {
    const core::Mask& mask = effects[i].mask;
    if (!mask.active() || !effects[i].enabled) continue;

    // The mask's centre as an offset from the layer's, in layer fractions,
    // turned with the layer and scaled by its drawn size.
    const auto [dx, dy] =
        turn((mask.x - 0.5) * frame->width, (mask.y - 0.5) * frame->height, frame->rotation);

    // A path's corners are offsets from its centre in *layer* fractions, and
    // the overlay wants them in canvas fractions. Scaled rather than turned:
    // the overlay carries the rotation and applies it itself, exactly as it
    // does for the half-extents of the other two shapes.
    std::vector<ui::MaskVertex> points;
    points.reserve(mask.points.size());
    for (const core::MaskPoint& point : mask.points) {
      // The handles are offsets in the same space as the points, so they scale
      // the same way. Left unturned for the same reason: the overlay carries
      // the rotation and applies it to everything it draws.
      points.push_back(ui::MaskVertex{
          .x = point.x * frame->width,
          .y = point.y * frame->height,
          .in_x = point.in_x * frame->width,
          .in_y = point.in_y * frame->height,
          .out_x = point.out_x * frame->width,
          .out_y = point.out_y * frame->height,
      });
    }

    out.push_back(MaskOverlayRef{
        .effect = i,
        .overlay = ui::MaskOverlay{
            .shape = static_cast<int>(mask.shape),
            .x = frame->x + dx,
            .y = frame->y + dy,
            .width = mask.width * frame->width,
            .height = mask.height * frame->height,
            .rotation = mask.rotation + frame->rotation,
            .points = std::move(points),
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

  // Through `set_effect_parameter` rather than straight onto the mask, for the
  // same reason `apply_monitor_box` goes through `set_clip_parameter`: a drag on
  // the picture and a drag on a number have to be the same edit, keyframes and
  // all. Written onto the mask, a drag would be silently thrown away the moment
  // that number was animated — the shape would move and the render would not.
  const double local_t = t - clip->start;
  const auto set = [&](std::string_view key, double value) {
    project = set_effect_parameter(std::move(project), clip_id, effect, key, value, local_t);
  };

  set("mask.x", (dx / frame->width) + 0.5);
  set("mask.y", (dy / frame->height) + 0.5);
  set("mask.width", overlay.width / frame->width);
  set("mask.height", overlay.height / frame->height);
  set("mask.rotation", overlay.rotation - frame->rotation);

  // The corners go straight back on the mask rather than through a parameter:
  // a path is a shape rather than a number, and there is no keyframe for it to
  // land on.
  //
  // Gated on the *shape* rather than on there being points, because for a path
  // the overlay is the whole truth — including when it is empty. Gating on
  // "there are some" meant a path that had just been cleared could not be
  // written down: the pen starts by emptying it, that emptying was refused, and
  // the next refresh handed the old corners straight back. The first three
  // points of a freshly drawn shape landed on top of the shape it was meant to
  // replace. The other two shapes still keep their corners untouched, which is
  // what lets you switch away from a path and back without losing it.
  if (static_cast<int>(core::MaskShape::Path) == overlay.shape) {
    const core::Clip* moved = core::find_clip(project, clip_id);
    if (moved == nullptr || effect >= moved->effects.size()) return project;

    core::Mask mask = moved->effects[effect].mask;
    mask.points.clear();
    mask.points.reserve(overlay.points.size());
    for (const ui::MaskVertex& point : overlay.points) {
      mask.points.push_back(core::MaskPoint{
          .x = point.x / frame->width,
          .y = point.y / frame->height,
          .in_x = point.in_x / frame->width,
          .in_y = point.in_y / frame->height,
          .out_x = point.out_x / frame->width,
          .out_y = point.out_y / frame->height,
      });
    }

    // And the half-extents are made to describe the corners, because on a path
    // they are the only thing that still describes it to anybody else.
    //
    // `width` and `height` are the ellipse's radii and half the rectangle's
    // sides. A path ignores them and lives entirely in its corners — so pulling
    // a corner about used to leave them at whatever the shape happened to be
    // before anyone drew anything. Switch that mask to Ellipse or Rectangle and
    // it jumped to a size and place with no relation to the outline that had
    // just been on the picture, because the two were never the same shape.
    //
    // Kept in step, the three shapes describe the same region: an ellipse or a
    // rectangle switched to from a path lands inside the outline that was there,
    // and switching back restores corners that still match the box. The centre
    // is left alone — the corners are offsets from it, and the drag already
    // moved it if it was the whole mask being moved rather than one corner.
    //
    // Measured on the flattened outline rather than on the placed points,
    // because a curve leaves its ends: a cubic stays inside the hull of its
    // control points, and the hull reaches wherever the handles do.
    double half_w = 0.0;
    double half_h = 0.0;
    for (const core::MaskPoint& point : core::flatten_mask_path(mask.points)) {
      half_w = std::max(half_w, std::abs(point.x));
      half_h = std::max(half_h, std::abs(point.y));
    }
    if (half_w > 0.0) mask.width = half_w;
    if (half_h > 0.0) mask.height = half_h;

    project = core::set_effect_mask(std::move(project), clip_id, effect, std::move(mask));
  }
  return project;
}

namespace {

/// A selected graphic layer, placed: the layer as placed in the graphic, and
/// the clip's own box in canvas pixels that the graphic is drawn into.
struct PlacedGraphicLayer {
  core::PlacedLayer placed;
  core::Graphic graphic;
  double key_time = 0.0;
  double canvas_w = 0.0;
  double canvas_h = 0.0;
  // The clip's box, in pixels.
  double frame_x = 0.0;
  double frame_y = 0.0;
  double frame_w = 0.0;
  double frame_h = 0.0;
  double frame_rotation = 0.0;
};

[[nodiscard]] std::optional<PlacedGraphicLayer> place_selected_layer(
    const core::Project& project, std::string_view clip_id, std::string_view layer_id, double t,
    const core::LayerMeasurer& measure) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return std::nullopt;
  const core::Graphic* graphic = graphic_of(project, clip->media_id);
  if (graphic == nullptr) return std::nullopt;
  if (t < clip->start || t >= clip->start + core::clip_duration(*clip)) return std::nullopt;
  const std::optional<ui::MonitorBox> frame = mask_frame(project, *clip, t);
  if (!frame.has_value()) return std::nullopt;

  PlacedGraphicLayer out;
  out.graphic = *graphic;
  out.canvas_w = project.sequence().canvas_w;
  out.canvas_h = project.sequence().canvas_h;
  if (out.canvas_w <= 0.0 || out.canvas_h <= 0.0) return std::nullopt;
  const double local = t - clip->start;
  const double duration = core::clip_duration(*clip);
  out.key_time = core::graphic_time(out.graphic, local, duration);
  bool found = false;
  for (const core::PlacedLayer& at : core::place_graphic(out.graphic, local, duration,
                                                         out.canvas_w, out.canvas_h, measure)) {
    if (at.layer->id != layer_id) continue;
    out.placed = at;
    found = true;
  }
  if (!found) return std::nullopt;
  // Pointed back into the copy this holds rather than the project's.
  out.placed.layer = core::find_layer(out.graphic, layer_id);
  out.frame_x = frame->x * out.canvas_w;
  out.frame_y = frame->y * out.canvas_h;
  out.frame_w = frame->width * out.canvas_w;
  out.frame_h = frame->height * out.canvas_h;
  out.frame_rotation = frame->rotation;
  return out;
}

[[nodiscard]] std::pair<double, double> turned(double x, double y, double degrees) {
  const double radians = degrees * std::numbers::pi / 180.0;
  return {x * std::cos(radians) - y * std::sin(radians),
          x * std::sin(radians) + y * std::cos(radians)};
}

}  // namespace

std::optional<ui::MonitorBox> graphic_layer_box(const core::Project& project,
                                                std::string_view clip_id,
                                                std::string_view layer_id, double t,
                                                const core::LayerMeasurer& measure) {
  const auto at = place_selected_layer(project, clip_id, layer_id, t, measure);
  if (!at.has_value()) return std::nullopt;
  const core::LayerBox& box = at->placed.box;
  // From the graphic's centre, scaled and turned as the clip draws it.
  const double sx = at->frame_w / at->canvas_w;
  const double sy = at->frame_h / at->canvas_h;
  const auto [dx, dy] = turned((box.center_x - at->canvas_w / 2.0) * sx,
                               (box.center_y - at->canvas_h / 2.0) * sy, at->frame_rotation);
  return ui::MonitorBox{
      .x = (at->frame_x + dx) / at->canvas_w,
      .y = (at->frame_y + dy) / at->canvas_h,
      .width = box.width * sx / at->canvas_w,
      .height = box.height * sy / at->canvas_h,
      .rotation = box.rotation_deg + at->frame_rotation,
  };
}

core::Project apply_graphic_layer_box(core::Project project, std::string_view clip_id,
                                      std::string_view layer_id, const ui::MonitorBox& box,
                                      double t, const core::LayerMeasurer& measure) {
  const auto at = place_selected_layer(project, clip_id, layer_id, t, measure);
  if (!at.has_value()) return project;
  const double sx = at->frame_w / at->canvas_w;
  const double sy = at->frame_h / at->canvas_h;
  if (sx <= 0.0 || sy <= 0.0) return project;

  // Straight back the way it came: into the graphic's own pixels.
  const auto [dx, dy] = turned(box.x * at->canvas_w - at->frame_x,
                               box.y * at->canvas_h - at->frame_y, -at->frame_rotation);
  const double centre_x = dx / sx + at->canvas_w / 2.0;
  const double centre_y = dy / sy + at->canvas_h / 2.0;
  const double width = box.width * at->canvas_w / sx;
  const double rotation = box.rotation - at->frame_rotation;

  const core::PlacedLayer& was = at->placed;
  const core::GraphicLayer& layer = *was.layer;
  // Where the anchor was, and the drawing's centre relative to it before the
  // turn: what a move, a scale and a turn are all made about.
  const double anchor_x = was.content.left + layer.anchor_x * was.content.width;
  const double anchor_y = was.content.top + layer.anchor_y * was.content.height;
  const auto [rx, ry] = turned(was.box.center_x - anchor_x, was.box.center_y - anchor_y,
                               -was.box.rotation_deg);
  // One scale for both directions: a layer has one, as Premiere's does.
  const double grow = was.box.width > 0.0 ? width / was.box.width : 1.0;
  const auto [nx, ny] = turned(rx * grow, ry * grow, rotation);
  const double new_anchor_x = centre_x - nx;
  const double new_anchor_y = centre_y - ny;

  const double time = at->key_time;
  core::Graphic graphic = at->graphic;
  const auto value = [&](std::string_view property) {
    return core::layer_value(layer, property, time);
  };
  graphic = set_layer_value(std::move(graphic), layer_id, "x", time,
                            value("x") + (new_anchor_x - anchor_x) / at->canvas_w);
  graphic = set_layer_value(std::move(graphic), layer_id, "y", time,
                            value("y") + (new_anchor_y - anchor_y) / at->canvas_h);
  if (std::abs(grow - 1.0) > 1e-9) {
    graphic = set_layer_value(std::move(graphic), layer_id, "scale", time, value("scale") * grow);
  }
  if (std::abs(rotation - was.box.rotation_deg) > 1e-9) {
    graphic = set_layer_value(std::move(graphic), layer_id, "rotation", time,
                              value("rotation") + (rotation - was.box.rotation_deg));
  }
  const core::Clip* clip = core::find_clip(project, clip_id);
  return clip == nullptr ? project : set_graphic(std::move(project), clip->media_id,
                                                  std::move(graphic));
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
                               (box.x - shift.dx) * canvas(project.sequence().canvas_w), local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::Y,
                               (box.y - shift.dy) * canvas(project.sequence().canvas_h), local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::ScaleX,
                               box.width / natural.width * kPercent, local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::ScaleY,
                               box.height / natural.height * kPercent, local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::Rotation, box.rotation,
                               local_t);
  return project;
}

core::Project scale_to_frame(core::Project project, std::span<const std::string> clip_ids,
                             FrameFit fit, double t) {
  constexpr double kPercent = 100.0;

  for (const std::string& id : clip_ids) {
    const core::Clip* clip = core::find_clip(project, id);
    if (clip == nullptr || clip->kind != core::TrackKind::Video) continue;

    // As a fraction of the canvas, which is what a scale of 1 covers. A clip
    // with nothing to measure is passed over rather than scaled by a guess.
    const core::Size natural = natural_fraction(project, *clip);
    if (natural.width <= 0.0 || natural.height <= 0.0) continue;

    // Fit is scale 1 by construction: the model stores scale relative to the
    // aspect-fit size, so 1 is exactly "as large as it goes without
    // distortion". Fill is however much more it takes for the shorter axis to
    // reach the edge — the same number on both axes, or the picture would be
    // stretched rather than cropped.
    const double factor =
        fit == FrameFit::Fit
            ? 1.0
            : std::max(1.0 / natural.width, 1.0 / natural.height);

    const double local_t = t - clip->start;
    project = set_clip_parameter(std::move(project), id, ClipParam::ScaleX,
                                 factor * kPercent, local_t);
    project = set_clip_parameter(std::move(project), id, ClipParam::ScaleY,
                                 factor * kPercent, local_t);
  }
  return project;
}

}  // namespace cutline::editor
