#include "cutline/ui/monitor.hpp"

#include "cutline/core/mask_path.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace cutline::ui {
namespace {

/// Half the side of a handle's grab square, in pixels. The drawn square is the
/// same size: a handle that is easier to hit than it looks is a handle you
/// grab by accident when you meant to move the layer.
constexpr double kHandleReach = 5.0;

/// The most points this widget will let a path grow to.
///
/// The same cap the model carries, repeated rather than included: this header
/// describes what a widget draws and does not otherwise know a project exists.
/// A static assertion where the two meet would be better than a comment, and
/// there is nowhere in the widget layer that sees both.
constexpr std::size_t kMaxOverlayPoints = 64;

/// The path as the corners that get filled, which is what it is drawn as too.
///
/// Straight through `core::flatten_mask_path`, the same call the renderer makes
/// — so the outline on the picture is the region being masked rather than an
/// approximation of it that can drift.
[[nodiscard]] std::vector<core::MaskPoint> outline_of(const MaskOverlay& mask) {
  std::vector<core::MaskPoint> control;
  control.reserve(mask.points.size());
  for (const MaskVertex& point : mask.points) {
    control.push_back(core::MaskPoint{.x = point.x,
                                      .y = point.y,
                                      .in_x = point.in_x,
                                      .in_y = point.in_y,
                                      .out_x = point.out_x,
                                      .out_y = point.out_y});
  }
  return core::flatten_mask_path(control);
}

/// How far above the top edge the rotation handle floats. Clear of the corner
/// handles either side of it, which is the whole reason it is not simply on
/// the edge.
constexpr double kRotateOffset = 13.0;

// The border around the picture is `kMonitorInset`, in the header because
// `picture()` is public and where it lands depends on it. It has to clear the
// rotation handle, which reaches furthest.
static_assert(kMonitorInset >= kRotateOffset + kHandleReach,
              "the border has to be wide enough for the rotation handle");

/// How close a drag has to come before it snaps, in pixels of the widget
/// rather than fractions of the canvas — this is about what the hand can aim
/// at, not about what the canvas is.
constexpr double kSnapReach = 6.0;

/// The degrees a rotation snaps to with shift held. Twelve of them round the
/// circle: enough for the angles anybody types in, few enough to be a snap.
constexpr double kRotationStep = 15.0;

/// The smallest a layer may be dragged to, in canvas fractions. Not zero: a
/// box with no width has no handles to grab, so it could be scaled down and
/// never back up.
constexpr double kMinExtent = 0.02;

/// How far a press must travel before it is a drag out of the picture rather
/// than a click on it. The media pool's own threshold, because it is the same
/// gesture starting somewhere else.
constexpr double kMonitorDragThreshold = 4.0;

[[nodiscard]] constexpr double to_radians(double degrees) noexcept {
  return degrees * std::numbers::pi / 180.0;
}

[[nodiscard]] std::pair<double, double> rotate(double x, double y, double radians) noexcept {
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  return {x * c - y * s, x * s + y * c};
}

[[nodiscard]] double distance(double ax, double ay, double bx, double by) noexcept {
  return std::hypot(ax - bx, ay - by);
}

/// One thing a drag can line up with: the value the centre has to take, and
/// the line to draw when it does. The two differ for an edge snap — the centre
/// lands half a box in from the frame, and the guide belongs on the frame.
struct SnapTarget {
  double centre = 0.0;
  double guide = 0.0;
};

/// The nearest target within `reach`, if any.
[[nodiscard]] std::optional<SnapTarget> snapped(double value,
                                                std::span<const SnapTarget> targets,
                                                double reach) noexcept {
  std::optional<SnapTarget> best;
  double closest = reach;
  for (const SnapTarget& target : targets) {
    const double gap = std::abs(value - target.centre);
    if (gap <= closest) {
      closest = gap;
      best = target;
    }
  }
  return best;
}

}  // namespace

MonitorView::MonitorView() {
  // The picture is letterboxed inside the panel, so nothing should ever run
  // over the edge; clipping is cheap insurance against a rounding error at the
  // boundary drawing a line of video across the panel beside it.
  set_clips_children(true);
}

void MonitorView::set_frame(const ImageView& frame) {
  frame_ = frame;
  texture_ = TextureView{};
}

void MonitorView::set_texture(const TextureView& frame) {
  texture_ = frame;
  frame_ = ImageView{};
}

void MonitorView::clear_frame() {
  frame_ = ImageView{};
  texture_ = TextureView{};
}

void MonitorView::set_canvas_aspect(double aspect) noexcept {
  if (aspect > 0.0) canvas_aspect_ = aspect;
}

void MonitorView::set_drop_lit(bool lit) noexcept {
  if (drop_lit_ == lit) return;
  drop_lit_ = lit;
  // A fresh frame and nothing more: nothing in the tree has moved.
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

Rect MonitorView::picture() const {
  // The frame's own shape when there is one, the sequence's when there is not.
  // Using the panel's shape while empty would make the picture jump into a
  // different rectangle the moment the first frame arrived.
  double aspect = canvas_aspect_;
  if (!frame_.empty()) aspect = frame_.aspect();
  else if (!texture_.empty()) aspect = texture_.aspect();
  return fit_aspect(bounds().inset(kMonitorInset), aspect);
}

std::optional<std::pair<double, double>> MonitorView::to_picture(double x, double y) const {
  const Rect area = picture();
  if (area.empty()) return std::nullopt;
  // Deliberately unclamped: a transform being dragged past the edge of the
  // frame is a normal thing to do, and clamping here would stop it dead at the
  // boundary.
  return std::pair{(x - area.x) / area.width, (y - area.y) / area.height};
}

void MonitorView::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = picture();
  if (area.empty()) return;

  const SurfaceStyle& style = theme.style(part(), state());

  // A drop landing here outlines the whole picture, because the whole picture
  // is the target: there is nothing smaller to aim at, and the clip it would
  // reach is whichever one is on screen.
  const auto outline_if_dropping = [&] {
    if (!drop_lit_) return;
    const SurfaceStyle& selected = theme.style(Part::Button, State::Selected);
    const Color ink = selected.border.a > 0.0 ? selected.border : selected.text;
    painter.stroke(area.inset(-2.0), 0.0, ink, 2.0);
  };

  if (!has_picture()) {
    // The shape of the sequence, drawn as a well, so it is obvious that this
    // is where the picture goes rather than looking like a broken panel.
    const SurfaceStyle& empty = theme.style(Part::Input, State::Disabled);
    paint_surface(painter, area, empty);
    if (!placeholder_.empty()) {
      painter.text(text_run(area, placeholder_, empty, theme.metrics.small_font_size,
                            TextAlign::Center, false));
    }
    outline_if_dropping();
    paint_masks(painter, theme);
    return;
  }

  if (!texture_.empty()) {
    painter.texture(area, texture_);
  } else {
    painter.image(area, frame_);
  }
  // A hairline around it, so a frame that is mostly black still reads as a
  // picture with edges rather than as a hole in the panel.
  if (style.border.a > 0.0) painter.stroke(area, 0.0, style.border, 1.0);

  outline_if_dropping();
  paint_masks(painter, theme);
  paint_overlay(painter, theme);
}

void MonitorView::paint_masks(Painter& painter, const Theme& theme) const {
  const Rect area = picture();
  if (area.empty() || masks_.empty()) return;

  const SurfaceStyle& panel = theme.style(Part::Panel, State::Normal);
  // White rather than the accent, and thin. A mask outline sits *inside* the
  // picture where the transform box sits around it, and the two in the same
  // colour would read as one shape with a line through it.
  const Color ink{1.0f, 1.0f, 1.0f, 0.85f};
  const Color grip_ink = theme.style(Part::Button, State::Selected).border.a > 0.0
                             ? theme.style(Part::Button, State::Selected).border
                             : panel.text;

  painter.push_clip(area, 0.0);
  for (std::size_t i = 0; i < masks_.size(); ++i) {
    const MaskOverlay& mask = masks_[i];
    const double cx = area.x + mask.x * area.width;
    const double cy = area.y + mask.y * area.height;
    const double half_w = std::abs(mask.width) * area.width;
    const double half_h = std::abs(mask.height) * area.height;
    const double radians = to_radians(mask.rotation);

    // A path being placed is drawn open, because it is: the run from the last
    // point back to the first is not part of it until the pen says so. And a
    // line trails to the pointer, so the edge about to be committed to can be
    // seen before the click that commits it.
    if (mask_drawing_.has_value() && *mask_drawing_ == i) {
      const auto on_screen = [&](double lx, double ly) {
        const auto [rx, ry] = rotate(lx * area.width, ly * area.height, radians);
        return std::pair{cx + rx, cy + ry};
      };

      constexpr int kCurveSteps = 12;
      for (std::size_t at = 0; at + 1 < mask.points.size(); ++at) {
        const MaskVertex& from = mask.points[at];
        const MaskVertex& to = mask.points[at + 1];
        const core::MaskPoint a{.x = from.x, .y = from.y, .out_x = from.out_x,
                                .out_y = from.out_y};
        const core::MaskPoint b{.x = to.x, .y = to.y, .in_x = to.in_x, .in_y = to.in_y};
        auto [previous_x, previous_y] = on_screen(a.x, a.y);
        for (int step = 1; step <= kCurveSteps; ++step) {
          const core::MaskPoint sample = core::mask_path_point_at(
              a, b, static_cast<double>(step) / static_cast<double>(kCurveSteps));
          const auto [sx, sy] = on_screen(sample.x, sample.y);
          painter.line(previous_x, previous_y, sx, sy, ink, 1.0);
          previous_x = sx;
          previous_y = sy;
        }
      }

      if (!mask.points.empty() && draw_pointer_known_ && !mask_dragging_.has_value()) {
        const MaskVertex& last = mask.points.back();
        const auto [lx, ly] = on_screen(last.x, last.y);
        painter.line(lx, ly, draw_x_, draw_y_, ink, 1.0);
      }

      for (std::size_t corner = 0; corner < mask.points.size(); ++corner) {
        const Rect handle = mask_corner_grip(i, corner);
        if (!handle.empty()) painter.fill(handle.inset(2.0), 0.0, Fill::solid(grip_ink));
      }
      continue;
    }

    // Drawn as a closed run of segments either way, because a rotated ellipse
    // is not a rectangle the painter can stroke and a rotated rectangle is not
    // one either. Thirty-two steps is smooth at any size this panel reaches.
    constexpr int kSteps = 32;
    // A path is drawn as the corners it fills as, curves already flattened, so
    // the outline and the mask are the same shape however it is bent.
    const std::vector<core::MaskPoint> outline =
        mask.shape == 3 ? outline_of(mask) : std::vector<core::MaskPoint>{};
    const int steps = mask.shape == 3 ? static_cast<int>(outline.size())
                      : mask.shape == 2 ? 4
                                        : kSteps;
    if (steps <= 0) continue;

    double previous_x = 0.0;
    double previous_y = 0.0;
    for (int step = 0; step <= steps; ++step) {
      const double t = static_cast<double>(step) / static_cast<double>(steps);
      double lx = 0.0;
      double ly = 0.0;
      if (mask.shape == 3) {
        // Its own corners, in the order they were drawn, closing on the first.
        const core::MaskPoint& corner = outline[static_cast<std::size_t>(step) % outline.size()];
        lx = corner.x * area.width;
        ly = corner.y * area.height;
      } else if (mask.shape == 2) {
        // The four corners, in order, closing back on the first.
        const double corner_x[4] = {-1.0, 1.0, 1.0, -1.0};
        const double corner_y[4] = {-1.0, -1.0, 1.0, 1.0};
        lx = corner_x[step % 4] * half_w;
        ly = corner_y[step % 4] * half_h;
      } else {
        const double angle = t * 2.0 * std::numbers::pi;
        lx = std::cos(angle) * half_w;
        ly = std::sin(angle) * half_h;
      }
      const auto [rx, ry] = rotate(lx, ly, radians);
      const double x = cx + rx;
      const double y = cy + ry;
      if (step > 0) painter.line(previous_x, previous_y, x, y, ink, 1.0);
      previous_x = x;
      previous_y = y;
    }

    const Rect grip = mask_grip(i);
    if (!grip.empty()) painter.fill(grip.inset(2.0), 0.0, Fill::solid(grip_ink));

    // A path has a handle on every corner instead: each one is what shapes it,
    // and a single grip would have nothing to mean.
    for (std::size_t corner = 0; corner < mask.points.size(); ++corner) {
      const Rect handle = mask_corner_grip(i, corner);
      if (!handle.empty()) painter.fill(handle.inset(2.0), 0.0, Fill::solid(grip_ink));
    }

    // And the two bezier handles of the point being worked on, each on the end
    // of a leg from the point it bends. Only that one point's: a path of any
    // size shows a thicket of squares otherwise, and Premiere shows them for
    // the selected point for the same reason.
    if (mask.shape == 3 && mask_selected_.has_value() && *mask_selected_ < mask.points.size() &&
        mask_dragging_.value_or(i) == i) {
      const MaskVertex& point = mask.points[*mask_selected_];
      const auto [px, py] = rotate(point.x * area.width, point.y * area.height, radians);
      for (const MaskHandle side : {MaskHandle::In, MaskHandle::Out}) {
        const Rect grip_rect = mask_handle_grip(i, *mask_selected_, side);
        if (grip_rect.empty()) continue;
        // The leg first, so the square sits on top of its own line.
        painter.line(cx + px, cy + py, grip_rect.x + grip_rect.width * 0.5,
                     grip_rect.y + grip_rect.height * 0.5, ink, 1.0);
        painter.fill(grip_rect.inset(2.0), 0.0, Fill::solid(ink));
      }
    }
  }
  painter.pop_clip();
}

void MonitorView::paint_overlay(Painter& painter, const Theme& theme) const {
  if (!box_.has_value()) return;
  const Rect area = picture();
  if (area.empty()) return;

  // The accent, so the overlay reads as the selection it is and picks up
  // whatever the theme's selection colour happens to be — over a picture, a
  // fixed colour is the one that will be invisible against some footage.
  const SurfaceStyle& selected = theme.style(Part::Button, State::Selected);
  const Color ink = selected.border.a > 0.0 ? selected.border : selected.text;

  // The guides first, so the box and its handles sit on top of them.
  for (const SnapGuide& guide : guides_) {
    const Color faint{ink.r, ink.g, ink.b, ink.a * 0.6f};
    if (guide.vertical) {
      const double x = area.x + guide.at * area.width;
      painter.line(x, area.y, x, area.bottom(), faint, 1.0);
    } else {
      const double y = area.y + guide.at * area.height;
      painter.line(area.x, y, area.right(), y, faint, 1.0);
    }
  }

  const auto c = corners();
  for (std::size_t i = 0; i < c.size(); ++i) {
    const auto& from = c[i];
    const auto& to = c[(i + 1) % c.size()];
    painter.line(from.first, from.second, to.first, to.second, ink, 1.0);
  }

  // The stalk to the rotation handle, so it reads as belonging to the box
  // rather than floating near it.
  const Rect spin = handle_rect(TransformHandle::Rotate);
  if (!spin.empty()) {
    const double top_x = (c[0].first + c[1].first) * 0.5;
    const double top_y = (c[0].second + c[1].second) * 0.5;
    painter.line(top_x, top_y, spin.x + kHandleReach, spin.y + kHandleReach, ink, 1.0);
  }

  constexpr std::array kDrawn{
      TransformHandle::TopLeft, TransformHandle::Top,    TransformHandle::TopRight,
      TransformHandle::Right,   TransformHandle::BottomRight, TransformHandle::Bottom,
      TransformHandle::BottomLeft, TransformHandle::Left, TransformHandle::Rotate};
  for (const TransformHandle handle : kDrawn) {
    const Rect box = handle_rect(handle);
    if (box.empty()) continue;
    // Filled pale and outlined in the accent: a solid accent square vanishes
    // against footage the same colour, and an outline alone vanishes against
    // detail.
    painter.fill(box, 0.0, Fill::solid(Color{1.0f, 1.0f, 1.0f, 0.9f}));
    painter.stroke(box, 0.0, ink, 1.0);
  }
}

// -------------------------------------------------------- transform handles --

void MonitorView::set_transform(std::optional<MonitorBox> box) {
  box_ = box;
  if (!box_.has_value()) {
    dragging_ = TransformHandle::None;
    guides_.clear();
  }
}

// ------------------------------------------------------------------- masks --

void MonitorView::set_masks(std::vector<MaskOverlay> masks) {
  // Placing survives the shapes being handed over again, which happens after
  // every point that lands: the model is what the points were written to, and
  // this is where they come back from. What cannot survive is the mask being
  // drawn going away entirely.
  if (mask_drawing_.has_value() && *mask_drawing_ >= masks.size()) mask_drawing_.reset();
  if (masks == masks_) return;
  masks_ = std::move(masks);
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

std::pair<double, double> MonitorView::mask_local(std::size_t index, double x, double y) const {
  const Rect area = picture();
  if (area.empty() || index >= masks_.size()) return {0.0, 0.0};
  if (area.width <= 0.0 || area.height <= 0.0) return {0.0, 0.0};

  const MaskOverlay& mask = masks_[index];
  const auto [lx, ly] = rotate(x - (area.x + mask.x * area.width),
                               y - (area.y + mask.y * area.height), -to_radians(mask.rotation));
  return {lx / area.width, ly / area.height};
}

bool MonitorView::place_mask_point(const MouseEvent& event) {
  const std::size_t index = *mask_drawing_;
  if (index >= masks_.size()) {
    finish_mask_drawing();
    return false;
  }
  MaskOverlay& mask = masks_[index];

  // Back on the first point closes the shape. Three is the fewest that encloses
  // anything, so below that the press is another point rather than a close —
  // otherwise a slightly wobbly second click would end the path at two.
  if (mask.points.size() >= 3 && mask_corner_grip(index, 0).contains(event.x, event.y)) {
    finish_mask_drawing();
    if (on_mask_commit_) on_mask_commit_(index, mask);
    return true;
  }

  if (mask.points.size() >= kMaxOverlayPoints) {
    finish_mask_drawing();
    return true;
  }

  const auto [px, py] = mask_local(index, event.x, event.y);
  mask.points.push_back(MaskVertex{.x = px, .y = py});

  // Held and dragged, the press pulls the handles out of the point it just
  // put down — which is how a curve is drawn in one gesture instead of being
  // placed and then bent afterwards. The drag machinery already does exactly
  // this for a handle, so the press hands over to it.
  mask_dragging_ = index;
  mask_resizing_ = false;
  mask_corner_ = mask.points.size() - 1;
  mask_handle_ = MaskHandle::Out;
  mask_handle_broken_ = event.modifiers.alt;
  mask_selected_ = mask_corner_;
  mask_placed_ = true;
  mask_origin_ = mask;
  press_x_ = event.x;
  press_y_ = event.y;

  if (on_mask_change_) on_mask_change_(index, mask);
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
  return true;
}

void MonitorView::begin_mask_drawing(std::size_t index) {
  if (index >= masks_.size()) return;
  mask_drawing_ = index;
  mask_selected_.reset();
  mask_dragging_.reset();
  mask_corner_.reset();
  mask_handle_.reset();
  draw_pointer_known_ = false;

  // From nothing. Premiere's pen starts on an empty frame, and a path that
  // began as somebody else's rectangle is not one you drew.
  masks_[index].points.clear();
  if (on_mask_commit_) on_mask_commit_(index, masks_[index]);
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

void MonitorView::finish_mask_drawing() {
  if (!mask_drawing_.has_value()) return;
  mask_drawing_.reset();
  draw_pointer_known_ = false;
  mask_dragging_.reset();
  mask_corner_.reset();
  mask_handle_.reset();
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

namespace {

/// A mask's centre and half-extents in widget pixels.
struct MaskBox {
  double cx = 0.0;
  double cy = 0.0;
  double half_w = 0.0;
  double half_h = 0.0;
  double radians = 0.0;
};

[[nodiscard]] MaskBox mask_box(const MaskOverlay& mask, const Rect& area) {
  return MaskBox{
      .cx = area.x + mask.x * area.width,
      .cy = area.y + mask.y * area.height,
      .half_w = std::abs(mask.width) * area.width,
      .half_h = std::abs(mask.height) * area.height,
      .radians = to_radians(mask.rotation),
  };
}

/// A point brought into the mask's own frame, where the shape is axis-aligned.
[[nodiscard]] std::pair<double, double> into_mask(const MaskBox& box, double x, double y) {
  const auto [lx, ly] = rotate(x - box.cx, y - box.cy, -box.radians);
  return {lx, ly};
}

/// Whether a point is inside a free-drawn path, by the same even-odd rule the
/// shader fills it with — so what you can grab and what you can see agree.
[[nodiscard]] bool inside_path(const MaskOverlay& mask, const Rect& area, double x, double y) {
  const std::vector<core::MaskPoint> outline = outline_of(mask);
  if (outline.size() < 3 || area.width <= 0.0 || area.height <= 0.0) return false;

  const auto [lx, ly] = rotate(x - (area.x + mask.x * area.width),
                               y - (area.y + mask.y * area.height), -to_radians(mask.rotation));
  const double px = lx / area.width;
  const double py = ly / area.height;

  bool inside = false;
  for (std::size_t i = 0, previous = outline.size() - 1; i < outline.size(); previous = i++) {
    const double cx = outline[i].x;
    const double cy = outline[i].y;
    const double ox = outline[previous].x;
    const double oy = outline[previous].y;
    if ((cy > py) != (oy > py)) {
      const double t = (py - cy) / (oy - cy);
      if (px < cx + t * (ox - cx)) inside = !inside;
    }
  }
  return inside;
}

}  // namespace

Rect MonitorView::mask_corner_grip(std::size_t index, std::size_t corner) const {
  const Rect area = picture();
  if (area.empty() || index >= masks_.size()) return Rect{};

  const MaskOverlay& mask = masks_[index];
  if (mask.shape != 3 || corner >= mask.points.size()) return Rect{};

  const auto [ox, oy] = rotate(mask.points[corner].x * area.width,
                               mask.points[corner].y * area.height,
                               to_radians(mask.rotation));
  const double cx = area.x + mask.x * area.width + ox;
  const double cy = area.y + mask.y * area.height + oy;
  return Rect{cx - kHandleReach, cy - kHandleReach, kHandleReach * 2.0, kHandleReach * 2.0};
}

std::optional<std::size_t> MonitorView::add_mask_point(std::size_t index, double x, double y) {
  const Rect area = picture();
  if (area.empty() || index >= masks_.size()) return std::nullopt;
  if (area.width <= 0.0 || area.height <= 0.0) return std::nullopt;

  MaskOverlay& mask = masks_[index];
  if (mask.shape != 3 || mask.points.size() < 2) return std::nullopt;

  // Into the path's own frame, the same way the hit test goes.
  const auto [lx, ly] = rotate(x - (area.x + mask.x * area.width),
                               y - (area.y + mask.y * area.height), -to_radians(mask.rotation));
  const double px = lx / area.width;
  const double py = ly / area.height;

  std::vector<core::MaskPoint> control;
  control.reserve(mask.points.size());
  for (const MaskVertex& point : mask.points) {
    control.push_back(core::MaskPoint{.x = point.x,
                                      .y = point.y,
                                      .in_x = point.in_x,
                                      .in_y = point.in_y,
                                      .out_x = point.out_x,
                                      .out_y = point.out_y});
  }

  const std::optional<core::MaskPathHit> hit = core::nearest_on_mask_path(control, px, py);
  if (!hit.has_value()) return std::nullopt;

  // Near enough to have been aimed at the outline. Measured against the widget
  // rather than the frame, because what a hand can aim is a number of pixels.
  const double away = std::hypot(hit->distance * area.width, hit->distance * area.height);
  if (away > kHandleReach * 3.0) return std::nullopt;

  const std::vector<core::MaskPoint> split = core::split_mask_path(control, hit->segment, hit->t);
  mask.points.clear();
  mask.points.reserve(split.size());
  for (const core::MaskPoint& point : split) {
    mask.points.push_back(MaskVertex{.x = point.x,
                                     .y = point.y,
                                     .in_x = point.in_x,
                                     .in_y = point.in_y,
                                     .out_x = point.out_x,
                                     .out_y = point.out_y});
  }
  return hit->segment + 1;
}

void MonitorView::curve_mask_point(std::size_t index, std::size_t corner) {
  if (index >= masks_.size()) return;
  MaskOverlay& mask = masks_[index];
  const std::size_t count = mask.points.size();
  if (count < 3 || corner >= count) return;

  const MaskVertex& previous = mask.points[(corner + count - 1) % count];
  const MaskVertex& next = mask.points[(corner + 1) % count];
  MaskVertex& point = mask.points[corner];

  // The direction the path is running as it passes through, taken from the
  // neighbours rather than from the point itself — the point is on the line, so
  // it says nothing about which way the line goes.
  const double dx = next.x - previous.x;
  const double dy = next.y - previous.y;
  const double length = std::hypot(dx, dy);
  if (length <= 0.0) return;

  const double ux = dx / length;
  const double uy = dy / length;
  // A third of the way to each neighbour: far enough to round the corner
  // visibly, near enough that it does not overshoot into the next one.
  const double to_next = std::hypot(next.x - point.x, next.y - point.y) / 3.0;
  const double to_previous = std::hypot(point.x - previous.x, point.y - previous.y) / 3.0;
  point.out_x = ux * to_next;
  point.out_y = uy * to_next;
  point.in_x = -ux * to_previous;
  point.in_y = -uy * to_previous;
}

Rect MonitorView::mask_handle_grip(std::size_t index, std::size_t corner,
                                   MaskHandle side) const {
  const Rect area = picture();
  if (area.empty() || index >= masks_.size()) return Rect{};
  // Only the selected point's, which is what makes the picture readable — and
  // what makes the hit test below unambiguous, since a handle sitting on its
  // own point would otherwise be indistinguishable from it.
  if (!mask_selected_.has_value() || *mask_selected_ != corner) return Rect{};

  const MaskOverlay& mask = masks_[index];
  if (mask.shape != 3 || corner >= mask.points.size()) return Rect{};

  const MaskVertex& point = mask.points[corner];
  const double hx = side == MaskHandle::In ? point.in_x : point.out_x;
  const double hy = side == MaskHandle::In ? point.in_y : point.out_y;

  const auto [ox, oy] = rotate((point.x + hx) * area.width, (point.y + hy) * area.height,
                               to_radians(mask.rotation));
  const double cx = area.x + mask.x * area.width + ox;
  const double cy = area.y + mask.y * area.height + oy;
  return Rect{cx - kHandleReach, cy - kHandleReach, kHandleReach * 2.0, kHandleReach * 2.0};
}

Rect MonitorView::mask_grip(std::size_t index) const {
  const Rect area = picture();
  if (area.empty() || index >= masks_.size()) return Rect{};
  // A path has no size to grip: its corners are its handles, and one more in
  // the middle of them would be a handle that means something different from
  // every other handle on the shape.
  if (masks_[index].shape == 3) return Rect{};

  // The bottom-right corner of the shape's own box, turned with it. One grip
  // rather than four: a mask is symmetric about its centre, so any corner says
  // the same thing, and three more would crowd a shape that is often small.
  const MaskBox box = mask_box(masks_[index], area);
  const auto [ox, oy] = rotate(box.half_w, box.half_h, box.radians);
  return Rect{box.cx + ox - kHandleReach, box.cy + oy - kHandleReach, kHandleReach * 2.0,
              kHandleReach * 2.0};
}

std::optional<std::size_t> MonitorView::mask_at(double x, double y) const {
  const Rect area = picture();
  if (area.empty()) return std::nullopt;

  // Backwards, so the last drawn is the first found — the same rule the layer
  // stack follows, and the one that makes the mask on top the one you grab.
  for (std::size_t i = masks_.size(); i-- > 0;) {
    if (mask_grip(i).contains(x, y)) return i;

    if (masks_[i].shape == 3) {
      // A corner first, then anywhere inside the shape, which moves the whole
      // path — the same two things the other shapes offer.
      for (std::size_t corner = 0; corner < masks_[i].points.size(); ++corner) {
        if (mask_corner_grip(i, corner).contains(x, y)) return i;
      }
      if (inside_path(masks_[i], area, x, y)) return i;
      continue;
    }

    const MaskBox box = mask_box(masks_[i], area);
    if (box.half_w <= 0.0 || box.half_h <= 0.0) continue;
    const auto [lx, ly] = into_mask(box, x, y);

    if (masks_[i].shape == 2) {
      if (std::abs(lx) <= box.half_w && std::abs(ly) <= box.half_h) return i;
    } else {
      const double nx = lx / box.half_w;
      const double ny = ly / box.half_h;
      if (nx * nx + ny * ny <= 1.0) return i;
    }
  }
  return std::nullopt;
}

std::pair<double, double> MonitorView::centre_px() const {
  const Rect area = picture();
  if (!box_.has_value() || area.empty()) return {0.0, 0.0};
  return {area.x + box_->x * area.width, area.y + box_->y * area.height};
}

std::array<std::pair<double, double>, 4> MonitorView::corners() const {
  const Rect area = picture();
  if (!box_.has_value() || area.empty()) return {};

  const auto [cx, cy] = centre_px();
  const double half_w = box_->width * area.width * 0.5;
  const double half_h = box_->height * area.height * 0.5;
  const double angle = to_radians(box_->rotation);

  // Clockwise from the top left, which is the order every handle enumerator
  // and every hit test below assumes.
  const std::array<std::pair<double, double>, 4> local{
      std::pair{-half_w, -half_h}, std::pair{half_w, -half_h},
      std::pair{half_w, half_h},   std::pair{-half_w, half_h}};

  std::array<std::pair<double, double>, 4> out{};
  for (std::size_t i = 0; i < local.size(); ++i) {
    const auto [dx, dy] = rotate(local[i].first, local[i].second, angle);
    out[i] = {cx + dx, cy + dy};
  }
  return out;
}

Rect MonitorView::handle_rect(TransformHandle handle) const {
  if (!box_.has_value() || handle == TransformHandle::None ||
      handle == TransformHandle::Move) {
    return {};
  }

  const auto c = corners();
  const auto midpoint = [&c](std::size_t a, std::size_t b) {
    return std::pair{(c[a].first + c[b].first) * 0.5, (c[a].second + c[b].second) * 0.5};
  };

  std::pair<double, double> at{};
  switch (handle) {
    case TransformHandle::TopLeft: at = c[0]; break;
    case TransformHandle::TopRight: at = c[1]; break;
    case TransformHandle::BottomRight: at = c[2]; break;
    case TransformHandle::BottomLeft: at = c[3]; break;
    case TransformHandle::Top: at = midpoint(0, 1); break;
    case TransformHandle::Right: at = midpoint(1, 2); break;
    case TransformHandle::Bottom: at = midpoint(2, 3); break;
    case TransformHandle::Left: at = midpoint(3, 0); break;
    case TransformHandle::Rotate: {
      // Out along the box's own "up", so it stays above the top edge however
      // far the layer has been turned.
      const auto top = midpoint(0, 1);
      const auto [ox, oy] = rotate(0.0, -kRotateOffset, to_radians(box_->rotation));
      at = {top.first + ox, top.second + oy};
      break;
    }
    default: return {};
  }
  return Rect{at.first - kHandleReach, at.second - kHandleReach, kHandleReach * 2.0,
              kHandleReach * 2.0};
}

TransformHandle MonitorView::handle_at(double x, double y) const {
  if (!box_.has_value()) return TransformHandle::None;

  // Corners before edges, and rotation before either: they overlap, and the
  // smaller, more specific target has to win or it can never be hit.
  constexpr std::array kOrder{TransformHandle::Rotate,   TransformHandle::TopLeft,
                              TransformHandle::TopRight, TransformHandle::BottomRight,
                              TransformHandle::BottomLeft, TransformHandle::Top,
                              TransformHandle::Right,    TransformHandle::Bottom,
                              TransformHandle::Left};
  for (const TransformHandle handle : kOrder) {
    if (handle_rect(handle).contains(x, y)) return handle;
  }

  // Inside the box itself, which moves it. Tested in the box's own frame so a
  // rotated layer is grabbed where it looks rather than where its bounding
  // rectangle is.
  const auto [cx, cy] = centre_px();
  const Rect area = picture();
  if (area.empty()) return TransformHandle::None;
  const auto [lx, ly] = rotate(x - cx, y - cy, -to_radians(box_->rotation));
  if (std::abs(lx) <= box_->width * area.width * 0.5 &&
      std::abs(ly) <= box_->height * area.height * 0.5) {
    return TransformHandle::Move;
  }
  return TransformHandle::None;
}

bool MonitorView::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  // Placing a path takes every press before anything else looks at it. That is
  // what a mode is for: while the pen is out, the picture is somewhere to put
  // points and not a layer to drag or a mask to pick up.
  if (mask_drawing_.has_value()) return place_mask_point(event);

  // Three things want this press, and the order between them is the whole of
  // what makes both gestures reachable.
  //
  // The layer's *edge and corner* handles come first: they are small, they sit
  // where a mask may well also be, and a corner a mask had swallowed would
  // leave the layer impossible to resize.
  //
  // A mask comes next — ahead of `Move`, which means "anywhere inside the
  // layer". Taking `Move` first would swallow every mask on the picture, which
  // is exactly what it did the first time this was tried.
  const TransformHandle handle = box_.has_value() ? handle_at(event.x, event.y)
                                                  : TransformHandle::None;
  const auto take_layer = [&] {
    dragging_ = handle;
    origin_ = *box_;
    press_x_ = event.x;
    press_y_ = event.y;
    guides_.clear();
    return true;
  };

  if (handle != TransformHandle::None && handle != TransformHandle::Move) return take_layer();

  if (const std::optional<std::size_t> mask = mask_at(event.x, event.y); mask.has_value()) {
    mask_dragging_ = mask;
    mask_resizing_ = mask_grip(*mask).contains(event.x, event.y);
    mask_corner_.reset();
    mask_handle_.reset();
    mask_handle_broken_ = event.modifiers.alt;

    // A bezier handle first of all. It sits further out than the point it
    // belongs to and only exists for the selected one, so testing it first
    // costs the other points nothing and makes the near ones reachable.
    if (mask_selected_.has_value()) {
      for (const MaskHandle side : {MaskHandle::In, MaskHandle::Out}) {
        if (mask_handle_grip(*mask, *mask_selected_, side).contains(event.x, event.y)) {
          mask_handle_ = side;
          mask_corner_ = mask_selected_;
          break;
        }
      }
    }

    // Then a corner, before the shape, so grabbing one of a path's handles
    // reshapes it rather than moving the whole thing — the same order the
    // layer's own handles are tested in, and for the same reason.
    if (!mask_handle_.has_value()) {
      for (std::size_t corner = 0; corner < masks_[*mask].points.size(); ++corner) {
        if (mask_corner_grip(*mask, corner).contains(event.x, event.y)) {
          // Alt on a point takes it out. A path needs three to enclose
          // anything, so the third is where removing stops — below that there
          // is no shape left to edit back up from.
          if (event.modifiers.alt && masks_[*mask].points.size() > 3) {
            masks_[*mask].points.erase(masks_[*mask].points.begin() +
                                       static_cast<std::ptrdiff_t>(corner));
            mask_selected_.reset();
            mask_dragging_.reset();
            mask_handle_broken_ = false;
            if (on_mask_commit_) on_mask_commit_(*mask, masks_[*mask]);
            return true;
          }

          mask_corner_ = corner;
          mask_selected_ = corner;

          // Double-clicking a point turns it between a corner and a curve.
          // That is the one gesture that changes a point's *kind* rather than
          // its position, and Premiere spells it the same way.
          if (event.click_count >= 2) {
            MaskVertex& point = masks_[*mask].points[corner];
            if (point.in_x == 0.0 && point.in_y == 0.0 && point.out_x == 0.0 &&
                point.out_y == 0.0) {
              curve_mask_point(*mask, corner);
            } else {
              point.in_x = point.in_y = point.out_x = point.out_y = 0.0;
            }
            mask_dragging_.reset();
            mask_corner_.reset();
            if (on_mask_commit_) on_mask_commit_(*mask, masks_[*mask]);
            return true;
          }
          break;
        }
      }
    }

    // Control and a click on the outline puts a point there. The curve is split
    // rather than re-cut, so the shape does not so much as twitch — adding
    // detail has to be something you can do without losing what you drew.
    if (!mask_corner_.has_value() && !mask_handle_.has_value() &&
        event.modifiers.control && masks_[*mask].shape == 3 &&
        masks_[*mask].points.size() < kMaxOverlayPoints) {
      if (const std::optional<std::size_t> added = add_mask_point(*mask, event.x, event.y);
          added.has_value()) {
        mask_selected_ = added;
        mask_dragging_.reset();
        mask_handle_broken_ = false;
        if (on_mask_commit_) on_mask_commit_(*mask, masks_[*mask]);
        return true;
      }
    }

    // A press on the body of the path, away from any point, puts the handles
    // away — the same as clicking off a selection anywhere else.
    if (!mask_corner_.has_value() && !mask_handle_.has_value()) mask_selected_.reset();

    mask_origin_ = masks_[*mask];
    press_x_ = event.x;
    press_y_ = event.y;
    return true;
  }
  mask_selected_.reset();

  if (handle == TransformHandle::Move) return take_layer();

  // Last, so it can only start where nothing else wanted the press: a layer's
  // handles and a mask both come first, and a monitor showing neither — the
  // source monitor, which has no selection to transform — is all picture.
  if (on_drag_out_ && picture().contains(event.x, event.y)) {
    pressed_out_ = true;
    press_x_ = event.x;
    press_y_ = event.y;
    return true;
  }
  return false;
}

bool MonitorView::on_mouse_move(const MouseEvent& event) {
  if (mask_dragging_.has_value()) {
    drag_mask(event.x, event.y);
    if (on_mask_change_) on_mask_change_(*mask_dragging_, masks_[*mask_dragging_]);
    return true;
  }

  // With the pen out and nothing held, the pointer is where the next point
  // would go — so the path trails a line to it and the shape can be seen
  // before it is committed to.
  if (mask_drawing_.has_value()) {
    draw_x_ = event.x;
    draw_y_ = event.y;
    draw_pointer_known_ = true;
    if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
    return true;
  }

  if (pressed_out_) {
    drag_x_ = event.x;
    drag_y_ = event.y;
    if (!dragging_out_) {
      // The same threshold the media pool uses, and for the same reason: a
      // press that wobbles by a pixel is a click, and treating it as a drag
      // would make the picture impossible to simply press on.
      dragging_out_ = std::hypot(event.x - press_x_, event.y - press_y_) >= kMonitorDragThreshold;
    }
    return true;
  }

  if (dragging_ == TransformHandle::None || !box_.has_value()) return false;
  drag_to(event.x, event.y, event.modifiers);
  if (on_change_) on_change_(*box_);
  return true;
}

void MonitorView::drag_mask(double x, double y) {
  const Rect area = picture();
  if (!mask_dragging_.has_value() || area.empty()) return;
  if (area.width <= 0.0 || area.height <= 0.0) return;

  // From the press rather than accumulated, so a drag that goes out and comes
  // back lands where it started rather than somewhere it drifted to.
  const double dx = (x - press_x_) / area.width;
  const double dy = (y - press_y_) / area.height;

  MaskOverlay next = mask_origin_;
  if (mask_handle_.has_value() && mask_corner_.has_value() &&
      *mask_corner_ < next.points.size()) {
    // A bezier handle. In the path's own frame like everything else here.
    const auto [lx, ly] = rotate(dx, dy, -to_radians(next.rotation));
    const MaskVertex& was = mask_origin_.points[*mask_corner_];
    MaskVertex& point = next.points[*mask_corner_];

    const bool out = *mask_handle_ == MaskHandle::Out;
    if (out) {
      point.out_x = was.out_x + lx;
      point.out_y = was.out_y + ly;
    } else {
      point.in_x = was.in_x + lx;
      point.in_y = was.in_y + ly;
    }

    // The pair moves together unless it is being broken. That is what keeps a
    // curve smooth through the point while it is being shaped, and holding Alt
    // is how you get a crease — the same modifier, and the same meaning, as
    // every other program with a pen in it.
    if (!mask_handle_broken_) {
      core::MaskPoint mirrored{.x = point.x,
                               .y = point.y,
                               .in_x = point.in_x,
                               .in_y = point.in_y,
                               .out_x = point.out_x,
                               .out_y = point.out_y};
      core::smooth_mask_point(mirrored, out);
      point.in_x = mirrored.in_x;
      point.in_y = mirrored.in_y;
      point.out_x = mirrored.out_x;
      point.out_y = mirrored.out_y;
    }
  } else if (mask_corner_.has_value() && *mask_corner_ < next.points.size()) {
    // In the path's own frame, so dragging a corner of a turned shape moves it
    // where the pointer went rather than where the rotation sends it. The
    // handles travel with the point: they are offsets from it, so moving it
    // moves the curve either side without reshaping it.
    const auto [lx, ly] = rotate(dx, dy, -to_radians(next.rotation));
    next.points[*mask_corner_].x = mask_origin_.points[*mask_corner_].x + lx;
    next.points[*mask_corner_].y = mask_origin_.points[*mask_corner_].y + ly;
  } else if (mask_resizing_) {
    // In the mask's own frame, so dragging a corner of a turned shape grows it
    // along its own axes rather than the screen's.
    const auto [lx, ly] = rotate(dx, dy, -to_radians(next.rotation));
    next.width = std::max(kMinMaskExtent, mask_origin_.width + lx);
    next.height = std::max(kMinMaskExtent, mask_origin_.height + ly);
  } else {
    next.x = mask_origin_.x + dx;
    next.y = mask_origin_.y + dy;
  }
  masks_[*mask_dragging_] = next;
}

bool MonitorView::on_mouse_up(const MouseEvent& event) {
  if (mask_dragging_.has_value()) {
    const std::size_t index = *mask_dragging_;
    const bool placed = mask_placed_;
    mask_dragging_.reset();
    mask_resizing_ = false;
    mask_corner_.reset();
    mask_handle_.reset();
    mask_handle_broken_ = false;
    mask_placed_ = false;
    if (index < masks_.size() && (placed || masks_[index] != mask_origin_) && on_mask_commit_) {
      on_mask_commit_(index, masks_[index]);
    }
    return true;
  }

  if (pressed_out_) {
    const bool dragged = dragging_out_;
    pressed_out_ = false;
    dragging_out_ = false;
    // Reported wherever it was released, including outside the monitor — the
    // press captured the pointer, so a drop on the timeline still arrives here.
    if (dragged && on_drag_out_) on_drag_out_(event.x, event.y);
    return true;
  }

  if (dragging_ == TransformHandle::None) return false;

  drag_to(event.x, event.y, event.modifiers);
  dragging_ = TransformHandle::None;
  guides_.clear();
  if (box_.has_value() && *box_ != origin_ && on_commit_) on_commit_(*box_);
  return true;
}

void MonitorView::drag_to(double x, double y, const Modifiers& modifiers) {
  guides_.clear();
  switch (dragging_) {
    case TransformHandle::Move: move_to(x, y, modifiers); break;
    case TransformHandle::Rotate: rotate_to(x, y, modifiers); break;
    case TransformHandle::None: break;
    default: resize_to(x, y, modifiers); break;
  }
}

void MonitorView::move_to(double x, double y, const Modifiers& modifiers) {
  const Rect area = picture();
  if (area.empty()) return;

  MonitorBox next = origin_;
  next.x = origin_.x + (x - press_x_) / area.width;
  next.y = origin_.y + (y - press_y_) / area.height;

  // Snapping is off while control is held, which is the way out of a snap that
  // is fighting you. Off, too, for a rotated layer on the edges: the edges of
  // a turned box are not the edges of anything the canvas has, and snapping a
  // corner of a rhombus to the frame is a coincidence rather than an
  // alignment. Its centre still snaps, because a centre is a centre.
  if (snapping_ && !modifiers.control) {
    const bool square = std::abs(std::fmod(origin_.rotation, 360.0)) < 1e-9;
    const double reach_x = kSnapReach / area.width;
    const double reach_y = kSnapReach / area.height;

    // Centre to centre, and each edge to the frame's matching edge. Expressed
    // as targets for the *centre* so one comparison covers all of them.
    std::vector<SnapTarget> xs{{.centre = 0.5, .guide = 0.5}};
    std::vector<SnapTarget> ys{{.centre = 0.5, .guide = 0.5}};
    if (square) {
      xs.push_back({.centre = next.width * 0.5, .guide = 0.0});
      xs.push_back({.centre = 1.0 - next.width * 0.5, .guide = 1.0});
      ys.push_back({.centre = next.height * 0.5, .guide = 0.0});
      ys.push_back({.centre = 1.0 - next.height * 0.5, .guide = 1.0});
    }

    if (const auto at = snapped(next.x, xs, reach_x)) {
      next.x = at->centre;
      guides_.push_back(SnapGuide{.vertical = true, .at = at->guide});
    }
    if (const auto at = snapped(next.y, ys, reach_y)) {
      next.y = at->centre;
      guides_.push_back(SnapGuide{.vertical = false, .at = at->guide});
    }
  }

  box_ = next;
}

void MonitorView::resize_to(double x, double y, const Modifiers& modifiers) {
  const Rect area = picture();
  if (area.empty()) return;

  const double angle = to_radians(origin_.rotation);
  const double cx = area.x + origin_.x * area.width;
  const double cy = area.y + origin_.y * area.height;
  const double half_w = origin_.width * area.width * 0.5;
  const double half_h = origin_.height * area.height * 0.5;

  // In the box's own frame, where a resize is arithmetic on two numbers
  // whatever the rotation is.
  const auto [px, py] = rotate(x - cx, y - cy, -angle);

  // Which sides the handle moves: -1 for the left or top, +1 for the right or
  // bottom, 0 for a side it leaves alone. The opposite side is what stays put,
  // which is what makes a corner drag feel like it is pinned there.
  int grip_x = 0;
  int grip_y = 0;
  switch (dragging_) {
    case TransformHandle::TopLeft: grip_x = -1; grip_y = -1; break;
    case TransformHandle::TopRight: grip_x = 1; grip_y = -1; break;
    case TransformHandle::BottomRight: grip_x = 1; grip_y = 1; break;
    case TransformHandle::BottomLeft: grip_x = -1; grip_y = 1; break;
    case TransformHandle::Top: grip_y = -1; break;
    case TransformHandle::Bottom: grip_y = 1; break;
    case TransformHandle::Left: grip_x = -1; break;
    case TransformHandle::Right: grip_x = 1; break;
    default: return;
  }

  const double min_w = kMinExtent * area.width;
  const double min_h = kMinExtent * area.height;

  double left = -half_w;
  double right = half_w;
  double top = -half_h;
  double bottom = half_h;
  if (grip_x < 0) left = std::min(px, right - min_w);
  if (grip_x > 0) right = std::max(px, left + min_w);
  if (grip_y < 0) top = std::min(py, bottom - min_h);
  if (grip_y > 0) bottom = std::max(py, top + min_h);

  double width = right - left;
  double height = bottom - top;

  // Shift keeps the shape, and the aspect lock makes that the default rather
  // than giving the same key a second, opposite meaning. Applied by taking the
  // larger of the two changes rather than one axis arbitrarily, so the layer
  // follows the pointer on whichever axis it was pulled hardest.
  const bool proportional = modifiers.shift || aspect_locked_;
  if (proportional && grip_x != 0 && grip_y != 0 && half_w > 0.0 && half_h > 0.0) {
    const double factor = std::max(width / (half_w * 2.0), height / (half_h * 2.0));
    const double wanted_w = std::max(min_w, half_w * 2.0 * factor);
    const double wanted_h = std::max(min_h, half_h * 2.0 * factor);
    // Grown or shrunk about the corner that is staying put, not about the
    // centre — otherwise the anchor drifts and the drag feels unhinged.
    if (grip_x < 0) left = right - wanted_w; else right = left + wanted_w;
    if (grip_y < 0) top = bottom - wanted_h; else bottom = top + wanted_h;
    width = right - left;
    height = bottom - top;
  }

  // The new centre, back in the widget's frame.
  const auto [ox, oy] = rotate((left + right) * 0.5, (top + bottom) * 0.5, angle);

  MonitorBox next = origin_;
  next.width = width / area.width;
  next.height = height / area.height;
  next.x = (cx + ox - area.x) / area.width;
  next.y = (cy + oy - area.y) / area.height;
  box_ = next;
}

void MonitorView::rotate_to(double x, double y, const Modifiers& modifiers) {
  const auto [cx, cy] = centre_px();
  if (distance(x, y, cx, cy) < 1.0) return;

  // The handle floats above the box, so pointing straight up is no rotation.
  double degrees = std::atan2(y - cy, x - cx) * 180.0 / std::numbers::pi + 90.0;
  if (modifiers.shift) degrees = std::round(degrees / kRotationStep) * kRotationStep;

  // Into (-180, 180], which is the range the inspector's slider covers and the
  // one the model is written against.
  degrees = std::fmod(degrees + 180.0, 360.0);
  if (degrees < 0.0) degrees += 360.0;

  MonitorBox next = origin_;
  next.rotation = degrees - 180.0;
  box_ = next;
}

}  // namespace cutline::ui
