#include "cutline/ui/painter.hpp"

#include <algorithm>
#include <utility>

namespace cutline::ui {

Rect Rect::inset(double amount) const noexcept {
  const double shrink = std::min(amount, std::min(width, height) / 2.0);
  return Rect{x + shrink, y + shrink, std::max(0.0, width - 2.0 * shrink),
              std::max(0.0, height - 2.0 * shrink)};
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
  painter.fill(bounds, radius, style.fill);

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
