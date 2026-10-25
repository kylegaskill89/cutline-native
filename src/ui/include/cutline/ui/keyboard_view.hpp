#pragma once

/// A keyboard, drawn, with what each key does under the modifiers shown.
///
/// Premiere's Keyboard Shortcuts window is built round one: a key with a
/// command on it is coloured and carries the command's name, so somebody can
/// see which keys are free before choosing one, and pressing Ctrl, Shift or
/// Alt on the drawing shows that layer instead. Keys this application cannot
/// bind -- Caps Lock, the Windows keys -- are drawn so the keyboard looks like
/// one, and do nothing.

#include "cutline/ui/event.hpp"
#include "cutline/ui/widget.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cutline::ui {

class KeyboardView : public Widget {
 public:
  /// What is on a key under the modifiers shown.
  struct Bound {
    std::string label;
    /// Offered only once the focused panel has declined the key, as against
    /// one that works everywhere. Drawn in a colour of its own, as Premiere
    /// draws its panel commands.
    bool panel = false;
  };

  /// Which modifier a key on the drawing is.
  enum class Modifier { None, Control, Shift, Alt };

  /// One key on the drawing, in key widths from its top left.
  struct Cap {
    Key key = Key::None;
    std::string legend;
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    Modifier modifier = Modifier::None;
  };

  KeyboardView();

  /// What each key does under the modifiers shown, replacing what was there.
  void set_bound(std::map<Key, Bound> bound) { bound_ = std::move(bound); }
  [[nodiscard]] const std::map<Key, Bound>& bound() const noexcept { return bound_; }

  /// The modifiers whose layer is shown, as drawn held down.
  void set_modifiers(Modifiers modifiers) noexcept { modifiers_ = modifiers; }
  [[nodiscard]] const Modifiers& modifiers() const noexcept { return modifiers_; }

  /// The key last chosen, drawn as chosen.
  void set_selected(Key key) noexcept { selected_ = key; }
  [[nodiscard]] Key selected() const noexcept { return selected_; }

  /// A key was clicked.
  void set_on_choose(std::function<void(Key)> on_choose) { on_choose_ = std::move(on_choose); }
  /// A modifier on the drawing was clicked, and the layer shown is now this.
  void set_on_modifiers(std::function<void(Modifiers)> on_modifiers) {
    on_modifiers_ = std::move(on_modifiers);
  }

  /// Every key on the drawing.
  [[nodiscard]] const std::vector<Cap>& caps() const noexcept { return caps_; }
  /// Where a key is drawn, in this widget's coordinates.
  [[nodiscard]] Rect cap_rect(const Cap& cap) const;
  /// The key drawn at a point, or null.
  [[nodiscard]] const Cap* cap_at(double x, double y) const;

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;
  [[nodiscard]] std::string tooltip_at(double x, double y) const override;

  bool on_mouse_down(const MouseEvent& event) override;

 private:
  /// The size of one key width, and where the drawing starts, to fit the
  /// bounds while keeping the keys square.
  [[nodiscard]] double unit() const;

  std::vector<Cap> caps_;
  std::map<Key, Bound> bound_;
  Modifiers modifiers_;
  Key selected_ = Key::None;
  std::function<void(Key)> on_choose_;
  std::function<void(Modifiers)> on_modifiers_;
};

/// The drawing is this many key widths across and this many deep.
inline constexpr double kKeyboardWidth = 18.5;
inline constexpr double kKeyboardDepth = 6.5;

}  // namespace cutline::ui
