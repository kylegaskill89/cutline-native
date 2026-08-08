/// The controls that edit a value.
///
/// `ValueRange` gets the most attention because it is the piece that is easy to
/// get subtly wrong: a slider that should stop at 5 stopping at 4.8 is not
/// something anyone notices until it matters.

#include "cutline/ui/controls.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() {
  return LayoutContext{default_theme(), measurer()};
}

[[nodiscard]] MouseEvent press(double x, double y, int clicks = 1) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left, .click_count = clicks};
}

// ------------------------------------------------------------ value range --

TEST(ValueRange, ClampsToItsBounds) {
  const ValueRange range{.minimum = 0.0, .maximum = 10.0};
  EXPECT_DOUBLE_EQ(range.clamp(-5.0), 0.0);
  EXPECT_DOUBLE_EQ(range.clamp(50.0), 10.0);
  EXPECT_DOUBLE_EQ(range.clamp(4.0), 4.0);
}

TEST(ValueRange, WorksWithItsBoundsEitherWayRound) {
  // A control that runs from 100 down to 0 is unusual but not wrong.
  const ValueRange range{.minimum = 100.0, .maximum = 0.0};
  EXPECT_DOUBLE_EQ(range.clamp(150.0), 100.0);
  EXPECT_DOUBLE_EQ(range.to_fraction(50.0), 0.5);
  EXPECT_DOUBLE_EQ(range.from_fraction(0.0), 100.0);
}

TEST(ValueRange, QuantisesFromTheMinimumRatherThanFromZero) {
  // The whole point. From zero this would offer 10, 20, 30 and never reach
  // either end of its own range.
  const ValueRange range{.minimum = 5.0, .maximum = 100.0, .step = 10.0};

  EXPECT_DOUBLE_EQ(range.quantise(5.0), 5.0);
  EXPECT_DOUBLE_EQ(range.quantise(14.0), 15.0);
  EXPECT_DOUBLE_EQ(range.quantise(16.0), 15.0);
  EXPECT_DOUBLE_EQ(range.quantise(24.9), 25.0);
}

TEST(ValueRange, QuantisingNeverEscapesTheBounds) {
  // Rounding up from near the top must not step past it.
  const ValueRange range{.minimum = 0.0, .maximum = 7.0, .step = 2.0};
  EXPECT_LE(range.quantise(6.9), 7.0);
  EXPECT_GE(range.quantise(-1.0), 0.0);
}

TEST(ValueRange, NoStepMeansContinuous) {
  const ValueRange range{.minimum = 0.0, .maximum = 1.0};
  EXPECT_DOUBLE_EQ(range.quantise(0.3333), 0.3333);
}

TEST(ValueRange, FractionsRoundTrip) {
  const ValueRange range{.minimum = -50.0, .maximum = 150.0};
  for (const double fraction : {0.0, 0.25, 0.5, 0.75, 1.0}) {
    EXPECT_NEAR(range.to_fraction(range.from_fraction(fraction)), fraction, 1e-9);
  }
}

TEST(ValueRange, ADegenerateRangeIsNotADivisionByZero) {
  const ValueRange range{.minimum = 3.0, .maximum = 3.0};
  EXPECT_DOUBLE_EQ(range.to_fraction(3.0), 0.0);
  EXPECT_DOUBLE_EQ(range.from_fraction(0.7), 3.0);
  EXPECT_DOUBLE_EQ(range.nudge(), 0.0);
}

TEST(ValueRange, NudgeIsTheStepOrAHundredthOfTheRange) {
  EXPECT_DOUBLE_EQ((ValueRange{.minimum = 0.0, .maximum = 10.0, .step = 0.5}).nudge(), 0.5);
  EXPECT_DOUBLE_EQ((ValueRange{.minimum = 0.0, .maximum = 200.0}).nudge(), 2.0);
}

// ----------------------------------------------------------------- slider --

/// A slider in a host, laid out wide enough for the thumb to travel.
struct Slid {
  Slid() {
    host = std::make_unique<WidgetHost>(std::make_unique<Widget>());
    slider = &host->root().emplace<Slider>(ValueRange{.minimum = 0.0, .maximum = 100.0}, 0.0);
    slider->set_on_change([this](double value) {
      ++changes;
      last = value;
    });
    host->resize(Rect{0.0, 0.0, 400.0, 100.0}, flat_context());
    slider->arrange(Rect{0.0, 0.0, 200.0, 24.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  Slider* slider = nullptr;
  int changes = 0;
  double last = -1.0;
};

TEST(Slider, StartsWhereItWasTold) {
  const Slider slider(ValueRange{.minimum = 0.0, .maximum = 10.0}, 4.0);
  EXPECT_DOUBLE_EQ(slider.value(), 4.0);
  EXPECT_DOUBLE_EQ(slider.fraction(), 0.4);
}

TEST(Slider, ClampsWhateverItIsGiven) {
  Slider slider(ValueRange{.minimum = 0.0, .maximum = 10.0});
  slider.set_value(99.0);
  EXPECT_DOUBLE_EQ(slider.value(), 10.0);
}

TEST(Slider, SettingAValueFromCodeDoesNotCallBack) {
  // Otherwise the handler that updates a model runs when the model updates the
  // slider, and the two chase each other.
  Slid test;
  test.slider->set_value(50.0);
  EXPECT_EQ(test.changes, 0);
}

TEST(Slider, TheThumbTravelsTheGrooveMinusItsOwnWidth) {
  // Same lesson as a scrollbar: mapping against the full width means the last
  // part of the range can never quite be reached.
  Slid test;
  test.slider->set_value(100.0);

  const Rect groove = test.slider->groove();
  const Rect thumb = test.slider->thumb();
  EXPECT_NEAR(thumb.right(), groove.right(), 1e-9);

  test.slider->set_value(0.0);
  EXPECT_NEAR(test.slider->thumb().x, groove.x, 1e-9);
}

TEST(Slider, ClickingTheGrooveJumpsThere) {
  Slid test;
  const Rect groove = test.slider->groove();
  test.host->mouse_down(press(groove.x + groove.width / 2.0, 12.0));

  EXPECT_NEAR(test.slider->value(), 50.0, 2.0);
  EXPECT_EQ(test.changes, 1);
}

TEST(Slider, DraggingFollowsThePointerPastTheEdges) {
  Slid test;
  const Rect groove = test.slider->groove();

  test.host->mouse_down(press(groove.x + 20.0, 12.0));
  ASSERT_EQ(test.host->captured(), test.slider);

  test.host->mouse_move(press(9000.0, 12.0));
  EXPECT_DOUBLE_EQ(test.slider->value(), 100.0);

  test.host->mouse_move(press(-9000.0, 12.0));
  EXPECT_DOUBLE_EQ(test.slider->value(), 0.0);
}

TEST(Slider, ADragRecordsOnceAtTheEnd) {
  // The same lesson the timeline learned: an edit on every change would put a
  // hundred entries in the undo stack for one drag.
  Slid test;
  int commits = 0;
  double committed = -1.0;
  test.slider->set_on_commit([&](double value) {
    ++commits;
    committed = value;
  });

  const Rect groove = test.slider->groove();
  test.host->mouse_down(press(groove.x + 20.0, 12.0));
  test.host->mouse_move(press(groove.x + 60.0, 12.0));
  test.host->mouse_move(press(groove.x + 120.0, 12.0));
  EXPECT_EQ(commits, 0) << "it recorded partway through the drag";
  EXPECT_GT(test.changes, 1) << "but it should have been following along";

  test.host->mouse_up(press(groove.x + 120.0, 12.0));
  EXPECT_EQ(commits, 1);
  EXPECT_DOUBLE_EQ(committed, test.slider->value());
}

TEST(Slider, ADragThatEndsWhereItStartedRecordsNothing) {
  Slid test;
  int commits = 0;
  test.slider->set_on_commit([&](double) { ++commits; });

  const Rect groove = test.slider->groove();
  test.host->mouse_down(press(groove.x, 12.0));
  test.host->mouse_move(press(groove.x + 80.0, 12.0));
  test.host->mouse_move(press(groove.x, 12.0));
  test.host->mouse_up(press(groove.x, 12.0));

  EXPECT_EQ(commits, 0);
}

TEST(Slider, EachKeyPressIsItsOwnGesture) {
  // So undoing a nudge undoes exactly that nudge, rather than the whole time
  // the key was held.
  Slid test;
  int commits = 0;
  test.slider->set_on_commit([&](double) { ++commits; });
  test.host->set_focus(test.slider);

  test.host->key_down(KeyEvent{.key = Key::Right});
  test.host->key_down(KeyEvent{.key = Key::Right});
  EXPECT_EQ(commits, 2);

  // At the end of the range there is nowhere to go, so nothing is recorded.
  test.host->key_down(KeyEvent{.key = Key::End});
  const int settled = commits;
  test.host->key_down(KeyEvent{.key = Key::Right});
  EXPECT_EQ(commits, settled);
}

TEST(Slider, ADoubleClickResetRecordsToo) {
  Slid test;
  int commits = 0;
  test.slider->set_on_commit([&](double) { ++commits; });
  test.slider->set_default_value(25.0);
  test.slider->set_value(90.0);

  test.host->mouse_down(press(10.0, 12.0, 2));
  EXPECT_EQ(commits, 1);
  EXPECT_DOUBLE_EQ(test.slider->value(), 25.0);
}

TEST(Slider, ReleasingEndsTheDrag) {
  Slid test;
  const Rect groove = test.slider->groove();
  test.host->mouse_down(press(groove.x + 20.0, 12.0));
  test.host->mouse_up(press(groove.x + 20.0, 12.0));

  const double settled = test.slider->value();
  test.host->mouse_move(press(groove.right(), 12.0));
  EXPECT_DOUBLE_EQ(test.slider->value(), settled);
}

TEST(Slider, ChangesAreReportedOnlyWhenTheValueActuallyMoves) {
  Slid test;
  const Rect groove = test.slider->groove();
  test.host->mouse_down(press(groove.x, 12.0));
  const int after_press = test.changes;

  test.host->mouse_move(press(groove.x, 12.0));
  test.host->mouse_move(press(groove.x - 5.0, 12.0));
  EXPECT_EQ(test.changes, after_press) << "it reported a change that did not happen";
}

TEST(Slider, ArrowKeysNudgeAndShiftNudgesFurther) {
  Slid test;
  test.slider->set_value(50.0);
  test.host->set_focus(test.slider);

  test.host->key_down(KeyEvent{.key = Key::Right});
  EXPECT_DOUBLE_EQ(test.slider->value(), 51.0);

  test.host->key_down(KeyEvent{.key = Key::Left});
  EXPECT_DOUBLE_EQ(test.slider->value(), 50.0);

  test.host->key_down(KeyEvent{.key = Key::Right, .modifiers = Modifiers{.shift = true}});
  EXPECT_DOUBLE_EQ(test.slider->value(), 60.0);
}

TEST(Slider, HomeAndEndGoToTheEnds) {
  Slid test;
  test.host->set_focus(test.slider);

  test.host->key_down(KeyEvent{.key = Key::End});
  EXPECT_DOUBLE_EQ(test.slider->value(), 100.0);
  test.host->key_down(KeyEvent{.key = Key::Home});
  EXPECT_DOUBLE_EQ(test.slider->value(), 0.0);
}

TEST(Slider, IgnoresShortcutsThatAreNotItsOwn) {
  Slid test;
  test.host->set_focus(test.slider);
  EXPECT_FALSE(test.host->key_down(
      KeyEvent{.key = Key::Right, .modifiers = Modifiers{.control = true}}));
  EXPECT_EQ(test.changes, 0);
}

TEST(Slider, DoubleClickingReturnsToTheDefault) {
  Slid test;
  test.slider->set_default_value(25.0);
  test.slider->set_value(90.0);

  test.host->mouse_down(press(10.0, 12.0, 2));
  EXPECT_DOUBLE_EQ(test.slider->value(), 25.0);
}

TEST(Slider, WithNoDefaultADoubleClickJustSetsAValue) {
  Slid test;
  const Rect groove = test.slider->groove();
  // A pixel inside the right edge: bounds are half-open, so the edge itself is
  // already outside the control.
  test.host->mouse_down(press(groove.right() - 1.0, 12.0, 2));
  EXPECT_DOUBLE_EQ(test.slider->value(), 100.0);
}

TEST(Slider, StepsAreHonouredWhileDragging) {
  Slid test;
  test.slider->set_range(ValueRange{.minimum = 0.0, .maximum = 100.0, .step = 25.0});
  const Rect groove = test.slider->groove();

  test.host->mouse_down(press(groove.x + groove.width * 0.4, 12.0));
  const double value = test.slider->value();
  EXPECT_DOUBLE_EQ(value, std::round(value / 25.0) * 25.0);
}

TEST(Slider, TakesItsHeightFromTheTheme) {
  const LayoutContext context = flat_context();
  const Slider slider;
  EXPECT_DOUBLE_EQ(slider.sizing(Axis::Vertical, context).basis,
                   context.metrics().control_height);
}

TEST(Slider, DrawsAGrooveAndAThumb) {
  Slid test;
  test.slider->set_value(60.0);

  RecordingPainter painter;
  test.slider->paint(painter, default_theme());

  EXPECT_GE(painter.count(DrawCall::Kind::Fill), 2u);
  bool has_thumb = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Fill &&
        call.fill == default_theme().style(Part::SliderThumb).fill) {
      has_thumb = true;
    }
  }
  EXPECT_TRUE(has_thumb) << "no thumb was drawn in the theme's thumb style";
}

TEST(Slider, ANarrowSliderIsStillHarmless) {
  Slid test;
  test.slider->arrange(Rect{0.0, 0.0, 2.0, 24.0}, flat_context());

  RecordingPainter painter;
  test.slider->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());

  test.host->mouse_down(press(1.0, 12.0));
  EXPECT_GE(test.slider->value(), 0.0);
}

// --------------------------------------------------------- numeric field --

/// A numeric field in a host, laid out wide enough to be dragged across.
struct Numeric {
  explicit Numeric(ValueRange range = ValueRange{.minimum = 0.0, .maximum = 100.0},
                   double value = 50.0) {
    host = std::make_unique<WidgetHost>(std::make_unique<Widget>());
    field = &host->root().emplace<NumericField>(range, value);
    field->set_on_change([this](double v) {
      ++changes;
      last = v;
    });
    field->set_on_commit([this](double v) {
      ++commits;
      committed = v;
    });
    host->resize(Rect{0.0, 0.0, 400.0, 100.0}, flat_context());
    field->arrange(Rect{0.0, 0.0, 120.0, 24.0}, flat_context());
  }

  /// Presses at `x`, drags to `x + by`, and lets go.
  void drag(double from, double by, Modifiers modifiers = {}) {
    host->mouse_down(MouseEvent{
        .x = from, .y = 12.0, .button = MouseButton::Left, .modifiers = modifiers,
        .click_count = 1});
    host->mouse_move(MouseEvent{.x = from + by, .y = 12.0, .modifiers = modifiers});
    host->mouse_up(MouseEvent{
        .x = from + by, .y = 12.0, .button = MouseButton::Left, .modifiers = modifiers});
  }

  void type(std::string_view text) {
    for (const char c : text) host->text(static_cast<char32_t>(c));
  }

  std::unique_ptr<WidgetHost> host;
  NumericField* field = nullptr;
  int changes = 0;
  int commits = 0;
  double last = -1.0;
  double committed = -1.0;
};

TEST(NumericField, ShowsTheValueAsANumber) {
  // The whole reason this control exists. A slider can only say "about two
  // thirds along", which is not something anybody can write down.
  const NumericField field(ValueRange{.minimum = 0.0, .maximum = 100.0}, 66.66);
  EXPECT_EQ(field.display_text(), "66.7");
}

TEST(NumericField, ShowsItsUnitWithoutASpaceBeforeIt) {
  NumericField field(ValueRange{.minimum = 0.0, .maximum = 360.0}, 90.0);
  field.set_suffix("°");
  field.set_decimals(0);
  EXPECT_EQ(field.display_text(), "90°");
}

TEST(NumericField, DraggingScrubsTheValue) {
  Numeric test;
  test.drag(60.0, 40.0);
  // The default rate crosses the range in `kScrubTravel` pixels.
  EXPECT_DOUBLE_EQ(test.field->value(), 50.0 + 40.0 * (100.0 / NumericField::kScrubTravel));
  EXPECT_EQ(test.commits, 1) << "one gesture is one entry in the undo stack";
}

TEST(NumericField, ScrubbingIsMeasuredFromThePressRatherThanTheLastMove) {
  // A drag that goes out and comes back has to come back to where it started.
  // Accumulating each move's delta drifts, and a stepped range drifts fastest.
  Numeric test;
  test.host->mouse_down(press(60.0, 12.0));
  test.host->mouse_move(MouseEvent{.x = 160.0, .y = 12.0});
  test.host->mouse_move(MouseEvent{.x = 60.0, .y = 12.0});
  test.host->mouse_up(press(60.0, 12.0));

  EXPECT_DOUBLE_EQ(test.field->value(), 50.0);
  EXPECT_EQ(test.commits, 0) << "it ended where it began, so nothing changed";
}

TEST(NumericField, ShiftScrubsCoarselyAndControlFinely) {
  Numeric coarse;
  coarse.drag(60.0, 10.0, Modifiers{.shift = true});

  Numeric fine;
  fine.drag(60.0, 10.0, Modifiers{.control = true});

  EXPECT_GT(coarse.field->value(), 50.0 + 10.0 * (100.0 / NumericField::kScrubTravel));
  EXPECT_LT(fine.field->value(), 50.0 + 10.0 * (100.0 / NumericField::kScrubTravel));
  EXPECT_GT(fine.field->value(), 50.0);
}

TEST(NumericField, AFineScrubAccumulatesAcrossAStepThatWouldRoundItAway) {
  // Each pixel of a fine drag is worth a fraction of the step. Quantising the
  // scrub as it went would round every one of them back to nothing, and the
  // value would never move at all.
  Numeric test(ValueRange{.minimum = 0.0, .maximum = 100.0, .step = 1.0}, 50.0);
  test.drag(60.0, 60.0, Modifiers{.control = true});
  EXPECT_DOUBLE_EQ(test.field->value(), 53.0);
}

TEST(NumericField, AClickThatDoesNotMoveOpensTheFieldInsteadOfNudging) {
  Numeric test;
  test.host->mouse_down(press(60.0, 12.0));
  test.host->mouse_up(press(60.0, 12.0));

  EXPECT_TRUE(test.field->editing());
  EXPECT_EQ(test.changes, 0) << "clicking a number must not change it";
}

TEST(NumericField, ASmallWobbleIsStillAClick) {
  Numeric test;
  test.host->mouse_down(press(60.0, 12.0));
  test.host->mouse_move(MouseEvent{.x = 60.0 + NumericField::kScrubThreshold - 1.0, .y = 12.0});
  test.host->mouse_up(press(60.0, 12.0));

  EXPECT_TRUE(test.field->editing());
  EXPECT_DOUBLE_EQ(test.field->value(), 50.0);
}

TEST(NumericField, AScrubIsNotAlsoAClick) {
  Numeric test;
  test.drag(60.0, 40.0);
  EXPECT_FALSE(test.field->editing());
}

TEST(NumericField, TypingANumberSetsIt) {
  Numeric test;
  test.field->begin_edit();
  ASSERT_TRUE(test.field->editing());

  test.type("12.5");
  test.host->key_down(KeyEvent{.key = Key::Enter});

  EXPECT_DOUBLE_EQ(test.field->value(), 12.5);
  EXPECT_EQ(test.commits, 1);
  EXPECT_FALSE(test.field->editing()) << "Enter is the end of the edit";
}

TEST(NumericField, TheFieldOpensWithTheNumberAloneAndSelected) {
  // Having to type round a degree sign would make the fast path slower than
  // the slow one.
  Numeric test(ValueRange{.minimum = 0.0, .maximum = 360.0}, 90.0);
  test.field->set_suffix("°");
  test.field->begin_edit();

  test.type("45");
  test.host->key_down(KeyEvent{.key = Key::Enter});
  EXPECT_DOUBLE_EQ(test.field->value(), 45.0) << "the selection was replaced, not appended to";
}

TEST(NumericField, TypingTheUnitBackInIsAccepted) {
  Numeric test;
  test.field->set_suffix("%");
  test.field->begin_edit();
  test.type("20%");
  test.host->key_down(KeyEvent{.key = Key::Enter});
  EXPECT_DOUBLE_EQ(test.field->value(), 20.0);
}

TEST(NumericField, NonsenseLeavesTheValueAlone) {
  Numeric test;
  test.field->begin_edit();
  test.type("about half");
  test.host->key_down(KeyEvent{.key = Key::Enter});

  EXPECT_DOUBLE_EQ(test.field->value(), 50.0);
  EXPECT_EQ(test.commits, 0);
}

TEST(NumericField, ATypedValueIsClampedToTheRange) {
  Numeric test;
  test.field->begin_edit();
  test.type("900");
  test.host->key_down(KeyEvent{.key = Key::Enter});
  EXPECT_DOUBLE_EQ(test.field->value(), 100.0);
}

TEST(NumericField, EscapeClosesTheFieldAndKeepsTheValue) {
  Numeric test;
  test.field->begin_edit();
  test.type("7");
  test.host->key_down(KeyEvent{.key = Key::Escape});

  EXPECT_FALSE(test.field->editing()) << "a field with no way out of it is worse than none";
  EXPECT_DOUBLE_EQ(test.field->value(), 50.0);
  EXPECT_EQ(test.commits, 0);
}

TEST(NumericField, TheKeyboardComesBackToTheNumberWhenTheEditEnds) {
  Numeric test;
  test.field->begin_edit();
  test.host->key_down(KeyEvent{.key = Key::Enter});
  EXPECT_EQ(test.host->focused(), test.field);
}

TEST(NumericField, DoubleClickingReturnsToTheDefault) {
  Numeric test;
  test.field->set_default_value(25.0);
  test.host->mouse_down(press(60.0, 12.0, 2));

  EXPECT_DOUBLE_EQ(test.field->value(), 25.0);
  EXPECT_EQ(test.commits, 1);
  EXPECT_FALSE(test.field->editing());
}

TEST(NumericField, ArrowKeysNudgeIt) {
  Numeric test(ValueRange{.minimum = 0.0, .maximum = 100.0, .step = 5.0}, 50.0);
  test.host->set_focus(test.field);

  test.host->key_down(KeyEvent{.key = Key::Up});
  EXPECT_DOUBLE_EQ(test.field->value(), 55.0);
  test.host->key_down(KeyEvent{.key = Key::Down});
  test.host->key_down(KeyEvent{.key = Key::Down});
  EXPECT_DOUBLE_EQ(test.field->value(), 45.0);
  EXPECT_EQ(test.commits, 3) << "each press is a gesture of its own";
}

TEST(NumericField, IsAsWideAsItsWidestValueRatherThanItsCurrentOne) {
  // Otherwise the row shuffles sideways as the number is scrubbed past 9.9.
  const NumericField field(ValueRange{.minimum = 0.0, .maximum = 1000.0}, 1.0);
  const LayoutItem narrow = field.sizing(Axis::Horizontal, flat_context());

  NumericField wide(ValueRange{.minimum = 0.0, .maximum = 1000.0}, 999.0);
  EXPECT_DOUBLE_EQ(narrow.basis, wide.sizing(Axis::Horizontal, flat_context()).basis);
}

TEST(NumericField, GivesUpRoomBeforeTheLabelBesideItDoes) {
  // A parameter row with two numbers, a reset and three keyframe controls does
  // not fit a narrow panel, and something has to give. It is this: a number cut
  // short can be read by widening the panel, whereas a row with no name on it
  // says nothing at all. That was on screen — an animated Position showing two
  // numbers and no word to say what they were.
  const NumericField field(ValueRange{.minimum = 0.0, .maximum = 1000.0}, 1.0);
  const LayoutItem item = field.sizing(Axis::Horizontal, flat_context());

  EXPECT_GT(item.shrink, 0.0);
  EXPECT_GT(item.min, 0.0) << "but not to nothing";
  EXPECT_LT(item.min, item.basis);
}

TEST(NumericField, ADisabledFieldStillShowsItsValue) {
  // A governed property — Scale Y under a locked aspect — has to stay readable.
  // Hiding it would leave nowhere to read the number the picture is at.
  Numeric test;
  test.field->set_enabled(false);
  EXPECT_EQ(test.field->display_text(), "50.0");

  RecordingPainter painter;
  test.field->paint(painter, default_theme());
  bool drawn = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.run.has_value() && call.run->text == "50.0") drawn = true;
  }
  EXPECT_TRUE(drawn);
}

TEST(NumericField, ADisabledFieldCannotBeScrubbedOrTyped) {
  Numeric test;
  test.field->set_enabled(false);
  test.drag(60.0, 40.0);
  EXPECT_DOUBLE_EQ(test.field->value(), 50.0);
  EXPECT_FALSE(test.field->editing());
}

TEST(NumericField, DrawsItsNumberInTheAccentColour) {
  // The colour is the affordance: nothing else says the number can be dragged.
  Numeric test;
  RecordingPainter painter;
  test.field->paint(painter, default_theme());

  bool found = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.run.has_value() && call.run->text == "50.0") {
      found = call.run->color == default_theme().accent;
    }
  }
  EXPECT_TRUE(found);
}

TEST(NumericField, DrawsTheFieldInsteadOfTheNumberWhileEditing) {
  Numeric test;
  test.field->begin_edit();
  test.field->arrange(Rect{0.0, 0.0, 120.0, 24.0}, flat_context());

  // The field opens showing the same number, so the number appearing is
  // expected. Twice is the bug: the hot text drawn underneath the field that
  // replaced it, one pixel off, in a different colour.
  RecordingPainter painter;
  test.field->paint(painter, default_theme());

  int drawn = 0;
  for (const DrawCall& call : painter.calls()) {
    if (call.run.has_value() && call.run->text == "50.0") ++drawn;
  }
  EXPECT_EQ(drawn, 1);
}

// ------------------------------------------------------------ radio group --

struct Radios {
  Radios() {
    host = std::make_unique<WidgetHost>(std::make_unique<Widget>());
    group = &host->root().emplace<RadioGroup>(
        std::vector<std::string>{"Change speed", "Trim head", "Trim tail"}, 0u);
    group->set_on_change([this](std::size_t index) {
      ++changes;
      last = index;
    });
    host->resize(Rect{0.0, 0.0, 400.0, 200.0}, flat_context());
    group->arrange(Rect{0.0, 0.0, 200.0, 72.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  RadioGroup* group = nullptr;
  int changes = 0;
  std::size_t last = 0;
};

TEST(RadioGroup, OneIsAlwaysTaken) {
  // A group with nothing chosen is a question that was not asked properly.
  const RadioGroup fresh{{"a", "b"}};
  EXPECT_EQ(fresh.selected(), 0u);
}

TEST(RadioGroup, ClickingARowTakesIt) {
  Radios test;
  const Rect second = test.group->row_rect(1);
  test.host->mouse_down(press(8.0, second.y + 2.0));
  test.host->mouse_up(press(8.0, second.y + 2.0));

  EXPECT_EQ(test.group->selected(), 1u);
  EXPECT_EQ(test.changes, 1);
  EXPECT_EQ(test.last, 1u);
}

TEST(RadioGroup, ClickingTheLabelTakesItToo) {
  // The circle is fourteen pixels. The row is the target.
  Radios test;
  const Rect third = test.group->row_rect(2);
  test.host->mouse_down(press(150.0, third.y + 2.0));
  test.host->mouse_up(press(150.0, third.y + 2.0));
  EXPECT_EQ(test.group->selected(), 2u);
}

TEST(RadioGroup, ClickingWhatIsAlreadyTakenSaysNothingChanged) {
  Radios test;
  const Rect first = test.group->row_rect(0);
  test.host->mouse_down(press(8.0, first.y + 2.0));
  test.host->mouse_up(press(8.0, first.y + 2.0));

  EXPECT_EQ(test.group->selected(), 0u);
  EXPECT_EQ(test.changes, 0) << "a choice that was already made is not a change";
}

TEST(RadioGroup, SlidingOffBeforeReleasingCancels) {
  Radios test;
  const Rect second = test.group->row_rect(1);
  test.host->mouse_down(press(8.0, second.y + 2.0));
  test.host->mouse_move(press(900.0, second.y + 2.0));
  test.host->mouse_up(press(900.0, second.y + 2.0));

  EXPECT_EQ(test.group->selected(), 0u);
  EXPECT_EQ(test.changes, 0);
}

TEST(RadioGroup, TheArrowsMoveTheSelectionRatherThanAHighlight) {
  // What a radio group does everywhere on this platform, and the reason the
  // whole group is one tab stop rather than a row of them.
  Radios test;
  test.host->set_focus(test.group);

  test.host->key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.group->selected(), 1u);
  test.host->key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.group->selected(), 2u);
  test.host->key_down(KeyEvent{.key = Key::Up});
  EXPECT_EQ(test.group->selected(), 1u);
  EXPECT_EQ(test.changes, 3);
}

TEST(RadioGroup, TheArrowsStopAtEitherEndRatherThanWrapping) {
  Radios test;
  test.host->set_focus(test.group);

  test.host->key_down(KeyEvent{.key = Key::Up});
  EXPECT_EQ(test.group->selected(), 0u) << "already at the top";

  test.host->key_down(KeyEvent{.key = Key::End});
  EXPECT_EQ(test.group->selected(), 2u);
  test.host->key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.group->selected(), 2u) << "already at the bottom";
}

TEST(RadioGroup, HomeAndEndReachEitherEnd) {
  Radios test;
  test.host->set_focus(test.group);
  test.host->key_down(KeyEvent{.key = Key::End});
  EXPECT_EQ(test.group->selected(), 2u);
  test.host->key_down(KeyEvent{.key = Key::Home});
  EXPECT_EQ(test.group->selected(), 0u);
}

TEST(RadioGroup, ANewListDoesNotKeepTheOldIndex) {
  // The old index named a row in the old list, which is to say the wrong one.
  Radios test;
  test.host->set_focus(test.group);
  test.host->key_down(KeyEvent{.key = Key::End});
  ASSERT_EQ(test.group->selected(), 2u);

  test.group->set_options({"only", "two"});
  EXPECT_EQ(test.group->selected(), 0u);
}

TEST(RadioGroup, SelectingPastTheEndIsIgnoredRatherThanLeavingNothingTaken) {
  Radios test;
  test.group->select(99);
  EXPECT_EQ(test.group->selected(), 0u);
}

TEST(RadioGroup, TheRowsStackAndTheCirclesSitInThem) {
  Radios test;
  const Rect first = test.group->row_rect(0);
  const Rect second = test.group->row_rect(1);
  EXPECT_DOUBLE_EQ(second.y, first.y + first.height);

  const Rect circle = test.group->dot(1);
  EXPECT_GT(circle.width, 0.0);
  EXPECT_DOUBLE_EQ(circle.width, circle.height) << "a circle, not an oval";
  EXPECT_GE(circle.y, second.y);
  EXPECT_LE(circle.bottom(), second.bottom());
}

TEST(RadioGroup, APointOutsideAnyRowIsNoRow) {
  Radios test;
  EXPECT_EQ(test.group->row_at(1000.0), test.group->options().size());
  EXPECT_EQ(test.group->row_at(-10.0), test.group->options().size());
}

TEST(RadioGroup, AnEmptyGroupIsHarmless) {
  RadioGroup nothing;
  EXPECT_TRUE(nothing.options().empty());
  EXPECT_EQ(nothing.row_at(0.0), 0u);
}

// --------------------------------------------------------------- checkbox --

struct Ticked {
  Ticked() {
    host = std::make_unique<WidgetHost>(std::make_unique<Widget>());
    box = &host->root().emplace<Checkbox>("Reverse", false);
    box->set_on_change([this](bool on) {
      ++changes;
      last = on;
    });
    host->resize(Rect{0.0, 0.0, 400.0, 100.0}, flat_context());
    box->arrange(Rect{0.0, 0.0, 160.0, 24.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  Checkbox* box = nullptr;
  int changes = 0;
  bool last = false;
};

TEST(Checkbox, ClickingTogglesIt) {
  Ticked test;
  test.host->mouse_down(press(8.0, 12.0));
  test.host->mouse_up(press(8.0, 12.0));

  EXPECT_TRUE(test.box->checked());
  EXPECT_EQ(test.changes, 1);
  EXPECT_TRUE(test.last);
}

TEST(Checkbox, ClickingTheLabelTogglesItToo) {
  // A four-pixel target is not a target.
  Ticked test;
  test.host->mouse_down(press(100.0, 12.0));
  test.host->mouse_up(press(100.0, 12.0));
  EXPECT_TRUE(test.box->checked());
}

TEST(Checkbox, SlidingOffBeforeReleasingCancels) {
  Ticked test;
  test.host->mouse_down(press(8.0, 12.0));
  test.host->mouse_move(press(900.0, 12.0));
  test.host->mouse_up(press(900.0, 12.0));

  EXPECT_FALSE(test.box->checked());
  EXPECT_EQ(test.changes, 0);
}

TEST(Checkbox, SpaceTogglesItWhenFocused) {
  Ticked test;
  test.host->set_focus(test.box);
  test.host->key_down(KeyEvent{.key = Key::Space});
  EXPECT_TRUE(test.box->checked());
}

TEST(Checkbox, SettingItFromCodeDoesNotCallBack) {
  Ticked test;
  test.box->set_checked(true);
  EXPECT_EQ(test.changes, 0);
}

TEST(Checkbox, IsWideEnoughForItsLabel) {
  const LayoutContext context = flat_context();
  const Checkbox with("Reverse the clip");
  const Checkbox without;

  EXPECT_GT(with.sizing(Axis::Horizontal, context).basis,
            without.sizing(Axis::Horizontal, context).basis);
  EXPECT_DOUBLE_EQ(without.sizing(Axis::Horizontal, context).basis,
                   context.metrics().control_height * 0.6);
}

TEST(Checkbox, TheBoxIsSquareAndCentred) {
  const Ticked test;
  const Rect square = test.box->box();
  EXPECT_DOUBLE_EQ(square.width, square.height);
  EXPECT_NEAR(square.y + square.height / 2.0, 12.0, 1e-9);
}

TEST(Checkbox, TheTickIsDrawnOnlyWhenChecked) {
  Ticked test;
  RecordingPainter unchecked;
  test.box->paint(unchecked, default_theme());
  EXPECT_EQ(unchecked.count(DrawCall::Kind::Line), 0u);

  test.box->set_checked(true);
  RecordingPainter checked;
  test.box->paint(checked, default_theme());
  EXPECT_EQ(checked.count(DrawCall::Kind::Line), 2u) << "a tick is two strokes";
}

TEST(Checkbox, TheTickNeedsNoFont) {
  // Same reason as the caption buttons: there is no font that can be relied on
  // to have a tick in it.
  Ticked test;
  test.box->set_checked(true);
  test.box->set_label("");

  RecordingPainter painter;
  test.box->paint(painter, default_theme());
  EXPECT_EQ(painter.count(DrawCall::Kind::Text), 0u);
  EXPECT_GT(painter.count(DrawCall::Kind::Line), 0u);
}

TEST(Checkbox, TakesTheKeyboardButNotEveryShortcut) {
  Ticked test;
  EXPECT_TRUE(test.box->focusable());
  test.host->set_focus(test.box);

  EXPECT_FALSE(test.host->key_down(
      KeyEvent{.key = Key::Space, .modifiers = Modifiers{.control = true}}));
  EXPECT_FALSE(test.box->checked());
}

TEST(TextField, SaysItIsOneByBeingHovered) {
  // A field and a label are the same thing to look at until you have clicked
  // one, which is a poor way to find out.
  const TextField field("00:00:00:00");
  EXPECT_EQ(field.cursor_at(5.0, 5.0), Cursor::Text);
}

TEST(TextField, AColumnCountAsksForExactlyThatMuchWidth) {
  // A field in a toolbar has to leave room for the buttons beside it, and a
  // flexible one takes everything going.
  TextField wide("00:00:00:00");
  TextField narrow("00:00:00:00");
  narrow.set_columns(11);

  const LayoutContext context = flat_context();
  const LayoutItem loose = wide.sizing(Axis::Horizontal, context);
  const LayoutItem fixed = narrow.sizing(Axis::Horizontal, context);

  EXPECT_GT(loose.grow, 0.0) << "a plain field still takes what it is given";
  EXPECT_DOUBLE_EQ(fixed.grow, 0.0);
  EXPECT_GT(fixed.basis, 0.0);
}

TEST(TextField, AColumnedFieldDoesNotChangeWidthAsItIsTyped) {
  TextField field("00:00:00:00");
  field.set_columns(11);
  const LayoutContext context = flat_context();
  const double before = field.sizing(Axis::Horizontal, context).basis;

  field.set_text("1");
  EXPECT_DOUBLE_EQ(field.sizing(Axis::Horizontal, context).basis, before);
}

// ------------------------------------------------------------- menu lists --

/// A list on its own, arranged where it was asked for.
struct Listed {
  explicit Listed(std::vector<std::string> items = {"One", "Two", "Three"}) {
    auto owned = std::make_unique<MenuList>(std::move(items));
    list = owned.get();
    host = std::make_unique<WidgetHost>(std::move(owned));
    host->resize(Rect{0.0, 0.0, 200.0, 300.0}, flat_context());
  }

  MenuList* list = nullptr;
  std::unique_ptr<WidgetHost> host;
};

TEST(MenuList, RowsFollowEachOtherDownTheList) {
  const Listed test;
  const Rect first = test.list->row_rect(0);
  const Rect second = test.list->row_rect(1);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  EXPECT_DOUBLE_EQ(second.y - first.y, test.list->row_height());
  EXPECT_DOUBLE_EQ(first.x, second.x);
}

TEST(MenuList, APointFindsTheRowItIsIn) {
  const Listed test;
  const Rect second = test.list->row_rect(1);
  EXPECT_EQ(test.list->row_at(second.y + second.height * 0.5), 1u);
}

TEST(MenuList, AboveOrBelowEveryRowIsNoRow) {
  const Listed test;
  EXPECT_GE(test.list->row_at(-50.0), test.list->items().size());
  EXPECT_GE(test.list->row_at(10000.0), test.list->items().size());
}

TEST(MenuList, ChoosingReportsWhichRow) {
  Listed test;
  std::optional<std::size_t> chosen;
  test.list->set_on_choose([&](std::size_t index) { chosen = index; });

  const Rect row = test.list->row_rect(2);
  test.host->mouse_down(press(row.x + 5.0, row.y + row.height * 0.5));
  test.host->mouse_up(press(row.x + 5.0, row.y + row.height * 0.5));

  ASSERT_TRUE(chosen.has_value());
  EXPECT_EQ(*chosen, 2u);
}

TEST(MenuList, TicksAreDrawnOnlyBesideTheRowsThatCarryThem) {
  Listed test;
  RecordingPainter plain;
  test.list->paint(plain, default_theme());
  EXPECT_EQ(plain.count(DrawCall::Kind::Line), 0u);

  test.list->set_checked({true, false, true});
  RecordingPainter ticked;
  test.list->paint(ticked, default_theme());
  EXPECT_EQ(ticked.count(DrawCall::Kind::Line), 4u) << "two rows, two strokes each";
}

TEST(MenuList, TickingIndentsEveryLabelAlike) {
  // Including the rows with no tick — labels that stepped in and out depending
  // on whether their own row happened to be ticked would read as a jumble.
  const auto label_lefts = [](const RecordingPainter& painter) {
    std::vector<double> lefts;
    for (const DrawCall& call : painter.calls()) {
      if (call.kind == DrawCall::Kind::Text && call.run.has_value()) {
        lefts.push_back(call.run->bounds.x);
      }
    }
    return lefts;
  };

  Listed test;
  RecordingPainter plain;
  test.list->paint(plain, default_theme());
  const std::vector<double> before = label_lefts(plain);
  ASSERT_EQ(before.size(), 3u);

  test.list->set_checked({true, false, false});
  RecordingPainter ticked;
  test.list->paint(ticked, default_theme());

  const std::vector<double> after = label_lefts(ticked);
  ASSERT_EQ(after.size(), 3u);
  EXPECT_GT(after.front(), before.front());
  EXPECT_DOUBLE_EQ(after[1], after.front());
  EXPECT_DOUBLE_EQ(after[2], after.front());
}

TEST(MenuList, NewItemsArriveWithoutTheOldTicks) {
  // Ticks are held by position, so keeping them across a change of items would
  // tick whatever happened to land on those rows.
  Listed test;
  test.list->set_checked({true, true, true});
  test.list->set_items({"Other", "Items"});
  EXPECT_TRUE(test.list->checked().empty());
}

TEST(MenuList, ReleasingOutsideEveryRowChoosesNothing) {
  Listed test;
  bool chosen = false;
  test.list->set_on_choose([&](std::size_t) { chosen = true; });

  test.host->mouse_down(press(10.0, 5.0));
  test.host->mouse_up(press(10.0, 1000.0));
  EXPECT_FALSE(chosen);
}

TEST(MenuList, TheKeyboardWrapsAtBothEnds) {
  Listed test;
  test.list->on_key_down(KeyEvent{.key = Key::Up});
  EXPECT_EQ(test.list->highlighted(), 2u) << "up from nothing should reach the last row";

  test.list->on_key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.list->highlighted(), 0u) << "down from the last row should wrap to the first";
}

// --------------------------------------------------------------- dropdown --

struct Chosen {
  Chosen() {
    auto root = std::make_unique<Box>(Axis::Vertical);
    box = &root->emplace<Dropdown>(std::vector<std::string>{"H.264", "HEVC"}, 0u);
    host = std::make_unique<WidgetHost>(std::move(root));
    host->resize(Rect{0.0, 0.0, 400.0, 300.0}, flat_context());
  }

  Dropdown* box = nullptr;
  std::unique_ptr<WidgetHost> host;
};

TEST(Dropdown, ShowsTheOptionItIsOn) {
  Chosen test;
  EXPECT_EQ(test.box->value(), "H.264");
  test.box->set_selected(1);
  EXPECT_EQ(test.box->value(), "HEVC");
}

TEST(Dropdown, AnOutOfRangeSelectionIsRefusedRatherThanRead) {
  Chosen test;
  test.box->set_selected(99);
  EXPECT_EQ(test.box->selected(), 0u) << "it should have kept the one it had";
  EXPECT_EQ(test.box->value(), "H.264");
}

TEST(Dropdown, PressingItOpensAListOnThePopupLayer) {
  Chosen test;
  EXPECT_FALSE(test.host->popup_open());

  const Rect area = test.box->bounds();
  test.host->mouse_down(press(area.x + 5.0, area.y + area.height * 0.5));

  EXPECT_TRUE(test.box->is_open());
  ASSERT_TRUE(test.host->popup_open());
  EXPECT_NE(dynamic_cast<MenuList*>(test.host->popup()), nullptr);
}

TEST(Dropdown, TheListIsPlacedUnderTheControl) {
  Chosen test;
  const Rect area = test.box->bounds();
  test.host->mouse_down(press(area.x + 5.0, area.y + area.height * 0.5));
  // Placed at the next layout, because opening happens in a click handler and
  // that has no theme to measure with.
  test.host->update_layout(flat_context());

  ASSERT_TRUE(test.host->popup_open());
  const Rect list = test.host->popup()->bounds();
  EXPECT_DOUBLE_EQ(list.y, area.bottom());
  EXPECT_GE(list.width, area.width) << "a list should be at least as wide as its control";
}

TEST(Dropdown, ChoosingFromTheListChangesTheValueAndClosesIt) {
  Chosen test;
  std::optional<std::size_t> reported;
  test.box->set_on_change([&](std::size_t index) { reported = index; });

  const Rect area = test.box->bounds();
  // Down *and* up: a press captures the control it lands on, and leaving the
  // button held would send the next press back to the dropdown rather than to
  // the list it opened.
  test.host->mouse_down(press(area.x + 5.0, area.y + area.height * 0.5));
  test.host->mouse_up(press(area.x + 5.0, area.y + area.height * 0.5));
  test.host->update_layout(flat_context());

  auto* list = dynamic_cast<MenuList*>(test.host->popup());
  ASSERT_NE(list, nullptr);
  const Rect row = list->row_rect(1);
  test.host->mouse_down(press(row.x + 5.0, row.y + row.height * 0.5));
  test.host->mouse_up(press(row.x + 5.0, row.y + row.height * 0.5));

  EXPECT_EQ(test.box->selected(), 1u);
  EXPECT_EQ(test.box->value(), "HEVC");
  ASSERT_TRUE(reported.has_value());
  EXPECT_EQ(*reported, 1u);
  EXPECT_FALSE(test.host->popup_open()) << "choosing should have closed the list";
}

TEST(Dropdown, TheArrowNeedsNoFont) {
  Chosen test;
  RecordingPainter painter;
  test.box->paint(painter, default_theme());
  // Lines, like the tick and the caption buttons, because no font can be
  // relied on to have an arrow in it.
  EXPECT_GE(painter.count(DrawCall::Kind::Line), 2u);
}

TEST(Dropdown, TheKeyboardStepsWithoutOpening) {
  Chosen test;
  test.box->on_key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.box->selected(), 1u);
  EXPECT_FALSE(test.box->is_open());

  // And stops rather than wrapping: a dropdown is a value, and stepping past
  // the last option should not land back on the first.
  test.box->on_key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.box->selected(), 1u);
}

// -------------------------------------------------------------- text field --

/// A field in a host, laid out and focused, which is the state every one of
/// these starts from: a caret only means anything once the keyboard is there.
struct Typed {
  explicit Typed(std::string text = {}, bool multiline = false) {
    auto owned = std::make_unique<TextField>(std::move(text));
    field = owned.get();
    field->set_multiline(multiline);
    host = std::make_unique<WidgetHost>(std::move(owned));
    host->resize(Rect{0.0, 0.0, 300.0, 80.0}, flat_context());
    host->set_focus(field);
  }

  /// Types a run of characters, as the window would.
  void type(std::string_view text) {
    for (const char c : text) host->text(static_cast<char32_t>(c));
  }

  void press(Key key, Modifiers modifiers = {}) {
    host->key_down(KeyEvent{.key = key, .modifiers = modifiers});
  }

  /// Lays out again, which is what the frame loop does before painting and what
  /// rebuilds the offset table a click reads.
  void settle() { host->update_layout(flat_context()); }

  TextField* field = nullptr;
  std::unique_ptr<WidgetHost> host;
};

TEST(TextField, TypingInsertsAtTheCaret) {
  Typed test;
  test.type("abc");
  EXPECT_EQ(test.field->text(), "abc");
  EXPECT_EQ(test.field->caret(), 3u);

  test.press(Key::Left);
  test.type("X");
  EXPECT_EQ(test.field->text(), "abXc");
}

TEST(TextField, ControlCharactersAreNotText) {
  // A tab and a newline arrive as keys. Taking them as text would put a
  // control character into the string nobody could see or remove.
  Typed test;
  test.host->text(U'\t');
  test.host->text(U'\n');
  EXPECT_TRUE(test.field->text().empty());
}

TEST(TextField, BackspaceRemovesTheCharacterBeforeTheCaret) {
  Typed test("abc");
  test.press(Key::Backspace);
  EXPECT_EQ(test.field->text(), "ab");
  EXPECT_EQ(test.field->caret(), 2u);
}

TEST(TextField, DeleteRemovesTheCharacterAfterIt) {
  Typed test("abc");
  test.field->set_caret(0);
  test.press(Key::Delete);
  EXPECT_EQ(test.field->text(), "bc");
  EXPECT_EQ(test.field->caret(), 0u);
}

TEST(TextField, BackspaceAtTheStartAndDeleteAtTheEndDoNothing) {
  Typed test("ab");
  test.field->set_caret(0);
  test.press(Key::Backspace);
  EXPECT_EQ(test.field->text(), "ab");

  test.field->set_caret(2);
  test.press(Key::Delete);
  EXPECT_EQ(test.field->text(), "ab");
}

TEST(TextField, AMultiByteCharacterIsEditedWhole) {
  // "é" is two bytes. A caret that moved by one would split it, and the field
  // would hold a string no longer valid UTF-8.
  Typed test;
  test.host->text(U'é');
  EXPECT_EQ(test.field->text().size(), 2u);
  EXPECT_EQ(test.field->caret(), 2u);

  test.press(Key::Left);
  EXPECT_EQ(test.field->caret(), 0u) << "one press should clear the whole character";

  test.press(Key::Delete);
  EXPECT_TRUE(test.field->text().empty());
}

TEST(TextField, ShiftAndAnArrowExtendTheSelection) {
  Typed test("abcd");
  test.field->set_caret(4);
  EXPECT_FALSE(test.field->has_selection());

  test.press(Key::Left, Modifiers{.shift = true});
  test.press(Key::Left, Modifiers{.shift = true});

  EXPECT_TRUE(test.field->has_selection());
  EXPECT_EQ(test.field->selection_begin(), 2u);
  EXPECT_EQ(test.field->selection_end(), 4u);
}

TEST(TextField, TypingReplacesTheSelection) {
  Typed test("abcd");
  test.field->select_all();
  test.type("X");
  EXPECT_EQ(test.field->text(), "X");
  EXPECT_FALSE(test.field->has_selection());
}

TEST(TextField, BackspaceDeletesTheSelectionRatherThanOneCharacter) {
  Typed test("abcd");
  test.field->select_all();
  test.press(Key::Backspace);
  EXPECT_TRUE(test.field->text().empty());
}

TEST(TextField, ControlAndASelectsEverything) {
  Typed test("abcd");
  test.press(Key::A, Modifiers{.control = true});
  EXPECT_EQ(test.field->selection_begin(), 0u);
  EXPECT_EQ(test.field->selection_end(), 4u);
}

TEST(TextField, AnArrowWithASelectionCollapsesItRatherThanMoving) {
  Typed test("abcd");
  test.field->select_all();
  test.press(Key::Left);
  EXPECT_FALSE(test.field->has_selection());
  EXPECT_EQ(test.field->caret(), 0u) << "left should land at the start of the selection";

  test.field->select_all();
  test.press(Key::Right);
  EXPECT_EQ(test.field->caret(), 4u) << "and right at its end";
}

TEST(TextField, HomeAndEndReachBothEnds) {
  Typed test("abcd");
  test.field->set_caret(2);
  test.press(Key::Home);
  EXPECT_EQ(test.field->caret(), 0u);
  test.press(Key::End);
  EXPECT_EQ(test.field->caret(), 4u);
}

TEST(TextField, ClickingPutsTheCaretUnderThePointer) {
  Typed test("abcdefgh");
  test.settle();

  // Past the right-hand end of the text, which is the end of it.
  const Rect area = test.field->bounds();
  test.host->mouse_down(press(area.right() - 2.0, area.y + area.height * 0.5));
  EXPECT_EQ(test.field->caret(), 8u);

  test.host->mouse_down(press(area.x + 1.0, area.y + area.height * 0.5));
  EXPECT_EQ(test.field->caret(), 0u);
}

TEST(TextField, DraggingSelects) {
  Typed test("abcdefgh");
  test.settle();

  const Rect area = test.field->bounds();
  const double y = area.y + area.height * 0.5;
  test.host->mouse_down(press(area.x + 1.0, y));
  test.host->mouse_move(MouseEvent{.x = area.right() - 2.0, .y = y});
  test.host->mouse_up(press(area.right() - 2.0, y));

  EXPECT_EQ(test.field->selection_begin(), 0u);
  EXPECT_EQ(test.field->selection_end(), 8u);
}

TEST(TextField, ADoubleClickTakesTheWholeValue) {
  Typed test("abcd");
  test.settle();

  const Rect area = test.field->bounds();
  test.host->mouse_down(press(area.x + 2.0, area.y + area.height * 0.5, 2));
  EXPECT_EQ(test.field->selection_begin(), 0u);
  EXPECT_EQ(test.field->selection_end(), 4u);
}

TEST(TextField, ChangeFiresOnEveryEditAndCommitDoesNot) {
  Typed test;
  int changes = 0;
  int commits = 0;
  test.field->set_on_change([&](const std::string&) { ++changes; });
  test.field->set_on_commit([&](const std::string&) { ++commits; });

  test.type("abc");
  EXPECT_EQ(changes, 3);
  EXPECT_EQ(commits, 0) << "a document must not collect an undo entry per letter";

  test.press(Key::Enter);
  EXPECT_EQ(commits, 1);
}

TEST(TextField, TheKeyboardLeavingCommits) {
  Typed test;
  std::string committed;
  test.field->set_on_commit([&](const std::string& value) { committed = value; });

  test.type("done");
  test.host->set_focus(nullptr);
  EXPECT_EQ(committed, "done");
}

TEST(TextField, ClickingAwayFromAFieldCommitsIt) {
  // A multiline field takes Enter as a line break, so clicking away is the only
  // way to be done with one — and focus does not otherwise follow a press onto
  // something that cannot take it.
  auto root = std::make_unique<Box>(Axis::Vertical);
  auto& field = root->emplace<TextField>();
  field.set_multiline(true);
  auto& elsewhere = root->emplace<Label>("not focusable");

  WidgetHost host(std::move(root));
  host.resize(Rect{0.0, 0.0, 200.0, 120.0}, flat_context());
  host.set_focus(&field);

  std::string committed;
  field.set_on_commit([&](const std::string& value) { committed = value; });
  host.text(U'h');
  host.text(U'i');

  const Rect away = elsewhere.bounds();
  host.mouse_down(press(away.x + 2.0, away.y + away.height * 0.5));

  EXPECT_EQ(committed, "hi");
  EXPECT_EQ(host.focused(), nullptr);
}

TEST(TextField, CommittingTwiceWithoutAChangeReportsOnce) {
  Typed test("same");
  int commits = 0;
  test.field->set_on_commit([&](const std::string&) { ++commits; });

  test.press(Key::Enter);
  test.press(Key::Enter);
  EXPECT_EQ(commits, 0) << "nothing was edited";

  test.type("r");
  test.press(Key::Enter);
  test.press(Key::Enter);
  EXPECT_EQ(commits, 1);
}

TEST(TextField, EscapePutsBackWhatWasThere) {
  Typed test("before");
  test.host->set_focus(test.field);  // the value is remembered on focus
  test.type("XYZ");
  ASSERT_NE(test.field->text(), "before");

  test.press(Key::Escape);
  EXPECT_EQ(test.field->text(), "before");
}

TEST(TextField, EnterBreaksTheLineOnlyWhenAskedTo) {
  Typed single("ab");
  single.press(Key::Enter);
  EXPECT_EQ(single.field->text(), "ab") << "a single-line field commits instead";

  Typed many("ab", true);
  many.press(Key::Enter);
  many.type("cd");
  EXPECT_EQ(many.field->text(), "ab\ncd");
}

TEST(TextField, UpAndDownMoveBetweenLinesKeepingTheColumn) {
  Typed test("abcd\nefgh", true);
  test.settle();

  test.field->set_caret(2);  // between b and c on the first line
  test.press(Key::Down);
  EXPECT_EQ(test.field->caret(), 7u) << "the same column on the second line";

  test.press(Key::Up);
  EXPECT_EQ(test.field->caret(), 2u) << "and back again";
}

TEST(TextField, HomeAndEndAreTheLinesEndsWhenThereAreLines) {
  Typed test("abcd\nefgh", true);
  test.settle();

  test.field->set_caret(7);
  test.press(Key::Home);
  EXPECT_EQ(test.field->caret(), 5u) << "the start of the second line, not of the text";
  test.press(Key::End);
  EXPECT_EQ(test.field->caret(), 9u);
}

TEST(TextField, ASingleLineFieldIgnoresTheLinesItWasGiven) {
  // Setting a value with a break in it must not make a one-line field two
  // lines tall, and Down must not wander into a line it does not draw.
  Typed test("ab\ncd");
  test.settle();
  test.field->set_caret(1);
  test.press(Key::Down);
  EXPECT_EQ(test.field->caret(), 1u);
}

TEST(TextField, ClickingTheSecondLinePutsTheCaretThere) {
  Typed test("abcd\nefgh", true);
  test.field->set_min_lines(2);
  test.host->resize(Rect{0.0, 0.0, 300.0, 80.0}, flat_context());
  test.settle();

  const Rect first = test.field->caret_rect(0);
  const Rect second = test.field->caret_rect(6);
  ASSERT_GT(second.y, first.y) << "the second line should be below the first";

  const std::size_t index = test.field->index_at(test.field->bounds().x + 1.0, second.y + 2.0);
  EXPECT_GE(index, 5u);
  EXPECT_LE(index, 9u);
}

TEST(TextField, SettingTheTextKeepsTheCaretInsideIt) {
  Typed test("a long value");
  test.field->set_caret(10);
  test.field->set_text("no");
  EXPECT_LE(test.field->caret(), 2u);
  EXPECT_FALSE(test.field->has_selection());
}

TEST(TextField, APlaceholderShowsOnlyWhileItIsEmpty) {
  Typed test;
  test.field->set_placeholder("Title text");
  test.settle();

  RecordingPainter empty;
  test.field->paint(empty, default_theme());
  EXPECT_EQ(empty.count(DrawCall::Kind::Text), 1u);

  test.type("x");
  test.settle();

  RecordingPainter filled;
  test.field->paint(filled, default_theme());
  ASSERT_EQ(filled.count(DrawCall::Kind::Text), 1u);

  // The last *text* call, not the last call: the caret is drawn after it.
  const auto drawn = std::ranges::find_if(filled.calls(), [](const DrawCall& call) {
    return call.kind == DrawCall::Kind::Text;
  });
  ASSERT_NE(drawn, filled.calls().end());
  ASSERT_TRUE(drawn->run.has_value());
  EXPECT_EQ(drawn->run->text, "x") << "the placeholder should be gone";
}

TEST(TextField, TheCaretIsDrawnOnlyWhenFocused) {
  Typed test("ab");
  test.settle();

  RecordingPainter focused;
  test.field->paint(focused, default_theme());
  const std::size_t with_caret = focused.count(DrawCall::Kind::Fill);

  test.host->set_focus(nullptr);
  RecordingPainter blurred;
  test.field->paint(blurred, default_theme());
  EXPECT_LT(blurred.count(DrawCall::Kind::Fill), with_caret);
}

TEST(TextField, ASelectionIsDrawnBehindTheText) {
  Typed test("abcd");
  test.settle();
  test.field->select_all();

  RecordingPainter painter;
  test.field->paint(painter, default_theme());

  // The wash, the caret, and the field's own surface.
  EXPECT_GE(painter.count(DrawCall::Kind::Fill), 2u);
  bool text_after_wash = false;
  bool seen_fill = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Fill) seen_fill = true;
    if (call.kind == DrawCall::Kind::Text && seen_fill) text_after_wash = true;
  }
  EXPECT_TRUE(text_after_wash) << "a selection must not be drawn over its own text";
}

// ------------------------------------------------------------ icon button --

/// The lines an icon button drew.
[[nodiscard]] std::vector<DrawCall> lines_of(IconButton::Icon icon) {
  IconButton button(icon);
  button.arrange(Rect{0.0, 0.0, 24.0, 24.0}, flat_context());

  RecordingPainter painter;
  button.paint(painter, default_theme());

  std::vector<DrawCall> lines;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Line) lines.push_back(call);
  }
  return lines;
}

TEST(IconButton, DrawsItsMarkWithoutAFont) {
  // Lines and strokes, like the tick and the dropdown's arrow: no font can be
  // relied on to have an arrow in it, and the ones that do disagree about its
  // size and baseline.
  for (const IconButton::Icon icon :
       {IconButton::Icon::ArrowUp, IconButton::Icon::ArrowDown, IconButton::Icon::Cross,
        IconButton::Icon::Plus, IconButton::Icon::Stopwatch, IconButton::Icon::Diamond,
        IconButton::Icon::Pointer, IconButton::Icon::Razor, IconButton::Icon::RateStretch,
        IconButton::Icon::Slip, IconButton::Icon::Slide}) {
    IconButton button(icon);
    button.arrange(Rect{0.0, 0.0, 24.0, 24.0}, flat_context());

    RecordingPainter painter;
    button.paint(painter, default_theme());
    EXPECT_GE(painter.count(DrawCall::Kind::Line) + painter.count(DrawCall::Kind::Stroke), 2u)
        << "icon " << static_cast<int>(icon);
    EXPECT_EQ(painter.count(DrawCall::Kind::Text), 0u)
        << "icon " << static_cast<int>(icon) << " should need no font";
  }
}

TEST(IconButton, NoTwoIconsAreDrawnAlike) {
  // The palette is five buttons in a row and the only thing telling them apart
  // is the mark. Two that drew the same would be a tool nobody could find.
  const auto shape = [](IconButton::Icon icon) {
    IconButton button(icon);
    button.arrange(Rect{0.0, 0.0, 24.0, 24.0}, flat_context());
    RecordingPainter painter;
    button.paint(painter, default_theme());

    std::vector<Rect> marks;
    for (const DrawCall& call : painter.calls()) {
      if (call.kind == DrawCall::Kind::Line || call.kind == DrawCall::Kind::Stroke ||
          call.kind == DrawCall::Kind::Fill) {
        marks.push_back(call.bounds);
      }
    }
    return marks;
  };

  constexpr std::array kIcons{IconButton::Icon::Pointer,   IconButton::Icon::Razor,
                              IconButton::Icon::RateStretch, IconButton::Icon::Slip,
                              IconButton::Icon::Slide,       IconButton::Icon::Reset,
                              IconButton::Icon::Stopwatch,   IconButton::Icon::Diamond};
  for (std::size_t i = 0; i < kIcons.size(); ++i) {
    for (std::size_t j = i + 1; j < kIcons.size(); ++j) {
      EXPECT_NE(shape(kIcons[i]), shape(kIcons[j]))
          << "icons " << static_cast<int>(kIcons[i]) << " and "
          << static_cast<int>(kIcons[j]);
    }
  }
}

TEST(IconButton, TheArrowsPointOppositeWays) {
  // An up arrow's apex is above where its strokes begin and a down arrow's is
  // below. Without this the two could be drawn identically and only a person
  // looking at the screen would ever know.
  const std::vector<DrawCall> up = lines_of(IconButton::Icon::ArrowUp);
  const std::vector<DrawCall> down = lines_of(IconButton::Icon::ArrowDown);
  ASSERT_EQ(up.size(), 2u);
  ASSERT_EQ(down.size(), 2u);

  // A line is stored as its first point plus an offset to the second, so the
  // sign of the offset is which way the stroke runs.
  EXPECT_LT(up.front().bounds.height, 0.0) << "the first stroke should rise";
  EXPECT_GT(down.front().bounds.height, 0.0) << "the first stroke should fall";
}

TEST(IconButton, AToggleShowsItsStateInTheMarkItself) {
  // No theme defines a selected state for a tool button, so the surface
  // underneath cannot be relied on to say anything. A stopwatch that looks the
  // same running as stopped is worse than no stopwatch at all.
  const auto marks = [](IconButton::Icon icon, bool on) {
    IconButton button(icon);
    button.set_selected(on);
    button.arrange(Rect{0.0, 0.0, 24.0, 24.0}, flat_context());

    RecordingPainter painter;
    button.paint(painter, default_theme());
    return painter.count(DrawCall::Kind::Fill);
  };

  EXPECT_GT(marks(IconButton::Icon::Stopwatch, true), marks(IconButton::Icon::Stopwatch, false));
  EXPECT_GT(marks(IconButton::Icon::Diamond, true), marks(IconButton::Icon::Diamond, false));
}

TEST(IconButton, IsSquareWhateverTheTheme) {
  const IconButton button(IconButton::Icon::Cross);
  const LayoutContext context = flat_context();
  EXPECT_DOUBLE_EQ(button.sizing(Axis::Horizontal, context).basis,
                   button.sizing(Axis::Vertical, context).basis);
}

TEST(IconButton, ANarrowOneKeepsItsHeightAndGivesUpWidth) {
  // Premiere's keyframe navigator is three small arrows in about the room one
  // control takes. Three square ones on a parameter row, beside a stopwatch, a
  // triangle and two numbers, left no space for the property's own name.
  IconButton button(IconButton::Icon::ArrowLeft);
  const LayoutContext context = flat_context();
  const double square = button.sizing(Axis::Horizontal, context).basis;

  button.set_narrow(true);
  EXPECT_LT(button.sizing(Axis::Horizontal, context).basis, square);
  EXPECT_DOUBLE_EQ(button.sizing(Axis::Vertical, context).basis,
                   IconButton(IconButton::Icon::ArrowLeft).sizing(Axis::Vertical, context).basis);
}

TEST(IconButton, ClicksLikeAnyOtherButton) {
  int clicks = 0;
  auto owned = std::make_unique<IconButton>(IconButton::Icon::Cross, [&] { ++clicks; });
  IconButton* button = owned.get();

  WidgetHost host(std::move(owned));
  host.resize(Rect{0.0, 0.0, 100.0, 40.0}, flat_context());

  const Rect area = button->bounds();
  const double x = area.x + area.width * 0.5;
  const double y = area.y + area.height * 0.5;
  host.mouse_down(press(x, y));
  host.mouse_up(press(x, y));
  EXPECT_EQ(clicks, 1);

  // And a press that slides off is cancelled, which is `Button`'s behaviour and
  // the reason this is one rather than a widget of its own.
  host.mouse_down(press(x, y));
  host.mouse_up(press(x + 500.0, y));
  EXPECT_EQ(clicks, 1);
}

// ----------------------------------------------------------- progress bar --

TEST(ProgressBar, FillsInProportion) {
  ProgressBar bar;
  bar.arrange(Rect{0.0, 0.0, 200.0, 20.0}, flat_context());

  bar.set_fraction(0.25);
  EXPECT_DOUBLE_EQ(bar.filled().width, 50.0);
  EXPECT_DOUBLE_EQ(bar.filled().height, 20.0);
}

TEST(ProgressBar, NonsenseIsClampedRatherThanDrawn) {
  ProgressBar bar;
  bar.arrange(Rect{0.0, 0.0, 200.0, 20.0}, flat_context());

  bar.set_fraction(-1.0);
  EXPECT_DOUBLE_EQ(bar.fraction(), 0.0);
  EXPECT_TRUE(bar.filled().empty());

  bar.set_fraction(4.0);
  EXPECT_DOUBLE_EQ(bar.fraction(), 1.0);
  EXPECT_DOUBLE_EQ(bar.filled().width, 200.0);
}

TEST(ProgressBar, NothingDoneDrawsNoFill) {
  ProgressBar bar;
  bar.arrange(Rect{0.0, 0.0, 200.0, 20.0}, flat_context());

  RecordingPainter painter;
  bar.paint(painter, default_theme());
  // The groove is the widget's own surface; an empty bar must not draw a
  // zero-width fill over it as well.
  EXPECT_EQ(painter.count(DrawCall::Kind::Fill), 1u);
}

}  // namespace
}  // namespace cutline::ui
