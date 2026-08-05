#include "cutline/ui/scrub_bar.hpp"

#include "cutline/ui/theme.hpp"

#include <algorithm>
#include <utility>

namespace cutline::ui {
namespace {

/// How tall the trough is. Thin: it is a strip under a picture, and the picture
/// is what the panel is for.
constexpr double kTroughHeight = 10.0;

/// The playhead's width, and how far either side of it counts as grabbing it.
/// Two pixels reads as a line rather than a bar; the reach is what makes it
/// possible to catch with a mouse.
constexpr double kPlayheadWidth = 2.0;

[[nodiscard]] Color fade(const Color& color, float amount) noexcept {
  return Color{color.r, color.g, color.b, color.a * amount};
}

}  // namespace

ScrubBar::ScrubBar() { set_clips_children(true); }

void ScrubBar::set_duration(double seconds) noexcept {
  duration_ = std::max(0.0, seconds);
  // The playhead comes with it. Opening a shorter source with the playhead
  // near the end of a longer one would otherwise leave it off the right edge.
  playhead_ = std::clamp(playhead_, 0.0, duration_);
}

void ScrubBar::set_playhead(double seconds) noexcept {
  playhead_ = std::clamp(seconds, 0.0, duration_);
}

void ScrubBar::set_marks(std::optional<double> in, std::optional<double> out) noexcept {
  in_ = in;
  out_ = out;
}

LayoutItem ScrubBar::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Horizontal) {
    // Takes whatever width there is, since the width *is* the scale.
    return LayoutItem::flexible(1.0, metrics.control_height);
  }
  return LayoutItem::fixed(kTroughHeight + metrics.padding_y * 2.0);
}

void ScrubBar::layout(const LayoutContext& context) { metrics_ = context.metrics(); }

Rect ScrubBar::track_area() const {
  const Rect whole = bounds();
  const double width = whole.width - metrics_.padding_x * 2.0;
  if (width <= 0.0 || whole.height <= 0.0) return {};

  // Vertically centred rather than filling, so the strip keeps its shape when
  // the row it is in is given more height than it asked for.
  const double height = std::min(kTroughHeight, whole.height);
  const double y = whole.y + (whole.height - height) * 0.5;
  return Rect{whole.x + metrics_.padding_x, y, width, height};
}

double ScrubBar::x_of(double seconds) const {
  const Rect area = track_area();
  if (area.empty()) return bounds().x;
  if (duration_ <= 0.0) return area.x;
  const double fraction = std::clamp(seconds / duration_, 0.0, 1.0);
  return area.x + area.width * fraction;
}

double ScrubBar::time_at(double x) const {
  const Rect area = track_area();
  if (area.empty() || area.width <= 0.0 || duration_ <= 0.0) return 0.0;
  const double fraction = std::clamp((x - area.x) / area.width, 0.0, 1.0);
  return fraction * duration_;
}

Rect ScrubBar::marked_area() const {
  if (!in_.has_value() && !out_.has_value()) return {};
  if (duration_ <= 0.0) return {};

  const double from = std::clamp(in_.value_or(0.0), 0.0, duration_);
  const double to = std::clamp(out_.value_or(duration_), 0.0, duration_);
  if (to <= from) return {};

  const Rect area = track_area();
  if (area.empty()) return {};
  const double left = x_of(from);
  return Rect{left, area.y, x_of(to) - left, area.height};
}

Rect ScrubBar::playhead_area() const {
  const Rect area = track_area();
  if (area.empty()) return {};
  // Standing a little proud of the trough at both ends, which is what makes it
  // read as a position on the strip rather than as part of the fill.
  const double overhang = 2.0;
  return Rect{x_of(playhead_) - kPlayheadWidth * 0.5, area.y - overhang, kPlayheadWidth,
              area.height + overhang * 2.0};
}

void ScrubBar::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = track_area();
  if (area.empty()) return;

  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);
  const double radius = area.height * 0.5;

  painter.fill(area, radius, Fill::solid(fade(style.text, 0.12f)));

  // The marked span in the playhead's colour, which is the timeline's rule
  // too: this and the playhead are the two things on the strip that say where
  // something is, and a second accent would be one too many.
  const SurfaceStyle& mark = theme.style(Part::Playhead, State::Normal);
  if (const Rect marked = marked_area(); !marked.empty()) {
    painter.fill(marked, radius, Fill::solid(fade(mark.fill.color, 0.45f)));
  }

  painter.stroke(area, radius, fade(style.text, 0.25f), 1.0);

  if (duration_ > 0.0) {
    painter.fill(playhead_area(), 0.0, mark.fill);
  }
}

void ScrubBar::scrub_to(double x) {
  if (duration_ <= 0.0) return;
  if (on_scrub) on_scrub(time_at(x));
}

bool ScrubBar::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  if (duration_ <= 0.0) return false;

  // Anywhere on the strip, not only on the playhead. Premiere's scrub bars jump
  // to the click and carry on dragging from there, which is one gesture for
  // "go here" and "go along from here" instead of two.
  scrubbing_ = true;
  scrub_to(event.x);
  return true;
}

bool ScrubBar::on_mouse_move(const MouseEvent& event) {
  if (!scrubbing_) return false;
  scrub_to(event.x);
  return true;
}

bool ScrubBar::on_mouse_up(const MouseEvent& event) {
  if (!scrubbing_ || event.button != MouseButton::Left) return false;
  scrubbing_ = false;
  return true;
}

}  // namespace cutline::ui
