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

  /// Ask for room for this many characters instead of taking whatever width is
  /// going. Zero, the default, is the flexible behaviour a field in a form
  /// wants; a number is for a field whose content has a known size and which
  /// would otherwise swallow a toolbar — a timecode is eleven characters and is
  /// never any other length.
  void set_columns(int columns) noexcept { columns_ = std::max(0, columns); }
  [[nodiscard]] int columns() const noexcept { return columns_; }

  /// Shown, dimmed, when the field is empty.
  void set_placeholder(std::string text) { placeholder_ = std::move(text); }
  [[nodiscard]] const std::string& placeholder() const noexcept { return placeholder_; }

  /// An I-beam, which is how a field says it is one. Without it a field and a
  /// label are the same thing to look at until you have clicked one.
  [[nodiscard]] Cursor cursor_at(double x, double y) const override;

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
  /// The edit is over, however it ended: Enter, Escape, or the keyboard
  /// leaving. For whoever put the field there, which usually has to take it
  /// away again — a field opened over something else and never closed is worse
  /// than no field at all. Runs after any commit, so a handler that reads the
  /// value sees the committed one.
  void set_on_finish(std::function<void()> on_finish) { on_finish_ = std::move(on_finish); }

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
  int columns_ = 0;
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
  std::function<void()> on_finish_;
};

/// A number that can be read, dragged and typed — Premiere's "hot text".
///
/// The control an inspector is actually made of, and the one thing `Slider` was
/// never able to be. A slider says *about* two thirds along; this says 66.7,
/// which is a number somebody can write down, match on another clip, and
/// reproduce tomorrow. Premiere shows the number always and hides the slider
/// behind a disclosure triangle, and it is the right way round: the exact value
/// is wanted far more often than the coarse gesture.
///
/// Three gestures, in Premiere's arrangement:
///
/// - **drag** the number left and right to scrub it;
/// - **click** it to type an exact one;
/// - **double-click** to go back to the parameter's default.
///
/// A press only becomes a scrub once it has moved past `kScrubThreshold`, so a
/// click that wobbles by a pixel still opens the field rather than nudging the
/// value and leaving it somewhere nobody asked for.
///
/// Typing is a `TextField` child rather than a caret and a selection written
/// again here. It is built once and hidden, not created and destroyed: the
/// thing that ends an edit is very often the field's own key handler, and
/// freeing it there would return into freed memory.
class NumericField : public Widget {
 public:
  /// How far the pointer must move before a press counts as a scrub rather
  /// than a click.
  static constexpr double kScrubThreshold = 3.0;
  /// Multipliers for a scrub with shift or control held. The same pair the
  /// slider's arrow keys use, so the two controls do not disagree about what
  /// shift means.
  static constexpr double kCoarseScrub = 10.0;
  static constexpr double kFineScrub = 0.1;
  /// The range crosses this many pixels of drag at the default rate. Chosen so
  /// a full sweep is a comfortable gesture rather than a flick or a journey.
  static constexpr double kScrubTravel = 200.0;

  explicit NumericField(ValueRange range = {}, double value = 0.0);

  [[nodiscard]] double value() const noexcept { return value_; }
  /// Clamped and quantised, without calling the change handler.
  void set_value(double value);

  [[nodiscard]] const ValueRange& range() const noexcept { return range_; }
  void set_range(const ValueRange& range);

  /// Digits after the point. Premiere shows one for most things, and a
  /// trailing `.0` is what makes a column of numbers line up.
  void set_decimals(int decimals) noexcept;
  [[nodiscard]] int decimals() const noexcept { return decimals_; }

  /// Drawn after the number: `%`, `px`, `°`. Also accepted, and ignored, when
  /// a value is typed back in, so copying a number out and pasting it in works.
  void set_suffix(std::string suffix);
  [[nodiscard]] const std::string& suffix() const noexcept { return suffix_; }

  /// What a double-click returns to.
  void set_default_value(std::optional<double> value) { default_ = value; }

  /// What one pixel of drag is worth. Zero derives it from the range, which is
  /// what nearly every parameter wants; set it for one whose useful span is
  /// much smaller than its legal one.
  void set_scrub_step(double step) noexcept;
  [[nodiscard]] double scrub_step() const noexcept;

  /// Every change, including each pixel of a scrub.
  void set_on_change(std::function<void(double)> on_change) {
    on_change_ = std::move(on_change);
  }
  /// Once, when a gesture finishes. Where the project gets written.
  void set_on_commit(std::function<void(double)> on_commit) {
    on_commit_ = std::move(on_commit);
  }

  /// The number as shown, suffix and all.
  [[nodiscard]] std::string display_text() const;

  [[nodiscard]] bool editing() const noexcept;
  /// Opens the field over the number, with everything selected.
  void begin_edit();

  [[nodiscard]] Part part() const noexcept override { return Part::Input; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  /// The number alone, which is what the field is opened with.
  [[nodiscard]] std::string number_text() const;
  /// Reads a typed string, ignoring surrounding space and any suffix. Nothing
  /// when it is not a number, which leaves the value alone.
  [[nodiscard]] std::optional<double> parse(std::string_view text) const;

  void commit(double value);
  void finish();
  void end_edit();

  ValueRange range_;
  double value_ = 0.0;
  std::optional<double> default_;
  int decimals_ = 1;
  std::string suffix_;
  double scrub_step_ = 0.0;

  /// Built in the constructor and hidden. See the note above.
  TextField* field_ = nullptr;

  bool pressed_here_ = false;
  bool scrubbing_ = false;
  double press_x_ = 0.0;
  /// The scrub's own value, kept unquantised so a fine drag across a stepped
  /// parameter accumulates instead of rounding to nothing on every pixel.
  double scrub_value_ = 0.0;
  double gesture_start_ = 0.0;

  double font_size_ = 13.0;
  double padding_ = 6.0;

  std::function<void(double)> on_change_;
  std::function<void(double)> on_commit_;
};

/// A mixer's fader: a vertical throw with a dB scale printed beside it.
///
/// Not `Slider` stood on its end. A slider is a value between two ends and its
/// look says nothing about what the numbers mean; a fader is read against a
/// *scale*, and the marks are not decoration — knowing you are 3 dB down is the
/// whole of what the control is for, and no slider can say it.
///
/// The reason this exists at all is that a mix is set by ear, with a hand on a
/// long throw and an eye on a meter beside it. That is why every mixer ever
/// built is shaped this way, and why the numeric fields this replaces were
/// never going to do: a number can be typed or scrubbed, and neither is riding
/// a level.
///
/// Values are **decibels**, not gain, and the travel is linear in them — which
/// is what makes the printed marks evenly spaced and honest. The conversion to
/// a gain multiplier belongs to the caller, where `gain_to_fader_db` already
/// lives.
class Fader : public Widget {
 public:
  explicit Fader(ValueRange range = {}, double value = 0.0);

  [[nodiscard]] double value() const noexcept { return value_; }
  /// Clamped. Does not call the change handler — that is for things the user
  /// did, so following the mix cannot loop back into it.
  void set_value(double value);

  [[nodiscard]] const ValueRange& range() const noexcept { return range_; }
  void set_range(const ValueRange& range);

  /// What a double-click returns to. Unity, for everything that has one.
  void set_default_value(std::optional<double> value) { default_ = value; }

  void set_on_change(std::function<void(double)> on_change) { on_change_ = std::move(on_change); }
  void set_on_commit(std::function<void(double)> on_commit) { on_commit_ = std::move(on_commit); }

  /// Whether the numbers are drawn beside the throw. Off in a strip too narrow
  /// to carry them, where the throw alone is still a fader.
  void set_shows_scale(bool shows) noexcept { shows_scale_ = shows; }
  [[nodiscard]] bool shows_scale() const noexcept { return shows_scale_; }

  /// Where the thumb sits along the throw, 0 at the bottom and 1 at the top.
  ///
  /// **Tapered, not linear in decibels.** A throw spread evenly from -60 to +6
  /// puts unity nine tenths of the way up and leaves the region anybody
  /// actually mixes in — the few decibels either side of it — squeezed into the
  /// last centimetre, while half the travel is spent between "very quiet" and
  /// "silent", where nobody is working. Every console ever built tapers this,
  /// and so does Premiere: unity sits about three quarters up and the bottom
  /// half of the throw covers everything below about -12.
  ///
  /// Found by looking at it. The linear version passed every test it had,
  /// because a test asks where a level is and gets a consistent answer either
  /// way — what it cannot ask is whether the answer leaves you anywhere to put
  /// your hand.
  [[nodiscard]] double fraction() const noexcept { return fraction_of(value_); }
  [[nodiscard]] double fraction_of(double db) const noexcept;
  [[nodiscard]] double db_at(double fraction) const noexcept;

  /// Where unity sits along the throw. Premiere's proportion.
  static constexpr double kUnityAt = 0.75;

  /// The throw the thumb travels in, and the cap itself.
  [[nodiscard]] Rect groove() const;
  [[nodiscard]] Rect thumb() const;

  /// Where a level sits, in absolute y. Public so the marks and the thumb
  /// cannot disagree about the mapping — the fault every scaled control has.
  [[nodiscard]] double y_of(double db) const;

  /// The levels the scale prints, loudest first. Premiere's own set.
  [[nodiscard]] static std::span<const double> scale_marks() noexcept;

  [[nodiscard]] Part part() const noexcept override { return Part::Slider; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  [[nodiscard]] double value_at(double y) const;
  void move_to(double value);
  void finish();

  ValueRange range_;
  double value_ = 0.0;
  std::optional<double> default_;

  bool dragging_ = false;
  double gesture_start_ = 0.0;

  bool shows_scale_ = true;
  double thumb_height_ = 14.0;
  double throw_width_ = 16.0;
  double scale_width_ = 26.0;
  double font_size_ = 10.0;

  std::function<void(double)> on_change_;
  std::function<void(double)> on_commit_;
};

/// A mixer's panner: a rotary dial with L and R either side of it.
///
/// Round because what it sets is round. A pan has a *centre* that both ends
/// travel away from, and a knob says that in its shape — the pointer stands
/// straight up at the middle, and how far it has fallen either way is the
/// answer, readable across a room. A horizontal slider says the same thing with
/// its centre marked, badly, and a numeric field says it only if you read it.
///
/// Dragged vertically rather than in a circle. Chasing a pointer round a dial
/// is precise for the first ninety degrees and hopeless after that, because the
/// hand has to travel further for the same change as the angle steepens; every
/// mixer built since knobs stopped being physical takes an up-and-down drag,
/// and so does Premiere.
class PanKnob : public Widget {
 public:
  explicit PanKnob(double value = 0.0,
                   ValueRange range = ValueRange{.minimum = -100.0, .maximum = 100.0});

  [[nodiscard]] double value() const noexcept { return value_; }
  /// Clamped. Silent, like every other control here: setting from code is the
  /// panel following the document, not somebody turning the knob.
  void set_value(double value);

  [[nodiscard]] const ValueRange& range() const noexcept { return range_; }

  void set_on_change(std::function<void(double)> on_change) { on_change_ = std::move(on_change); }
  void set_on_commit(std::function<void(double)> on_commit) { on_commit_ = std::move(on_commit); }

  /// Whether the L and R are drawn. Off in a strip with no width for them.
  void set_shows_ends(bool shows) noexcept { shows_ends_ = shows; }
  [[nodiscard]] bool shows_ends() const noexcept { return shows_ends_; }

  /// The dial itself, which is square and centred in whatever it is given.
  [[nodiscard]] Rect dial() const;

  /// Where the pointer stands, in degrees clockwise from straight up. Public
  /// for the same reason `Fader::y_of` is: the drawing and any test of it read
  /// one mapping rather than two that can drift.
  [[nodiscard]] double angle_of(double value) const;

  /// How far either way the pointer swings. Not a full turn — a dial that can
  /// come back round to where it started cannot be read at a glance.
  static constexpr double kSweepDegrees = 135.0;

  [[nodiscard]] Part part() const noexcept override { return Part::Slider; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  void move_to(double value);
  void finish();

  ValueRange range_;
  double value_ = 0.0;

  bool dragging_ = false;
  double press_y_ = 0.0;
  double press_value_ = 0.0;
  double gesture_start_ = 0.0;

  bool shows_ends_ = true;
  double dial_size_ = 34.0;
  double font_size_ = 10.0;
  double end_width_ = 8.0;

  std::function<void(double)> on_change_;
  std::function<void(double)> on_commit_;
};

/// A column of mutually exclusive choices, exactly one of them taken.
///
/// The control every dialogue wants when a question has three or four answers
/// and the answers should all be visible at once. Without it those questions
/// were being asked with dropdowns, which hide every choice but the current one
/// behind a click — fine for a long list of formats, wrong for "which of these
/// four marks should I give up", where seeing the alternatives *is* the
/// deciding.
///
/// One widget rather than a box of small ones, which is not only cheaper: a
/// radio group is a single stop for the keyboard, and the arrow keys move the
/// selection *within* it. Built out of children, each row would take its own
/// tab stop and the arrows would mean nothing, which is not how any radio group
/// on this platform behaves.
///
/// There is always a selection. A group with nothing taken is a question that
/// has not been asked properly — the caller decides the default, and every path
/// through this leaves exactly one row chosen.
class RadioGroup : public Widget {
 public:
  explicit RadioGroup(std::vector<std::string> options = {}, std::size_t selected = 0);

  [[nodiscard]] const std::vector<std::string>& options() const noexcept { return options_; }
  void set_options(std::vector<std::string> options, std::size_t selected = 0);

  [[nodiscard]] std::size_t selected() const noexcept { return selected_; }
  /// Ignores an index past the end, so a group is never left with nothing
  /// taken by a caller counting wrong.
  void select(std::size_t index);

  void set_on_change(std::function<void(std::size_t)> on_change) {
    on_change_ = std::move(on_change);
  }

  /// The row's whole strip, and the circle drawn at the head of it.
  [[nodiscard]] Rect row_rect(std::size_t index) const;
  [[nodiscard]] Rect dot(std::size_t index) const;
  /// Which row a point is in, or `options().size()` for none.
  [[nodiscard]] std::size_t row_at(double y) const;

  [[nodiscard]] Part part() const noexcept override { return Part::Input; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  void choose(std::size_t index);

  std::vector<std::string> options_;
  std::size_t selected_ = 0;

  double row_height_ = 22.0;
  double dot_size_ = 14.0;
  double gap_ = 8.0;
  double font_size_ = 13.0;

  std::function<void(std::size_t)> on_change_;
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

  /// Which rows carry a tick, one flag per item.
  ///
  /// Different from `current`, which marks the one row a dropdown is showing:
  /// any number of these can be ticked at once, which is what a menu of things
  /// that are either on or off needs — the panels that are open, say. A list
  /// with any ticks at all indents every row, ticked or not, so the labels line
  /// up as a column rather than stepping in and out.
  void set_checked(std::vector<bool> checked) { checked_ = std::move(checked); }
  [[nodiscard]] const std::vector<bool>& checked() const noexcept { return checked_; }

  /// The row under the pointer or the keyboard, and what Enter would take.
  [[nodiscard]] std::size_t highlighted() const noexcept { return highlighted_; }

  void set_on_choose(std::function<void(std::size_t)> on_choose) {
    on_choose_ = std::move(on_choose);
  }

  /// Whether the list takes all the height it is given rather than only what
  /// its rows need.
  ///
  /// Off for a menu, which is as tall as what is in it. On for a list used as a
  /// column of categories down the side of a window, where a panel that stops
  /// half way down reads as unfinished rather than as compact.
  void set_fills_height(bool fills) noexcept { fills_height_ = fills; }
  [[nodiscard]] bool fills_height() const noexcept { return fills_height_; }

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
  std::vector<bool> checked_;
  std::size_t current_ = static_cast<std::size_t>(-1);
  std::size_t highlighted_ = static_cast<std::size_t>(-1);
  /// Taken at layout, where the metrics are, because painting and hit-testing
  /// both need it and neither has them.
  double row_height_ = 22.0;
  bool fills_height_ = false;
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
    /// Premiere's ◀ ▶ either side of a keyframe marker: go to the previous or
    /// the next one on this property.
    ArrowLeft,
    ArrowRight,
    Cross,
    Plus,
    /// Premiere's animation toggle: a clock face with one hand.
    Stopwatch,
    /// One keyframe. Hollow, with the button's own selected state saying
    /// whether there is a keyframe here — a filled and a hollow diamond a few
    /// pixels across are much harder to tell apart than a lit button and an
    /// unlit one.
    Diamond,
    /// Premiere's disclosure triangle: a chevron pointing right when what it
    /// governs is hidden, and down when it is showing. Drawn smaller than the
    /// arrows, which sit in the same panel and mean something else entirely.
    Disclosure,
    /// Premiere's reset: an arrow curving back on itself. Drawn as a
    /// three-quarter ring with a head, because a full circle would read as the
    /// stopwatch two columns to its left.
    Reset,

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
    /// An edge with everything behind it following: one bar, and arrows leaving
    /// it in the same direction.
    Ripple,
    /// A join moving: two blocks that meet, with the line between them carrying
    /// arrows both ways.
    Roll,
  };

  explicit IconButton(Icon icon, std::function<void()> on_click = {});

  [[nodiscard]] Icon icon() const noexcept { return icon_; }
  void set_icon(Icon icon) noexcept { icon_ = icon; }

  /// Narrower than it is tall, for a run of buttons that belong together.
  ///
  /// Premiere's keyframe navigator is three small arrows occupying about the
  /// room one control would take, not three square controls side by side. Three
  /// of those, a stopwatch, a disclosure triangle and two numbers do not fit a
  /// parameter row, and the property's own name is what was dropped to make
  /// them fit. Off by default: a button on its own wants a square target.
  [[nodiscard]] bool narrow() const noexcept { return narrow_; }
  void set_narrow(bool narrow) noexcept { narrow_ = narrow; }

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  Icon icon_;
  bool narrow_ = false;
};

/// One of the icon marks, drawn into `area` about its centre.
///
/// A free function because the marks have a second home: the tool cursors are
/// these same drawings rendered into a small bitmap, and a razor that looked
/// one way in the palette and another under the pointer would be two razors.
///
/// `reach` is the mark's half-size in pixels and `width` its stroke, both given
/// rather than derived from `area` — a cursor is drawn several times the size
/// of a button's mark and wants a stroke to match, not a scaled-up version of a
/// one-and-a-half-pixel line.
/// `on` is the toggled state, which one mark depends on: the disclosure
/// triangle points down when what it governs is open and right when it is not.
void draw_icon(Painter& painter, IconButton::Icon icon, const Rect& area, const Color& color,
               double reach, double width, bool on = false);

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
