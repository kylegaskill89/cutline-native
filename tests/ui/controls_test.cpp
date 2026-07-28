/// The controls that edit a value.
///
/// `ValueRange` gets the most attention because it is the piece that is easy to
/// get subtly wrong: a slider that should stop at 5 stopping at 4.8 is not
/// something anyone notices until it matters.

#include "cutline/ui/controls.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <gtest/gtest.h>

#include <memory>
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

}  // namespace
}  // namespace cutline::ui
