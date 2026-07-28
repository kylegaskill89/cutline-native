#pragma once

/// Turning a theme's description of a surface into drawing.
///
/// `Painter` is deliberately a small set of primitives rather than a
/// "draw a button" call. The decision of *what a button is made of* — a shadow
/// behind it, then a fill, then a bevel, then a border, in that order — belongs
/// to one place, `paint_surface`, so every backend gets the same answer and the
/// ordering can be tested without drawing anything at all.
///
/// That is why this is an interface. `RecordingPainter` captures the calls as
/// data, which makes "does an XP button draw its bevel inset when pressed" a
/// unit test rather than something to squint at. `SkiaPainter` does the same
/// calls for real. If a theme ever needs something the primitives cannot
/// express, that is the model being wrong, and it shows up here rather than
/// three layers later.

#include "cutline/ui/theme.hpp"

#include <string>
#include <string_view>

namespace cutline::ui {

/// A rectangle in layout coordinates: pixels, y down, origin top-left.
struct Rect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;

  [[nodiscard]] double right() const noexcept { return x + width; }
  [[nodiscard]] double bottom() const noexcept { return y + height; }
  [[nodiscard]] bool empty() const noexcept { return width <= 0.0 || height <= 0.0; }

  [[nodiscard]] bool contains(double px, double py) const noexcept {
    return px >= x && px < right() && py >= y && py < bottom();
  }

  /// Shrunk on every side, never past zero.
  [[nodiscard]] Rect inset(double amount) const noexcept;

  friend bool operator==(const Rect&, const Rect&) = default;
};

enum class TextAlign { Left, Center, Right };

struct TextRun {
  Rect bounds;
  std::string text;
  Color color;
  double size = 13.0;
  /// Radius of the halo behind the text. Zero means none. This is what keeps a
  /// label readable over glass, where the backdrop may be any colour.
  double glow = 0.0;
  TextAlign align = TextAlign::Left;
  bool bold = false;

  friend bool operator==(const TextRun&, const TextRun&) = default;
};

/// Measuring text, which layout needs and drawing owns.
///
/// Split out from `Painter` because a widget works out how wide it wants to be
/// long before there is a canvas to draw on, and only the backend that
/// rasterises the font can answer the question. Nothing else about painting is
/// needed to size a button to its label.
class TextMeasurer {
 public:
  virtual ~TextMeasurer() = default;

  /// Width of a string at a size, in layout pixels. Empty text is zero.
  [[nodiscard]] virtual double measure(std::string_view text, double size, bool bold) const = 0;
};

/// The drawing primitives every backend must provide.
class Painter : public TextMeasurer {
 public:

  /// Restricts drawing to a rounded rectangle until the matching `pop_clip`.
  virtual void push_clip(const Rect& bounds, double corner_radius) = 0;
  virtual void pop_clip() = 0;

  virtual void fill(const Rect& bounds, double corner_radius, const Fill& fill) = 0;

  /// Stroked on the inside of `bounds`, so a border never grows a control.
  virtual void stroke(const Rect& bounds, double corner_radius, const Color& color,
                      double width) = 0;

  /// Light and dark edges on opposite sides. Not expressible as a fill: which
  /// side gets which colour is what makes a control look pressed.
  virtual void bevel(const Rect& bounds, const Bevel& bevel) = 0;

  virtual void shadow(const Rect& bounds, double corner_radius, const Shadow& shadow) = 0;

  /// Blurs whatever has already been drawn beneath `bounds`. The one primitive
  /// that reads the surface rather than writing to it, and the reason Vista's
  /// glass is possible at all.
  virtual void backdrop_blur(const Rect& bounds, double corner_radius, double radius) = 0;

  virtual void text(const TextRun& run) = 0;
};

/// Draws a themed surface, in the order the pieces have to go down:
/// outer shadow, fill, inner shadow, bevel, border.
///
/// The order is not arbitrary. An inner shadow drawn before the fill would be
/// painted over; a border drawn before the bevel would be half-covered by it.
/// Getting this wrong is subtle enough to survive a glance, which is why it
/// lives in one function with tests on it.
void paint_surface(Painter& painter, const Rect& bounds, const SurfaceStyle& style);

/// The text colour and glow a style asks for, positioned in `bounds`.
/// A convenience so widgets do not reassemble it and drift apart.
[[nodiscard]] TextRun text_run(const Rect& bounds, std::string text, const SurfaceStyle& style,
                               double size, TextAlign align = TextAlign::Left, bool bold = false);

}  // namespace cutline::ui
