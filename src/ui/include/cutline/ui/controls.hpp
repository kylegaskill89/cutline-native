#pragma once

/// The controls that edit a value.
///
/// Kept apart from `widgets.hpp`, which is about structure — boxes, panels,
/// splitters. These are the things an inspector is made of, and they all share
/// one problem: turning a pointer position into a number and back without the
/// two disagreeing.
///
/// That shared part is `ValueRange`, and it is a plain struct rather than a
/// base class so it can be tested on its own. Quantisation in particular is
/// easy to get subtly wrong in a way nobody notices until a slider that should
/// stop at 5 stops at 4.8.

#include "cutline/ui/layout.hpp"
#include "cutline/ui/widget.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace cutline::ui {

/// A bounded, optionally quantised number.
struct ValueRange {
  double minimum = 0.0;
  double maximum = 1.0;
  /// The increment values are held to. Zero is continuous.
  double step = 0.0;

  /// Into range. Works with the bounds either way round, so a slider that runs
  /// from 100 down to 0 behaves like any other.
  [[nodiscard]] double clamp(double value) const noexcept;

  /// Snapped to the nearest step, counted **from the minimum**.
  ///
  /// That is the part worth stating. Quantising from zero puts the steps in the
  /// wrong places whenever the minimum is not itself a multiple of the step —
  /// a control from 5 to 100 in tens would offer 10, 20, 30 and never 5.
  [[nodiscard]] double quantise(double value) const noexcept;

  /// Where a value sits along the range, from 0 to 1.
  [[nodiscard]] double to_fraction(double value) const noexcept;
  /// And back, quantised.
  [[nodiscard]] double from_fraction(double fraction) const noexcept;

  /// How much one arrow-key press should move by: the step, or a hundredth of
  /// the range where there is none.
  [[nodiscard]] double nudge() const noexcept;

  friend bool operator==(const ValueRange&, const ValueRange&) = default;
};

/// A value dragged along a groove.
class Slider : public Widget {
 public:
  explicit Slider(ValueRange range = {}, double value = 0.0);

  [[nodiscard]] double value() const noexcept { return value_; }
  /// Clamped and quantised. Does not call the change handler — that is for
  /// things the user did, so setting a value from code cannot loop back.
  void set_value(double value);

  [[nodiscard]] const ValueRange& range() const noexcept { return range_; }
  void set_range(const ValueRange& range);

  /// What a double-click returns to. Every effect parameter has one, and
  /// getting back to it is otherwise a matter of dragging carefully.
  void set_default_value(std::optional<double> value) { default_ = value; }

  void set_on_change(std::function<void(double)> on_change) {
    on_change_ = std::move(on_change);
  }

  [[nodiscard]] double fraction() const noexcept { return range_.to_fraction(value_); }
  [[nodiscard]] Rect groove() const;
  [[nodiscard]] Rect thumb() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Slider; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  /// Takes the thumb's size from the theme, because input and painting both
  /// need it and neither has a context to ask.
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  /// The value a pointer at `x` means, accounting for the thumb's own width.
  [[nodiscard]] double value_at(double x) const;
  void commit(double value);

  ValueRange range_;
  double value_ = 0.0;
  std::optional<double> default_;
  bool dragging_ = false;
  double thumb_size_ = 12.0;

  std::function<void(double)> on_change_;
};

/// A box that is either ticked or not, with a label beside it.
class Checkbox : public Widget {
 public:
  explicit Checkbox(std::string label = {}, bool checked = false);

  [[nodiscard]] bool checked() const noexcept { return checked_; }
  void set_checked(bool checked) noexcept { checked_ = checked; }

  [[nodiscard]] const std::string& label() const noexcept { return label_; }
  void set_label(std::string label) { label_ = std::move(label); }

  void set_on_change(std::function<void(bool)> on_change) { on_change_ = std::move(on_change); }

  /// The box itself, which is what the tick is drawn in. The label sits after
  /// it, and clicking either toggles.
  [[nodiscard]] Rect box() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Input; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  void toggle();

  std::string label_;
  bool checked_ = false;
  double box_size_ = 14.0;
  double gap_ = 8.0;
  double font_size_ = 13.0;

  std::function<void(bool)> on_change_;
};

}  // namespace cutline::ui
