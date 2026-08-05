/// The time strip under a monitor.
///
/// Almost all of it is a mapping between seconds and pixels, which is exactly
/// the kind of thing that is maddening to check by looking and trivial to
/// assert — a playhead a pixel out, or a marked span that starts where it ends.
/// The rest is one gesture: a press anywhere goes there and carries on
/// dragging.

#include "cutline/ui/scrub_bar.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() {
  return LayoutContext{default_theme(), measurer()};
}

struct Fixture {
  Fixture() {
    host = std::make_unique<WidgetHost>(std::make_unique<ScrubBar>());
    bar = static_cast<ScrubBar*>(&host->root());
    host->resize(Rect{0.0, 0.0, 400.0, 30.0}, flat_context());
    bar->set_duration(10.0);
    bar->on_scrub = [this](double time) { scrubbed.push_back(time); };
  }

  [[nodiscard]] std::vector<DrawCall> painted() {
    RecordingPainter painter;
    bar->paint(painter, default_theme());
    return painter.calls();
  }

  void press(double x) {
    host->mouse_down(MouseEvent{.x = x, .y = 15.0});
  }
  void move(double x) { host->mouse_move(MouseEvent{.x = x, .y = 15.0}); }
  void release(double x) { host->mouse_up(MouseEvent{.x = x, .y = 15.0}); }

  std::unique_ptr<WidgetHost> host;
  ScrubBar* bar = nullptr;
  std::vector<double> scrubbed;
};

}  // namespace

// ---------------------------------------------------------------- mapping --

TEST(ScrubBar, TheWholeWidthIsTheWholeDuration) {
  // The property the widget exists for: no zoom and no scroll, so the left
  // edge is always the start and the right edge always the end.
  const Fixture fixture;
  const Rect area = fixture.bar->track_area();

  EXPECT_NEAR(fixture.bar->x_of(0.0), area.x, 0.001);
  EXPECT_NEAR(fixture.bar->x_of(10.0), area.right(), 0.001);
  EXPECT_NEAR(fixture.bar->x_of(5.0), area.x + area.width * 0.5, 0.001);
}

TEST(ScrubBar, TimeAndPositionAgreeBothWays) {
  const Fixture fixture;
  for (const double time : {0.0, 2.5, 7.25, 10.0}) {
    EXPECT_NEAR(fixture.bar->time_at(fixture.bar->x_of(time)), time, 0.001);
  }
}

TEST(ScrubBar, APositionOffTheEndIsClampedRatherThanExtrapolated) {
  // A drag that leaves the widget still lands on a real time.
  const Fixture fixture;
  EXPECT_DOUBLE_EQ(fixture.bar->time_at(-500.0), 0.0);
  EXPECT_DOUBLE_EQ(fixture.bar->time_at(5000.0), 10.0);
}

TEST(ScrubBar, TheStripSitsInsideThePadding) {
  const Fixture fixture;
  const Rect area = fixture.bar->track_area();
  const Rect box = fixture.bar->bounds();

  EXPECT_FALSE(area.empty());
  EXPECT_GT(area.x, box.x);
  EXPECT_LT(area.right(), box.right());
}

// ------------------------------------------------------------------ marks --

TEST(ScrubBar, NothingMarkedIsNoSpan) {
  const Fixture fixture;
  EXPECT_TRUE(fixture.bar->marked_area().empty());
}

TEST(ScrubBar, OneEndMarkedRunsToTheEndOfTheSource) {
  // The same rule the marks themselves keep, and the reason it is worth
  // asserting here too: a bar that showed nothing until both ends were marked
  // would make marking an in look as though it had failed.
  Fixture fixture;
  fixture.bar->set_marks(6.0, std::nullopt);

  const Rect span = fixture.bar->marked_area();
  ASSERT_FALSE(span.empty());
  EXPECT_NEAR(span.x, fixture.bar->x_of(6.0), 0.001);
  EXPECT_NEAR(span.right(), fixture.bar->track_area().right(), 0.001);
}

TEST(ScrubBar, AnOutAloneRunsFromTheStart) {
  Fixture fixture;
  fixture.bar->set_marks(std::nullopt, 4.0);

  const Rect span = fixture.bar->marked_area();
  ASSERT_FALSE(span.empty());
  EXPECT_NEAR(span.x, fixture.bar->track_area().x, 0.001);
  EXPECT_NEAR(span.right(), fixture.bar->x_of(4.0), 0.001);
}

TEST(ScrubBar, MarksPastTheSourceAreDrawnInsideIt) {
  Fixture fixture;
  fixture.bar->set_marks(-3.0, 99.0);

  const Rect span = fixture.bar->marked_area();
  const Rect area = fixture.bar->track_area();
  EXPECT_NEAR(span.x, area.x, 0.001);
  EXPECT_NEAR(span.right(), area.right(), 0.001);
}

// -------------------------------------------------------------- playhead --

TEST(ScrubBar, ThePlayheadIsClampedIntoTheDuration) {
  Fixture fixture;
  fixture.bar->set_playhead(99.0);
  EXPECT_DOUBLE_EQ(fixture.bar->playhead(), 10.0);
  fixture.bar->set_playhead(-1.0);
  EXPECT_DOUBLE_EQ(fixture.bar->playhead(), 0.0);
}

TEST(ScrubBar, AShorterSourceBringsThePlayheadBackWithIt) {
  // Otherwise opening a short source after a long one leaves the playhead off
  // the right-hand end, pointing past a file that stopped before it.
  Fixture fixture;
  fixture.bar->set_playhead(9.0);
  fixture.bar->set_duration(4.0);
  EXPECT_DOUBLE_EQ(fixture.bar->playhead(), 4.0);
}

// -------------------------------------------------------------- scrubbing --

TEST(ScrubBar, APressGoesToWhereItLanded) {
  Fixture fixture;
  fixture.press(fixture.bar->x_of(3.0));

  ASSERT_EQ(fixture.scrubbed.size(), 1u);
  EXPECT_NEAR(fixture.scrubbed[0], 3.0, 0.001);
}

TEST(ScrubBar, ThePressCarriesOnAsADrag) {
  Fixture fixture;
  fixture.press(fixture.bar->x_of(1.0));
  fixture.move(fixture.bar->x_of(5.0));
  fixture.move(fixture.bar->x_of(8.0));

  ASSERT_EQ(fixture.scrubbed.size(), 3u);
  EXPECT_NEAR(fixture.scrubbed.back(), 8.0, 0.001);
  EXPECT_TRUE(fixture.bar->scrubbing());

  fixture.release(fixture.bar->x_of(8.0));
  EXPECT_FALSE(fixture.bar->scrubbing());
}

TEST(ScrubBar, MovingWithoutPressingScrubsNothing) {
  Fixture fixture;
  fixture.move(fixture.bar->x_of(5.0));
  EXPECT_TRUE(fixture.scrubbed.empty());
}

TEST(ScrubBar, ADragPastTheEdgeStillReportsARealTime) {
  // The capture keeps the events coming after the pointer has left the widget,
  // which is the whole point of capture and the case a clamp is for.
  Fixture fixture;
  fixture.press(fixture.bar->x_of(5.0));
  fixture.move(-200.0);
  EXPECT_DOUBLE_EQ(fixture.scrubbed.back(), 0.0);
  fixture.move(9000.0);
  EXPECT_DOUBLE_EQ(fixture.scrubbed.back(), 10.0);
}

TEST(ScrubBar, WithNothingLoadedThereIsNothingToScrub) {
  Fixture fixture;
  fixture.bar->set_duration(0.0);
  fixture.press(fixture.bar->x_of(0.0));
  EXPECT_TRUE(fixture.scrubbed.empty());
  EXPECT_FALSE(fixture.bar->scrubbing());
}

// ----------------------------------------------------------------- paint --

TEST(ScrubBar, TheStripIsDrawnEvenWithNoSource) {
  // A monitor with nothing open still has the strip, or the panel changes
  // shape the moment a source arrives.
  Fixture fixture;
  fixture.bar->set_duration(0.0);
  EXPECT_FALSE(fixture.painted().empty());
}

TEST(ScrubBar, TheMarkedSpanIsDrawnWhenThereIsOne) {
  Fixture fixture;
  const std::size_t plain = fixture.painted().size();
  fixture.bar->set_marks(2.0, 6.0);
  EXPECT_GT(fixture.painted().size(), plain);
}

}  // namespace cutline::ui
