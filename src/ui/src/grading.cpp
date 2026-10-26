#include "cutline/ui/grading.hpp"

#include "cutline/ui/color_picker.hpp"
#include "cutline/ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cutline::ui {
namespace {

/// How far from a handle a press still takes it.
constexpr double kReach = 7.0;

[[nodiscard]] Color fade(const Color& color, float amount) noexcept {
  return Color{color.r, color.g, color.b, color.a * amount};
}

void put(std::vector<std::uint8_t>& pixels, std::size_t at, const Color& color, float alpha) {
  pixels[at + 0] = static_cast<std::uint8_t>(std::lround(std::clamp(color.r, 0.0f, 1.0f) * 255.0f));
  pixels[at + 1] = static_cast<std::uint8_t>(std::lround(std::clamp(color.g, 0.0f, 1.0f) * 255.0f));
  pixels[at + 2] = static_cast<std::uint8_t>(std::lround(std::clamp(color.b, 0.0f, 1.0f) * 255.0f));
  pixels[at + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

/// A ring round a point: the puck and the curve's points.
void ring(Painter& painter, double x, double y, double radius, const Color& fill,
          const Color& edge) {
  const Rect box{x - radius, y - radius, radius * 2.0, radius * 2.0};
  painter.fill(box, radius, Fill::solid(fill));
  painter.stroke(box, radius, edge, 1.5);
}

}  // namespace

// ------------------------------------------------------------- ColorWheel --

ColorWheel::ColorWheel(double diameter) : diameter_(diameter) {}

void ColorWheel::set_shade(Shade shade) {
  shade_ = std::move(shade);
  pixels_size_ = 0;
  regenerate();
}

void ColorWheel::set_position(double x, double y) {
  const double length = std::hypot(x, y);
  const double scale = length > 1.0 ? 1.0 / length : 1.0;
  x_ = x * scale;
  y_ = y * scale;
}

LayoutItem ColorWheel::sizing(Axis axis, const LayoutContext& context) const {
  (void)axis;
  (void)context;
  return LayoutItem::fixed(diameter_ + 4.0);
}

void ColorWheel::layout(const LayoutContext& context) {
  (void)context;
  regenerate();
}

Rect ColorWheel::disc() const {
  const Rect whole = bounds();
  const double size = std::min(whole.width, whole.height) - 4.0;
  if (size <= 0.0) return {};
  return Rect{whole.x + (whole.width - size) * 0.5, whole.y + (whole.height - size) * 0.5, size,
              size};
}

void ColorWheel::regenerate() {
  const int size = static_cast<int>(std::lround(diameter_));
  if (size <= 0 || !shade_ || size == pixels_size_) return;
  pixels_size_ = size;
  pixels_.assign(static_cast<std::size_t>(size) * size * 4, 0);
  const double r = size * 0.5;
  for (int py = 0; py < size; ++py) {
    for (int px = 0; px < size; ++px) {
      const double x = (px + 0.5 - r) / r;
      const double y = (r - py - 0.5) / r;
      const double length = std::hypot(x, y);
      // A pixel's worth of soft edge, so the disc is round rather than stepped.
      const auto alpha = static_cast<float>(std::clamp((1.0 - length) * r + 0.5, 0.0, 1.0));
      if (alpha <= 0.0f) continue;
      put(pixels_, (static_cast<std::size_t>(py) * size + px) * 4, shade_(x, y), alpha);
    }
  }
}

void ColorWheel::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = disc();
  if (area.empty()) return;
  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);
  if (!pixels_.empty()) {
    painter.image(area, ImageView{pixels_.data(), pixels_size_, pixels_size_, pixels_size_ * 4});
  }
  const double r = area.width * 0.5;
  const double cx = area.x + r;
  const double cy = area.y + r;
  painter.stroke(area, r, fade(style.text, 0.35f), 1.0);
  // The cross through the middle, so the centre can be found by eye.
  painter.line(cx - r, cy, cx + r, cy, fade(style.text, 0.18f), 1.0);
  painter.line(cx, cy - r, cx, cy + r, fade(style.text, 0.18f), 1.0);
  ring(painter, cx + x_ * r, cy - y_ * r, 5.0, Color{1.0f, 1.0f, 1.0f, 0.25f},
       Color{1.0f, 1.0f, 1.0f, 0.95f});
}

void ColorWheel::move_to(double px, double py) {
  const Rect area = disc();
  if (area.empty()) return;
  const double r = area.width * 0.5;
  set_position((px - area.x - r) / r, (area.y + r - py) / r);
  if (on_change_) on_change_(x_, y_);
}

bool ColorWheel::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !disc().contains(event.x, event.y)) return false;
  // A double click puts the puck back in the middle, which is how every
  // grading application resets a wheel.
  if (event.click_count >= 2) {
    set_position(0.0, 0.0);
    if (on_change_) on_change_(x_, y_);
    if (on_commit_) on_commit_(x_, y_);
    return true;
  }
  dragging_ = true;
  move_to(event.x, event.y);
  return true;
}

bool ColorWheel::on_mouse_move(const MouseEvent& event) {
  if (!dragging_) return false;
  move_to(event.x, event.y);
  return true;
}

bool ColorWheel::on_mouse_up(const MouseEvent& event) {
  if (!dragging_ || event.button != MouseButton::Left) return false;
  dragging_ = false;
  if (on_commit_) on_commit_(x_, y_);
  return true;
}

// ------------------------------------------------------------- CurveGraph --

CurveGraph::CurveGraph(Backdrop backdrop, Color line, bool diagonal, bool periodic,
                       double height)
    : backdrop_(backdrop), line_(line), diagonal_(diagonal), periodic_(periodic),
      height_(height) {}

void CurveGraph::set_points(std::vector<CurvePointView> points) {
  points_ = std::move(points);
  std::ranges::sort(points_, {}, &CurvePointView::x);
}

void CurveGraph::set_samples(std::vector<float> samples) { samples_ = std::move(samples); }

LayoutItem CurveGraph::sizing(Axis axis, const LayoutContext& context) const {
  (void)context;
  if (axis == Axis::Horizontal) return LayoutItem::flexible(1.0, 120.0);
  return LayoutItem::fixed(height_);
}

void CurveGraph::layout(const LayoutContext& context) {
  (void)context;
  regenerate();
}

Rect CurveGraph::plot() const {
  const Rect whole = bounds();
  if (whole.width <= 12.0 || whole.height <= 12.0) return {};
  return Rect{whole.x + 6.0, whole.y + 6.0, whole.width - 12.0, whole.height - 12.0};
}

void CurveGraph::regenerate() {
  const Rect area = plot();
  const int width = static_cast<int>(std::lround(area.width));
  if (width <= 0 || width == strip_width_) return;
  strip_width_ = width;
  strip_.assign(static_cast<std::size_t>(width) * 4, 0);
  for (int px = 0; px < width; ++px) {
    const double at = (px + 0.5) / width;
    Color colour;
    switch (backdrop_) {
      case Backdrop::Hue:
        colour = from_hsv(Hsv{.h = at * 360.0, .s = 0.55, .v = 0.42});
        break;
      case Backdrop::Saturation:
        colour = from_hsv(Hsv{.h = 0.0, .s = at * 0.7, .v = 0.42});
        break;
      case Backdrop::Tone: {
        const auto grey = static_cast<float>(0.08 + at * 0.22);
        colour = Color{grey, grey, grey, 1.0f};
        break;
      }
    }
    put(strip_, static_cast<std::size_t>(px) * 4, colour, 1.0f);
  }
}

double CurveGraph::sample_at(double x) const {
  if (samples_.size() < 2) return diagonal_ ? x : 0.5;
  const double f = std::clamp(x, 0.0, 1.0) * static_cast<double>(samples_.size() - 1);
  const auto i0 = std::min(static_cast<std::size_t>(f), samples_.size() - 2);
  const double t = f - static_cast<double>(i0);
  return samples_[i0] * (1.0 - t) + samples_[i0 + 1] * t;
}

double CurveGraph::screen_x(double x) const {
  const Rect area = plot();
  return area.x + x * area.width;
}

double CurveGraph::screen_y(double y) const {
  const Rect area = plot();
  return area.y + (1.0 - y) * area.height;
}

void CurveGraph::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = plot();
  if (area.empty()) return;
  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);
  if (!strip_.empty()) {
    painter.image(area, ImageView{strip_.data(), strip_width_, 1, strip_width_ * 4});
  }
  // A grid in quarters, and the line the curve rests on.
  for (int i = 1; i < 4; ++i) {
    const double gx = area.x + area.width * i / 4.0;
    const double gy = area.y + area.height * i / 4.0;
    painter.line(gx, area.y, gx, area.y + area.height, fade(style.text, 0.10f), 1.0);
    painter.line(area.x, gy, area.x + area.width, gy, fade(style.text, 0.10f), 1.0);
  }
  if (diagonal_) {
    painter.line(area.x, area.y + area.height, area.x + area.width, area.y,
                 fade(style.text, 0.2f), 1.0);
  } else {
    painter.line(area.x, screen_y(0.5), area.x + area.width, screen_y(0.5),
                 fade(style.text, 0.2f), 1.0);
  }
  painter.stroke(area, 0.0, fade(style.text, 0.3f), 1.0);

  // The curve the grade applies, a segment every two pixels.
  const int steps = std::max(8, static_cast<int>(area.width / 2.0));
  double last_x = screen_x(0.0);
  double last_y = screen_y(sample_at(0.0));
  for (int i = 1; i <= steps; ++i) {
    const double x = static_cast<double>(i) / steps;
    const double sx = screen_x(x);
    const double sy = screen_y(sample_at(x));
    painter.line(last_x, last_y, sx, sy, line_, 1.5);
    last_x = sx;
    last_y = sy;
  }

  for (const CurvePointView& point : points_) {
    ring(painter, screen_x(point.x), screen_y(point.y), 4.0, fade(line_, 0.35f), line_);
  }
  // A straight curve's ends can be taken hold of too, so they are shown.
  if (diagonal_) {
    if (points_.empty() || points_.front().x > 0.0) {
      ring(painter, screen_x(0.0), screen_y(0.0), 3.0, Color{}, fade(line_, 0.6f));
    }
    if (points_.empty() || points_.back().x < 1.0) {
      ring(painter, screen_x(1.0), screen_y(1.0), 3.0, Color{}, fade(line_, 0.6f));
    }
  }
}

int CurveGraph::point_near(double px, double py) const {
  int best = -1;
  double nearest = kReach;
  for (std::size_t i = 0; i < points_.size(); ++i) {
    const double d =
        std::hypot(screen_x(points_[i].x) - px, screen_y(points_[i].y) - py);
    if (d <= nearest) {
      nearest = d;
      best = static_cast<int>(i);
    }
  }
  return best;
}

void CurveGraph::sort_keeping(int& dragged) {
  const CurvePointView held = points_[static_cast<std::size_t>(dragged)];
  std::ranges::stable_sort(points_, {}, &CurvePointView::x);
  const auto found = std::ranges::find(points_, held);
  dragged = static_cast<int>(found - points_.begin());
}

bool CurveGraph::on_mouse_down(const MouseEvent& event) {
  const Rect area = plot();
  if (event.button != MouseButton::Left || area.empty()) return false;
  const Rect reach{area.x - kReach, area.y - kReach, area.width + kReach * 2.0,
                   area.height + kReach * 2.0};
  if (!reach.contains(event.x, event.y)) return false;

  const double x = std::clamp((event.x - area.x) / area.width, 0.0, 1.0);
  const double y = std::clamp(1.0 - (event.y - area.y) / area.height, 0.0, 1.0);
  int near = point_near(event.x, event.y);

  // Control-click takes a point away, as in Premiere.
  if (near >= 0 && event.modifiers.control) {
    points_.erase(points_.begin() + near);
    if (on_change_) on_change_(points_);
    if (on_commit_) on_commit_(points_);
    return true;
  }
  if (near < 0 && event.click_count >= 2) {
    // A double click off the points straightens the curve.
    points_.clear();
    if (on_change_) on_change_(points_);
    if (on_commit_) on_commit_(points_);
    return true;
  }
  if (near < 0 && diagonal_) {
    // An end of a straight curve, taken hold of, becomes a point of its own.
    for (const double end : {0.0, 1.0}) {
      if (std::hypot(screen_x(end) - event.x, screen_y(end) - event.y) <= kReach) {
        points_.push_back({.x = end, .y = end});
        near = static_cast<int>(points_.size()) - 1;
        sort_keeping(near);
        break;
      }
    }
  }
  if (near < 0) {
    if (periodic_) {
      // A hue curve is bent in one place, so a press puts down three points:
      // the one to drag and one either side holding the rest of the curve
      // where it was.
      for (const double offset : {-0.08, 0.08}) {
        const double side = x + offset - std::floor(x + offset);
        points_.push_back({.x = side, .y = sample_at(side)});
      }
      points_.push_back({.x = x, .y = sample_at(x)});
    } else {
      points_.push_back({.x = x, .y = y});
    }
    near = static_cast<int>(points_.size()) - 1;
    sort_keeping(near);
  }
  dragging_ = near;
  if (on_change_) on_change_(points_);
  return true;
}

bool CurveGraph::on_mouse_move(const MouseEvent& event) {
  if (dragging_ < 0) return false;
  const Rect area = plot();
  if (area.empty()) return true;
  CurvePointView& point = points_[static_cast<std::size_t>(dragging_)];
  point.x = std::clamp((event.x - area.x) / area.width, 0.0, 1.0);
  point.y = std::clamp(1.0 - (event.y - area.y) / area.height, 0.0, 1.0);
  sort_keeping(dragging_);
  if (on_change_) on_change_(points_);
  return true;
}

bool CurveGraph::on_mouse_up(const MouseEvent& event) {
  if (dragging_ < 0 || event.button != MouseButton::Left) return false;
  dragging_ = -1;
  if (on_commit_) on_commit_(points_);
  return true;
}

// ------------------------------------------------------------ RangeSlider --

RangeSlider::RangeSlider(Shade shade, bool wraps) : shade_(std::move(shade)), wraps_(wraps) {}

void RangeSlider::set_range(double low, double high, double soft) {
  low_ = std::clamp(low, 0.0, 1.0);
  high_ = std::clamp(high, 0.0, 1.0);
  if (!wraps_ && high_ < low_) std::swap(low_, high_);
  soft_ = std::clamp(soft, 0.0, 0.5);
  strip_width_ = 0;
  regenerate();
}

LayoutItem RangeSlider::sizing(Axis axis, const LayoutContext& context) const {
  (void)context;
  if (axis == Axis::Horizontal) return LayoutItem::flexible(1.0, 120.0);
  return LayoutItem::fixed(30.0);
}

void RangeSlider::layout(const LayoutContext& context) {
  (void)context;
  regenerate();
}

Rect RangeSlider::track() const {
  const Rect whole = bounds();
  if (whole.width <= 16.0) return {};
  return Rect{whole.x + 8.0, whole.y + (whole.height - 10.0) * 0.5, whole.width - 16.0, 10.0};
}

double RangeSlider::at(double px) const {
  const Rect area = track();
  if (area.empty()) return 0.0;
  return std::clamp((px - area.x) / area.width, 0.0, 1.0);
}

double RangeSlider::screen(double where) const {
  const Rect area = track();
  return area.x + where * area.width;
}

bool RangeSlider::inside(double where) const {
  if (low_ <= high_) return where >= low_ && where <= high_;
  return where >= low_ || where <= high_;
}

void RangeSlider::regenerate() {
  const Rect area = track();
  const int width = static_cast<int>(std::lround(area.width));
  if (width <= 0 || !shade_) return;
  strip_width_ = width;
  strip_.assign(static_cast<std::size_t>(width) * 4, 0);
  // What lies outside the range is dimmed, and the softness between half so,
  // so the track itself shows what will be selected.
  const auto distance_out = [this](double where) {
    if (inside(where)) return 0.0;
    const auto gap = [this](double a, double b) {
      const double d = std::abs(a - b);
      return wraps_ ? std::min(d, 1.0 - d) : d;
    };
    return std::min(gap(where, low_), gap(where, high_));
  };
  for (int px = 0; px < width; ++px) {
    const double where = (px + 0.5) / width;
    const double out = distance_out(where);
    const double kept = out <= 0.0 ? 1.0 : soft_ > 0.0 ? std::max(0.0, 1.0 - out / soft_) : 0.0;
    const Color colour = shade_(where);
    const auto dim = static_cast<float>(0.25 + 0.75 * kept);
    put(strip_, static_cast<std::size_t>(px) * 4,
        Color{colour.r * dim, colour.g * dim, colour.b * dim, 1.0f}, 1.0f);
  }
}

void RangeSlider::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = track();
  if (area.empty()) return;
  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);
  if (!strip_.empty()) {
    painter.image(area, ImageView{strip_.data(), strip_width_, 1, strip_width_ * 4});
  }
  painter.stroke(area, 0.0, fade(style.text, 0.3f), 1.0);

  // The range above the track, the softness below it.
  const auto wrap = [this](double where) {
    return wraps_ ? where - std::floor(where) : std::clamp(where, 0.0, 1.0);
  };
  for (const double end : {low_, high_}) {
    const double x = screen(end);
    painter.fill(Rect{x - 3.0, area.y - 7.0, 6.0, 6.0}, 1.0, Fill::solid(style.text));
    painter.line(x, area.y - 1.0, x, area.y + area.height, style.text, 1.0);
  }
  for (const double end : {wrap(low_ - soft_), wrap(high_ + soft_)}) {
    const double x = screen(end);
    painter.stroke(Rect{x - 3.0, area.y + area.height + 1.0, 6.0, 6.0}, 1.0, style.text, 1.0);
  }
}

bool RangeSlider::on_mouse_down(const MouseEvent& event) {
  const Rect area = track();
  if (event.button != MouseButton::Left || area.empty() || !bounds().contains(event.x, event.y)) {
    return false;
  }
  if (event.click_count >= 2) {
    set_range(0.0, 1.0, 0.0);
    if (on_change_) on_change_(low_, high_, soft_);
    if (on_commit_) on_commit_(low_, high_, soft_);
    return true;
  }
  const auto wrap = [this](double where) {
    return wraps_ ? where - std::floor(where) : std::clamp(where, 0.0, 1.0);
  };
  const bool above = event.y < area.y + area.height * 0.5;
  const auto near = [&](double where) { return std::abs(screen(where) - event.x) <= kReach; };
  grip_ = Grip::None;
  if (above) {
    if (near(low_)) grip_ = Grip::Low;
    if (near(high_) && (grip_ == Grip::None ||
                        std::abs(screen(high_) - event.x) < std::abs(screen(low_) - event.x))) {
      grip_ = Grip::High;
    }
  } else {
    if (near(wrap(low_ - soft_))) grip_ = Grip::SoftLow;
    if (near(wrap(high_ + soft_))) grip_ = Grip::SoftHigh;
  }
  if (grip_ == Grip::None && inside(at(event.x))) grip_ = Grip::Whole;
  if (grip_ == Grip::None) return true;
  grabbed_at_ = at(event.x);
  grabbed_low_ = low_;
  grabbed_high_ = high_;
  return true;
}

bool RangeSlider::on_mouse_move(const MouseEvent& event) {
  if (grip_ == Grip::None) return false;
  const double where = at(event.x);
  const auto gap = [this](double a, double b) {
    const double d = a - b;
    return wraps_ ? d - std::floor(d) : d;
  };
  switch (grip_) {
    case Grip::Low:
      low_ = wraps_ ? where : std::min(where, high_);
      break;
    case Grip::High:
      high_ = wraps_ ? where : std::max(where, low_);
      break;
    case Grip::SoftLow:
      soft_ = std::clamp(wraps_ ? std::min(gap(low_, where), 0.5) : low_ - where, 0.0, 0.5);
      break;
    case Grip::SoftHigh:
      soft_ = std::clamp(wraps_ ? std::min(gap(where, high_), 0.5) : where - high_, 0.0, 0.5);
      break;
    case Grip::Whole: {
      double shift = where - grabbed_at_;
      if (wraps_) {
        low_ = grabbed_low_ + shift - std::floor(grabbed_low_ + shift);
        high_ = grabbed_high_ + shift - std::floor(grabbed_high_ + shift);
      } else {
        shift = std::clamp(shift, -grabbed_low_, 1.0 - grabbed_high_);
        low_ = grabbed_low_ + shift;
        high_ = grabbed_high_ + shift;
      }
      break;
    }
    case Grip::None:
      break;
  }
  regenerate();
  if (on_change_) on_change_(low_, high_, soft_);
  return true;
}

bool RangeSlider::on_mouse_up(const MouseEvent& event) {
  if (grip_ == Grip::None || event.button != MouseButton::Left) return false;
  grip_ = Grip::None;
  if (on_commit_) on_commit_(low_, high_, soft_);
  return true;
}

}  // namespace cutline::ui
