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
// For `Button`, which the icon button is one of: everything about pressing,
// releasing and cancelling a click is the same, and only the mark differs.
#include "cutline/ui/widgets.hpp"

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

  /// Every change, including each pixel of a drag. For following along — a
  /// preview that should update as the value moves.
  void set_on_change(std::function<void(double)> on_change) {
    on_change_ = std::move(on_change);
  }

  /// Once, when a gesture finishes: the button comes up, or a key lands. For
  /// recording — an edit that fired on every change would put a hundred
  /// entries in the undo stack for one drag, which is the same lesson the
  /// timeline learned about dragging clips.
  void set_on_commit(std::function<void(double)> on_commit) {
    on_commit_ = std::move(on_commit);
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
  /// Reports the end of a gesture, if it actually moved anything.
  void finish();

  ValueRange range_;
  double value_ = 0.0;
  std::optional<double> default_;
  bool dragging_ = false;
  double thumb_size_ = 12.0;
  /// What the value was when the gesture began, so a drag that ends where it
  /// started reports nothing.
  double gesture_start_ = 0.0;

  std::function<void(double)> on_change_;
  std::function<void(double)> on_commit_;
};

/// A field of editable text.
///
/// Caret, selection, and enough keyboard to be usable without a mouse. What a
/// title's content is typed into, and what numeric entry will eventually be
/// built on.
///
/// **Indices are byte offsets into UTF-8**, and every movement lands on a code
/// point boundary. Bytes rather than code points because the string is what
/// everything else wants, and a caret counted in code points would have to be
/// converted at every edge.
///
/// Widths come from a table built at layout, where a text measurer is in hand.
/// Painting and hit-testing both need it and neither has one, which is the same
/// reason `MenuList` takes its row height there. A press arriving between a
/// change and the next layout reads a table one edit out of date; since the
/// change came from a keystroke and layout runs before the next frame is drawn,
/// that window contains no input.
class TextField : public Widget {
 public:
  explicit TextField(std::string text = {});

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  /// Replaces the contents. The caret and selection are clamped to fit, so a
  /// field whose value is refreshed from elsewhere cannot end up pointing past
  /// the end of it.
  void set_text(std::string text);

  /// Shown, dimmed, when the field is empty.
  void set_placeholder(std::string text) { placeholder_ = std::move(text); }
  [[nodiscard]] const std::string& placeholder() const noexcept { return placeholder_; }

  /// Whether Enter inserts a line break instead of committing.
  void set_multiline(bool multiline) noexcept;
  [[nodiscard]] bool multiline() const noexcept { return multiline_; }

  /// How many lines of room to ask for. Only meaningful for a multiline field.
  void set_min_lines(int lines) noexcept { min_lines_ = std::max(1, lines); }

  /// Every edit, as it happens.
  void set_on_change(std::function<void(const std::string&)> on_change) {
    on_change_ = std::move(on_change);
  }
  /// The moment to write the value somewhere: Enter on a single-line field, or
  /// the keyboard leaving. Not every keystroke, so a document does not collect
  /// one undo entry per character typed.
  void set_on_commit(std::function<void(const std::string&)> on_commit) {
    on_commit_ = std::move(on_commit);
  }

  [[nodiscard]] std::size_t caret() const noexcept { return caret_; }
  /// Moves the caret and drops any selection. Clamped, and snapped to a code
  /// point boundary.
  void set_caret(std::size_t index) noexcept;

  [[nodiscard]] std::size_t selection_begin() const noexcept { return std::min(anchor_, caret_); }
  [[nodiscard]] std::size_t selection_end() const noexcept { return std::max(anchor_, caret_); }
  [[nodiscard]] bool has_selection() const noexcept { return anchor_ != caret_; }
  void select_all() noexcept;

  /// The byte index nearest a point, for a click.
  [[nodiscard]] std::size_t index_at(double x, double y) const;
  /// Where the caret sits for an index, in window coordinates.
  [[nodiscard]] Rect caret_rect(std::size_t index) const;

  [[nodiscard]] Part part() const noexcept override { return Part::Input; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;
  bool on_text(char32_t codepoint) override;
  [[nodiscard]] bool wants_text() const noexcept override { return true; }
  void on_focus_changed(bool focused) override;

 private:
  /// One line of the field, with the x offset of every code point boundary in
  /// it — measured at layout, used by painting and by hit-testing.
  struct Line {
    std::size_t begin = 0;  ///< byte index of the first character
    std::size_t end = 0;    ///< byte index one past the last, before any '\n'
    std::vector<double> offsets;
  };

  void replace_selection(std::string_view with);
  void erase_before_caret();
  void erase_after_caret();
  void move_caret(std::size_t to, bool extend) noexcept;
  void changed();
  void commit();

  [[nodiscard]] std::size_t line_of(std::size_t index) const noexcept;
  [[nodiscard]] std::size_t next_boundary(std::size_t index) const noexcept;
  [[nodiscard]] std::size_t previous_boundary(std::size_t index) const noexcept;

  std::string text_;
  std::string placeholder_;
  /// What the text was when the keyboard arrived, so Escape can put it back.
  std::string committed_;

  std::size_t caret_ = 0;
  /// The other end of the selection. Equal to the caret means none.
  std::size_t anchor_ = 0;

  bool multiline_ = false;
  int min_lines_ = 1;
  bool dragging_ = false;

  /// Rebuilt at layout.
  std::vector<Line> lines_;
  double font_size_ = 13.0;
  double line_height_ = 18.0;
  double padding_ = 6.0;

  std::function<void(const std::string&)> on_change_;
  std::function<void(const std::string&)> on_commit_;
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

/// A vertical list of choices, drawn as a menu.
///
/// Built for the host's popup layer rather than for the tree, which is what a
/// dropdown's list and a menu's contents both are. One widget drawing rows
/// rather than a box of buttons, for the same reason the browser and the
/// timeline draw their own: a hundred entries would otherwise be a hundred
/// widgets to lay out, each needing the same styling talked into it.
class MenuList : public Widget {
 public:
  explicit MenuList(std::vector<std::string> items = {});

  [[nodiscard]] const std::vector<std::string>& items() const noexcept { return items_; }
  void set_items(std::vector<std::string> items);

  /// The row drawn as the current one — a dropdown's selected entry. Past the
  /// end means none, which is what a menu wants.
  [[nodiscard]] std::size_t current() const noexcept { return current_; }
  void set_current(std::size_t index) noexcept { current_ = index; }

  /// The row under the pointer or the keyboard, and what Enter would take.
  [[nodiscard]] std::size_t highlighted() const noexcept { return highlighted_; }

  void set_on_choose(std::function<void(std::size_t)> on_choose) {
    on_choose_ = std::move(on_choose);
  }

  [[nodiscard]] double row_height() const noexcept { return row_height_; }
  /// Where row `index` is. Empty when it is not one.
  [[nodiscard]] Rect row_rect(std::size_t index) const;
  /// The row at a point, or past the end when there is none.
  [[nodiscard]] std::size_t row_at(double y) const;

  [[nodiscard]] Part part() const noexcept override { return Part::Menu; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  void choose(std::size_t index);

  std::vector<std::string> items_;
  std::size_t current_ = static_cast<std::size_t>(-1);
  std::size_t highlighted_ = static_cast<std::size_t>(-1);
  /// Taken at layout, where the metrics are, because painting and hit-testing
  /// both need it and neither has them.
  double row_height_ = 22.0;
  double font_size_ = 13.0;
  double padding_ = 4.0;

  std::function<void(std::size_t)> on_choose_;
};

/// One choice from a list — the control Premiere is mostly built out of.
///
/// Shows the current value and opens a `MenuList` on the host's popup layer.
/// It has to be a popup: a dropdown near the bottom of a panel must draw its
/// list over everything beneath it, and a list inside the tree would be
/// clipped away by the panel holding it.
class Dropdown : public Widget {
 public:
  explicit Dropdown(std::vector<std::string> options = {}, std::size_t selected = 0);
  ~Dropdown() override;

  [[nodiscard]] const std::vector<std::string>& options() const noexcept { return options_; }
  void set_options(std::vector<std::string> options);

  [[nodiscard]] std::size_t selected() const noexcept { return selected_; }
  /// Sets it without calling back — for showing a value that changed elsewhere.
  void set_selected(std::size_t index) noexcept;
  /// The current option's text, or empty when there is none.
  [[nodiscard]] const std::string& value() const noexcept;

  void set_on_change(std::function<void(std::size_t)> on_change) {
    on_change_ = std::move(on_change);
  }

  /// Opens the list. Does nothing without a host to open it on, which is the
  /// case in a layout test.
  void open();
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  /// Where the arrow is drawn, on the trailing edge.
  [[nodiscard]] Rect arrow() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Input; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  std::vector<std::string> options_;
  std::size_t selected_ = 0;
  bool open_ = false;
  double arrow_width_ = 18.0;

  std::function<void(std::size_t)> on_change_;
};

/// A button carrying a drawn mark rather than a word.
///
/// The marks are lines, like the dropdown's arrow and the checkbox's tick, and
/// for the same reason: no font can be relied on to have an arrow in it, and
/// the ones that do disagree about its size and baseline. A row of three of
/// these — up, down, remove — is what a stack of effects needs and what a word
/// per button would make far too wide.
///
/// A `Button` with no text, so it inherits the square sizing and every bit of
/// the press-and-release behaviour.
class IconButton : public Button {
 public:
  enum class Icon {
    ArrowUp,
    ArrowDown,
    Cross,
    Plus,
    /// Premiere's animation toggle: a clock face with one hand.
    Stopwatch,
    /// One keyframe. Hollow, with the button's own selected state saying
    /// whether there is a keyframe here — a filled and a hollow diamond a few
    /// pixels across are much harder to tell apart than a lit button and an
    /// unlit one.
    Diamond,

    // The tool palette. Each of these says what the tool does to a clip rather
    // than what the tool looks like: a pointer, a cut, a stretch, contents
    // moving inside a fixed frame, and a frame moving between two fixed ones.
    // Line drawings, like every other icon here, because no font can be relied
    // on to have any of them.
    Pointer,
    Razor,
    RateStretch,
    Slip,
    Slide,
  };

  explicit IconButton(Icon icon, std::function<void()> on_click = {});

  [[nodiscard]] Icon icon() const noexcept { return icon_; }
  void set_icon(Icon icon) noexcept { icon_ = icon; }

  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  Icon icon_;
};

/// A bar that fills as something finishes.
///
/// Takes no input and reports nothing. Kept apart from `Slider` deliberately:
/// they look alike and mean opposite things, and a progress bar that can be
/// dragged is a bug rather than a feature.
class ProgressBar : public Widget {
 public:
  explicit ProgressBar(double fraction = 0.0);

  /// Clamped to 0..1.
  [[nodiscard]] double fraction() const noexcept { return fraction_; }
  void set_fraction(double fraction) noexcept;

  /// Drawn over the bar. Empty for none.
  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  void set_text(std::string text) { text_ = std::move(text); }

  /// The filled part of the groove.
  [[nodiscard]] Rect filled() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Slider; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  double fraction_ = 0.0;
  std::string text_;
};

}  // namespace cutline::ui
