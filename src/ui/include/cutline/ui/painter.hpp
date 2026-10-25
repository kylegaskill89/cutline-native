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

#include <cstdint>
#include <optional>
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
  /// What this and `other` have in common, or an empty rectangle.
  [[nodiscard]] Rect intersect(const Rect& other) const noexcept;

  friend bool operator==(const Rect&, const Rect&) = default;
};

/// A borrowed view of 8-bit RGBA pixels, top row first.
///
/// Borrowed on purpose: a decoded frame is megabytes, and the interface has no
/// business copying one to draw it. Whoever hands this over keeps the pixels
/// alive until the draw returns, which is the whole of the contract.
struct ImageView {
  const std::uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  /// Bytes per row. Zero means tightly packed, which is `width * 4`.
  int stride = 0;

  [[nodiscard]] bool empty() const noexcept {
    return pixels == nullptr || width <= 0 || height <= 0;
  }
  [[nodiscard]] int row_bytes() const noexcept { return stride > 0 ? stride : width * 4; }
  /// Width over height, or zero when there is no image.
  [[nodiscard]] double aspect() const noexcept;

  friend bool operator==(const ImageView&, const ImageView&) = default;
};

/// A borrowed frame that is already on the GPU.
///
/// The same contract as `ImageView` — borrowed, alive until the draw returns —
/// for the case where the pixels never came to the CPU in the first place. A
/// composited frame is rendered on the graphics card and shown on the same
/// card, and the round trip through system memory in between bought nothing
/// except a way for the interface to be written without a GPU.
///
/// Untyped, and deliberately so. This library builds and is tested with no
/// Direct3D anywhere near it; naming `ID3D12Resource` here would end that. The
/// one painter that can do anything with this knows what the numbers mean, and
/// every other painter can see that it is not empty and say so.
struct TextureView {
  void* texture = nullptr;  ///< ID3D12Resource*
  int width = 0;
  int height = 0;
  unsigned format = 0;  ///< DXGI_FORMAT
  unsigned state = 0;   ///< D3D12_RESOURCE_STATES the texture is in

  /// Bumped by whoever owns the texture whenever it is rebuilt. A painter may
  /// keep something derived from a texture between frames, and the handle alone
  /// cannot say whether it is still the same one — a freed resource's address
  /// gets handed straight back out.
  unsigned generation = 0;

  /// The same picture for an HDR display, when there is one: scRGB, at its
  /// real brightness, in `state` like the other. A window showing HDR draws
  /// this one through the hole the painter leaves for it; anything else
  /// ignores it.
  void* hdr_texture = nullptr;  ///< ID3D12Resource*
  unsigned hdr_format = 0;      ///< DXGI_FORMAT

  [[nodiscard]] bool empty() const noexcept {
    return texture == nullptr || width <= 0 || height <= 0;
  }
  /// Width over height, or zero when there is no texture.
  [[nodiscard]] double aspect() const noexcept;

  friend bool operator==(const TextureView&, const TextureView&) = default;
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
  /// A typeface for this run alone, by family name; empty for the one the
  /// painter was given. XP set its captions in Trebuchet and the rest in
  /// Tahoma.
  std::string family;
  /// A copy of the text drawn behind it, offset and in its own colour. XP's
  /// captions had a hard navy one a pixel down and to the right.
  std::optional<Shadow> shadow;

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

  /// The typeface what follows is measured and drawn in, by family name, or
  /// empty for the system's. Set by the host from the theme before it lays
  /// out and before it paints, so the two always agree. Const for the reason
  /// `measure` is: a measurer is lent as const, and which face it measures in
  /// is not part of what it is.
  virtual void set_typeface(std::string_view family) const { (void)family; }

  /// How what follows is smoothed, set beside the typeface for the same
  /// reason: smoothing changes where glyphs land, and so how wide text is.
  virtual void set_text_smoothing(TextSmoothing smoothing) const { (void)smoothing; }
};

/// The drawing primitives every backend must provide.
class Painter : public TextMeasurer {
 public:
  /// Device pixels per unit of what is being drawn: the display's scale. For
  /// marks drawn from lines, which are only crisp when their edges land on
  /// pixel edges and so have to know where those are.
  [[nodiscard]] virtual double device_scale() const noexcept { return 1.0; }

  /// Restricts drawing to a rounded rectangle until the matching `pop_clip`.
  virtual void push_clip(const Rect& bounds, double corner_radius) = 0;
  virtual void pop_clip() = 0;

  /// Draws what follows at `opacity`, as one picture, until `pop_opacity`:
  /// the whole of a popup fading in, rather than each of its pieces at once
  /// with the ones underneath showing through the ones on top.
  virtual void push_opacity(float opacity) { (void)opacity; }
  virtual void pop_opacity() {}

  virtual void fill(const Rect& bounds, double corner_radius, const Fill& fill) = 0;

  /// Stroked on the inside of `bounds`, so a border never grows a control.
  virtual void stroke(const Rect& bounds, double corner_radius, const Color& color,
                      double width) = 0;

  /// Light and dark edges on opposite sides. Not expressible as a fill: which
  /// side gets which colour is what makes a control look pressed.
  virtual void bevel(const Rect& bounds, const Bevel& bevel) = 0;

  /// A straight line between two points.
  ///
  /// The one shape here that is not a rectangle, and it earns its place: a
  /// close button's cross cannot be drawn without it, and neither can a
  /// waveform, an audio meter, or a keyframe curve.
  virtual void line(double x1, double y1, double x2, double y2, const Color& color,
                    double width) = 0;

  virtual void shadow(const Rect& bounds, double corner_radius, const Shadow& shadow) = 0;

  /// Draws pixels stretched to fill `bounds`. Letterboxing is the caller's
  /// business — `fit_aspect` works out the rectangle, this fills whatever it is
  /// given, so a deliberately squeezed frame stays possible.
  virtual void image(const Rect& bounds, const ImageView& pixels) = 0;

  /// The same, for a frame that is already on the graphics card.
  ///
  /// Separate from `image` rather than an overload taking either, because the
  /// two are not interchangeable: a painter drawing into a raster surface
  /// cannot do this at all, and quietly drawing nothing would look like a
  /// black preview rather than like a missing capability. Whoever calls this
  /// is expected to know which kind of painter it has.
  virtual void texture(const Rect& bounds, const TextureView& frame) = 0;

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

/// A scrollbar's track and its thumb, wherever one is drawn.
///
/// A theme styles both as they look standing up; lying down, the light runs
/// the other way, so a gradient is turned a quarter for a bar wider than it is
/// tall. The thumb carries a grip across its middle where the theme asks for
/// one and the thumb is long enough to hold it.
void paint_scroll_track(Painter& painter, const Rect& bounds, const Theme& theme);
void paint_scroll_thumb(Painter& painter, const Rect& bounds, const Theme& theme, State state);

/// What a drag would land on: an existing thing -- a panel to dock beside, a
/// clip to put an effect on, a bin or a folder to file into. The accent washed
/// over it and drawn round it.
///
/// One look for every drag in the application. Each used to draw its own, and
/// the four of them disagreed about whether there was a wash, how thick the
/// line was and whether the corners were round -- so a drop meant something
/// slightly different to the eye in each panel.
void paint_drop_target(Painter& painter, const Theme& theme, const Rect& bounds,
                       double corner_radius);

/// Where something a drag would make would go -- a clip from the browser, a
/// transition on a cut: what it will look like, faded, with the same accent
/// outline a drop target has. `content` is the fill of the thing it will be.
void paint_drop_ghost(Painter& painter, const Theme& theme, const Rect& bounds,
                      double corner_radius, const Color& content);

/// The text colour and glow a style asks for, positioned in `bounds`.
/// A convenience so widgets do not reassemble it and drift apart.
[[nodiscard]] TextRun text_run(const Rect& bounds, std::string text, const SurfaceStyle& style,
                               double size, TextAlign align = TextAlign::Left, bool bold = false);

}  // namespace cutline::ui
