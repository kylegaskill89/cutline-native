/// The keyframe timeline.
///
/// Most of what is here is geometry, because that is what the view is: a map
/// from a time to an x and back. A keyframe drawn a few pixels off its time is
/// maddening to find by looking and trivial to state as an assertion.

#include "cutline/ui/keyframe_view.hpp"

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

[[nodiscard]] KeyframeView::Model two_lanes() {
  return KeyframeView::Model{
      .duration = 10.0,
      .lanes = {KeyframeView::Lane{.name = "Position X", .times = {0.0, 5.0, 10.0}},
                KeyframeView::Lane{.name = "Opacity", .times = {2.5}}}};
}

/// A view in a host, laid out wide enough that a second is several pixels.
struct Laid {
  Laid() {
    host = std::make_unique<WidgetHost>(std::make_unique<Widget>());
    view = &host->root().emplace<KeyframeView>();
    view->set_model(two_lanes());
    host->resize(Rect{0.0, 0.0, 600.0, 400.0}, flat_context());
    view->arrange(Rect{0.0, 0.0, 496.0, 200.0}, flat_context());
  }

  /// The centre of a keyframe's diamond.
  [[nodiscard]] std::pair<double, double> centre(std::size_t lane, std::size_t index) const {
    const Rect mark = view->keyframe_rect(lane, index);
    return {mark.x + mark.width * 0.5, mark.y + mark.height * 0.5};
  }

  std::unique_ptr<WidgetHost> host;
  KeyframeView* view = nullptr;
};

[[nodiscard]] MouseEvent press(double x, double y, bool shift = false) {
  return MouseEvent{
      .x = x, .y = y, .button = MouseButton::Left, .modifiers = {.shift = shift},
      .click_count = 1};
}

// --------------------------------------------------------------- geometry --

TEST(KeyframeView, TimeAndPositionRoundTrip) {
  Laid test;
  for (const double t : {0.0, 2.5, 5.0, 7.5, 10.0}) {
    EXPECT_NEAR(test.view->time_at(test.view->x_of(t)), t, 1e-6);
  }
}

TEST(KeyframeView, TheAxisStartsAfterTheNameGutter) {
  // Otherwise a keyframe at zero is drawn on top of its own property's name.
  Laid test;
  EXPECT_GE(test.view->x_of(0.0), KeyframeView::kNameWidth);
  EXPECT_LE(test.view->x_of(10.0), 496.0);
}

TEST(KeyframeView, AKeyframeAtEitherEndIsWhollyInsideTheWidget) {
  // Switching the stopwatch on at the head of a clip makes a keyframe at
  // exactly zero. Drawn on the edge, its diamond is half outside — and a press
  // outside a widget's bounds routes somewhere else entirely, so it could not
  // be grabbed at all.
  Laid test;
  const Rect area = test.view->bounds();
  EXPECT_GE(test.view->x_of(0.0) - KeyframeView::kGrabReach, area.x);
  EXPECT_LE(test.view->x_of(10.0) + KeyframeView::kGrabReach, area.right());
}

TEST(KeyframeView, TimesOutsideTheClipAreClampedRatherThanDrawnOutside) {
  Laid test;
  EXPECT_NEAR(test.view->x_of(-5.0), test.view->x_of(0.0), 1e-9);
  EXPECT_NEAR(test.view->x_of(99.0), test.view->x_of(10.0), 1e-9);
}

TEST(KeyframeView, AClipOfNoLengthIsNotADivisionByZero) {
  KeyframeView view;
  view.set_model(KeyframeView::Model{
      .duration = 0.0, .lanes = {KeyframeView::Lane{.name = "Opacity", .times = {0.0}}}});
  view.arrange(Rect{0.0, 0.0, 300.0, 100.0}, flat_context());

  EXPECT_DOUBLE_EQ(view.time_at(200.0), 0.0);
  RecordingPainter painter;
  view.paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(KeyframeView, EachLaneGetsItsOwnRow) {
  Laid test;
  const Rect first = test.view->lane_rect(0);
  const Rect second = test.view->lane_rect(1);
  EXPECT_GT(first.y, test.view->ruler().y) << "the first lane is under the ruler";
  EXPECT_NEAR(second.y, first.bottom(), 1e-9);
  EXPECT_EQ(test.view->lane_at(first.y + first.height * 0.5), 0u);
  EXPECT_EQ(test.view->lane_at(second.y + second.height * 0.5), 1u);
}

TEST(KeyframeView, APointAboveTheLanesIsInNoneOfThem) {
  Laid test;
  EXPECT_EQ(test.view->lane_at(test.view->ruler().y + 2.0), test.view->model().lanes.size());
}

// ------------------------------------------------------------- hit-testing --

TEST(KeyframeView, AKeyframeIsFoundWhereItIsDrawn) {
  Laid test;
  const auto [x, y] = test.centre(0, 1);
  const KeyframeHit hit = test.view->keyframe_at(x, y);
  EXPECT_TRUE(hit.found);
  EXPECT_EQ(hit.lane, 0u);
  EXPECT_EQ(hit.index, 1u);
}

TEST(KeyframeView, TheNearestKeyframeWinsRatherThanTheFirst) {
  // Two points a few pixels apart resolve to the one under the pointer, not to
  // whichever came first in the list.
  KeyframeView view;
  view.set_model(KeyframeView::Model{
      .duration = 10.0, .lanes = {KeyframeView::Lane{.name = "X", .times = {5.0, 5.08}}}});
  view.arrange(Rect{0.0, 0.0, 496.0, 200.0}, flat_context());

  const Rect second = view.keyframe_rect(0, 1);
  const KeyframeHit hit =
      view.keyframe_at(second.x + second.width * 0.5, second.y + second.height * 0.5);
  EXPECT_TRUE(hit.found);
  EXPECT_EQ(hit.index, 1u);
}

TEST(KeyframeView, EmptySpaceHitsNothing) {
  Laid test;
  const Rect track = test.view->track_rect(1);
  // Lane 1 has one keyframe, at 2.5s. Well away from it.
  const KeyframeHit hit = test.view->keyframe_at(track.right() - 4.0, track.y + track.height / 2);
  EXPECT_FALSE(hit.found);
}

// -------------------------------------------------------------- selection --

TEST(KeyframeView, ClickingAKeyframeSelectsIt) {
  Laid test;
  int reports = 0;
  test.view->set_on_select([&] { ++reports; });

  const auto [x, y] = test.centre(0, 2);
  test.host->mouse_down(press(x, y));

  ASSERT_EQ(test.view->selection().size(), 1u);
  EXPECT_TRUE(test.view->is_selected(0, 2));
  EXPECT_EQ(reports, 1);
}

TEST(KeyframeView, ShiftClickAddsToTheSelection) {
  // How an animation gets shaped: pick out several points and set the curve
  // between them in one go.
  Laid test;
  const auto [x0, y0] = test.centre(0, 0);
  const auto [x1, y1] = test.centre(0, 2);
  test.host->mouse_down(press(x0, y0));
  test.host->mouse_up(press(x0, y0));
  test.host->mouse_down(press(x1, y1, true));

  EXPECT_EQ(test.view->selection().size(), 2u);
  EXPECT_TRUE(test.view->is_selected(0, 0));
  EXPECT_TRUE(test.view->is_selected(0, 2));
}

TEST(KeyframeView, ShiftClickingASelectedKeyframeTakesItBackOut) {
  Laid test;
  const auto [x, y] = test.centre(0, 0);
  test.host->mouse_down(press(x, y));
  test.host->mouse_up(press(x, y));
  test.host->mouse_down(press(x, y, true));
  EXPECT_TRUE(test.view->selection().empty());
}

TEST(KeyframeView, ClickingASelectedKeyframeKeepsTheWholeSelection) {
  // Otherwise pressing on one of several to drag them all throws the rest away
  // before the drag has begun.
  Laid test;
  const auto [x0, y0] = test.centre(0, 0);
  const auto [x1, y1] = test.centre(0, 2);
  test.host->mouse_down(press(x0, y0));
  test.host->mouse_up(press(x0, y0));
  test.host->mouse_down(press(x1, y1, true));
  test.host->mouse_up(press(x1, y1));

  test.host->mouse_down(press(x0, y0));
  EXPECT_EQ(test.view->selection().size(), 2u);
}

TEST(KeyframeView, ClickingEmptySpaceClearsTheSelectionAndScrubs) {
  Laid test;
  double scrubbed = -1.0;
  test.view->set_on_scrub([&](double t) { scrubbed = t; });

  const auto [x, y] = test.centre(0, 0);
  test.host->mouse_down(press(x, y));
  test.host->mouse_up(press(x, y));
  ASSERT_FALSE(test.view->selection().empty());

  const Rect track = test.view->track_rect(1);
  test.host->mouse_down(press(track.right() - 4.0, track.y + track.height / 2));

  EXPECT_TRUE(test.view->selection().empty());
  EXPECT_NEAR(scrubbed, 10.0, 0.3);
}

TEST(KeyframeView, ANewModelDropsTheSelection) {
  // Indices into the old model mean nothing in the new one, and a stale
  // selection is how an edit lands on a keyframe that is no longer there.
  Laid test;
  const auto [x, y] = test.centre(0, 0);
  test.host->mouse_down(press(x, y));
  ASSERT_FALSE(test.view->selection().empty());

  test.view->set_model(two_lanes());
  EXPECT_TRUE(test.view->selection().empty());
}

// ---------------------------------------------------------------- dragging --

TEST(KeyframeView, DraggingAKeyframeReportsItsNewTime) {
  Laid test;
  double moved_to = -1.0;
  test.view->set_on_move([&](std::size_t, std::size_t, double to) { moved_to = to; });

  const auto [x, y] = test.centre(0, 1);  // 5 seconds
  test.host->mouse_down(press(x, y));
  test.host->mouse_move(MouseEvent{.x = test.view->x_of(7.0), .y = y});

  EXPECT_NEAR(moved_to, 7.0, 1e-6);
}

TEST(KeyframeView, ASmallWobbleIsAClickRatherThanADrag) {
  Laid test;
  int moves = 0;
  test.view->set_on_move([&](std::size_t, std::size_t, double) { ++moves; });

  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(press(x, y));
  test.host->mouse_move(MouseEvent{.x = x + KeyframeView::kDragThreshold - 1.0, .y = y});
  EXPECT_EQ(moves, 0);
}

TEST(KeyframeView, ADragRecordsOnceAtTheEndAndNamesBothItsEnds) {
  Laid test;
  int commits = 0;
  double from = -1.0;
  double to = -1.0;
  test.view->set_on_move_commit([&](std::size_t, std::size_t, double a, double b) {
    ++commits;
    from = a;
    to = b;
  });

  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(press(x, y));
  test.host->mouse_move(MouseEvent{.x = test.view->x_of(6.0), .y = y});
  test.host->mouse_move(MouseEvent{.x = test.view->x_of(8.0), .y = y});
  EXPECT_EQ(commits, 0) << "it recorded partway through the drag";

  test.host->mouse_up(press(test.view->x_of(8.0), y));
  EXPECT_EQ(commits, 1);
  EXPECT_NEAR(from, 5.0, 1e-6);
  EXPECT_NEAR(to, 8.0, 1e-6);
}

TEST(KeyframeView, ADragThatEndsWhereItStartedRecordsNothing) {
  Laid test;
  int commits = 0;
  test.view->set_on_move_commit(
      [&](std::size_t, std::size_t, double, double) { ++commits; });

  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(press(x, y));
  test.host->mouse_move(MouseEvent{.x = test.view->x_of(8.0), .y = y});
  test.host->mouse_move(MouseEvent{.x = x, .y = y});
  test.host->mouse_up(press(x, y));

  EXPECT_EQ(commits, 0);
}

TEST(KeyframeView, DraggingPastTheEndsStopsAtThem) {
  Laid test;
  double moved_to = -1.0;
  test.view->set_on_move([&](std::size_t, std::size_t, double to) { moved_to = to; });

  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(press(x, y));
  test.host->mouse_move(MouseEvent{.x = 9000.0, .y = y});
  EXPECT_NEAR(moved_to, 10.0, 1e-6);

  test.host->mouse_move(MouseEvent{.x = -9000.0, .y = y});
  EXPECT_NEAR(moved_to, 0.0, 1e-6);
}

// ---------------------------------------------------------------- painting --

TEST(KeyframeView, DrawsADiamondPerKeyframe) {
  Laid test;
  RecordingPainter painter;
  test.view->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());

  // Four keyframes, four lines each. Plus the lane lines, the ruler's ticks
  // and the playhead, so this is a floor rather than an equality.
  EXPECT_GE(painter.count(DrawCall::Kind::Line), 4u * 4u);
}

TEST(KeyframeView, ASelectedKeyframeIsDrawnInTheAccent) {
  Laid test;
  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(press(x, y));

  RecordingPainter painter;
  test.view->paint(painter, default_theme());

  bool found = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind != DrawCall::Kind::Line) continue;
    // The diamond's top vertex, which no other line starts at.
    if (std::abs(call.bounds.x - x) < 0.01 &&
        std::abs(call.bounds.y - (y - KeyframeView::kDiamond)) < 0.01) {
      found = call.color == default_theme().accent;
    }
  }
  EXPECT_TRUE(found);
}

TEST(KeyframeView, EveryLanesNameIsWritten) {
  Laid test;
  RecordingPainter painter;
  test.view->paint(painter, default_theme());

  bool position = false;
  bool opacity = false;
  for (const DrawCall& call : painter.calls()) {
    if (!call.run.has_value()) continue;
    position = position || call.run->text == "Position X";
    opacity = opacity || call.run->text == "Opacity";
  }
  EXPECT_TRUE(position);
  EXPECT_TRUE(opacity);
}

TEST(KeyframeView, AClipWithNoAnimationSaysSoRatherThanDrawingNothing) {
  KeyframeView view;
  view.arrange(Rect{0.0, 0.0, 300.0, 60.0}, flat_context());

  RecordingPainter painter;
  view.paint(painter, default_theme());

  bool said = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.run.has_value() && call.run->text == "Nothing is animated") said = true;
  }
  EXPECT_TRUE(said);
}

TEST(KeyframeView, AsksForExactlyItsRows) {
  // A view that asked for more would draw empty lanes; one that asked for less
  // would hide the last property animated.
  Laid test;
  const LayoutItem two = test.view->sizing(Axis::Vertical, flat_context());

  KeyframeView one;
  one.set_model(KeyframeView::Model{
      .duration = 10.0, .lanes = {KeyframeView::Lane{.name = "X", .times = {1.0}}}});
  const LayoutItem single = one.sizing(Axis::Vertical, flat_context());

  EXPECT_GT(two.basis, single.basis);
  EXPECT_NEAR(two.basis - single.basis, default_theme().metrics.list_row_height, 1e-9);
}

}  // namespace
}  // namespace cutline::ui
