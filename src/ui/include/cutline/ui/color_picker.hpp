#pragma once

/// Choosing a colour.
///
/// Kept apart from `controls.hpp` because it is the one control whose contents
/// no theme can restyle. A slider's groove is chrome and belongs to the theme;
/// the inside of a colour picker is the colour itself, and an XP picker that
/// drew red differently would be wrong rather than characterful. Only the frame
/// around it is themed.
///
/// The model is **HSV, held rather than derived**. That distinction is the
/// whole reason this is not a thin wrapper over `Color`: hue and saturation are
/// not recoverable from every colour. Black is every hue at once, and so is
/// white; every grey has no saturation to read back. A picker that converted
/// its colour to HSV on each frame would swing the square back to red the
/// moment a value was dragged to zero, and lose the hue somebody had just
/// spent a drag choosing. So the coordinates are the state, and the colour is
/// what they produce.

#include "cutline/ui/controls.hpp"
#include "cutline/ui/layout.hpp"
#include "cutline/ui/widget.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cutline::ui {

/// Hue in degrees (0 to 360, red at both ends), saturation and value 0 to 1.
struct Hsv {
  double h = 0.0;
  double s = 0.0;
  double v = 0.0;

  friend bool operator==(const Hsv&, const Hsv&) = default;
};

/// The canonical conversion: a grey reads as hue 0, saturation 0.
///
/// Lossy on purpose, and the loss is why `ColorPicker` keeps its own. Alpha is
/// not part of HSV and is dropped.
[[nodiscard]] Hsv to_hsv(const Color& color) noexcept;

/// The other way. Hue wraps, saturation and value are clamped.
[[nodiscard]] Color from_hsv(const Hsv& hsv, float alpha = 1.0f) noexcept;

/// A saturation/value square, a hue strip, an optional alpha strip, and a hex
/// field.
///
/// Built for the popup layer, like `MenuList`: a picker opening from a control
/// near the bottom of the inspector has to draw over its neighbours, and one
/// nested in the panel would be clipped away.
///
/// The square is drawn as an image rather than as a fill, because it is a
/// two-dimensional gradient and `Fill` only has one axis. It is regenerated
/// when the hue or the size changes, which during a hue drag is once a frame at
/// a few thousand pixels — cheap, and the alternative is a primitive that
/// exists for one control.
class ColorPicker : public Widget {
 public:
  explicit ColorPicker(Color color = Color{1.0f, 1.0f, 1.0f, 1.0f});

  /// What the coordinates currently mean.
  [[nodiscard]] Color color() const noexcept;
  /// Sets it without calling back.
  ///
  /// Keeps the hue when the colour has no saturation to read one from, and the
  /// saturation when it has no value — see the note at the top. Setting the
  /// same colour twice therefore leaves the square where it was rather than
  /// dragging it to a corner.
  void set_color(const Color& color);

  [[nodiscard]] const Hsv& hsv() const noexcept { return hsv_; }
  void set_hsv(const Hsv& hsv);

  [[nodiscard]] float alpha() const noexcept { return alpha_; }
  void set_alpha(float alpha) noexcept;

  /// Whether the alpha strip is shown. Off for a colour whose alpha means
  /// nothing — a chroma keyer's key colour is matched on hue, and offering a
  /// transparency that is silently discarded is worse than not offering one.
  [[nodiscard]] bool alpha_enabled() const noexcept { return alpha_enabled_; }
  void set_alpha_enabled(bool enabled);

  /// Every change, including each pixel of a drag. For a live preview.
  void set_on_change(std::function<void(const Color&)> on_change) {
    on_change_ = std::move(on_change);
  }
  /// Once, when a gesture finishes. For recording an edit — the same split as
  /// `Slider`, and for the same reason: a drag is one undo entry, not ninety.
  void set_on_commit(std::function<void(const Color&)> on_commit) {
    on_commit_ = std::move(on_commit);
  }

  /// The saturation/value square: saturation left to right, value bottom to
  /// top, which is the arrangement every other picker uses.
  [[nodiscard]] Rect field() const;
  [[nodiscard]] Rect hue_strip() const;
  /// Empty when alpha is not enabled.
  [[nodiscard]] Rect alpha_strip() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Menu; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  /// Which region a drag is working in. A drag belongs to the region it started
  /// in for its whole life, so sliding off the square onto the hue strip keeps
  /// setting saturation instead of jumping the hue.
  enum class Region { None, Field, Hue, Alpha };

  [[nodiscard]] Region region_at(double x, double y) const;
  /// Applies a pointer position to a region.
  void drag_to(Region region, double x, double y);
  void changed();
  void commit();
  /// Rebuilds the square's pixels for the current hue and size.
  void refresh_field();
  /// Puts the current colour in the hex field, unless it is being typed in.
  void refresh_hex();

  Hsv hsv_;
  float alpha_ = 1.0f;
  bool alpha_enabled_ = true;

  Region dragging_ = Region::None;
  /// What the colour was when the gesture began, so a drag that ends where it
  /// started reports nothing.
  Color gesture_start_;

  /// Owned by the tree, kept for updating its text as the square is dragged.
  TextField* hex_ = nullptr;

  /// The square, as premultiplied-free straight RGBA — every pixel is opaque,
  /// so the distinction does not arise. Rebuilt by `refresh_field`.
  std::vector<std::uint8_t> field_pixels_;
  int field_w_ = 0;
  int field_h_ = 0;
  /// The hue the pixels were generated for, so a repaint that changes nothing
  /// does not regenerate them. Negative means "never generated".
  double field_hue_ = -1.0;

  double padding_ = 8.0;
  double gap_ = 6.0;
  double hex_height_ = 24.0;

  std::function<void(const Color&)> on_change_;
  std::function<void(const Color&)> on_commit_;
};

/// The control that stands in a row: a block of the colour, its hex, and a
/// picker on click.
///
/// A button rather than a field, because typing a hex is the fallback and
/// pointing at a colour is the point. The hex is shown anyway — it is the only
/// form of a colour that can be written down, said aloud, or pasted from
/// somewhere else.
class ColorSwatch : public Widget {
 public:
  explicit ColorSwatch(Color color = Color{1.0f, 1.0f, 1.0f, 1.0f});
  ~ColorSwatch() override;

  [[nodiscard]] const Color& color() const noexcept { return color_; }
  /// Sets it without calling back.
  void set_color(const Color& color) noexcept { color_ = color; }

  [[nodiscard]] bool alpha_enabled() const noexcept { return alpha_enabled_; }
  void set_alpha_enabled(bool enabled) noexcept { alpha_enabled_ = enabled; }

  void set_on_change(std::function<void(const Color&)> on_change) {
    on_change_ = std::move(on_change);
  }
  void set_on_commit(std::function<void(const Color&)> on_commit) {
    on_commit_ = std::move(on_commit);
  }

  /// Opens the picker. Does nothing without a host to open it on, which is the
  /// case in a layout test.
  void open();
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  /// The block of colour, on the leading edge.
  [[nodiscard]] Rect block() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Input; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  Color color_;
  bool alpha_enabled_ = true;
  bool open_ = false;

  std::function<void(const Color&)> on_change_;
  std::function<void(const Color&)> on_commit_;
};

/// Draws the grey chequerboard that says "this is partly transparent".
///
/// Shared because two controls need the same one and a picker whose square of
/// chequers did not line up with the swatch's would look like a bug.
void paint_checkerboard(Painter& painter, const Rect& bounds, double corner_radius,
                        double square = 5.0);

}  // namespace cutline::ui
