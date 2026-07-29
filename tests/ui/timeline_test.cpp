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
#include <memory>
#include <optional>
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
  TimelineBlock last;
  BlockRef which;
  DragMode how = DragMode::None;
  fixture.view->set_on_edit([&](BlockRef ref, DragMode mode, TimelineBlock block) {
    ++edits;
    which = ref;
    how = mode;
    last = block;
  });

  const Rect box = fixture.view->block_rect(0, 0);
  fixture.host->mouse_down(press(box.x + 50.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 150.0, box.y + 10.0));
  fixture.host->mouse_move(press(box.x + 250.0, box.y + 10.0));
  EXPECT_EQ(edits, 0) << "an edit during the drag would fill the undo stack";

  fixture.host->mouse_up(press(box.x + 250.0, box.y + 10.0));
  EXPECT_EQ(edits, 1);
  EXPECT_EQ(which, (BlockRef{0, 0}));
  EXPECT_NEAR(last.start, 4.0, 1.0 / kFps);
  // The mode is reported rather than inferred: moving a clip and trimming
  // both its edges by the same amount leave the same numbers behind.
  EXPECT_EQ(how, DragMode::Move);
}

TEST(Timeline, AClickReportsNoEdit) {
  Fixture fixture;
  int edits = 0;
  fixture.view->set_on_edit([&](BlockRef, DragMode, TimelineBlock) { ++edits; });

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

}  // namespace
}  // namespace cutline::ui
