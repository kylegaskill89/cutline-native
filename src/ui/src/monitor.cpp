#include "cutline/ui/monitor.hpp"

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

    // Drawn as a closed run of segments either way, because a rotated ellipse
    // is not a rectangle the painter can stroke and a rotated rectangle is not
    // one either. Thirty-two steps is smooth at any size this panel reaches.
    constexpr int kSteps = 32;
    const int steps = mask.shape == 2 ? 4 : kSteps;

    double previous_x = 0.0;
    double previous_y = 0.0;
    for (int step = 0; step <= steps; ++step) {
      const double t = static_cast<double>(step) / static_cast<double>(steps);
      double lx = 0.0;
      double ly = 0.0;
      if (mask.shape == 2) {
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
  if (masks == masks_) return;
  masks_ = std::move(masks);
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

}  // namespace

Rect MonitorView::mask_grip(std::size_t index) const {
  const Rect area = picture();
  if (area.empty() || index >= masks_.size()) return Rect{};

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
    mask_origin_ = masks_[*mask];
    press_x_ = event.x;
    press_y_ = event.y;
    return true;
  }

  if (handle == TransformHandle::Move) return take_layer();
  return false;
}

bool MonitorView::on_mouse_move(const MouseEvent& event) {
  if (mask_dragging_.has_value()) {
    drag_mask(event.x, event.y);
    if (on_mask_change_) on_mask_change_(*mask_dragging_, masks_[*mask_dragging_]);
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
  if (mask_resizing_) {
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
    mask_dragging_.reset();
    mask_resizing_ = false;
    if (index < masks_.size() && masks_[index] != mask_origin_ && on_mask_commit_) {
      on_mask_commit_(index, masks_[index]);
    }
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
