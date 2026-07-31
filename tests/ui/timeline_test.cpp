/// The timeline, driven from a handful of structs.
///
/// It takes what it draws as plain data rather than reading a project, which is
/// what makes all of this possible with no media, no decoding and no window:
/// where a clip lands at a given zoom, what a click at a point selects, that
/// scrubbing keeps working when the pointer leaves the ruler, and that the
/// playhead never draws across the track headers.

#include "cutline/ui/timeline.hpp"

#include "cutline/core/time.hpp"
#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::ui {
namespace {

constexpr double kFps = 30.0;

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() {
  return LayoutContext{default_theme(), measurer()};
}

[[nodiscard]] MouseEvent press(double x, double y) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left};
}

[[nodiscard]] TimelineModel sample_model() {
  TimelineModel model;
  model.fps = kFps;
  model.tracks = {
      TimelineTrack{.name = "V2",
                    .blocks = {TimelineBlock{.start = 2.0, .end = 6.0, .label = "insert"}}},
      TimelineTrack{.name = "V1",
                    .blocks = {TimelineBlock{.start = 0.0, .end = 5.0, .label = "wide"},
                               TimelineBlock{.start = 5.0, .end = 12.0, .label = "close"}}},
      TimelineTrack{.name = "A1",
                    .audio = true,
                    .blocks = {TimelineBlock{.start = 0.0, .end = 12.0, .label = "dialogue"}}},
  };
  return model;
}

/// A timeline in a host, laid out at a known size and zoom.
struct Fixture {
  Fixture() {
    host = std::make_unique<WidgetHost>(std::make_unique<TimelineView>());
    view = static_cast<TimelineView*>(&host->root());
    view->set_model(sample_model());
    view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = 0.0});
    host->resize(Rect{0.0, 0.0, 1000.0, 400.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  TimelineView* view = nullptr;
};

// -------------------------------------------------------------- geometry --

TEST(Timeline, HeadersAndTimeDivideTheWidth) {
  const Fixture fixture;
  const Metrics& metrics = default_theme().metrics;

  EXPECT_DOUBLE_EQ(fixture.view->header_area().width, metrics.track_header_width);
  EXPECT_DOUBLE_EQ(fixture.view->time_area().x, metrics.track_header_width);
  EXPECT_DOUBLE_EQ(fixture.view->time_area().right(), 1000.0);
}

TEST(Timeline, TheRulerSitsAboveTheTracks) {
  const Fixture fixture;
  const Metrics& metrics = default_theme().metrics;

  EXPECT_DOUBLE_EQ(fixture.view->ruler_area().height, metrics.ruler_height);
  EXPECT_DOUBLE_EQ(fixture.view->tracks_area().y, fixture.view->ruler_area().bottom());
  EXPECT_DOUBLE_EQ(fixture.view->tracks_area().bottom(), 400.0);
}

TEST(Timeline, AudioTracksAreShorterBecauseTheThemeSaysSo) {
  const Fixture fixture;
  const Metrics& metrics = default_theme().metrics;

  EXPECT_DOUBLE_EQ(fixture.view->track_rect(0).height, metrics.track_height);
  EXPECT_DOUBLE_EQ(fixture.view->track_rect(2).height, metrics.audio_track_height);
}

TEST(Timeline, TracksStackInOrder) {
  const Fixture fixture;
  EXPECT_DOUBLE_EQ(fixture.view->track_rect(1).y, fixture.view->track_rect(0).bottom());
  EXPECT_DOUBLE_EQ(fixture.view->track_rect(0).y, fixture.view->tracks_area().y);
}

TEST(Timeline, ABlockLandsWhereItsTimeSays) {
  const Fixture fixture;
  const Rect box = fixture.view->block_rect(0, 0);  // 2s to 6s at 100 px/s

  EXPECT_DOUBLE_EQ(box.x, fixture.view->time_area().x + 200.0);
  EXPECT_DOUBLE_EQ(box.width, 400.0);
  EXPECT_DOUBLE_EQ(box.height, fixture.view->track_rect(0).height);
}

TEST(Timeline, KeyframesAreDrawnOnTheBlockThatCarriesThem) {
  const Fixture plain;
  RecordingPainter before;
  plain.view->paint(before, default_theme());

  TimelineModel animated = sample_model();
  // Two keyframes on the second track's first block, which runs 0s to 5s.
  animated.tracks[1].blocks[0].keyframes = {1.0, 3.0};

  Fixture keyed;
  keyed.view->set_model(animated);
  keyed.host->resize(Rect{0.0, 0.0, 1000.0, 400.0}, flat_context());

  RecordingPainter after;
  keyed.view->paint(after, default_theme());

  // Four lines to a diamond, so two of them is eight more than were there.
  EXPECT_EQ(after.count(DrawCall::Kind::Line), before.count(DrawCall::Kind::Line) + 8);
}

TEST(Timeline, AKeyframeIsDrawnAtItsOwnTimeWithinTheBlock) {
  TimelineModel animated = sample_model();
  animated.tracks[1].blocks[0].keyframes = {1.0};

  Fixture fixture;
  fixture.view->set_model(animated);
  fixture.host->resize(Rect{0.0, 0.0, 1000.0, 400.0}, flat_context());

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  const Rect box = fixture.view->block_rect(1, 0);
  // One second in at 100 px/s, and low in the block: the mark sits along its
  // foot so a label cannot cover it.
  const double expected = box.x + 100.0;
  const bool marked = std::ranges::any_of(painter.calls(), [expected, &box](const DrawCall& c) {
    return c.kind == DrawCall::Kind::Line && std::abs(c.bounds.x - expected) <= 4.0 &&
           c.bounds.y > box.y + box.height * 0.5;
  });
  EXPECT_TRUE(marked) << "no keyframe mark one second into the block";
}

TEST(Timeline, ScrollingMovesTheBlocksAndNotTheHeaders) {
  Fixture fixture;
  const double before = fixture.view->block_rect(0, 0).x;
  const double header = fixture.view->header_rect(0).x;

  fixture.view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = 1.0});

  EXPECT_DOUBLE_EQ(fixture.view->block_rect(0, 0).x, before - 100.0);
  EXPECT_DOUBLE_EQ(fixture.view->header_rect(0).x, header);
}

TEST(Timeline, AVeryShortClipIsStillWideEnoughToFind) {
  // A one-frame clip zoomed out is a fraction of a pixel. Drawing it as
  // nothing means it cannot be found or clicked, which is exactly when
  // somebody is hunting for it.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[0].blocks = {TimelineBlock{.start = 1.0, .end = 1.0 + 1.0 / kFps}};
  fixture.view->set_model(model);
  fixture.view->set_scale(TimeScale{.pixels_per_second = 1.0});

  EXPECT_GE(fixture.view->block_rect(0, 0).width, 2.0);
}

TEST(Timeline, TheContentIsAsLongAsItsLastClip) {
  const TimelineModel model = sample_model();
  EXPECT_DOUBLE_EQ(model.content_duration(), 12.0);
}

TEST(Timeline, AnExplicitDurationWinsWhenItIsLonger) {
  TimelineModel model = sample_model();
  model.duration = 40.0;
  EXPECT_DOUBLE_EQ(model.content_duration(), 40.0);
}

TEST(Timeline, AnEmptyTimelineIsHarmless) {
  WidgetHost host(std::make_unique<TimelineView>());
  auto& view = static_cast<TimelineView&>(host.root());
  host.resize(Rect{0.0, 0.0, 800.0, 300.0}, flat_context());

  EXPECT_TRUE(view.track_rect(0).empty());
  EXPECT_FALSE(view.block_at(400.0, 200.0).has_value());
  EXPECT_FALSE(host.mouse_down(press(400.0, 5000.0))) << "a click off the end of it was taken";

  RecordingPainter painter;
  view.paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
}

// ----------------------------------------------------------- hit testing --

TEST(Timeline, ClickingAClipSelectsIt) {
  Fixture fixture;
  const Rect box = fixture.view->block_rect(1, 1);  // "close", 5s to 12s

  fixture.host->mouse_down(press(box.x + 10.0, box.y + 10.0));

  const auto selected = fixture.view->selection();
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, (BlockRef{1, 1}));
}

TEST(Timeline, ClickingEmptyTrackClearsTheSelection) {
  Fixture fixture;
  fixture.view->select(BlockRef{1, 0});
  ASSERT_TRUE(fixture.view->selection().has_value());

  // Well past the last clip on that track.
  const Rect row = fixture.view->track_rect(0);
  fixture.host->mouse_down(press(fixture.view->time_area().right() - 20.0, row.y + 5.0));

  EXPECT_FALSE(fixture.view->selection().has_value());
}

TEST(Timeline, OnlyOneClipIsSelectedAtATime) {
  Fixture fixture;
  fixture.view->select(BlockRef{1, 0});
  fixture.view->select(BlockRef{2, 0});

  int selected = 0;
  for (const TimelineTrack& track : fixture.view->model().tracks) {
    for (const TimelineBlock& block : track.blocks) {
      if (block.selected) ++selected;
    }
  }
  EXPECT_EQ(selected, 1);
}

TEST(Timeline, SelectionIsReportedOnce) {
  Fixture fixture;
  std::vector<std::optional<BlockRef>> reported;
  fixture.view->set_on_select([&](std::optional<BlockRef> ref) { reported.push_back(ref); });

  const Rect box = fixture.view->block_rect(1, 0);
  fixture.host->mouse_down(press(box.x + 5.0, box.y + 5.0));

  ASSERT_EQ(reported.size(), 1u);
  ASSERT_TRUE(reported[0].has_value());
  EXPECT_EQ(*reported[0], (BlockRef{1, 0}));
}

TEST(Timeline, TheHeaderColumnIsNotPartOfTheTracks) {
  const Fixture fixture;
  // A point inside the headers is at a time that would otherwise land on a
  // clip. It must not select one.
  const Rect row = fixture.view->track_rect(1);
  EXPECT_FALSE(fixture.view->block_at(10.0, row.y + 5.0).has_value());
}

TEST(Timeline, TheTopmostOverlappingClipAnswers) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[0].blocks = {TimelineBlock{.start = 0.0, .end = 8.0, .label = "under"},
                            TimelineBlock{.start = 2.0, .end = 4.0, .label = "over"}};
  fixture.view->set_model(model);

  const Rect over = fixture.view->block_rect(0, 1);
  const auto hit = fixture.view->block_at(over.x + 5.0, over.y + 5.0);
  ASSERT_TRUE(hit.has_value());
  EXPECT_EQ(hit->block, 1u) << "the clip drawn underneath answered the click";
}

// ------------------------------------------------------------------ drops --

TEST(Timeline, ADropReportsTheTrackAndTheTimeUnderIt) {
  const Fixture fixture;  // 100 px/s, starting at zero
  const Rect row = fixture.view->track_rect(1);

  const auto where = fixture.view->drop_at(fixture.view->time_area().x + 350.0, row.y + 5.0);
  ASSERT_TRUE(where.has_value());
  EXPECT_EQ(where->track, 1u);
  EXPECT_DOUBLE_EQ(where->time, 3.5);
}

TEST(Timeline, ADropFollowsTheScroll) {
  Fixture fixture;
  fixture.view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = 4.0});

  const Rect row = fixture.view->track_rect(0);
  const auto where = fixture.view->drop_at(fixture.view->time_area().x + 100.0, row.y + 5.0);
  ASSERT_TRUE(where.has_value());
  EXPECT_DOUBLE_EQ(where->time, 5.0);
}

TEST(Timeline, ADropOverTheHeadersIsRefused) {
  // Not clamped to zero: a drop that has no time is not a drop at the start of
  // the sequence, and quietly turning one into the other puts clips where
  // nobody asked for them.
  const Fixture fixture;
  const Rect row = fixture.view->track_rect(1);
  EXPECT_FALSE(fixture.view->drop_at(10.0, row.y + 5.0).has_value());
}

TEST(Timeline, ADropOverTheRulerOrPastTheLastTrackIsRefused) {
  const Fixture fixture;
  const Rect ruler = fixture.view->ruler_area();
  EXPECT_FALSE(fixture.view->drop_at(ruler.x + 50.0, ruler.y + 2.0).has_value());

  const Rect last = fixture.view->track_rect(2);
  EXPECT_FALSE(fixture.view->drop_at(last.x + 50.0, last.bottom() + 20.0).has_value());
}

TEST(Timeline, ADropOnAnAudioTrackIsStillADrop) {
  // Whether an audio track can take what was dropped is a question about the
  // project, and the timeline does not answer those.
  const Fixture fixture;
  const Rect row = fixture.view->track_rect(2);
  ASSERT_TRUE(fixture.view->model().tracks[2].audio);

  const auto where = fixture.view->drop_at(fixture.view->time_area().x + 200.0, row.y + 5.0);
  ASSERT_TRUE(where.has_value());
  EXPECT_EQ(where->track, 2u);
}

// -------------------------------------------------------------- scrubbing --

TEST(Timeline, ClickingTheRulerMovesThePlayhead) {
  Fixture fixture;
  const Rect ruler = fixture.view->ruler_area();

  fixture.host->mouse_down(press(ruler.x + 350.0, ruler.y + 5.0));
  EXPECT_NEAR(fixture.view->playhead(), 3.5, 1.0 / kFps);
}

TEST(Timeline, ThePlayheadLandsOnAFrame) {
  // A playhead between two frames refers to a picture that does not exist.
  Fixture fixture;
  const Rect ruler = fixture.view->ruler_area();
  fixture.host->mouse_down(press(ruler.x + 337.0, ruler.y + 5.0));

  const double frames = fixture.view->playhead() * kFps;
  EXPECT_NEAR(frames, std::round(frames), 1e-6);
}

TEST(Timeline, ScrubbingKeepsWorkingOffTheRuler) {
  // A scrub spends most of its life with the pointer somewhere else entirely,
  // which only works because the press captured it.
  Fixture fixture;
  const Rect ruler = fixture.view->ruler_area();

  fixture.host->mouse_down(press(ruler.x + 100.0, ruler.y + 5.0));
  ASSERT_EQ(fixture.host->captured(), fixture.view);

  fixture.host->mouse_move(press(ruler.x + 600.0, 5000.0));
  EXPECT_NEAR(fixture.view->playhead(), 6.0, 1.0 / kFps);

  fixture.host->mouse_up(press(ruler.x + 600.0, 5000.0));
  fixture.host->mouse_move(press(ruler.x + 900.0, 5000.0));
  EXPECT_NEAR(fixture.view->playhead(), 6.0, 1.0 / kFps) << "it kept scrubbing after release";
}

TEST(Timeline, ScrubbingNeverGoesBeforeZero) {
  Fixture fixture;
  const Rect ruler = fixture.view->ruler_area();
  fixture.host->mouse_down(press(ruler.x + 100.0, ruler.y + 5.0));
  fixture.host->mouse_move(press(-4000.0, ruler.y + 5.0));

  EXPECT_DOUBLE_EQ(fixture.view->playhead(), 0.0);
}

TEST(Timeline, EveryScrubIsReported) {
  Fixture fixture;
  int calls = 0;
  double last = -1.0;
  fixture.view->set_on_scrub([&](double at) {
    ++calls;
    last = at;
  });

  const Rect ruler = fixture.view->ruler_area();
  fixture.host->mouse_down(press(ruler.x + 100.0, ruler.y + 5.0));
  fixture.host->mouse_move(press(ruler.x + 300.0, ruler.y + 5.0));

  EXPECT_EQ(calls, 2);
  EXPECT_NEAR(last, 3.0, 1.0 / kFps);
}

TEST(Timeline, ThePlayheadSitsWhereItsTimeSays) {
  Fixture fixture;
  fixture.view->set_playhead(4.0);
  EXPECT_DOUBLE_EQ(fixture.view->playhead_x(), fixture.view->time_area().x + 400.0);
}

// ------------------------------------------------------- wheel and zoom --

TEST(Timeline, ControlWheelZoomsAboutThePointer) {
  Fixture fixture;
  const double x = fixture.view->time_area().x + 300.0;
  const double under = fixture.view->scale().to_time(300.0);

  fixture.host->wheel(WheelEvent{
      .x = x, .y = 200.0, .delta_y = -1.0, .modifiers = Modifiers{.control = true}});

  EXPECT_GT(fixture.view->scale().pixels_per_second, 100.0);
  EXPECT_NEAR(fixture.view->scale().to_time(300.0), under, 1e-9);
}

TEST(Timeline, ShiftWheelMovesThroughTime) {
  Fixture fixture;
  fixture.host->wheel(WheelEvent{.x = 500.0,
                                 .y = 200.0,
                                 .delta_y = 1.0,
                                 .modifiers = Modifiers{.shift = true}});
  EXPECT_GT(fixture.view->scale().start, 0.0);
}

TEST(Timeline, TheWheelScrollsTheTracksWhenThereAreMoreThanFit) {
  Fixture fixture;
  // A short view, so three tracks cannot all be shown.
  fixture.host->resize(Rect{0.0, 0.0, 1000.0, 90.0}, flat_context());
  ASSERT_TRUE(fixture.view->vertical().scrollable());

  const double before = fixture.view->track_rect(0).y;
  fixture.host->wheel(WheelEvent{.x = 500.0, .y = 60.0, .delta_y = 1.0});

  EXPECT_LT(fixture.view->track_rect(0).y, before);
}

TEST(Timeline, TheWheelMovesThroughTimeWhenTheTracksAlreadyFit) {
  // So the gesture always does something rather than silently doing nothing.
  Fixture fixture;
  ASSERT_FALSE(fixture.view->vertical().scrollable());

  EXPECT_TRUE(fixture.host->wheel(WheelEvent{.x = 500.0, .y = 200.0, .delta_y = 1.0}));
  EXPECT_GT(fixture.view->scale().start, 0.0);
}

TEST(Timeline, TheWheelBubblesOnceItCannotMoveFurther) {
  Fixture fixture;
  fixture.view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = 12.0});
  EXPECT_FALSE(fixture.host->wheel(WheelEvent{.x = 500.0, .y = 200.0, .delta_y = 1.0}));
}

TEST(Timeline, FittingPutsTheWholeProjectOnScreen) {
  Fixture fixture;
  fixture.view->zoom_to_fit();

  const double width = fixture.view->time_area().width;
  EXPECT_NEAR(fixture.view->scale().to_x(12.0), width, 1e-6);
}

// --------------------------------------------------------------- snapping --

TEST(Snapping, CollectsTheStartThePlayheadAndEveryClipEdge) {
  const std::vector<double> points = snap_points(sample_model(), 7.5, std::nullopt);

  for (const double want : {0.0, 7.5, 2.0, 6.0, 5.0, 12.0}) {
    EXPECT_NE(std::ranges::find(points, want), points.end()) << "missing " << want;
  }
  EXPECT_TRUE(std::ranges::is_sorted(points));
}

TEST(Snapping, AClipNeverSnapsToItself) {
  // Its own edges are the ones following the pointer. Leaving them in pins the
  // drag exactly where it started, which looks like the timeline is frozen.
  const std::vector<double> points = snap_points(sample_model(), 0.0, BlockRef{0, 0});
  EXPECT_EQ(std::ranges::find(points, 2.0), points.end());
  EXPECT_EQ(std::ranges::find(points, 6.0), points.end());
}

TEST(Snapping, PicksTheNearestWithinTolerance) {
  const std::vector<double> points{0.0, 5.0, 12.0};
  EXPECT_EQ(nearest_snap(points, 5.2, 0.5), 5.0);
  EXPECT_EQ(nearest_snap(points, 4.6, 0.5), 5.0);
  EXPECT_FALSE(nearest_snap(points, 8.0, 0.5).has_value());
}

TEST(Snapping, ATieGoesToTheEarlierPoint) {
  // Otherwise a clip dropped exactly between two edges lands wherever
  // iteration order happened to put it.
  const std::vector<double> points{4.0, 6.0};
  EXPECT_EQ(nearest_snap(points, 5.0, 2.0), 4.0);
}

TEST(Snapping, NoToleranceMeansNoSnapping) {
  const std::vector<double> points{0.0, 5.0};
  EXPECT_FALSE(nearest_snap(points, 5.0, 0.0).has_value());
}

// ---------------------------------------------------------------- drags --

TEST(Timeline, TheEdgesOfAClipTrimAndTheMiddleMoves) {
  const Fixture fixture;
  // "wide", 0s to 5s, which is wholly on screen — a clip whose far edge is
  // scrolled out cannot be hit tested there at all.
  const Rect box = fixture.view->block_rect(1, 0);
  ASSERT_LT(box.right(), fixture.view->time_area().right());
  const double y = box.y + 5.0;

  EXPECT_EQ(fixture.view->zone_at(box.x + 2.0, y), DragMode::TrimStart);
  EXPECT_EQ(fixture.view->zone_at(box.right() - 2.0, y), DragMode::TrimEnd);
  EXPECT_EQ(fixture.view->zone_at(box.x + box.width / 2.0, y), DragMode::Move);
  EXPECT_EQ(fixture.view->zone_at(box.x + 2.0, 5.0), DragMode::None) << "that is the ruler";
}

TEST(Timeline, AShortClipIsStillMostlyMoveHandle) {
  // A clip that was all trim handle could never be picked up and slid.
  Fixture fixture;
  fixture.view->set_scale(TimeScale{.pixels_per_second = 4.0});
  const Rect box = fixture.view->block_rect(0, 0);
  ASSERT_LT(box.width, 24.0);

  EXPECT_EQ(fixture.view->zone_at(box.x + box.width / 2.0, box.y + 5.0), DragMode::Move);
}

TEST(Timeline, AClickDoesNotNudgeAClip) {
  // Without a threshold, selecting a clip moves it by however much the hand
  // wobbled between press and release.
  Fixture fixture;
  const Rect box = fixture.view->block_rect(1, 0);
  const TimelineBlock before = fixture.view->model().tracks[1].blocks[0];

  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 41.0, box.y + 10.0));
  fixture.host->mouse_up(press(box.x + 41.0, box.y + 10.0));

  // Position only: the click did select it, which is exactly what it was for.
  const TimelineBlock& after = fixture.view->model().tracks[1].blocks[0];
  EXPECT_DOUBLE_EQ(after.start, before.start);
  EXPECT_DOUBLE_EQ(after.end, before.end);
}

TEST(Timeline, DraggingAClipSlidesItAndKeepsItsLength) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);  // 2s to 6s
  const double length = 4.0;

  fixture.host->mouse_down(press(box.x + 100.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 400.0, box.y + 10.0));  // +3s at 100 px/s

  const TimelineBlock& block = fixture.view->model().tracks[0].blocks[0];
  EXPECT_NEAR(block.start, 5.0, 1.0 / kFps);
  EXPECT_NEAR(block.duration(), length, 1e-9) << "the clip changed length while moving";
}

TEST(Timeline, AClipCannotBeDraggedBeforeTheStart) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.x + 100.0, box.y + 10.0));
  fixture.host->mouse_move(press(-5000.0, box.y + 10.0));

  EXPECT_DOUBLE_EQ(fixture.view->model().tracks[0].blocks[0].start, 0.0);
  EXPECT_NEAR(fixture.view->model().tracks[0].blocks[0].duration(), 4.0, 1e-9);
}

TEST(Timeline, PositionsComeFromWhereTheDragStarted) {
  // Computed from the press rather than from the last position, so rounding
  // cannot accumulate over a long drag. Out and back must land exactly home.
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(1, 0);
  const TimelineBlock before = fixture.view->model().tracks[1].blocks[0];

  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  for (double x = 60.0; x < 900.0; x += 37.0) {
    fixture.host->mouse_move(press(box.x + x, box.y + 10.0));
  }
  fixture.host->mouse_move(press(box.x + 50.0, box.y + 10.0));

  const TimelineBlock& after = fixture.view->model().tracks[1].blocks[0];
  EXPECT_DOUBLE_EQ(after.start, before.start) << "the drag drifted on the way out and back";
  EXPECT_DOUBLE_EQ(after.end, before.end);
}

TEST(Timeline, TrimmingTheStartLeavesTheEndAlone) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);  // 2s to 6s
  const double end = 6.0;

  fixture.host->mouse_down(press(box.x + 1.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 101.0, box.y + 10.0));  // +1s

  const TimelineBlock& block = fixture.view->model().tracks[0].blocks[0];
  EXPECT_NEAR(block.start, 3.0, 1.0 / kFps);
  EXPECT_DOUBLE_EQ(block.end, end);
}

TEST(Timeline, TrimmingTheEndLeavesTheStartAlone) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.right() - 1.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.right() + 199.0, box.y + 10.0));  // +2s

  const TimelineBlock& block = fixture.view->model().tracks[0].blocks[0];
  EXPECT_DOUBLE_EQ(block.start, 2.0);
  EXPECT_NEAR(block.end, 8.0, 1.0 / kFps);
}

TEST(Timeline, AClipCannotBeTrimmedOutOfExistence) {
  // One frame is the floor. A clip trimmed to nothing vanishes, and there is
  // then nothing left to drag back.
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.x + 1.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.right() + 4000.0, box.y + 10.0));

  const TimelineBlock& block = fixture.view->model().tracks[0].blocks[0];
  EXPECT_GE(block.duration(), 1.0 / kFps - 1e-9);
  EXPECT_LE(block.start, block.end);
}

TEST(Timeline, TrimmingTheEndBackwardsStopsAtTheStart) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.right() - 1.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x - 4000.0, box.y + 10.0));

  const TimelineBlock& block = fixture.view->model().tracks[0].blocks[0];
  EXPECT_GE(block.duration(), 1.0 / kFps - 1e-9);
}

TEST(Timeline, ADraggedClipSticksToTheOneBeforeIt) {
  // V1 has clips meeting at 5s. Dragging V2's clip near that edge should
  // click onto it rather than landing a few frames off.
  Fixture fixture;
  const Rect box = fixture.view->block_rect(0, 0);  // 2s to 6s

  // Aim for a start of about 5.03s, which is inside the snap distance of 5s.
  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 353.0, box.y + 10.0));

  EXPECT_DOUBLE_EQ(fixture.view->model().tracks[0].blocks[0].start, 5.0);
}

TEST(Timeline, SnappingCanBeTurnedOff) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 353.0, box.y + 10.0));

  EXPECT_NE(fixture.view->model().tracks[0].blocks[0].start, 5.0);
}

TEST(Timeline, ADraggedClipSticksToThePlayhead) {
  Fixture fixture;
  fixture.view->set_playhead(9.0);
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 754.0, box.y + 10.0));

  EXPECT_DOUBLE_EQ(fixture.view->model().tracks[0].blocks[0].start, 9.0);
}

TEST(Timeline, AnEditIsReportedOnceWhenTheDragEnds) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  int edits = 0;
  TimelineEdit last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) {
    ++edits;
    last = edit;
  });

  const Rect box = fixture.view->block_rect(0, 0);
  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 150.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 250.0, box.y + 10.0));
  EXPECT_EQ(edits, 0) << "an edit during the drag would fill the undo stack";

  fixture.host->mouse_up(press(box.x + 250.0, box.y + 10.0));
  EXPECT_EQ(edits, 1);
  EXPECT_EQ(last.block, (BlockRef{0, 0}));
  EXPECT_NEAR(last.result.start, 4.0, 1.0 / kFps);
  // The mode is reported rather than inferred: moving a clip and trimming
  // both its edges by the same amount leave the same numbers behind.
  EXPECT_EQ(last.mode, DragMode::Move);
}

TEST(Timeline, AClickReportsNoEdit) {
  Fixture fixture;
  int edits = 0;
  fixture.view->set_on_edit([&](const TimelineEdit&) { ++edits; });

  const Rect box = fixture.view->block_rect(0, 0);
  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  fixture.host->mouse_up(press(box.x + 50.0, box.y + 10.0));

  EXPECT_EQ(edits, 0);
}

TEST(Timeline, ADragSurvivesThePointerLeavingTheWidget) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  ASSERT_EQ(fixture.host->captured(), fixture.view);

  fixture.host->mouse_move(press(box.x + 350.0, -9000.0));
  EXPECT_NEAR(fixture.view->model().tracks[0].blocks[0].start, 5.0, 1.0 / kFps);
}

TEST(Timeline, ReleasingEndsTheDrag) {
  Fixture fixture;
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 350.0, box.y + 10.0));
  fixture.host->mouse_up(press(box.x + 350.0, box.y + 10.0));

  EXPECT_EQ(fixture.view->drag_mode(), DragMode::None);
  EXPECT_FALSE(fixture.view->dragging().has_value());

  const double settled = fixture.view->model().tracks[0].blocks[0].start;
  fixture.host->mouse_move(press(box.x + 900.0, box.y + 10.0));
  EXPECT_DOUBLE_EQ(fixture.view->model().tracks[0].blocks[0].start, settled);
}

TEST(Timeline, DraggedEdgesLandOnFrames) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  const Rect box = fixture.view->block_rect(0, 0);

  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 237.0, box.y + 10.0));

  const double frames = fixture.view->model().tracks[0].blocks[0].start * kFps;
  EXPECT_NEAR(frames, std::round(frames), 1e-6);
}

// --------------------------------------------------------------- painting --

TEST(Timeline, DrawsClipsInTheThemesClipStyle) {
  const Fixture fixture;
  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  bool found = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Fill &&
        call.fill == default_theme().style(Part::Clip).fill) {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "no clip was drawn in the theme's clip style";
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(Timeline, ASelectedClipIsDrawnDifferently) {
  Fixture fixture;
  const auto anything_filled_with = [&](const Fill& want) {
    RecordingPainter painter;
    fixture.view->paint(painter, default_theme());
    for (const DrawCall& call : painter.calls()) {
      if (call.kind == DrawCall::Kind::Fill && call.fill == want) return true;
    }
    return false;
  };

  const Fill selected = default_theme().style(Part::Clip, State::Selected).fill;
  ASSERT_NE(selected, default_theme().style(Part::Clip, State::Normal).fill)
      << "the theme draws a selected clip the same as an unselected one";

  EXPECT_FALSE(anything_filled_with(selected)) << "something was already drawn as selected";
  fixture.view->select(BlockRef{1, 0});
  EXPECT_TRUE(anything_filled_with(selected));
}

TEST(Timeline, TheRulerIsMarkedAndLabelled) {
  const Fixture fixture;
  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  EXPECT_GT(painter.count(DrawCall::Kind::Line), 0u) << "the ruler has no ticks";
  EXPECT_GT(painter.count(DrawCall::Kind::Text), 0u) << "the ruler has no labels";
}

TEST(Timeline, ThePlayheadIsDrawnInsideTheTimeAreaOnly) {
  // Otherwise it draws a red line down the middle of the track headers.
  Fixture fixture;
  fixture.view->set_playhead(3.0);

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  bool clipped = false;
  int depth = 0;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::PushClip) ++depth;
    if (call.kind == DrawCall::Kind::PopClip) --depth;
    if (call.kind == DrawCall::Kind::Line && call.bounds.height != 0.0 && depth > 0) {
      clipped = true;
    }
  }
  EXPECT_TRUE(clipped) << "the playhead was drawn outside any clip";
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(Timeline, ClipsScrolledOutOfViewAreNotDrawn) {
  // A long project has far more clips off screen than on it.
  Fixture fixture;
  TimelineModel model = sample_model();
  for (int i = 0; i < 500; ++i) {
    model.tracks[0].blocks.push_back(
        TimelineBlock{.start = 100.0 + i * 5.0, .end = 104.0 + i * 5.0, .label = "far"});
  }
  fixture.view->set_model(model);

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());
  EXPECT_LT(painter.calls().size(), 200u) << "everything off screen was drawn anyway";
}

TEST(Timeline, AMutedTrackHeaderLooksDisabled) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[2].muted = true;
  fixture.view->set_model(model);

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  bool found = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Fill &&
        call.fill == default_theme().style(Part::TrackHeader, State::Disabled).fill) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

// -------------------------------------------------------- transitions --

TEST(Transitions, AreNotDrawnWhenThereAreNone) {
  const Fixture fixture;
  EXPECT_TRUE(fixture.view->transition_rect(1, 0).empty());
}

TEST(Transitions, StraddleTheCutTheySitOn) {
  // Half either side, which is where the model puts one.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[1].blocks[0].transition = BlockTransition{.duration = 2.0, .label = "Dissolve"};
  fixture.view->set_model(model);

  const Rect box = fixture.view->transition_rect(1, 0);
  const Rect outgoing = fixture.view->block_rect(1, 0);
  ASSERT_FALSE(box.empty());

  EXPECT_DOUBLE_EQ(box.x, outgoing.right() - 100.0) << "one second before the cut";
  EXPECT_DOUBLE_EQ(box.right(), outgoing.right() + 100.0) << "and one after";
  EXPECT_DOUBLE_EQ(box.y, outgoing.y);
  EXPECT_DOUBLE_EQ(box.height, outgoing.height);
}

TEST(Transitions, AreDrawnOverBothClipsTheyJoin) {
  // Drawn after every block on the track, so the incoming half is not painted
  // over by the clip it reaches into.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[1].blocks[0].transition = BlockTransition{.duration = 2.0};
  fixture.view->set_model(model);

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  const Rect box = fixture.view->transition_rect(1, 0);
  int last_block = -1;
  int the_transition = -1;
  for (int i = 0; i < static_cast<int>(painter.calls().size()); ++i) {
    const DrawCall& call = painter.calls()[static_cast<std::size_t>(i)];
    if (call.kind != DrawCall::Kind::Fill) continue;
    if (call.bounds == fixture.view->block_rect(1, 1).inset(1.0)) last_block = i;
    if (call.bounds == box.inset(1.0)) the_transition = i;
  }
  ASSERT_GE(last_block, 0);
  ASSERT_GE(the_transition, 0);
  EXPECT_GT(the_transition, last_block);
}

TEST(Transitions, CarryADiagonal) {
  // What every editor draws a transition as: it says "one becomes the other
  // across here" in a way no colour does.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[1].blocks[0].transition = BlockTransition{.duration = 2.0};
  fixture.view->set_model(model);

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  const Rect box = fixture.view->transition_rect(1, 0);
  bool found = false;
  for (const DrawCall& call : painter.calls()) {
    // A line is stored as its first point plus an offset to the second, so a
    // diagonal has extent in both directions and rises as it goes right.
    if (call.kind != DrawCall::Kind::Line) continue;
    if (call.bounds.width > 0.0 && call.bounds.height < 0.0 &&
        std::abs(call.bounds.width - (box.width - 2.0)) < 1e-9) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

/// Whether a transition's name was drawn anywhere.
[[nodiscard]] bool names_the_transition(const TimelineView& view, std::string_view label) {
  RecordingPainter painter;
  view.paint(painter, default_theme());
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Text && call.run.has_value() && call.run->text == label) {
      return true;
    }
  }
  return false;
}

TEST(Transitions, ShowTheirNameWhenThereIsRoom) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[1].blocks[0].transition = BlockTransition{.duration = 4.0, .label = "Push"};
  fixture.view->set_model(model);

  EXPECT_TRUE(names_the_transition(*fixture.view, "Push"));

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(Transitions, AreSilentWhenTheNameWouldNotFit) {
  // Measured rather than guessed at. A name centred in a box too small for it
  // overflows both ends, and what survives the clip is a word missing its first
  // and last letters sitting on top of the clip's own label.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[1].blocks[0].transition =
      BlockTransition{.duration = 0.4, .label = "Cross Dissolve"};
  fixture.view->set_model(model);

  EXPECT_FALSE(names_the_transition(*fixture.view, "Cross Dissolve"));
  EXPECT_FALSE(fixture.view->transition_rect(1, 0).empty()) << "still drawn, just not named";
}

TEST(Transitions, TheSameNameFitsOnceThereIsRoomForIt) {
  // The pair that makes the one above about the measurement rather than about
  // the string.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[1].blocks[0].transition =
      BlockTransition{.duration = 4.0, .label = "Cross Dissolve"};
  fixture.view->set_model(model);

  EXPECT_TRUE(names_the_transition(*fixture.view, "Cross Dissolve"));
}

// ------------------------------------------------------- the marked span --

TEST(MarkedSpan, IsNotDrawnWhenNothingIsMarked) {
  // An unmarked sequence is the whole sequence, and a bar across all of it
  // would say something had been chosen when nothing had.
  const Fixture fixture;
  EXPECT_TRUE(fixture.view->marked_bar().empty());
}

TEST(MarkedSpan, RunsBetweenTheTwoMarks) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.in_point = 2.0;
  model.out_point = 5.0;
  fixture.view->set_model(model);

  const Rect bar = fixture.view->marked_bar();
  const Rect ruler = fixture.view->ruler_area();
  ASSERT_FALSE(bar.empty());
  EXPECT_DOUBLE_EQ(bar.x, ruler.x + 200.0) << "two seconds at a hundred a second";
  EXPECT_DOUBLE_EQ(bar.width, 300.0);
  EXPECT_LE(bar.bottom(), ruler.bottom());
  EXPECT_GE(bar.y, ruler.y);
}

TEST(MarkedSpan, AnInAloneReachesTheEndOfTheSequence) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.in_point = 2.0;
  fixture.view->set_model(model);

  // The sample runs to twelve seconds.
  EXPECT_DOUBLE_EQ(fixture.view->marked_bar().width, 1000.0);
}

TEST(MarkedSpan, AnOutAloneReachesBackToTheStart) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.out_point = 3.0;
  fixture.view->set_model(model);

  const Rect bar = fixture.view->marked_bar();
  EXPECT_DOUBLE_EQ(bar.x, fixture.view->ruler_area().x);
  EXPECT_DOUBLE_EQ(bar.width, 300.0);
}

TEST(MarkedSpan, IsDrawnInsideTheRulersClip) {
  // Otherwise a span scrolled off the left is painted across the track headers.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.in_point = 0.0;
  model.out_point = 12.0;
  fixture.view->set_model(model);

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());

  int depth = 0;
  bool inside = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::PushClip) ++depth;
    if (call.kind == DrawCall::Kind::PopClip) --depth;
    if (call.kind == DrawCall::Kind::Fill && call.bounds == fixture.view->marked_bar()) {
      inside = depth > 0;
    }
  }
  EXPECT_TRUE(inside);
}

// ---------------------------------------------------------------- markers --

TEST(Markers, SitOnTheRulerAtTheirTime) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.markers = {TimelineMarker{.time = 3.0, .label = "cue"}};
  fixture.view->set_model(model);

  const Rect tab = fixture.view->marker_rect(0);
  const Rect ruler = fixture.view->ruler_area();
  ASSERT_FALSE(tab.empty());

  EXPECT_NEAR(tab.x + tab.width * 0.5, ruler.x + 300.0, 1e-9);
  EXPECT_GE(tab.y, ruler.y);
  EXPECT_LE(tab.bottom(), ruler.bottom());
}

TEST(Markers, DoNotOverlapTheMarkedSpanAlongTheFoot) {
  // Both live on the ruler and both say where something is; they must not fight
  // for the same pixels.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.markers = {TimelineMarker{.time = 3.0}};
  model.in_point = 1.0;
  model.out_point = 6.0;
  fixture.view->set_model(model);

  EXPECT_LE(fixture.view->marker_rect(0).bottom(), fixture.view->marked_bar().y);
}

TEST(Markers, ScrolledOutOfSightAreNotDrawn) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.markers = {TimelineMarker{.time = 500.0}};
  fixture.view->set_model(model);

  EXPECT_TRUE(fixture.view->marker_rect(0).empty());
  EXPECT_TRUE(fixture.view->marker_rect(9).empty()) << "and neither is one that is not there";
}

TEST(Markers, WearTheirOwnColourWhenTheyHaveOne) {
  // What a marker's colour is for: somebody has said this one means something
  // the others do not.
  const auto fill_at = [](std::string color) {
    Fixture fixture;
    TimelineModel model = sample_model();
    model.markers = {TimelineMarker{.time = 3.0, .color = std::move(color)}};
    fixture.view->set_model(model);

    RecordingPainter painter;
    fixture.view->paint(painter, default_theme());
    for (const DrawCall& call : painter.calls()) {
      if (call.kind == DrawCall::Kind::Fill && call.bounds == fixture.view->marker_rect(0)) {
        return call.fill.color;
      }
    }
    return Color{};
  };

  EXPECT_EQ(fill_at("#ff0000"), (Color{1.0f, 0.0f, 0.0f, 1.0f}));
  EXPECT_NE(fill_at(""), (Color{1.0f, 0.0f, 0.0f, 1.0f})) << "and the theme's when it has none";
}

TEST(Markers, AreDrawnInsideTheRulersClip) {
  Fixture fixture;
  TimelineModel model = sample_model();
  model.markers = {TimelineMarker{.time = 3.0}};
  fixture.view->set_model(model);

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());

  int depth = 0;
  bool inside = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::PushClip) ++depth;
    if (call.kind == DrawCall::Kind::PopClip) --depth;
    if (call.kind == DrawCall::Kind::Fill && call.bounds == fixture.view->marker_rect(0)) {
      inside = depth > 0;
    }
  }
  EXPECT_TRUE(inside);
}

// --------------------------------------------------- the header switches --

TEST(TrackSwitch, AudioAndVideoTracksOfferDifferentOnes) {
  // A solo on a video track would mean nothing and a mute on one even less.
  const Fixture fixture;

  EXPECT_TRUE(fixture.view->has_control(0, TrackControl::Hide)) << "V2";
  EXPECT_TRUE(fixture.view->has_control(0, TrackControl::Lock));
  EXPECT_FALSE(fixture.view->has_control(0, TrackControl::Mute));
  EXPECT_FALSE(fixture.view->has_control(0, TrackControl::Solo));

  EXPECT_TRUE(fixture.view->has_control(2, TrackControl::Mute)) << "A1";
  EXPECT_TRUE(fixture.view->has_control(2, TrackControl::Solo));
  EXPECT_TRUE(fixture.view->has_control(2, TrackControl::Lock));
  EXPECT_FALSE(fixture.view->has_control(2, TrackControl::Hide));
}

TEST(TrackSwitch, SitInTheirOwnHeaderAndNowhereElse) {
  const Fixture fixture;
  const Rect header = fixture.view->header_rect(2);

  for (const TrackControl control :
       {TrackControl::Mute, TrackControl::Solo, TrackControl::Lock}) {
    const Rect box = fixture.view->control_rect(2, control);
    ASSERT_FALSE(box.empty()) << to_string(control);
    EXPECT_GE(box.x, header.x);
    EXPECT_LE(box.right(), header.right());
    EXPECT_GE(box.y, header.y);
    EXPECT_LE(box.bottom(), header.bottom());
  }
}

TEST(TrackSwitch, DoNotOverlapEachOther) {
  const Fixture fixture;
  const Rect mute = fixture.view->control_rect(2, TrackControl::Mute);
  const Rect solo = fixture.view->control_rect(2, TrackControl::Solo);
  const Rect lock = fixture.view->control_rect(2, TrackControl::Lock);

  EXPECT_LE(mute.right(), solo.x);
  EXPECT_LE(solo.right(), lock.x);
}

TEST(TrackSwitch, AreNotOfferedWhenThereIsNoRoomForThem) {
  // Refused rather than clipped: a switch drawn half outside its own header, or
  // over the track's name, is worse than one that admits there is no room.
  Fixture fixture;
  fixture.host->resize(Rect{0.0, 0.0, 30.0, 400.0}, flat_context());

  EXPECT_TRUE(fixture.view->control_rect(2, TrackControl::Mute).empty());
  EXPECT_FALSE(fixture.view->control_at(5.0, 100.0).has_value());
}

TEST(TrackSwitch, APressFlipsItAndSaysSo) {
  Fixture fixture;
  std::optional<TrackControlRef> toggled;
  fixture.view->set_on_track_toggle([&](TrackControlRef which) { toggled = which; });

  const Rect box = fixture.view->control_rect(2, TrackControl::Mute);
  ASSERT_FALSE(box.empty());
  fixture.host->mouse_down(press(box.x + 2.0, box.y + 2.0));

  ASSERT_TRUE(toggled.has_value());
  EXPECT_EQ(*toggled, (TrackControlRef{.track = 2, .control = TrackControl::Mute}));
  // Flipped in the view as well, so the press shows on the frame it happened
  // rather than only once the document has come back round.
  EXPECT_TRUE(fixture.view->model().tracks[2].switches.mute);
}

TEST(TrackSwitch, APressOnOneDoesNotScrubOrSelect) {
  Fixture fixture;
  int scrubs = 0;
  int selections = 0;
  fixture.view->set_on_scrub([&](double) { ++scrubs; });
  fixture.view->set_on_select([&](std::optional<BlockRef>) { ++selections; });

  const Rect box = fixture.view->control_rect(2, TrackControl::Solo);
  fixture.host->mouse_down(press(box.x + 2.0, box.y + 2.0));

  EXPECT_EQ(scrubs, 0);
  EXPECT_EQ(selections, 0);
  EXPECT_EQ(fixture.view->drag_mode(), DragMode::None);
}

TEST(TrackSwitch, TheHeaderBesideThemStillDoesNothing) {
  // Only the switches are live. A press on the name is not a mute.
  Fixture fixture;
  std::optional<TrackControlRef> toggled;
  fixture.view->set_on_track_toggle([&](TrackControlRef which) { toggled = which; });

  const Rect header = fixture.view->header_rect(2);
  fixture.host->mouse_down(press(header.right() - 2.0, header.y + 2.0));
  EXPECT_FALSE(toggled.has_value());
}

TEST(TrackSwitch, ALitSwitchLooksDifferentFromADarkOne) {
  const auto fills = [](bool on) {
    Fixture fixture;
    TimelineModel model = sample_model();
    model.tracks[2].switches.mute = on;
    fixture.view->set_model(model);

    RecordingPainter painter;
    fixture.view->paint(painter, default_theme());
    std::vector<Fill> found;
    for (const DrawCall& call : painter.calls()) {
      if (call.kind == DrawCall::Kind::Fill) found.push_back(call.fill);
    }
    return found;
  };

  EXPECT_NE(fills(true), fills(false));
}

TEST(TrackSwitch, IsDrawnAsALetterRatherThanAPictogram) {
  // A padlock and an eye both need arcs the painter has no other use for, and
  // at twelve pixels a drawn padlock is a grey smudge.
  const Fixture fixture;
  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  std::vector<std::string> letters;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind != DrawCall::Kind::Text || !call.run.has_value()) continue;
    if (call.run->text.size() == 1) letters.push_back(call.run->text);
  }
  EXPECT_NE(std::ranges::find(letters, "M"), letters.end());
  EXPECT_NE(std::ranges::find(letters, "S"), letters.end());
  EXPECT_NE(std::ranges::find(letters, "L"), letters.end());
  EXPECT_NE(std::ranges::find(letters, "H"), letters.end());
}

TEST(TrackSwitch, WhatIsLitIsWhatTheProjectHoldsNotWhatTakesEffect) {
  // The distinction the two fields exist for. A track silenced because another
  // is soloed is not muted, and lighting its M would leave somebody pressing a
  // button that is already off.
  Fixture fixture;
  TimelineModel model = sample_model();
  model.tracks[2].muted = true;              // not heard
  model.tracks[2].switches.mute = false;     // but nobody muted it
  fixture.view->set_model(model);

  EXPECT_TRUE(fixture.view->model().tracks[2].muted);
  EXPECT_FALSE(fixture.view->model().tracks[2].switches.mute);
}

// ------------------------------------------------------------- the tools --

TEST(Tools, TheSelectionToolIsTheDefault) {
  const Fixture fixture;
  EXPECT_EQ(fixture.view->tool(), Tool::Selection);
}

TEST(Tools, EachToolSaysWhatAPressOnTheMiddleOfAClipMeans) {
  Fixture fixture;
  const Rect box = fixture.view->block_rect(0, 0);
  const double middle = box.x + box.width * 0.5;
  const double y = box.y + 10.0;

  fixture.view->set_tool(Tool::Selection);
  EXPECT_EQ(fixture.view->zone_at(middle, y), DragMode::Move);
  fixture.view->set_tool(Tool::Razor);
  EXPECT_EQ(fixture.view->zone_at(middle, y), DragMode::Razor);
  fixture.view->set_tool(Tool::Slip);
  EXPECT_EQ(fixture.view->zone_at(middle, y), DragMode::Slip);
  fixture.view->set_tool(Tool::Slide);
  EXPECT_EQ(fixture.view->zone_at(middle, y), DragMode::Slide);
}

TEST(Tools, AToolStillDoesNothingOffAClip) {
  Fixture fixture;
  fixture.view->set_tool(Tool::Razor);
  const Rect tracks = fixture.view->tracks_area();
  EXPECT_EQ(fixture.view->zone_at(tracks.right() - 2.0, tracks.bottom() - 2.0), DragMode::None);
}

TEST(Tools, RateStretchTakesWhicheverEndIsNearer) {
  // Halves rather than the trim handles: a rate stretch has nothing to do in
  // the middle, so a dead zone there would be a tool ignoring most of what it
  // is pointed at.
  Fixture fixture;
  fixture.view->set_tool(Tool::RateStretch);
  const Rect box = fixture.view->block_rect(0, 0);
  const double y = box.y + 10.0;

  EXPECT_EQ(fixture.view->zone_at(box.x + box.width * 0.25, y), DragMode::RateStart);
  EXPECT_EQ(fixture.view->zone_at(box.x + box.width * 0.75, y), DragMode::RateEnd);
}

TEST(Tools, TheSelectionToolsHandlesAreStillTheEdges) {
  Fixture fixture;
  const Rect box = fixture.view->block_rect(0, 0);
  const double y = box.y + 10.0;

  EXPECT_EQ(fixture.view->zone_at(box.x + 1.0, y), DragMode::TrimStart);
  EXPECT_EQ(fixture.view->zone_at(box.right() - 1.0, y), DragMode::TrimEnd);
}

TEST(Tools, WhichEdgeIsBeingPulledIsTheSameQuestionForTrimAndRate) {
  EXPECT_TRUE(pulls_start(DragMode::TrimStart));
  EXPECT_TRUE(pulls_start(DragMode::RateStart));
  EXPECT_TRUE(pulls_end(DragMode::TrimEnd));
  EXPECT_TRUE(pulls_end(DragMode::RateEnd));
  EXPECT_FALSE(pulls_start(DragMode::Move));
  EXPECT_FALSE(pulls_end(DragMode::Slide));
}

TEST(Tools, EveryToolHasAName) {
  // Used for widget names, so a palette button can be found in a test.
  EXPECT_EQ(to_string(Tool::Selection), "selection");
  EXPECT_EQ(to_string(Tool::Razor), "razor");
  EXPECT_EQ(to_string(Tool::RateStretch), "rate");
  EXPECT_EQ(to_string(Tool::Slip), "slip");
  EXPECT_EQ(to_string(Tool::Slide), "slide");
}

// ----------------------------------------------------------------- razor --

TEST(Razor, CutsOnThePressWithNothingToFollow) {
  Fixture fixture;
  fixture.view->set_tool(Tool::Razor);

  std::vector<TimelineEdit> edits;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { edits.push_back(edit); });

  const Rect box = fixture.view->block_rect(1, 0);
  fixture.host->mouse_down(press(box.x + 60.0, box.y + 10.0));

  ASSERT_EQ(edits.size(), 1u) << "a cut has no release to wait for";
  EXPECT_EQ(edits[0].mode, DragMode::Razor);
  EXPECT_EQ(edits[0].block, (BlockRef{1, 0}));
  EXPECT_NEAR(edits[0].at, 0.6, 0.02) << "sixty pixels at a hundred a second";
  EXPECT_FALSE(edits[0].all_tracks);

  fixture.host->mouse_up(press(box.x + 60.0, box.y + 10.0));
  EXPECT_EQ(edits.size(), 1u) << "and nothing more on the way up";
}

TEST(Razor, DoesNotSelectWhatItCuts) {
  // The tool is used repeatedly. Leaving one of the two halves highlighted
  // after every cut is a running commentary nobody asked for.
  Fixture fixture;
  fixture.view->set_tool(Tool::Razor);
  int selections = 0;
  fixture.view->set_on_select([&](std::optional<BlockRef>) { ++selections; });

  const Rect box = fixture.view->block_rect(1, 0);
  fixture.host->mouse_down(press(box.x + 60.0, box.y + 10.0));

  EXPECT_EQ(selections, 0);
  EXPECT_FALSE(fixture.view->selection().has_value());
}

TEST(Razor, DoesNotMoveTheClipItIsDraggedAcross) {
  Fixture fixture;
  fixture.view->set_tool(Tool::Razor);
  const TimelineBlock before = fixture.view->model().tracks[1].blocks[0];
  const Rect box = fixture.view->block_rect(1, 0);

  fixture.host->mouse_down(press(box.x + 60.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 260.0, box.y + 10.0));
  fixture.host->mouse_up(press(box.x + 260.0, box.y + 10.0));

  EXPECT_EQ(fixture.view->model().tracks[1].blocks[0], before);
}

TEST(Razor, ShiftAsksForEveryTrack) {
  Fixture fixture;
  fixture.view->set_tool(Tool::Razor);
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect box = fixture.view->block_rect(1, 0);
  MouseEvent event = press(box.x + 60.0, box.y + 10.0);
  event.modifiers.shift = true;
  fixture.host->mouse_down(event);

  ASSERT_TRUE(last.has_value());
  EXPECT_TRUE(last->all_tracks);
}

// ---------------------------------------------------------- rate stretch --

TEST(RateStretch, CanPullAnEdgePastTheEndOfTheClipsOwnLength) {
  // A trim would run out of source; this changes the speed instead, so the
  // view must let the edge go wherever the pointer does.
  Fixture fixture;
  fixture.view->set_snapping(false);
  fixture.view->set_tool(Tool::RateStretch);

  // The wide clip: 0 to 5 on V1, so both its edges are on screen.
  const Rect box = fixture.view->block_rect(1, 0);
  fixture.host->mouse_down(press(box.right() - 5.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.right() + 300.0, box.y + 10.0));

  const TimelineBlock& block = fixture.view->model().tracks[1].blocks[0];
  EXPECT_NEAR(block.end, 8.05, 0.05) << "three hundred and five pixels further on";
  EXPECT_DOUBLE_EQ(block.start, 0.0);
}

TEST(RateStretch, KeepsAtLeastAFrame) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  fixture.view->set_tool(Tool::RateStretch);

  const Rect box = fixture.view->block_rect(1, 0);
  fixture.host->mouse_down(press(box.right() - 5.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x - 4000.0, box.y + 10.0));

  const TimelineBlock& block = fixture.view->model().tracks[1].blocks[0];
  EXPECT_GE(block.duration(), 1.0 / kFps - 1e-9);
}

TEST(RateStretch, ReportsWhichEdgeWasPulled) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  fixture.view->set_tool(Tool::RateStretch);
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  // The close-up, 5 to 12: its in-edge pulled back a second, which is a
  // hundred pixels at this zoom.
  const Rect box = fixture.view->block_rect(1, 1);
  fixture.host->mouse_down(press(box.x + 5.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x - 95.0, box.y + 10.0));
  fixture.host->mouse_up(press(box.x - 95.0, box.y + 10.0));

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::RateStart);
  EXPECT_NEAR(last->result.start, 4.0, 0.02);
}

// ------------------------------------------------------------------ slip --

TEST(Slip, LeavesTheClipExactlyWhereItIs) {
  // The one mode with nothing to watch, and that is the point: what moves is
  // inside the block.
  Fixture fixture;
  fixture.view->set_tool(Tool::Slip);
  const std::vector<TimelineBlock> before = fixture.view->model().tracks[1].blocks;

  const Rect box = fixture.view->block_rect(1, 1);
  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 200.0, box.y + 10.0));

  // Geometry, not the blocks themselves: the press selected one, and being
  // selected is part of the model too.
  const std::vector<TimelineBlock>& after = fixture.view->model().tracks[1].blocks;
  ASSERT_EQ(after.size(), before.size());
  for (std::size_t i = 0; i < after.size(); ++i) {
    EXPECT_DOUBLE_EQ(after[i].start, before[i].start);
    EXPECT_DOUBLE_EQ(after[i].end, before[i].end);
  }
}

TEST(Slip, ReportsHowFarTheGestureWent) {
  Fixture fixture;
  fixture.view->set_tool(Tool::Slip);
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect box = fixture.view->block_rect(1, 1);
  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 160.0, box.y + 10.0));
  fixture.host->mouse_up(press(box.x + 160.0, box.y + 10.0));

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::Slip);
  // A hundred and twenty pixels at a hundred a second, and the block reported
  // is the block unchanged, because that is what the mode means.
  EXPECT_NEAR(last->delta, 1.2, 0.02);
  EXPECT_EQ(last->result, fixture.view->model().tracks[1].blocks[1]);
}

TEST(Slip, ReportsWholeFramesLikeEveryOtherEdit) {
  Fixture fixture;
  fixture.view->set_tool(Tool::Slip);
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect box = fixture.view->block_rect(1, 1);
  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  // Seventeen pixels: a third of a frame past a whole number of them.
  fixture.host->mouse_move(press(box.x + 57.0, box.y + 10.0));
  fixture.host->mouse_up(press(box.x + 57.0, box.y + 10.0));

  ASSERT_TRUE(last.has_value());
  const double frames = last->delta * kFps;
  EXPECT_NEAR(frames, std::round(frames), 1e-9);
}

// ----------------------------------------------------------------- slide --

TEST(Slide, TakesTheLengthOutOfItsNeighbours) {
  // V1 holds two abutting clips: 0 to 5 and 5 to 12. Sliding the second later
  // grows the first and leaves the end of the sequence alone.
  Fixture fixture;
  fixture.view->set_snapping(false);
  fixture.view->set_tool(Tool::Slide);

  const Rect box = fixture.view->block_rect(1, 1);
  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 140.0, box.y + 10.0));  // one second later

  const std::vector<TimelineBlock>& blocks = fixture.view->model().tracks[1].blocks;
  EXPECT_NEAR(blocks[1].start, 6.0, 0.02);
  EXPECT_NEAR(blocks[1].end, 13.0, 0.02);
  EXPECT_NEAR(blocks[0].end, 6.0, 0.02) << "the clip before grew into the gap";
  EXPECT_DOUBLE_EQ(blocks[0].start, 0.0);
}

TEST(Slide, LeavesTheClipItSlidesTheLengthItWas) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  fixture.view->set_tool(Tool::Slide);
  const double was = fixture.view->model().tracks[1].blocks[1].duration();

  const Rect box = fixture.view->block_rect(1, 1);
  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 220.0, box.y + 10.0));

  EXPECT_NEAR(fixture.view->model().tracks[1].blocks[1].duration(), was, 1e-9);
}

TEST(Slide, CannotEatTheClipBeforeIt) {
  // The neighbour that shrinks has to keep a frame, or it disappears and there
  // is nothing left to slide back into.
  Fixture fixture;
  fixture.view->set_snapping(false);
  fixture.view->set_tool(Tool::Slide);

  const Rect box = fixture.view->block_rect(1, 1);
  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x - 2000.0, box.y + 10.0));

  const std::vector<TimelineBlock>& blocks = fixture.view->model().tracks[1].blocks;
  EXPECT_GE(blocks[0].duration(), 1.0 / kFps - 1e-9);
  EXPECT_GE(blocks[1].start, blocks[0].start);
}

TEST(Slide, AClipWithNothingBesideItDoesNotMove) {
  // The insert on V2 has a gap either side, so there is nothing to take the
  // length out of — which is what the core says too, and if the view disagreed
  // it would preview a slide the project then refused.
  Fixture fixture;
  fixture.view->set_snapping(false);
  fixture.view->set_tool(Tool::Slide);
  const TimelineBlock before = fixture.view->model().tracks[0].blocks[0];

  const Rect box = fixture.view->block_rect(0, 0);
  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 200.0, box.y + 10.0));

  const TimelineBlock& after = fixture.view->model().tracks[0].blocks[0];
  EXPECT_DOUBLE_EQ(after.start, before.start);
  EXPECT_DOUBLE_EQ(after.end, before.end);
}

TEST(Slide, ReportsWhereTheClipEndedUp) {
  Fixture fixture;
  fixture.view->set_snapping(false);
  fixture.view->set_tool(Tool::Slide);
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect box = fixture.view->block_rect(1, 1);
  fixture.host->mouse_down(press(box.x + 40.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 140.0, box.y + 10.0));
  fixture.host->mouse_up(press(box.x + 140.0, box.y + 10.0));

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::Slide);
  EXPECT_NEAR(last->result.start, 6.0, 0.02);
}

// ------------------------------------------------------ the volume band --
//
// A clip's gain, as a line across its block. The arithmetic is the interesting
// half: gain is stored as a linear multiplier and drawn on a decibel scale,
// because a linear one puts every trim anybody actually makes within a few
// pixels of the top of a forty-pixel clip.

[[nodiscard]] MouseEvent alt_press(double x, double y) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left, .modifiers = {.alt = true}};
}

/// A model whose one audio clip carries a band. Deliberately separate from
/// `sample_model`, whose audio block has none: the band sits over the top of a
/// clip and takes presses that would otherwise move it, so the tests for moving
/// a clip must keep a clip with nothing on it.
[[nodiscard]] TimelineModel banded_model(GainBand band = {}) {
  TimelineModel model;
  model.fps = kFps;
  model.max_gain = 2.0;
  model.tracks = {
      TimelineTrack{.name = "V1",
                    .blocks = {TimelineBlock{.start = 0.0, .end = 8.0, .label = "wide"}}},
      TimelineTrack{.name = "A1",
                    .audio = true,
                    .blocks = {TimelineBlock{
                        .start = 0.0, .end = 8.0, .label = "dialogue", .gain = std::move(band)}}},
  };
  return model;
}

struct BandFixture {
  explicit BandFixture(GainBand band = {}) {
    host = std::make_unique<WidgetHost>(std::make_unique<TimelineView>());
    view = static_cast<TimelineView*>(&host->root());
    view->set_model(banded_model(std::move(band)));
    view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = 0.0});
    view->set_snapping(false);
    host->resize(Rect{0.0, 0.0, 1000.0, 400.0}, flat_context());
  }

  /// The audio track's only block.
  [[nodiscard]] const TimelineBlock& block() const { return view->model().tracks[1].blocks[0]; }
  [[nodiscard]] const GainBand& band() const { return *block().gain; }

  std::unique_ptr<WidgetHost> host;
  TimelineView* view = nullptr;
};

TEST(GainBand, SilenceIsTheFootAndTheCeilingIsTheTop) {
  EXPECT_DOUBLE_EQ(gain_to_band(0.0, 2.0), 0.0);
  EXPECT_DOUBLE_EQ(gain_to_band(2.0, 2.0), 1.0);
  EXPECT_DOUBLE_EQ(band_to_gain(0.0, 2.0), 0.0);
  EXPECT_DOUBLE_EQ(band_to_gain(1.0, 2.0), 2.0);
}

// The whole reason the scale is decibels. On a linear scale these three steps
// would be 0.5, 0.25 and 0.125 of the height; here they are the same distance,
// which is what makes a trim near unity draggable at all.
TEST(GainBand, EqualDecibelStepsAreEqualDistances) {
  const double unity = gain_to_band(1.0, 2.0);
  const double down6 = gain_to_band(0.5, 2.0);
  const double down12 = gain_to_band(0.25, 2.0);
  const double down18 = gain_to_band(0.125, 2.0);

  EXPECT_NEAR(unity - down6, down6 - down12, 1e-9);
  EXPECT_NEAR(down6 - down12, down12 - down18, 1e-9);
  EXPECT_GT(unity - down6, 0.1);  // and each is worth a real part of the clip
}

TEST(GainBand, UnitySitsHighBecauseThereIsLittleRoomAboveIt) {
  // Six decibels of headroom against thirty-six below, so a clip at unity draws
  // its line near the top. That is the shape of the thing, not an accident: the
  // model allows twice unity and no more.
  const double unity = gain_to_band(1.0, 2.0);
  EXPECT_GT(unity, 0.8);
  EXPECT_LT(unity, 0.95);
}

TEST(GainBand, EverythingBelowTheFloorIsSilence) {
  EXPECT_DOUBLE_EQ(gain_to_band(std::pow(10.0, kGainFloorDb / 20.0) * 0.5, 2.0), 0.0);
  EXPECT_DOUBLE_EQ(gain_to_band(1e-9, 2.0), 0.0);
}

TEST(GainBand, AHeightMeansTheGainThatWouldBeDrawnThere) {
  for (const double gain : {0.05, 0.25, 0.5, 1.0, 1.5, 2.0}) {
    EXPECT_NEAR(band_to_gain(gain_to_band(gain, 2.0), 2.0), gain, 1e-9)
        << "round trip through the band at gain " << gain;
  }
}

TEST(GainBand, AVideoClipHasNoBand) {
  const BandFixture fixture;
  EXPECT_TRUE(fixture.view->gain_area(0, 0).empty());

  const Rect box = fixture.view->block_rect(0, 0);
  EXPECT_FALSE(fixture.view->over_gain_band(box.x + 100.0, box.y + box.height * 0.5));
}

TEST(GainBand, TheBandIsDrawnOnEveryAudioClipRatherThanTheSelectedOne) {
  BandFixture fixture;
  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  // Nothing is selected, and the line is there anyway: which clips carry a
  // level is what somebody reading a mix wants to see without clicking through
  // them one at a time.
  EXPECT_FALSE(fixture.view->selection().has_value());

  // A line's endpoints are its bounds' origin and that origin plus its extents.
  const double y = fixture.view->gain_to_y(1, 0, 1.0);
  const bool drew_the_band = std::ranges::any_of(painter.calls(), [&](const DrawCall& call) {
    return call.kind == DrawCall::Kind::Line && std::abs(call.bounds.y - y) < 0.51 &&
           std::abs(call.bounds.height) < 0.51 && call.bounds.width > 100.0;
  });
  EXPECT_TRUE(drew_the_band);
}

TEST(GainBand, LoudIsUp) {
  const BandFixture fixture;
  EXPECT_LT(fixture.view->gain_to_y(1, 0, 2.0), fixture.view->gain_to_y(1, 0, 1.0));
  EXPECT_LT(fixture.view->gain_to_y(1, 0, 1.0), fixture.view->gain_to_y(1, 0, 0.25));
}

TEST(GainBand, DraggingTheBandSetsTheLevel) {
  BandFixture fixture;
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect box = fixture.view->block_rect(1, 0);
  const double y = fixture.view->gain_to_y(1, 0, 1.0);
  fixture.host->mouse_down(press(box.x + 200.0, y));
  fixture.host->mouse_move(press(box.x + 200.0, y + 10.0));
  fixture.host->mouse_up(press(box.x + 200.0, y + 10.0));

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::GainLevel);
  EXPECT_LT(last->gain, 1.0);
  EXPECT_GT(last->gain, 0.0);
  // And the view shows it, so the drag can be seen while it happens.
  EXPECT_DOUBLE_EQ(fixture.band().level, last->gain);
}

// A band pulled straight down travels no distance in x at all. A drag threshold
// measured along one axis would sit there refusing to start, which is exactly
// the gesture this control is for.
TEST(GainBand, AStraightDownDragStartsAtAll) {
  BandFixture fixture;
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect box = fixture.view->block_rect(1, 0);
  const double x = box.x + 200.0;
  const double y = fixture.view->gain_to_y(1, 0, 1.0);
  fixture.host->mouse_down(press(x, y));
  fixture.host->mouse_move(press(x, y + 12.0));
  fixture.host->mouse_up(press(x, y + 12.0));

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::GainLevel);
}

TEST(GainBand, APressBelowTheBandStillMovesTheClip) {
  const BandFixture fixture;
  const Rect box = fixture.view->block_rect(1, 0);
  // Well under the line, which at unity sits near the top of the block.
  EXPECT_EQ(fixture.view->zone_at(box.x + 200.0, box.bottom() - 4.0), DragMode::Move);
}

// An automated band is grabbed where the *line* is, which is wherever the
// points evaluate to rather than the stored level — the level is not what is
// playing once there is automation on the clip.
TEST(GainBand, AnAutomatedBandIsGrabbedWhereTheLineIs) {
  const BandFixture fixture{GainBand{.level = 1.0, .points = {{2.0, 0.5}, {5.0, 1.0}}}};
  const Rect box = fixture.view->block_rect(1, 0);

  // Halfway along the ramp between the two points.
  const double x = box.x + 350.0;
  const double on_the_line = fixture.view->gain_to_y(1, 0, 0.75);
  EXPECT_TRUE(fixture.view->over_gain_band(x, on_the_line));
  EXPECT_FALSE(fixture.view->over_gain_band(x, on_the_line + 20.0))
      << "well below the line is the clip body, not the band";
}

TEST(GainBand, DraggingAnAutomatedBandMovesTheStretchRatherThanTheLevel) {
  const BandFixture fixture{GainBand{.level = 1.0, .points = {{2.0, 0.5}, {5.0, 1.0}}}};
  const Rect box = fixture.view->block_rect(1, 0);
  EXPECT_EQ(fixture.view->zone_at(box.x + 350.0, fixture.view->gain_to_y(1, 0, 0.75)),
            DragMode::GainSegment);
}

// Which points a stretch carries: one at each end in the middle of the band,
// and the single end that holds the flat run outside the points up.
TEST(GainBand, AStretchIsHeldByThePointsAtItsEnds) {
  const BandFixture fixture{GainBand{.points = {{2.0, 0.5}, {5.0, 1.0}, {7.0, 0.25}}}};
  const Rect area = fixture.view->gain_area(1, 0);

  EXPECT_EQ(fixture.view->gain_segment_at(1, 0, area.x + 50.0), (std::vector<std::size_t>{0}))
      << "before the first point the line is flat";
  EXPECT_EQ(fixture.view->gain_segment_at(1, 0, area.x + 350.0),
            (std::vector<std::size_t>{0, 1}));
  EXPECT_EQ(fixture.view->gain_segment_at(1, 0, area.x + 600.0),
            (std::vector<std::size_t>{1, 2}));
  EXPECT_EQ(fixture.view->gain_segment_at(1, 0, area.x + 780.0), (std::vector<std::size_t>{2}))
      << "after the last point it is flat again";
}

// The whole point of moving both ends by the same number of decibels: a ramp
// stays the ramp it was. Setting both to the pointer's height would flatten it.
TEST(GainBand, DraggingAStretchKeepsItsShape) {
  BandFixture fixture{GainBand{.points = {{2.0, 0.25}, {6.0, 1.0}}}};
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const double before = gain_to_band(1.0, 2.0) - gain_to_band(0.25, 2.0);

  const double x = fixture.view->block_rect(1, 0).x + 400.0;
  const double y = fixture.view->gain_to_y(1, 0, 0.625);
  fixture.host->mouse_down(press(x, y));
  fixture.host->mouse_move(press(x, y + 8.0));
  fixture.host->mouse_up(press(x, y + 8.0));

  const std::vector<GainPoint>& points = fixture.band().points;
  ASSERT_EQ(points.size(), 2u);
  // Both quieter, by the same amount, and their times untouched.
  EXPECT_LT(points[0].v, 0.25);
  EXPECT_LT(points[1].v, 1.0);
  EXPECT_DOUBLE_EQ(points[0].t, 2.0);
  EXPECT_DOUBLE_EQ(points[1].t, 6.0);
  EXPECT_NEAR(gain_to_band(points[1].v, 2.0) - gain_to_band(points[0].v, 2.0), before, 1e-6);

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::GainSegment);
  EXPECT_EQ(last->gain_moved.size(), 2u);
}

TEST(GainBand, DraggingTheFlatRunBeforeTheFirstPointCarriesOnlyThatPoint) {
  BandFixture fixture{GainBand{.points = {{4.0, 1.0}, {6.0, 0.5}}}};
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const double x = fixture.view->block_rect(1, 0).x + 100.0;  // 1s, before the first
  const double y = fixture.view->gain_to_y(1, 0, 1.0);
  fixture.host->mouse_down(press(x, y));
  fixture.host->mouse_move(press(x, y + 10.0));
  fixture.host->mouse_up(press(x, y + 10.0));

  ASSERT_TRUE(last.has_value());
  ASSERT_EQ(last->gain_moved.size(), 1u);
  EXPECT_DOUBLE_EQ(last->gain_moved[0].t, 4.0);
  EXPECT_LT(fixture.band().points[0].v, 1.0);
  EXPECT_DOUBLE_EQ(fixture.band().points[1].v, 0.5) << "the far point stayed put";
}

// A point sits on the line, so a press near both has to mean the point — it is
// the more precise target and the one being aimed at.
TEST(GainBand, APressOnAPointTakesThePointRatherThanTheStretch) {
  const BandFixture fixture{GainBand{.points = {{2.0, 0.5}, {5.0, 1.0}}}};
  const Rect dot = fixture.view->gain_point_rect(1, 0, 0);
  EXPECT_EQ(fixture.view->zone_at(dot.x + dot.width * 0.5, dot.y + dot.height * 0.5),
            DragMode::GainPointDrag);
}

TEST(GainBand, AltPutsAPointWhereTheBandAlreadyIs) {
  BandFixture fixture{GainBand{.level = 0.5}};
  const Rect box = fixture.view->block_rect(1, 0);

  // Deliberately nowhere near the line: alt anywhere on the clip puts a point
  // on the band, because a two-pixel line is not something to be asked to hit
  // before you are allowed to automate anything.
  fixture.host->mouse_down(alt_press(box.x + 300.0, box.bottom() - 3.0));

  ASSERT_EQ(fixture.band().points.size(), 1u);
  // At the level the band already had, so adding a point changes nothing about
  // what plays — the same bargain the inspector's stopwatch makes.
  EXPECT_DOUBLE_EQ(fixture.band().points[0].v, 0.5);
  EXPECT_NEAR(fixture.band().points[0].t, 3.0, 1.0 / kFps);
}

TEST(GainBand, APointAddedOnAnAutomatedClipTakesTheValueTheBandWasAt) {
  BandFixture fixture{GainBand{.points = {{0.0, 0.0}, {4.0, 1.0}}}};
  const Rect box = fixture.view->block_rect(1, 0);

  // Halfway along a ramp from silence to unity.
  fixture.host->mouse_down(alt_press(box.x + 200.0, box.y + 5.0));

  ASSERT_EQ(fixture.band().points.size(), 3u);
  EXPECT_NEAR(fixture.band().points[1].t, 2.0, 1.0 / kFps);
  EXPECT_NEAR(fixture.band().points[1].v, 0.5, 0.02);
}

TEST(GainBand, AddingAPointAndMovingOneAreTheSameEdit) {
  BandFixture fixture{GainBand{.level = 0.5}};
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect box = fixture.view->block_rect(1, 0);
  fixture.host->mouse_down(alt_press(box.x + 300.0, box.bottom() - 3.0));
  fixture.host->mouse_up(press(box.x + 300.0, box.bottom() - 3.0));

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::GainPointDrag);
  // A point that was just created reports the same place in each, so whoever
  // applies it does not have to know whether it is new.
  EXPECT_EQ(last->gain_from, last->gain_to);
}

TEST(GainBand, AltOnAPointTakesItAway) {
  BandFixture fixture{GainBand{.points = {{2.0, 0.5}, {5.0, 1.0}}}};
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect dot = fixture.view->gain_point_rect(1, 0, 0);
  fixture.host->mouse_down(
      alt_press(dot.x + dot.width * 0.5, dot.y + dot.height * 0.5));

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::GainPointRemove);
  EXPECT_DOUBLE_EQ(last->gain_from.t, 2.0);
  // Gone from the view too, so the press is visible on the frame it happened.
  ASSERT_EQ(fixture.band().points.size(), 1u);
  EXPECT_DOUBLE_EQ(fixture.band().points[0].t, 5.0);
}

TEST(GainBand, APointIsDraggedInTimeAsWellAsLevel) {
  BandFixture fixture{GainBand{.points = {{2.0, 1.0}}}};
  std::optional<TimelineEdit> last;
  fixture.view->set_on_edit([&](const TimelineEdit& edit) { last = edit; });

  const Rect dot = fixture.view->gain_point_rect(1, 0, 0);
  const double x = dot.x + dot.width * 0.5;
  const double y = dot.y + dot.height * 0.5;
  fixture.host->mouse_down(press(x, y));
  fixture.host->mouse_move(press(x + 100.0, y + 8.0));
  fixture.host->mouse_up(press(x + 100.0, y + 8.0));

  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->mode, DragMode::GainPointDrag);
  EXPECT_DOUBLE_EQ(last->gain_from.t, 2.0);
  EXPECT_NEAR(last->gain_to.t, 3.0, 1.0 / kFps);
  EXPECT_LT(last->gain_to.v, 1.0);
}

TEST(GainBand, APointNeverLeavesItsOwnClip) {
  BandFixture fixture{GainBand{.points = {{2.0, 1.0}}}};
  const Rect dot = fixture.view->gain_point_rect(1, 0, 0);
  const double y = dot.y + dot.height * 0.5;

  fixture.host->mouse_down(press(dot.x + dot.width * 0.5, y));
  // Far past the end of an eight-second clip.
  fixture.host->mouse_move(press(dot.x + 2000.0, y));

  EXPECT_LE(fixture.band().points[0].t, 8.0);

  fixture.host->mouse_move(press(dot.x - 2000.0, y));
  EXPECT_GE(fixture.band().points[0].t, 0.0);
}

// Everything that reads a keyframe list assumes it is sorted — the drawing
// here, the evaluator in the core — and dragging one point past another is the
// one gesture that can break that.
TEST(GainBand, APointDraggedPastAnotherKeepsTheListInOrder) {
  BandFixture fixture{GainBand{.points = {{1.0, 0.25}, {4.0, 1.0}}}};

  const Rect dot = fixture.view->gain_point_rect(1, 0, 0);
  const double y = dot.y + dot.height * 0.5;
  fixture.host->mouse_down(press(dot.x + dot.width * 0.5, y));
  fixture.host->mouse_move(press(dot.x + 600.0, y));  // past the one at 4s
  fixture.host->mouse_up(press(dot.x + 600.0, y));

  const std::vector<GainPoint>& points = fixture.band().points;
  ASSERT_EQ(points.size(), 2u);
  EXPECT_TRUE(std::ranges::is_sorted(points, {}, &GainPoint::t));
  // And the one that moved is still the one that moved, at its new time. Its
  // level is near rather than equal because a drag reads it back off the
  // pointer's height, which is a whole number of pixels.
  EXPECT_DOUBLE_EQ(points[0].t, 4.0);
  EXPECT_NEAR(points[1].t, 7.0, 1.0 / kFps);
  EXPECT_NEAR(points[1].v, 0.25, 0.01);
}

TEST(GainBand, TwoPointsAreNeverPutAtTheSameInstant) {
  BandFixture fixture{GainBand{.points = {{3.0, 0.5}}}};
  const Rect dot = fixture.view->gain_point_rect(1, 0, 0);

  // Alt just off the point, close enough to round to the same frame. A second
  // keyframe there would give the evaluator two answers for one moment.
  fixture.host->mouse_down(alt_press(dot.x + dot.width * 0.5, dot.y - 20.0));
  EXPECT_EQ(fixture.band().points.size(), 1u);
}

// The band belongs to the selection tool. The other four each mean one thing
// everywhere on a clip, which is what makes them usable without hunting.
TEST(GainBand, TheOtherToolsKeepTheWholeClip) {
  const BandFixture fixture;
  const Rect box = fixture.view->block_rect(1, 0);
  const double y = fixture.view->gain_to_y(1, 0, 1.0);

  for (const auto [tool, mode] : {std::pair{Tool::Razor, DragMode::Razor},
                                  std::pair{Tool::Slip, DragMode::Slip},
                                  std::pair{Tool::Slide, DragMode::Slide}}) {
    BandFixture other;
    other.view->set_tool(tool);
    EXPECT_EQ(other.view->zone_at(box.x + 200.0, y), mode);
  }
}

// ---------------------------------------------------------- the waveform --
//
// The envelope describes a *source*; where a block sits in it is on the block.
// That is what lets one envelope serve a source used a dozen times over, and
// what makes a trimmed, retimed or reversed clip draw the audio it actually
// plays.

/// An envelope whose buckets say what second they came from, so a test can
/// tell which part of a source ended up drawn.
[[nodiscard]] std::shared_ptr<const Waveform> ramp_waveform(double seconds) {
  auto wave = std::make_shared<Waveform>();
  wave->buckets_per_second = 10.0;
  const auto buckets = static_cast<std::size_t>(seconds * wave->buckets_per_second);
  for (std::size_t i = 0; i < buckets; ++i) {
    const auto level = static_cast<float>(i) / static_cast<float>(buckets);
    wave->minimum.push_back(-level);
    wave->maximum.push_back(level);
  }
  return wave;
}

[[nodiscard]] TimelineModel waved_model(TimelineBlock block) {
  TimelineModel model;
  model.fps = kFps;
  model.tracks = {TimelineTrack{.name = "A1", .audio = true, .blocks = {std::move(block)}}};
  return model;
}

struct WaveFixture {
  explicit WaveFixture(TimelineBlock block) {
    host = std::make_unique<WidgetHost>(std::make_unique<TimelineView>());
    view = static_cast<TimelineView*>(&host->root());
    view->set_model(waved_model(std::move(block)));
    view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = 0.0});
    host->resize(Rect{0.0, 0.0, 1000.0, 400.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  TimelineView* view = nullptr;
};

TEST(Waveform, ABlockWithNoEnvelopeYetHasNowhereToDrawOne) {
  // What every audio clip looks like for the moment after it is imported,
  // while the source is still being decoded on a worker.
  const WaveFixture fixture{TimelineBlock{.start = 0.0, .end = 4.0}};
  EXPECT_TRUE(fixture.view->waveform_area(0, 0).empty());
}

TEST(Waveform, AnUntrimmedBlockReadsItsSourceFromTheStart) {
  const TimelineBlock block{.start = 0.0, .end = 4.0, .waveform = ramp_waveform(10.0)};
  EXPECT_DOUBLE_EQ(source_time_of(block, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(source_time_of(block, 2.5), 2.5);
}

TEST(Waveform, ATrimmedBlockReadsFromWhereItWasTrimmedTo) {
  const TimelineBlock block{
      .start = 0.0, .end = 4.0, .waveform = ramp_waveform(10.0), .source_in = 6.0};
  EXPECT_DOUBLE_EQ(source_time_of(block, 0.0), 6.0);
  EXPECT_DOUBLE_EQ(source_time_of(block, 2.0), 8.0);
}

// A clip at 2x covers twice as much source in the same distance. Drawing the
// envelope at 1:1 would show footage the clip does not play, which is the sort
// of thing that looks right until the one clip somebody retimed.
TEST(Waveform, ARetimedBlockCoversItsSourceFaster) {
  const TimelineBlock block{
      .start = 0.0, .end = 4.0, .waveform = ramp_waveform(20.0), .speed = 2.0};
  EXPECT_DOUBLE_EQ(source_time_of(block, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(source_time_of(block, 2.0), 4.0);
  EXPECT_DOUBLE_EQ(source_time_of(block, 4.0), 8.0);
}

TEST(Waveform, AReversedBlockStartsAtItsSourcesEnd) {
  const TimelineBlock block{
      .start = 0.0, .end = 4.0, .waveform = ramp_waveform(10.0), .source_in = 2.0,
      .reverse = true};
  // Four seconds of clip at speed 1 spans source 2 to 6, played backwards.
  EXPECT_DOUBLE_EQ(source_time_of(block, 0.0), 6.0);
  EXPECT_DOUBLE_EQ(source_time_of(block, 4.0), 2.0);
}

TEST(Waveform, TheEnvelopeIsDrawnAcrossTheBlock) {
  const WaveFixture plain{TimelineBlock{.start = 0.0, .end = 4.0}};
  RecordingPainter before;
  plain.view->paint(before, default_theme());

  const WaveFixture waved{
      TimelineBlock{.start = 0.0, .end = 4.0, .waveform = ramp_waveform(10.0)}};
  RecordingPainter after;
  waved.view->paint(after, default_theme());

  // A column per pixel across four seconds at a hundred pixels a second, so
  // several hundred more lines than an empty clip draws.
  EXPECT_GT(after.count(DrawCall::Kind::Line), before.count(DrawCall::Kind::Line) + 300);
}

// The cost is what is on screen, not how long the clip is. A ten-minute source
// zoomed out to a few pixels draws a few columns.
TEST(Waveform, ALongClipCostsWhatIsVisibleRatherThanWhatItHolds) {
  WaveFixture fixture{
      TimelineBlock{.start = 0.0, .end = 600.0, .waveform = ramp_waveform(600.0)}};
  fixture.view->set_scale(TimeScale{.pixels_per_second = 0.5, .start = 0.0});

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());
  // Six hundred seconds at half a pixel each is three hundred columns, not the
  // six thousand buckets the envelope holds.
  EXPECT_LT(painter.count(DrawCall::Kind::Line), 500u);
}

TEST(Waveform, ASilentSourceStillDrawsAClipRatherThanAGap) {
  auto silent = std::make_shared<Waveform>();
  silent->buckets_per_second = 10.0;
  silent->minimum.assign(40, 0.0f);
  silent->maximum.assign(40, 0.0f);

  const WaveFixture fixture{
      TimelineBlock{.start = 0.0, .end = 4.0, .waveform = std::move(silent)}};
  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  // A centre line rather than nothing: a hole where the audio is quiet reads as
  // a hole in the clip.
  EXPECT_GT(painter.count(DrawCall::Kind::Line), 300u);
}

// The envelope goes down before the label and the volume band, so both read
// over the top of it rather than being buried.
TEST(Waveform, TheEnvelopeIsDrawnUnderTheVolumeBand) {
  TimelineBlock block{.start = 0.0, .end = 4.0};
  block.gain = GainBand{};
  block.waveform = ramp_waveform(10.0);

  const WaveFixture fixture{std::move(block)};
  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  // A column is a vertical line inside the envelope's own strip. Identified by
  // where it is rather than only by its shape, because the playhead is a
  // vertical line too and it is drawn last of everything.
  const Rect strip = fixture.view->waveform_area(0, 0);
  ASSERT_FALSE(strip.empty());

  const std::vector<DrawCall>& calls = painter.calls();
  std::size_t last_column = 0;
  std::size_t band = 0;
  for (std::size_t i = 0; i < calls.size(); ++i) {
    if (calls[i].kind != DrawCall::Kind::Line) continue;
    const Rect& at = calls[i].bounds;
    const bool vertical = std::abs(at.width) < 0.01;
    if (vertical && at.y >= strip.y - 0.5 && at.y + std::abs(at.height) <= strip.bottom() + 0.5) {
      last_column = i;
    }
    if (at.width > 100.0 && std::abs(at.height) < 0.51) band = i;
  }
  EXPECT_GT(band, 0u);
  EXPECT_GT(last_column, 0u);
  EXPECT_LT(last_column, band);
}

// --------------------------------------------------------- the filmstrip --

/// Frames at known source times, each a flat colour, so a test can say which
/// one ended up in which tile.
[[nodiscard]] std::shared_ptr<const Filmstrip> strip_at(std::vector<double> times) {
  auto strip = std::make_shared<Filmstrip>();
  std::uint8_t shade = 10;
  for (const double t : times) {
    FilmFrame frame;
    frame.t = t;
    frame.width = 16;
    frame.height = 9;
    frame.rgba.assign(static_cast<std::size_t>(frame.width * frame.height * 4), shade);
    shade = static_cast<std::uint8_t>(shade + 40);
    strip->frames.push_back(std::move(frame));
  }
  return strip;
}

[[nodiscard]] TimelineModel filmed_model(TimelineBlock block) {
  TimelineModel model;
  model.fps = kFps;
  model.tracks = {TimelineTrack{.name = "V1", .blocks = {std::move(block)}}};
  return model;
}

struct FilmFixture {
  explicit FilmFixture(TimelineBlock block) {
    host = std::make_unique<WidgetHost>(std::make_unique<TimelineView>());
    view = static_cast<TimelineView*>(&host->root());
    view->set_model(filmed_model(std::move(block)));
    view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = 0.0});
    host->resize(Rect{0.0, 0.0, 1000.0, 400.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  TimelineView* view = nullptr;
};

TEST(Filmstrip, TheNearestFrameToATimeIsTheOneShown) {
  const std::shared_ptr<const Filmstrip> strip = strip_at({0.0, 10.0, 20.0});

  ASSERT_NE(strip->nearest(0.0), nullptr);
  EXPECT_DOUBLE_EQ(strip->nearest(0.0)->t, 0.0);
  EXPECT_DOUBLE_EQ(strip->nearest(4.0)->t, 0.0);
  EXPECT_DOUBLE_EQ(strip->nearest(6.0)->t, 10.0);
  // Outside the range at either end, the end frame is the nearest one there is.
  EXPECT_DOUBLE_EQ(strip->nearest(-100.0)->t, 0.0);
  EXPECT_DOUBLE_EQ(strip->nearest(999.0)->t, 20.0);
}

TEST(Filmstrip, AnEmptyStripHasNoNearestFrame) {
  const Filmstrip empty;
  EXPECT_EQ(empty.nearest(0.0), nullptr);
}

TEST(Filmstrip, ABlockWithNoFramesYetHasNowhereToDrawThem) {
  const FilmFixture fixture{TimelineBlock{.start = 0.0, .end = 4.0}};
  EXPECT_TRUE(fixture.view->filmstrip_area(0, 0).empty());
}

TEST(Filmstrip, TheStripIsDrawnAcrossTheBlock) {
  const FilmFixture plain{TimelineBlock{.start = 0.0, .end = 6.0}};
  RecordingPainter before;
  plain.view->paint(before, default_theme());
  EXPECT_EQ(before.count(DrawCall::Kind::Image), 0u);

  TimelineBlock filmed{.start = 0.0, .end = 6.0};
  filmed.filmstrip = strip_at({0.0, 3.0, 6.0});
  const FilmFixture fixture{std::move(filmed)};

  RecordingPainter after;
  fixture.view->paint(after, default_theme());
  // Six hundred pixels of clip, tiled by a 16:9 thumbnail at the track's
  // height, so several tiles rather than one stretched frame.
  EXPECT_GT(after.count(DrawCall::Kind::Image), 3u);
}

// The cost is what is on screen. A source scrolled almost entirely out of view
// costs the tiles that are visible, not the ones that are not.
TEST(Filmstrip, ALongClipCostsWhatIsVisible) {
  TimelineBlock filmed{.start = 0.0, .end = 600.0};
  filmed.filmstrip = strip_at({0.0, 200.0, 400.0});
  FilmFixture fixture{std::move(filmed)};
  fixture.view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = 0.0});

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  // A thousand pixels of window at a track's height is a few dozen tiles, not
  // the sixty thousand pixels of clip.
  EXPECT_LT(painter.count(DrawCall::Kind::Image), 40u);
}

TEST(Filmstrip, TilesKeepTheFramesShapeRatherThanStretching) {
  TimelineBlock filmed{.start = 0.0, .end = 6.0};
  filmed.filmstrip = strip_at({0.0, 3.0});
  const FilmFixture fixture{std::move(filmed)};

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  const DrawCall* first = painter.first(DrawCall::Kind::Image);
  ASSERT_NE(first, nullptr);
  const Rect strip = fixture.view->filmstrip_area(0, 0);
  ASSERT_FALSE(strip.empty());
  // 16 by 9, so a tile is its height times that and not some share of the clip.
  EXPECT_NEAR(first->bounds.width / first->bounds.height, 16.0 / 9.0, 0.01);
  EXPECT_NEAR(first->bounds.height, strip.height, 0.01);
}

// Which frame is in which tile must not depend on where the view is scrolled.
// The tiles are a grid on the *clip*, so scrolling slides the strip along with
// the block rather than reshuffling which frame sits where.
TEST(Filmstrip, ScrollingSlidesTheStripRatherThanReshufflingIt) {
  // Every tile's offset from the block's own left edge, in tile widths.
  const auto offsets_in_tiles = [](double start) {
    TimelineBlock filmed{.start = 0.0, .end = 30.0};
    filmed.filmstrip = strip_at({0.0, 10.0, 20.0});
    FilmFixture fixture{std::move(filmed)};
    fixture.view->set_scale(TimeScale{.pixels_per_second = 100.0, .start = start});

    RecordingPainter painter;
    fixture.view->paint(painter, default_theme());

    // From the strip rather than the block: the strip is inset from the clip's
    // border, and the grid is anchored to where the frames actually start.
    const double left = fixture.view->filmstrip_area(0, 0).x;
    std::vector<double> out;
    for (const DrawCall& call : painter.calls()) {
      if (call.kind != DrawCall::Kind::Image) continue;
      out.push_back((call.bounds.x - left) / call.bounds.width);
    }
    return out;
  };

  for (const double start : {0.0, 1.0, 3.7}) {
    const std::vector<double> tiles = offsets_in_tiles(start);
    ASSERT_FALSE(tiles.empty()) << "scrolled to " << start;
    for (const double at : tiles) {
      EXPECT_NEAR(at, std::round(at), 1e-6)
          << "a tile landed " << at << " tiles along, scrolled to " << start;
    }
  }
}

}  // namespace
}  // namespace cutline::ui
