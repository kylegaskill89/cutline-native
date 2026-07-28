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

#include <memory>
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
