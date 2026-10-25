#include "cutline/ui/painter.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cutline::ui {

double ImageView::aspect() const noexcept {
  if (empty()) return 0.0;
  return static_cast<double>(width) / static_cast<double>(height);
}

double TextureView::aspect() const noexcept {
  if (empty()) return 0.0;
  return static_cast<double>(width) / static_cast<double>(height);
}

Rect Rect::inset(double amount) const noexcept {
  const double shrink = std::min(amount, std::min(width, height) / 2.0);
  return Rect{x + shrink, y + shrink, std::max(0.0, width - 2.0 * shrink),
              std::max(0.0, height - 2.0 * shrink)};
}

Rect Rect::intersect(const Rect& other) const noexcept {
  const double left = std::max(x, other.x);
  const double top = std::max(y, other.y);
  const double right = std::min(x + width, other.x + other.width);
  const double bottom = std::min(y + height, other.y + other.height);
  return Rect{left, top, std::max(0.0, right - left), std::max(0.0, bottom - top)};
}

namespace {

/// How strongly a drop's accent shows. Enough to see over any panel; little
/// enough that what is being aimed at still shows through, which is the thing
/// being looked at while aiming.
constexpr float kDropWash = 0.28F;
constexpr float kGhostFill = 0.45F;
constexpr double kDropLine = 2.0;

}  // namespace

void paint_drop_target(Painter& painter, const Theme& theme, const Rect& bounds,
                       double corner_radius) {
  if (bounds.empty()) return;
  Color wash = theme.accent;
  wash.a *= kDropWash;
  painter.fill(bounds, corner_radius, Fill::solid(wash));
  painter.stroke(bounds, corner_radius, theme.accent, kDropLine);
}

void paint_drop_ghost(Painter& painter, const Theme& theme, const Rect& bounds,
                      double corner_radius, const Color& content) {
  if (bounds.empty()) return;
  Color faded = content;
  faded.a *= kGhostFill;
  painter.fill(bounds, corner_radius, Fill::solid(faded));
  painter.stroke(bounds, corner_radius, theme.accent, kDropLine);
}

void paint_surface(Painter& painter, const Rect& bounds, const SurfaceStyle& style) {
  if (bounds.empty()) return;

  const double radius = style.corner_radius;

  // Behind everything, and outside the shape, so it must not be clipped to it.
  if (style.shadow && !style.shadow->inner) {
    painter.shadow(bounds, radius, *style.shadow);
  }

  // Glass reads the surface beneath before anything is drawn over it, so the
  // blur has to happen before the tint rather than as part of it.
  if (style.fill.kind == FillKind::Glass && style.fill.blur_radius > 0.0) {
    painter.backdrop_blur(bounds, radius, style.fill.blur_radius);
  }
  if (style.rim && style.rim->width > 0.0) {
    // The band, then the body inside it: two fills rather than a gradient
    // stroke, which the painter has no such thing as and which would have to
    // follow the rounded corners besides.
    painter.fill(bounds, radius, style.rim->fill);
    const double in = style.border_width + style.rim->width;
    painter.fill(bounds.inset(in), std::max(0.0, radius - in), style.fill);
  } else {
    painter.fill(bounds, radius, style.fill);
  }

  // Inside the shape, and over the fill: an inner shadow drawn before the fill
  // would simply be painted over.
  if (style.shadow && style.shadow->inner) {
    painter.push_clip(bounds, radius);
    painter.shadow(bounds, radius, *style.shadow);
    painter.pop_clip();
  }

  if (style.bevel) painter.bevel(bounds, *style.bevel);

  // Last, so it is never half-covered by the bevel it sits against.
  if (style.border_width > 0.0 && style.border.a > 0.0) {
    painter.stroke(bounds, radius, style.border, style.border_width);
  }

  // Over the border, which it replaces along the top, and clipped to the
  // shape so it follows the rounded corners down.
  if (style.cap && style.cap->width > 0.0) {
    painter.push_clip(bounds, radius);
    painter.fill(Rect{bounds.x, bounds.y, bounds.width, std::min(bounds.height, style.cap->width)},
                 0.0, style.cap->fill);
    painter.pop_clip();
  }
}

namespace {

/// `style` as it looks lying down rather than standing up.
[[nodiscard]] SurfaceStyle lying_down(SurfaceStyle style, const Rect& bounds) {
  if (bounds.width > bounds.height && style.fill.kind == FillKind::Gradient) {
    style.fill.angle_deg -= 90.0;
  }
  return style;
}

}  // namespace

void paint_scroll_track(Painter& painter, const Rect& bounds, const Theme& theme) {
  paint_surface(painter, bounds, lying_down(theme.style(Part::Scrollbar, State::Normal), bounds));
}

void paint_scroll_thumb(Painter& painter, const Rect& bounds, const Theme& theme, State state) {
  const SurfaceStyle style = lying_down(theme.style(Part::ScrollThumb, state), bounds);
  paint_surface(painter, bounds, style);

  // XP's grip: four lines in the thumb's ink, each with a line of its edge's
  // colour beside it, six pixels long and a pixel apart, across the middle.
  const double length = theme.metrics.scroll_grip;
  if (length <= 0.0) return;
  const bool across = bounds.width > bounds.height;
  const double along = across ? bounds.width : bounds.height;
  const double breadth = across ? bounds.height : bounds.width;
  constexpr double kLines = 4.0;
  if (along < kLines * 2.0 + 8.0 || breadth < length + 4.0) return;

  const double cx = bounds.x + bounds.width / 2.0;
  const double cy = bounds.y + bounds.height / 2.0;
  for (int i = 0; i < static_cast<int>(kLines); ++i) {
    const double offset = -kLines + 2.0 * i;
    if (across) {
      const double x = std::floor(cx + offset);
      const double y = std::floor(cy - length / 2.0);
      painter.fill(Rect{x, y, 1.0, length}, 0.0, Fill::solid(style.border));
      painter.fill(Rect{x + 1.0, y + 1.0, 1.0, length}, 0.0, Fill::solid(style.text));
    } else {
      const double x = std::floor(cx - length / 2.0);
      const double y = std::floor(cy + offset);
      painter.fill(Rect{x, y, length, 1.0}, 0.0, Fill::solid(style.border));
      painter.fill(Rect{x + 1.0, y + 1.0, length, 1.0}, 0.0, Fill::solid(style.text));
    }
  }
}

TextRun text_run(const Rect& bounds, std::string text, const SurfaceStyle& style, double size,
                 TextAlign align, bool bold) {
  return TextRun{
      .bounds = bounds,
      .text = std::move(text),
      .color = style.text,
      .size = size,
      .glow = style.text_glow,
      .align = align,
      .bold = bold,
  };
}

}  // namespace cutline::ui
