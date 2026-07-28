/// Time in pixels.
///
/// The ruler ladder is the interesting part. Time is base sixty above a second
/// and base `fps` below it, so a decimal step sequence — which is what falls
/// out of the obvious implementation — produces a ruler marked every 0.4
/// seconds that no editor can read.

#include "cutline/ui/timescale.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace cutline::ui {
namespace {

constexpr double kTolerance = 1e-9;
constexpr double kFps = 30.0;

[[nodiscard]] std::vector<double> major_times(const std::vector<Tick>& ticks) {
  std::vector<double> out;
  for (const Tick& tick : ticks) {
    if (tick.major) out.push_back(tick.time);
  }
  return out;
}

// --------------------------------------------------------------- mapping --

TEST(TimeScale, MapsTimeToPixelsAndBack) {
  const TimeScale scale{.pixels_per_second = 120.0, .start = 2.0};

  EXPECT_DOUBLE_EQ(scale.to_x(2.0), 0.0) << "the start sits at the left edge";
  EXPECT_DOUBLE_EQ(scale.to_x(3.0), 120.0);
  EXPECT_DOUBLE_EQ(scale.to_time(120.0), 3.0);
  EXPECT_DOUBLE_EQ(scale.width_of(0.5), 60.0);
}

TEST(TimeScale, TimeBeforeTheStartIsToTheLeft) {
  // Clips that begin off-screen still have to be drawn, or the one being
  // scrolled towards pops into existence at the edge.
  const TimeScale scale{.pixels_per_second = 100.0, .start = 5.0};
  EXPECT_DOUBLE_EQ(scale.to_x(4.0), -100.0);
}

TEST(TimeScale, VisibleDurationFollowsTheZoom) {
  const TimeScale scale{.pixels_per_second = 50.0};
  EXPECT_DOUBLE_EQ(scale.visible_duration(1000.0), 20.0);
}

// ------------------------------------------------------------------ zoom --

TEST(TimeScale, ZoomingKeepsTheFrameUnderTheCursorUnderTheCursor) {
  TimeScale scale{.pixels_per_second = 100.0, .start = 10.0};
  const double under = scale.to_time(300.0);

  scale.zoom_about(300.0, 2.5);
  EXPECT_NEAR(scale.to_time(300.0), under, 1e-9);
  EXPECT_DOUBLE_EQ(scale.pixels_per_second, 250.0);
}

TEST(TimeScale, ZoomingOutNearTheStartDoesNotGoNegative) {
  TimeScale scale{.pixels_per_second = 400.0, .start = 1.0};
  scale.zoom_about(50.0, 0.01);
  EXPECT_GE(scale.start, 0.0);
}

TEST(TimeScale, ZoomIsBounded) {
  TimeScale scale;
  for (int i = 0; i < 200; ++i) scale.zoom_about(100.0, 2.0);
  EXPECT_DOUBLE_EQ(scale.pixels_per_second, kMaxPixelsPerSecond);

  for (int i = 0; i < 400; ++i) scale.zoom_about(100.0, 0.5);
  EXPECT_DOUBLE_EQ(scale.pixels_per_second, kMinPixelsPerSecond);
}

TEST(TimeScale, ADegenerateZoomChangesNothing) {
  const TimeScale before{.pixels_per_second = 100.0, .start = 3.0};
  TimeScale scale = before;
  scale.zoom_about(50.0, 0.0);
  EXPECT_EQ(scale, before);
}

TEST(TimeScale, FittingPutsTheWholeProjectOnScreen) {
  TimeScale scale;
  scale.fit(60.0, 1200.0);

  EXPECT_DOUBLE_EQ(scale.start, 0.0);
  EXPECT_DOUBLE_EQ(scale.to_x(60.0), 1200.0);
}

TEST(TimeScale, FittingNothingIsHarmless) {
  TimeScale scale{.pixels_per_second = 100.0, .start = 4.0};
  scale.fit(0.0, 800.0);
  EXPECT_DOUBLE_EQ(scale.start, 0.0);
  EXPECT_DOUBLE_EQ(scale.pixels_per_second, 100.0) << "an empty project should not rescale";
}

// --------------------------------------------------------------- scroll --

TEST(TimeScale, ScrollingStopsWithTheContentEndAtTheLeftEdge) {
  // Which leaves a screen of empty space after it, on purpose: dropping a clip
  // at the very end of a project needs somewhere to drop it.
  TimeScale scale{.pixels_per_second = 100.0, .start = 1000.0};
  scale.clamp_start(60.0);

  EXPECT_DOUBLE_EQ(scale.start, 60.0);
  EXPECT_DOUBLE_EQ(scale.to_x(60.0), 0.0);
  EXPECT_GT(scale.to_time(1000.0), 60.0) << "there is no room past the last clip";
}

TEST(TimeScale, ScrollingStopsAtZero) {
  TimeScale scale{.pixels_per_second = 100.0, .start = -50.0};
  scale.clamp_start(60.0);
  EXPECT_DOUBLE_EQ(scale.start, 0.0);
}

TEST(TimeScale, AnEmptyProjectDoesNotScroll) {
  TimeScale scale{.pixels_per_second = 100.0, .start = 30.0};
  scale.clamp_start(0.0);
  EXPECT_DOUBLE_EQ(scale.start, 0.0);
}

// ---------------------------------------------------------------- ticks --

TEST(TickInterval, CountsInFramesWhenZoomedRightIn) {
  // At 1200 px/s a frame is 40 pixels, so one and two frames are both too
  // close to label. Five is the first step on the ladder that clears 90.
  EXPECT_NEAR(tick_interval(1200.0, kFps, 90.0), 5.0 / kFps, 1e-9);

  // Further in, single frames become the readable step, which is the unit the
  // footage is actually cut in.
  EXPECT_NEAR(tick_interval(4000.0, kFps, 90.0), 1.0 / kFps, 1e-9);
}

TEST(TickInterval, NeverMarksBetweenFrames) {
  // Every sub-second interval has to be a whole number of frames. A mark
  // halfway between two frames points at something that does not exist.
  for (const double zoom : {200.0, 400.0, 900.0, 2000.0, 6000.0}) {
    const double interval = tick_interval(zoom, kFps, 90.0);
    if (interval >= 1.0) continue;
    const double frames = interval * kFps;
    EXPECT_NEAR(frames, std::round(frames), 1e-9) << "at " << zoom << " px/s";
  }
}

TEST(TickInterval, UsesTheClockLadderAboveASecond) {
  // Base sixty, not base ten: no ruler should ever be marked every 20 seconds
  // or every 100.
  const std::vector<double> allowed{1.0,   2.0,   5.0,    10.0,   15.0,   30.0,
                                    60.0,  120.0, 300.0,  600.0,  900.0,  1800.0,
                                    3600.0, 7200.0, 21600.0, 43200.0};

  for (const double zoom : {0.5, 1.0, 4.0, 20.0, 60.0, 140.0}) {
    const double interval = tick_interval(zoom, kFps, 90.0);
    if (interval < 1.0) continue;
    EXPECT_NE(std::ranges::find(allowed, interval), allowed.end())
        << "at " << zoom << " px/s the ruler wanted a step of " << interval;
  }
}

TEST(TickInterval, LabelsAreNeverCloserThanAsked) {
  for (const double zoom : {0.3, 1.0, 7.0, 63.0, 250.0, 3000.0, 15000.0}) {
    const double interval = tick_interval(zoom, kFps, 90.0);
    EXPECT_GE(interval * zoom, 90.0 - 1e-9) << "labels would overlap at " << zoom << " px/s";
  }
}

TEST(TickInterval, GrowsAsTheViewZoomsOut) {
  double previous = 0.0;
  for (const double zoom : {2000.0, 500.0, 100.0, 20.0, 4.0, 1.0, 0.3}) {
    const double interval = tick_interval(zoom, kFps, 90.0);
    EXPECT_GE(interval, previous) << "the ladder went backwards at " << zoom;
    previous = interval;
  }
}

TEST(TickInterval, SurvivesADegenerateScale) {
  EXPECT_GT(tick_interval(0.0, kFps, 90.0), 0.0);
  EXPECT_GT(tick_interval(1e-6, kFps, 90.0), 0.0);
}

// --------------------------------------------------------------- rulers --

TEST(RulerTicks, LabelledMarksLandOnTheInterval) {
  const TimeScale scale{.pixels_per_second = 100.0};
  const std::vector<Tick> ticks = ruler_ticks(scale, 0.0, 10.0, kFps);
  const std::vector<double> majors = major_times(ticks);

  ASSERT_FALSE(majors.empty());
  const double interval = tick_interval(100.0, kFps, 90.0);
  for (const double time : majors) {
    const double steps = time / interval;
    EXPECT_NEAR(steps, std::round(steps), 1e-6) << "a label at " << time << " is off the grid";
  }
}

TEST(RulerTicks, MinorTicksDivideTheMajorOnesExactly) {
  // Subdivisions that do not divide the labelled step drift out of alignment
  // and make the ruler look broken.
  const TimeScale scale{.pixels_per_second = 260.0};
  const std::vector<Tick> ticks = ruler_ticks(scale, 0.0, 6.0, kFps);
  const std::vector<double> majors = major_times(ticks);
  ASSERT_GE(majors.size(), 2u);

  const double major = majors[1] - majors[0];
  for (const Tick& tick : ticks) {
    if (!tick.major) continue;
    const double steps = tick.time / major;
    EXPECT_NEAR(steps, std::round(steps), 1e-6);
  }

  // And every tick is a whole number of minor steps from the first.
  ASSERT_GE(ticks.size(), 2u);
  const double minor = ticks[1].time - ticks[0].time;
  ASSERT_GT(minor, 0.0);
  for (const Tick& tick : ticks) {
    const double steps = (tick.time - ticks[0].time) / minor;
    EXPECT_NEAR(steps, std::round(steps), 1e-6);
  }
}

TEST(RulerTicks, CoverTheRangeAskedFor) {
  const TimeScale scale{.pixels_per_second = 40.0, .start = 12.0};
  const std::vector<Tick> ticks = ruler_ticks(scale, 12.0, 42.0, kFps);

  ASSERT_FALSE(ticks.empty());
  EXPECT_LE(ticks.front().time, 12.0 + 1e-9);
  EXPECT_GE(ticks.back().time, 41.0) << "the right of the ruler would be blank";
  for (const Tick& tick : ticks) EXPECT_LE(tick.time, 42.0 + 1e-9);
}

TEST(RulerTicks, AreSpacedFarEnoughApartToSee) {
  const TimeScale scale{.pixels_per_second = 100.0};
  const std::vector<Tick> ticks = ruler_ticks(scale, 0.0, 20.0, kFps, 90.0, 7.0);

  for (std::size_t i = 1; i < ticks.size(); ++i) {
    const double gap = scale.width_of(ticks[i].time - ticks[i - 1].time);
    EXPECT_GE(gap, 7.0 - 1e-6) << "the ruler is a grey smear at " << ticks[i].time;
  }
}

TEST(RulerTicks, AnEmptyOrBackwardsRangeGivesNothing) {
  const TimeScale scale;
  EXPECT_TRUE(ruler_ticks(scale, 5.0, 5.0, kFps).empty());
  EXPECT_TRUE(ruler_ticks(scale, 9.0, 2.0, kFps).empty());
}

TEST(RulerTicks, AreBoundedEvenAtARidiculousZoom) {
  // A pathological scale must not ask the painter for a million marks.
  const TimeScale scale{.pixels_per_second = kMaxPixelsPerSecond};
  const std::vector<Tick> ticks = ruler_ticks(scale, 0.0, 100000.0, kFps);
  EXPECT_LE(ticks.size(), 4096u);
}

TEST(RulerTicks, StayReadableAcrossEveryZoomLevel) {
  // The property the whole ladder exists for, swept rather than spot-checked.
  const TimeScale base;
  for (double zoom = kMinPixelsPerSecond; zoom < kMaxPixelsPerSecond; zoom *= 1.7) {
    const TimeScale scale{.pixels_per_second = zoom};
    const std::vector<Tick> ticks = ruler_ticks(scale, 0.0, base.visible_duration(1200.0) * 20.0,
                                                kFps, 90.0, 7.0);
    const std::vector<double> majors = major_times(ticks);
    if (majors.size() < 2) continue;

    const double gap = scale.width_of(majors[1] - majors[0]);
    EXPECT_GE(gap, 90.0 - 1e-6) << "labels overlap at " << zoom << " px/s";
  }
}

}  // namespace
}  // namespace cutline::ui
