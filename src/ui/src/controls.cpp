#include "cutline/ui/controls.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::ui {
namespace {

/// A range narrower than this is treated as having no extent at all, which
/// keeps every fraction from becoming a division by nearly zero.
constexpr double kDegenerate = 1e-12;

/// Coarse nudge, for arrow keys with shift held.
constexpr double kCoarseNudge = 10.0;

}  // namespace

// -------------------------------------------------------------- the range --

double ValueRange::clamp(double value) const noexcept {
  return std::clamp(value, std::min(minimum, maximum), std::max(minimum, maximum));
}

double ValueRange::quantise(double value) const noexcept {
  if (step <= 0.0) return clamp(value);
  // Counted from the minimum, not from zero. A control from 5 to 100 in tens
  // must offer 5, 15, 25 — quantising from zero would offer 10, 20, 30 and
  // never let it reach either end.
  const double steps = std::round((value - minimum) / step);
  return clamp(minimum + steps * step);
}

double ValueRange::to_fraction(double value) const noexcept {
  const double span = maximum - minimum;
  if (std::abs(span) < kDegenerate) return 0.0;
  return std::clamp((clamp(value) - minimum) / span, 0.0, 1.0);
}

double ValueRange::from_fraction(double fraction) const noexcept {
  return quantise(minimum + std::clamp(fraction, 0.0, 1.0) * (maximum - minimum));
}

double ValueRange::nudge() const noexcept {
  if (step > 0.0) return step;
  const double span = std::abs(maximum - minimum);
  return span < kDegenerate ? 0.0 : span / 100.0;
}

// ------------------------------------------------------------------ slider --

Slider::Slider(ValueRange range, double value) : range_(range) {
  set_focusable(true);
  value_ = range_.quantise(value);
}

void Slider::set_value(double value) { value_ = range_.quantise(value); }

void Slider::set_range(const ValueRange& range) {
  range_ = range;
  value_ = range_.quantise(value_);
}

void Slider::commit(double value) {
  const double next = range_.quantise(value);
  if (next == value_) return;
  value_ = next;
  if (on_change_) on_change_(value_);
}

LayoutItem Slider::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) return LayoutItem::fixed(metrics.control_height);
  // Wide enough that the thumb has somewhere to travel, and happy to be wider.
  return LayoutItem::flexible(1.0, metrics.control_height * 3.0);
}

void Slider::layout(const LayoutContext& context) {
  // From the theme rather than a constant: a bevelled thumb needs more room
  // than a flat one, and no control should be deciding that for itself.
  thumb_size_ = context.metrics().control_height * 0.6;
}

Rect Slider::groove() const {
  const double height = std::max(4.0, bounds().height * 0.25);
  return Rect{bounds().x, bounds().y + (bounds().height - height) / 2.0, bounds().width,
              height};
}

Rect Slider::thumb() const {
  const Rect track = groove();
  const double size = std::min(thumb_size_, bounds().width);
  // The thumb travels the groove minus its own width. Mapping against the full
  // width instead is the same bug as a scrollbar that cannot reach the end.
  const double travel = std::max(0.0, track.width - size);
  return Rect{track.x + fraction() * travel, bounds().y, size, bounds().height};
}

double Slider::value_at(double x) const {
  const Rect track = groove();
  const double size = std::min(thumb_size_, bounds().width);
  const double travel = track.width - size;
  if (travel <= 0.0) return range_.minimum;
  // Measured from the thumb's centre, so the value under the pointer is the
  // one the thumb ends up showing.
  return range_.from_fraction((x - track.x - size / 2.0) / travel);
}

void Slider::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& groove_style = theme.style(Part::Slider, state());
  const Rect track = groove();
  paint_surface(painter, track, groove_style);

  // The filled part, so the value is readable without finding the thumb.
  const Rect knob = thumb();
  const double filled = knob.x + knob.width / 2.0 - track.x;
  if (filled > 0.0) {
    const SurfaceStyle& thumb_style = theme.style(Part::SliderThumb, state());
    painter.fill(Rect{track.x, track.y, filled, track.height}, groove_style.corner_radius,
                 thumb_style.fill);
  }

  paint_surface(painter, knob, theme.style(Part::SliderThumb, state()));
}

void Slider::finish() {
  if (value_ == gesture_start_) return;
  if (on_commit_) on_commit_(value_);
}

bool Slider::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  gesture_start_ = value_;

  // A double-click returns to the parameter's default. Getting back to it is
  // otherwise a matter of dragging very carefully.
  if (event.click_count >= 2 && default_.has_value()) {
    commit(*default_);
    finish();
    return true;
  }

  // Clicking anywhere on the groove jumps there rather than paging. For a
  // value this is what is wanted: the pointer is already where the answer is.
  dragging_ = true;
  commit(value_at(event.x));
  return true;
}

bool Slider::on_mouse_move(const MouseEvent& event) {
  if (!dragging_) return false;
  commit(value_at(event.x));
  return true;
}

bool Slider::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !dragging_) return false;
  dragging_ = false;
  finish();
  return true;
}

bool Slider::on_key_down(const KeyEvent& event) {
  if (event.modifiers.control || event.modifiers.alt) return false;
  const double amount = range_.nudge() * (event.modifiers.shift ? kCoarseNudge : 1.0);

  // Each press is a gesture of its own, so holding an arrow key records one
  // entry per step rather than one for the whole hold. That is what makes
  // undoing a nudge undo exactly that nudge.
  gesture_start_ = value_;

  switch (event.key) {
    case Key::Left:
    case Key::Down:
      commit(value_ - amount);
      break;
    case Key::Right:
    case Key::Up:
      commit(value_ + amount);
      break;
    case Key::Home:
      commit(range_.minimum);
      break;
    case Key::End:
      commit(range_.maximum);
      break;
    default:
      return false;
  }

  finish();
  return true;
}

// ---------------------------------------------------------------- checkbox --

Checkbox::Checkbox(std::string label, bool checked)
    : label_(std::move(label)), checked_(checked) {
  set_focusable(true);
}

void Checkbox::layout(const LayoutContext& context) {
  const Metrics& metrics = context.metrics();
  box_size_ = metrics.control_height * 0.6;
  gap_ = metrics.spacing;
  font_size_ = metrics.font_size;
}

Rect Checkbox::box() const {
  const double size = std::min(box_size_, std::min(bounds().width, bounds().height));
  return Rect{bounds().x, bounds().y + (bounds().height - size) / 2.0, size, size};
}

LayoutItem Checkbox::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) return LayoutItem::fixed(metrics.control_height);

  const double box = metrics.control_height * 0.6;
  if (label_.empty()) return LayoutItem::fixed(box);
  return LayoutItem::fixed(box + metrics.spacing +
                           context.text.measure(label_, metrics.font_size, false));
}

void Checkbox::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(Part::Input, state());
  const Rect square = box();
  paint_surface(painter, square, style);

  if (checked_) {
    // Drawn from lines rather than a glyph, for the same reason the caption
    // buttons are: there is no font that can be relied on to have a tick.
    const double inset_by = square.width * 0.25;
    const double left = square.x + inset_by;
    const double right = square.right() - inset_by;
    const double top = square.y + inset_by;
    const double bottom = square.bottom() - inset_by;
    const double elbow_x = left + (right - left) * 0.36;

    painter.line(left, top + (bottom - top) * 0.5, elbow_x, bottom, style.text, 2.0);
    painter.line(elbow_x, bottom, right, top, style.text, 2.0);
  }

  if (label_.empty()) return;
  const Rect text{square.right() + gap_, bounds().y,
                  std::max(0.0, bounds().right() - square.right() - gap_), bounds().height};
  painter.text(text_run(text, label_, style, font_size_, TextAlign::Left, false));
}

void Checkbox::toggle() {
  checked_ = !checked_;
  if (on_change_) on_change_(checked_);
}

bool Checkbox::on_mouse_down(const MouseEvent& event) {
  // Taken so the press captures; the toggle happens on release, so sliding off
  // cancels it the way it does on a button.
  return event.button == MouseButton::Left;
}

bool Checkbox::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  if (hit(event.x, event.y)) toggle();
  return true;
}

bool Checkbox::on_key_down(const KeyEvent& event) {
  if (event.key != Key::Space || !event.modifiers.none()) return false;
  toggle();
  return true;
}

}  // namespace cutline::ui
