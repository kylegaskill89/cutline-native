#include "cutline/render/plan.hpp"

#include "cutline/core/properties.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::render {
namespace {

using core::BlendMode;
using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

constexpr int kCanvasW = 1920;
constexpr int kCanvasH = 1080;

Media source(std::string id, double duration = 60.0) {
  Media m;
  m.id = std::move(id);
  m.has_video = true;
  m.duration = duration;
  m.width = 1920;
  m.height = 1080;
  return m;
}

Clip clip(std::string id, std::string media_id, double start, double length,
          double source_in = 0.0) {
  Clip c;
  c.id = std::move(id);
  c.media_id = std::move(media_id);
  c.start = start;
  c.source_in = source_in;
  c.source_out = source_in + length;
  return c;
}

Track video_track(std::string id, std::vector<Clip> clips) {
  Track t;
  t.id = std::move(id);
  t.kind = TrackKind::Video;
  t.clips = std::move(clips);
  return t;
}

/// A project with one video track holding one ten-second clip.
Project single_clip() {
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  p.tracks = {video_track("v1", {clip("a", "m", 0.0, 10.0)})};
  return p;
}

// ------------------------------------------------------------------ basics --

TEST(PlanFrame, AnEmptyProjectPlansNothing) {
  Project p;
  EXPECT_TRUE(plan_frame(p, 0.0).empty());
}

TEST(PlanFrame, FindsTheClipUnderThePlayhead) {
  const Project p = single_clip();
  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);

  ASSERT_EQ(layers.size(), 1u);
  EXPECT_EQ(layers[0].clip->id, "a");
  EXPECT_EQ(layers[0].media->id, "m");
  EXPECT_EQ(layers[0].content, LayerContent::Video);
}

TEST(PlanFrame, PlansNothingBeforeOrAfterTheClip) {
  const Project p = single_clip();
  EXPECT_TRUE(plan_frame(p, 15.0).empty());
}

TEST(PlanFrame, TheClipStartIsInclusiveAndTheEndExclusive) {
  // Half-open, so two abutting clips never both draw on the boundary frame.
  const Project p = single_clip();
  EXPECT_EQ(plan_frame(p, 0.0).size(), 1u);
  EXPECT_EQ(plan_frame(p, 10.0).size(), 0u);
}

TEST(PlanFrame, AbuttingClipsNeverBothDraw) {
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  p.tracks = {video_track("v1", {clip("a", "m", 0.0, 5.0), clip("b", "m", 5.0, 5.0)})};

  const std::vector<PlannedLayer> at_cut = plan_frame(p, 5.0);
  ASSERT_EQ(at_cut.size(), 1u);
  EXPECT_EQ(at_cut[0].clip->id, "b");
}

// -------------------------------------------------------------- visibility --

TEST(PlanFrame, ADisabledClipIsLeftOut) {
  Project p = single_clip();
  p.tracks[0].clips[0].disabled = true;
  EXPECT_TRUE(plan_frame(p, 5.0).empty());
}

TEST(PlanFrame, AHiddenTrackIsLeftOut) {
  Project p = single_clip();
  p.tracks[0].hidden = true;
  EXPECT_TRUE(plan_frame(p, 5.0).empty());
}

TEST(PlanFrame, HidingOneTrackLeavesTheOthers) {
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  p.tracks = {video_track("top", {clip("t", "m", 0.0, 10.0)}),
              video_track("bottom", {clip("b", "m", 0.0, 10.0)})};
  p.tracks[0].hidden = true;

  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);
  ASSERT_EQ(layers.size(), 1u);
  EXPECT_EQ(layers[0].clip->id, "b");
}

TEST(PlanFrame, AudioTracksAreNotPlanned) {
  Project p = single_clip();
  Track audio;
  audio.id = "a1";
  audio.kind = TrackKind::Audio;
  audio.clips = {clip("audio", "m", 0.0, 10.0)};
  p.tracks.push_back(audio);

  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);
  ASSERT_EQ(layers.size(), 1u);
  EXPECT_EQ(layers[0].clip->id, "a");
}

// -------------------------------------------------------------- draw order --

TEST(PlanFrame, TheBottomTrackDrawsFirst) {
  // Tracks are stored top-first, so the last one in the list is the base and
  // must come out of the plan first — everything else composites over it.
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  p.tracks = {video_track("top", {clip("t", "m", 0.0, 10.0)}),
              video_track("middle", {clip("m2", "m", 0.0, 10.0)}),
              video_track("bottom", {clip("b", "m", 0.0, 10.0)})};

  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);
  ASSERT_EQ(layers.size(), 3u);
  EXPECT_EQ(layers[0].clip->id, "b");
  EXPECT_EQ(layers[1].clip->id, "m2");
  EXPECT_EQ(layers[2].clip->id, "t");
}

TEST(PlanFrame, TrackIndexCountsFromTheBottom) {
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  p.tracks = {video_track("top", {clip("t", "m", 0.0, 10.0)}),
              video_track("bottom", {clip("b", "m", 0.0, 10.0)})};

  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);
  ASSERT_EQ(layers.size(), 2u);
  EXPECT_EQ(layers[0].track_index, 0);
  EXPECT_EQ(layers[1].track_index, 1);
}

TEST(PlanFrame, DuringADissolveTheIncomingClipDrawsOnTop) {
  // Both segments are live at once during a transition. The one that starts
  // later is the incoming clip, and it has to land over the outgoing one or
  // the cross-fade runs backwards.
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};

  Clip a = clip("a", "m", 0.0, 5.0, 5.0);
  a.transition_out = core::Transition{.kind = core::TransitionKind::Dissolve, .duration = 2.0};
  p.tracks = {video_track("v1", {a, clip("b", "m", 5.0, 5.0, 5.0)})};

  // Mid-transition: the overlap is centred on the cut.
  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);
  ASSERT_EQ(layers.size(), 2u);
  EXPECT_EQ(layers[0].clip->id, "a") << "outgoing clip should be underneath";
  EXPECT_EQ(layers[1].clip->id, "b") << "incoming clip should be on top";
  EXPECT_GT(layers[1].alpha, 0.0);
  EXPECT_LT(layers[1].alpha, 1.0) << "the incoming clip should be ramping up";
}

// ----------------------------------------------------------------- content --

TEST(PlanFrame, RecognisesEachKindOfGeneratedMedia) {
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;

  Media title;
  title.id = "title";
  title.is_text = true;
  title.text = core::TextSpec{};

  Media matte;
  matte.id = "matte";
  matte.is_color = true;
  matte.color = "#ff0000";

  Media adjustment;
  adjustment.id = "adj";
  adjustment.is_adjustment = true;

  Media still;
  still.id = "still";
  still.is_image = true;
  still.width = 800;
  still.height = 600;

  p.media = {title, matte, adjustment, still};
  // Stored top-first, so this list reverses on the way out.
  p.tracks = {video_track("v4", {clip("d", "still", 0.0, 10.0)}),
              video_track("v3", {clip("c", "adj", 0.0, 10.0)}),
              video_track("v2", {clip("b", "matte", 0.0, 10.0)}),
              video_track("v1", {clip("a", "title", 0.0, 10.0)})};

  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);
  ASSERT_EQ(layers.size(), 4u);
  EXPECT_EQ(layers[0].content, LayerContent::Text);
  EXPECT_EQ(layers[1].content, LayerContent::Color);
  EXPECT_EQ(layers[2].content, LayerContent::Adjustment);
  EXPECT_EQ(layers[3].content, LayerContent::Still);
}

TEST(PlanFrame, AClipWithMissingMediaStillPlansRatherThanVanishing) {
  // Media can go missing between sessions. The layer must survive so the UI can
  // show it as offline; silently dropping it would look like the clip was
  // deleted.
  Project p = single_clip();
  p.media.clear();

  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);
  ASSERT_EQ(layers.size(), 1u);
  EXPECT_EQ(layers[0].media, nullptr);
}

// -------------------------------------------------------------- properties --

TEST(PlanFrame, CarriesTheBlendMode) {
  Project p = single_clip();
  p.tracks[0].clips[0].blend = BlendMode::Screen;
  EXPECT_EQ(plan_frame(p, 5.0)[0].blend, BlendMode::Screen);
}

TEST(PlanFrame, CarriesOpacityAsAlpha) {
  Project p = single_clip();
  p.tracks[0].clips[0].opacity = 0.25;
  EXPECT_DOUBLE_EQ(plan_frame(p, 5.0)[0].alpha, 0.25);
}

TEST(PlanFrame, GeometryComesFromTheCanvasSize) {
  const Project p = single_clip();
  const PlannedLayer layer = plan_frame(p, 5.0)[0];

  EXPECT_DOUBLE_EQ(layer.box.center_x, kCanvasW / 2.0);
  EXPECT_DOUBLE_EQ(layer.box.center_y, kCanvasH / 2.0);
  EXPECT_DOUBLE_EQ(layer.box.width, static_cast<double>(kCanvasW));
  EXPECT_DOUBLE_EQ(layer.box.height, static_cast<double>(kCanvasH));
}

// -------------------------------------------------------------- source time --

TEST(PlanFrame, SourceTimeTracksThePlayheadThroughTheTrim) {
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  // Source [20, 30) placed at timeline 4.
  p.tracks = {video_track("v1", {clip("a", "m", 4.0, 10.0, 20.0)})};

  EXPECT_DOUBLE_EQ(plan_frame(p, 4.0)[0].source_time, 20.0);
  EXPECT_DOUBLE_EQ(plan_frame(p, 9.0)[0].source_time, 25.0);
}

TEST(PlanFrame, SourceTimeAccountsForSpeed) {
  Project p = single_clip();
  p.tracks[0].clips[0].speed = 2.0;

  // At double speed, one second of timeline consumes two of source.
  EXPECT_DOUBLE_EQ(plan_frame(p, 1.0)[0].source_time, 2.0);
}

TEST(PlanFrame, SourceTimeRunsBackwardsForAReversedClip) {
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  Clip c = clip("a", "m", 0.0, 10.0, 0.0);
  c.reverse = true;
  p.tracks = {video_track("v1", {c})};

  const double early = plan_frame(p, 1.0)[0].source_time;
  const double late = plan_frame(p, 8.0)[0].source_time;
  EXPECT_GT(early, late) << "a reversed clip should walk its source backwards";
}

TEST(PlanFrame, SourceTimeNeverReachesTheSegmentEnd) {
  // Landing exactly on source_out would decode the first frame past the trim,
  // showing one frame of the wrong shot.
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  p.tracks = {video_track("v1", {clip("a", "m", 0.0, 10.0, 5.0)})};

  const PlannedLayer last = plan_frame(p, 9.9999)[0];
  EXPECT_LT(last.source_time, 15.0);
}

TEST(PlanFrame, SourceTimeNeverPrecedesTheSegmentStart) {
  Project p;
  p.canvas_w = kCanvasW;
  p.canvas_h = kCanvasH;
  p.media = {source("m")};
  p.tracks = {video_track("v1", {clip("a", "m", 0.0, 10.0, 5.0)})};

  EXPECT_GE(plan_frame(p, 0.0)[0].source_time, 5.0);
}

}  // namespace
}  // namespace cutline::render
