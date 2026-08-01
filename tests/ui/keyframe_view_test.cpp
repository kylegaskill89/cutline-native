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

TEST(KeyframeView, ReadingAPositionBackNeverEscapesTheClip) {
  // `x_of` deliberately does *not* clamp — once the view can be zoomed, a
  // keyframe outside it has to map outside the axis so that painting clips it
  // away. The reverse direction is what has to stay inside: a press is a time
  // in the clip, and there is no such time as minus two seconds.
  Laid test;
  EXPECT_DOUBLE_EQ(test.view->time_at(-9000.0), 0.0);
  EXPECT_DOUBLE_EQ(test.view->time_at(9000.0), 10.0);
  EXPECT_LT(test.view->x_of(-5.0), test.view->x_of(0.0));
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

TEST(KeyframeView, ClickingEmptyLaneSpaceClearsTheSelection) {
  Laid test;
  const auto [x, y] = test.centre(0, 0);
  test.host->mouse_down(press(x, y));
  test.host->mouse_up(press(x, y));
  ASSERT_FALSE(test.view->selection().empty());

  // Lane 1 has one keyframe, at 2.5s. Well away from it, and released without
  // moving, which is a click rather than a sweep.
  const Rect track = test.view->track_rect(1);
  const double empty_x = track.right() - 4.0;
  const double empty_y = track.y + track.height / 2;
  test.host->mouse_down(press(empty_x, empty_y));
  test.host->mouse_up(press(empty_x, empty_y));

  EXPECT_TRUE(test.view->selection().empty());
}

TEST(KeyframeView, TheRulerScrubsAndTheLanesDoNot) {
  // Dragging in a lane is a rubber band. Scrubbing from there would mean the
  // playhead jumped every time somebody swept for keyframes.
  Laid test;
  double scrubbed = -1.0;
  test.view->set_on_scrub([&](double t) { scrubbed = t; });

  const Rect strip = test.view->ruler();
  test.host->mouse_down(press(test.view->x_of(4.0), strip.y + strip.height / 2));
  EXPECT_NEAR(scrubbed, 4.0, 1e-6);

  scrubbed = -1.0;
  const Rect track = test.view->track_rect(1);
  test.host->mouse_down(press(track.right() - 4.0, track.y + track.height / 2));
  EXPECT_DOUBLE_EQ(scrubbed, -1.0) << "a press in a lane scrubbed";
}

// ----------------------------------------------------------------- marquee --

/// 7 seconds: the emptiest part of the first lane, which has keyframes at 0, 5
/// and 10. A band has to *begin* somewhere no keyframe is, or the press takes
/// hold of one and the gesture is a drag rather than a sweep.
constexpr double kEmpty = 7.0;

TEST(KeyframeView, SweepingARubberBandTakesEverythingUnderIt) {
  Laid test;
  const Rect first = test.view->lane_rect(0);
  const double y = first.y + first.height / 2;

  // From empty space back across the keyframes at 5s and 0s.
  test.host->mouse_down(press(test.view->x_of(kEmpty), y));
  test.host->mouse_move(MouseEvent{.x = test.view->x_of(0.0), .y = y + 4.0});

  EXPECT_EQ(test.view->selection().size(), 2u);
  EXPECT_TRUE(test.view->is_selected(0, 0));
  EXPECT_TRUE(test.view->is_selected(0, 1));
  EXPECT_FALSE(test.view->is_selected(0, 2)) << "the one at 10s was outside the band";
}

TEST(KeyframeView, ABandTakesWholeRowsRatherThanTheDiamondsOwnFewPixels) {
  // Sweeping through the exact vertical middle of every diamond is not a
  // gesture anybody can perform.
  Laid test;
  const Rect first = test.view->lane_rect(0);
  test.host->mouse_down(press(test.view->x_of(kEmpty), first.y + 1.0));
  test.host->mouse_move(MouseEvent{.x = test.view->x_of(0.0), .y = first.y + 2.0});

  EXPECT_EQ(test.view->selection().size(), 2u);
}

TEST(KeyframeView, ABandReachesAcrossLanes) {
  Laid test;
  const Rect first = test.view->lane_rect(0);
  const Rect second = test.view->lane_rect(1);

  test.host->mouse_down(press(test.view->x_of(kEmpty), first.y + 1.0));
  test.host->mouse_move(MouseEvent{.x = test.view->x_of(0.0), .y = second.bottom() - 1.0});

  // The first lane's 0 and 5, and the second lane's 2.5.
  EXPECT_EQ(test.view->selection().size(), 3u);
  EXPECT_TRUE(test.view->is_selected(1, 0));
}

TEST(KeyframeView, ShrinkingTheBandReleasesWhatItNoLongerCovers) {
  Laid test;
  const Rect first = test.view->lane_rect(0);
  const double y = first.y + first.height / 2;

  test.host->mouse_down(press(test.view->x_of(kEmpty), y));
  test.host->mouse_move(MouseEvent{.x = test.view->x_of(0.0), .y = y});
  ASSERT_EQ(test.view->selection().size(), 2u);

  test.host->mouse_move(MouseEvent{.x = test.view->x_of(4.0), .y = y});
  EXPECT_EQ(test.view->selection().size(), 1u);
  EXPECT_TRUE(test.view->is_selected(0, 1)) << "the one at 5s is what is still covered";
}

TEST(KeyframeView, ShiftSweepingAddsToWhatWasAlreadySelected) {
  Laid test;
  const auto [x, y] = test.centre(1, 0);
  test.host->mouse_down(press(x, y));
  test.host->mouse_up(press(x, y));
  ASSERT_EQ(test.view->selection().size(), 1u);

  const Rect first = test.view->lane_rect(0);
  const double row = first.y + first.height / 2;
  test.host->mouse_down(press(test.view->x_of(kEmpty), row, true));
  test.host->mouse_move(
      MouseEvent{.x = test.view->x_of(0.0), .y = row, .modifiers = {.shift = true}});

  EXPECT_EQ(test.view->selection().size(), 3u);
  EXPECT_TRUE(test.view->is_selected(1, 0)) << "the earlier selection was thrown away";
}

TEST(KeyframeView, TheBandIsDrawnWhileItIsBeingSwept) {
  Laid test;
  const Rect first = test.view->lane_rect(0);
  const double y = first.y + first.height / 2;
  test.host->mouse_down(press(test.view->x_of(kEmpty), y));
  EXPECT_TRUE(test.view->marquee().empty()) << "drawn before it had moved";

  test.host->mouse_move(MouseEvent{.x = test.view->x_of(0.0), .y = y + 10.0});
  EXPECT_FALSE(test.view->marquee().empty());

  test.host->mouse_up(press(test.view->x_of(0.0), y + 10.0));
  EXPECT_TRUE(test.view->marquee().empty()) << "left on screen after the release";
}

// ---------------------------------------------------------- context menu --

TEST(KeyframeView, ARightClickOnAKeyframeSelectsItAndAsksForAMenu) {
  Laid test;
  int menus = 0;
  test.view->set_on_context_menu([&](double, double) { ++menus; });

  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(MouseEvent{.x = x, .y = y, .button = MouseButton::Right});

  EXPECT_EQ(menus, 1);
  EXPECT_TRUE(test.view->is_selected(0, 1));
}

TEST(KeyframeView, ARightClickOnASelectionLeavesItAlone) {
  // Otherwise a menu meant to act on five keyframes acts on one.
  Laid test;
  const auto [x0, y0] = test.centre(0, 0);
  const auto [x1, y1] = test.centre(0, 1);
  test.host->mouse_down(press(x0, y0));
  test.host->mouse_up(press(x0, y0));
  test.host->mouse_down(press(x1, y1, true));
  test.host->mouse_up(press(x1, y1, true));
  ASSERT_EQ(test.view->selection().size(), 2u);

  test.host->mouse_down(MouseEvent{.x = x1, .y = y1, .button = MouseButton::Right});
  EXPECT_EQ(test.view->selection().size(), 2u);
}

TEST(KeyframeView, ARightClickOnNothingOffersNoMenu) {
  // An empty menu is worse than none.
  Laid test;
  int menus = 0;
  test.view->set_on_context_menu([&](double, double) { ++menus; });

  const Rect track = test.view->track_rect(1);
  test.host->mouse_down(MouseEvent{
      .x = track.right() - 4.0, .y = track.y + track.height / 2, .button = MouseButton::Right});
  EXPECT_EQ(menus, 0);
}

TEST(KeyframeView, ARightClickDoesNotStartADrag) {
  Laid test;
  int moves = 0;
  test.view->set_on_move([&](std::size_t, std::size_t, double) { ++moves; });

  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(MouseEvent{.x = x, .y = y, .button = MouseButton::Right});
  test.host->mouse_move(MouseEvent{.x = x + 40.0, .y = y});
  EXPECT_EQ(moves, 0);
}

// ------------------------------------------------------------------ zoom --

TEST(KeyframeView, ShowsTheWholeClipUntilItIsZoomed) {
  Laid test;
  EXPECT_DOUBLE_EQ(test.view->view_start(), 0.0);
  EXPECT_DOUBLE_EQ(test.view->view_span(), 10.0);
}

TEST(KeyframeView, ZoomingKeepsTheTimeUnderThePointerUnderIt) {
  // Zooming about the left edge instead means scrolling back to what you were
  // looking at after every notch.
  Laid test;
  const double x = test.view->x_of(7.5);
  const double before = test.view->time_at(x);

  test.view->zoom_about(x, 4.0);
  EXPECT_NEAR(test.view->time_at(x), before, 1e-6);
  EXPECT_NEAR(test.view->view_span(), 2.5, 1e-6);
}

TEST(KeyframeView, TheViewNeverRunsPastEitherEndOfTheClip) {
  // There is nothing before a clip starts or after it finishes, and scrolling
  // into it only produces empty space nobody asked for.
  Laid test;
  test.view->set_view(-5.0, 4.0);
  EXPECT_DOUBLE_EQ(test.view->view_start(), 0.0);

  test.view->set_view(99.0, 4.0);
  EXPECT_DOUBLE_EQ(test.view->view_start(), 6.0);
}

TEST(KeyframeView, ZoomingOutStopsAtTheWholeClip) {
  Laid test;
  test.view->zoom_about(test.view->x_of(5.0), 0.01);
  EXPECT_DOUBLE_EQ(test.view->view_span(), 10.0);
  EXPECT_DOUBLE_EQ(test.view->view_start(), 0.0);
}

TEST(KeyframeView, ZoomingInStopsAtAUsefulSpan) {
  Laid test;
  for (int i = 0; i < 100; ++i) test.view->zoom_about(test.view->x_of(5.0), 4.0);
  EXPECT_NEAR(test.view->view_span(), 10.0 * KeyframeView::kMinSpan, 1e-9);
}

TEST(KeyframeView, TheWheelZoomsAndShiftWheelScrolls) {
  Laid test;
  const Rect first = test.view->lane_rect(0);
  const double y = first.y + first.height / 2;
  const double x = test.view->x_of(5.0);

  test.host->wheel(WheelEvent{.x = x, .y = y, .delta_y = -1.0});
  EXPECT_LT(test.view->view_span(), 10.0) << "the wheel did not zoom in";

  const double start = test.view->view_start();
  test.host->wheel(WheelEvent{.x = x, .y = y, .delta_y = 1.0, .modifiers = {.shift = true}});
  EXPECT_GT(test.view->view_start(), start);
}

TEST(KeyframeView, ShiftWheelDoesNothingWhileTheWholeClipIsShowing) {
  // There is nowhere to scroll to, and taking the event would stop the panel
  // beneath from scrolling instead.
  Laid test;
  const Rect first = test.view->lane_rect(0);
  EXPECT_FALSE(test.host->wheel(WheelEvent{.x = test.view->x_of(5.0),
                                           .y = first.y + first.height / 2,
                                           .delta_y = 1.0,
                                           .modifiers = {.shift = true}}));
}

TEST(KeyframeView, AKeyframeScrolledOutOfViewCannotBeGrabbed) {
  // Without this one just past the left edge can be taken hold of by pressing
  // on a property's name.
  Laid test;
  test.view->set_view(6.0, 4.0);

  const Rect row = test.view->lane_rect(0);
  const Rect track = test.view->track_rect(0);
  // The keyframe at 5s now maps to an x inside the name gutter.
  ASSERT_LT(test.view->x_of(5.0), track.x);
  EXPECT_FALSE(test.view->keyframe_at(test.view->x_of(5.0), row.y + row.height / 2).found);
}

TEST(KeyframeView, ANewClipIsShownWholeRatherThanAtTheLastOnesZoom) {
  Laid test;
  test.view->zoom_about(test.view->x_of(5.0), 4.0);
  ASSERT_LT(test.view->view_span(), 10.0);

  test.view->set_model(KeyframeView::Model{
      .duration = 3.0, .lanes = {KeyframeView::Lane{.name = "X", .times = {1.0}}}});
  EXPECT_DOUBLE_EQ(test.view->view_span(), 3.0);
  EXPECT_DOUBLE_EQ(test.view->view_start(), 0.0);
}

TEST(KeyframeView, ZoomedInItStillPaintsInsideItself) {
  Laid test;
  test.view->set_view(4.0, 1.0);

  RecordingPainter painter;
  test.view->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
}

// ---------------------------------------------------------------- delete --

TEST(KeyframeView, DeleteAsksForTheSelectionToGo) {
  Laid test;
  int deletes = 0;
  test.view->set_on_delete([&] { ++deletes; });

  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(press(x, y));
  test.host->set_focus(test.view);
  test.host->key_down(KeyEvent{.key = Key::Delete});
  EXPECT_EQ(deletes, 1);
}

TEST(KeyframeView, CopyAndPasteAreAskedForRatherThanDone) {
  // The clipboard is not the view's: keyframes copied here go back onto a
  // *clip*, and the view has never known which clip it is showing.
  Laid test;
  int copies = 0;
  int pastes = 0;
  test.view->set_on_copy([&] { ++copies; });
  test.view->set_on_paste([&] { ++pastes; });

  const auto [x, y] = test.centre(0, 1);
  test.host->mouse_down(press(x, y));
  test.host->set_focus(test.view);

  test.host->key_down(KeyEvent{.key = Key::C, .modifiers = {.control = true}});
  test.host->key_down(KeyEvent{.key = Key::V, .modifiers = {.control = true}});
  EXPECT_EQ(copies, 1);
  EXPECT_EQ(pastes, 1);
}

TEST(KeyframeView, CopyingNothingIsNotSwallowed) {
  // Ctrl+C with no keyframes selected has to carry on to whatever else it
  // means — copying the selected clip's effect stack, for one.
  Laid test;
  int copies = 0;
  test.view->set_on_copy([&] { ++copies; });
  test.host->set_focus(test.view);

  EXPECT_FALSE(test.host->key_down(KeyEvent{.key = Key::C, .modifiers = {.control = true}}));
  EXPECT_EQ(copies, 0);
}

// ----------------------------------------------------------------- graph --

/// A lane with a curve on it, so the graph has something to draw.
[[nodiscard]] KeyframeView::Model with_curve() {
  KeyframeView::Model model{.duration = 10.0,
                            .lanes = {KeyframeView::Lane{.name = "Position X",
                                                         .times = {0.0, 10.0}}}};
  for (int i = 0; i < 64; ++i) model.lanes[0].curve.push_back(static_cast<double>(i));
  return model;
}

TEST(KeyframeView, ALaneWithACurveOffersAWayToOpenIt) {
  Laid test;
  EXPECT_TRUE(test.view->reveal_rect(0).empty()) << "offered on a lane with no curve";

  test.view->set_model(with_curve());
  test.view->arrange(Rect{0.0, 0.0, 496.0, 200.0}, flat_context());
  EXPECT_FALSE(test.view->reveal_rect(0).empty());
}

TEST(KeyframeView, OpeningALaneMakesRoomForItsGraph) {
  Laid test;
  test.view->set_model(with_curve());
  test.view->arrange(Rect{0.0, 0.0, 496.0, 200.0}, flat_context());

  const double closed = test.view->lane_rect(0).height;
  const Rect reveal = test.view->reveal_rect(0);
  test.host->mouse_down(press(reveal.x + 2.0, reveal.y + reveal.height / 2));
  test.view->arrange(Rect{0.0, 0.0, 496.0, 200.0}, flat_context());

  EXPECT_TRUE(test.view->is_expanded(0));
  EXPECT_NEAR(test.view->lane_rect(0).height - closed, KeyframeView::kGraphHeight, 1e-9);
  EXPECT_FALSE(test.view->graph_rect(0).empty());
}

TEST(KeyframeView, TheDiamondsStayWhereTheyWereWhenAGraphOpens) {
  // The graph appears underneath them rather than pushing them down the row,
  // which would move every keyframe out from under the pointer.
  Laid test;
  test.view->set_model(with_curve());
  test.view->arrange(Rect{0.0, 0.0, 496.0, 200.0}, flat_context());
  const Rect before = test.view->keyframe_rect(0, 0);

  test.view->set_expanded("Position X", true);
  test.view->arrange(Rect{0.0, 0.0, 496.0, 200.0}, flat_context());
  EXPECT_EQ(test.view->keyframe_rect(0, 0), before);
}

TEST(KeyframeView, AnOpenGraphPushesTheLanesBelowItDown) {
  KeyframeView view;
  KeyframeView::Model model = with_curve();
  model.lanes.push_back(KeyframeView::Lane{.name = "Opacity", .times = {5.0}});
  view.set_model(std::move(model));
  view.arrange(Rect{0.0, 0.0, 496.0, 260.0}, flat_context());

  const double before = view.lane_rect(1).y;
  view.set_expanded("Position X", true);
  view.arrange(Rect{0.0, 0.0, 496.0, 260.0}, flat_context());

  EXPECT_NEAR(view.lane_rect(1).y - before, KeyframeView::kGraphHeight, 1e-9);
  EXPECT_EQ(view.lane_at(view.lane_rect(1).y + 2.0), 1u) << "hit-testing did not follow";
}

TEST(KeyframeView, AnOpenGraphAsksForTheRoomItNeeds) {
  KeyframeView view;
  view.set_model(with_curve());
  const double closed = view.sizing(Axis::Vertical, flat_context()).basis;

  view.set_expanded("Position X", true);
  EXPECT_NEAR(view.sizing(Axis::Vertical, flat_context()).basis - closed,
              KeyframeView::kGraphHeight, 1e-9);
}

TEST(KeyframeView, TheGraphIsRememberedByNameRatherThanByPosition) {
  // The model is rebuilt on every edit, and adding an effect renumbers every
  // lane below it.
  KeyframeView view;
  KeyframeView::Model model = with_curve();
  model.lanes.push_back(KeyframeView::Lane{.name = "Opacity", .times = {5.0}});
  view.set_model(std::move(model));
  view.set_expanded("Position X", true);
  ASSERT_TRUE(view.is_expanded(0));

  // The same properties, the other way round.
  KeyframeView::Model swapped{.duration = 10.0};
  swapped.lanes.push_back(KeyframeView::Lane{.name = "Opacity", .times = {5.0}});
  swapped.lanes.push_back(with_curve().lanes[0]);
  view.set_model(std::move(swapped));

  EXPECT_FALSE(view.is_expanded(0));
  EXPECT_TRUE(view.is_expanded(1));
}

TEST(KeyframeView, AGraphIsDrawnInsideItsOwnBox) {
  KeyframeView view;
  view.set_model(with_curve());
  view.set_expanded("Position X", true);
  view.arrange(Rect{0.0, 0.0, 496.0, 260.0}, flat_context());

  RecordingPainter painter;
  view.paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
  EXPECT_GT(painter.count(DrawCall::Kind::Line), 60u) << "the curve was not drawn";
}

TEST(KeyframeView, ACurveThatNeverMovesIsNotADivisionByZero) {
  KeyframeView view;
  KeyframeView::Model model{
      .duration = 10.0,
      .lanes = {KeyframeView::Lane{.name = "X", .times = {0.0, 10.0}, .curve = {1.0, 1.0, 1.0}}}};
  view.set_model(std::move(model));
  view.set_expanded("X", true);
  view.arrange(Rect{0.0, 0.0, 496.0, 200.0}, flat_context());

  RecordingPainter painter;
  view.paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(KeyframeView, DeleteWithNothingSelectedIsNotSwallowed) {
  // It has to carry on to whatever else Delete means — removing the selected
  // clip, for one.
  Laid test;
  int deletes = 0;
  test.view->set_on_delete([&] { ++deletes; });
  test.host->set_focus(test.view);

  EXPECT_FALSE(test.host->key_down(KeyEvent{.key = Key::Delete}));
  EXPECT_EQ(deletes, 0);
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
