/// Between a clip's transform and the box the handles are drawn on.
///
/// The whole of this is one conversion each way, and the reason it needs
/// testing is that the two systems look alike: both are about where a layer is,
/// and one of them is a scale relative to the media while the other is a
/// fraction of the canvas. Getting them the wrong way round produces a picture
/// that is only wrong when the media is not the shape of the frame.

#include "cutline/editor/monitor_binding.hpp"

#include "cutline/core/animate.hpp"
#include "cutline/core/query.hpp"
#include "cutline/editor/inspector.hpp"

#include <gtest/gtest.h>

namespace cutline::editor {
namespace {

/// A 1920x1080 sequence with one clip of `media_w` by `media_h` footage on it.
[[nodiscard]] core::Project project_with(int media_w, int media_h) {
  core::Project p;
  p.canvas_w = 1920;
  p.canvas_h = 1080;

  core::Media media;
  media.id = "m1";
  media.name = "shot.mp4";
  media.duration = 10.0;
  media.has_video = true;
  media.width = media_w;
  media.height = media_h;
  p.media.push_back(media);

  core::Track track{.id = "v1", .kind = core::TrackKind::Video};
  track.clips.push_back(core::Clip{.id = "c1",
                                   .media_id = "m1",
                                   .kind = core::TrackKind::Video,
                                   .source_in = 0.0,
                                   .source_out = 4.0,
                                   .start = 2.0});
  p.tracks.push_back(std::move(track));
  return p;
}

[[nodiscard]] const core::Clip& only_clip(const core::Project& p) {
  return p.tracks.front().clips.front();
}

TEST(MonitorBinding, FootageTheShapeOfTheFrameFillsIt) {
  const auto box = monitor_box(project_with(1920, 1080), "c1", 2.0);
  ASSERT_TRUE(box.has_value());

  EXPECT_DOUBLE_EQ(box->x, 0.5);
  EXPECT_DOUBLE_EQ(box->y, 0.5);
  EXPECT_DOUBLE_EQ(box->width, 1.0);
  EXPECT_DOUBLE_EQ(box->height, 1.0);
  EXPECT_DOUBLE_EQ(box->rotation, 0.0);
}

// Scale 1 is aspect-fit, not "fills the frame", so a square clip in a wide
// sequence is as tall as the frame and nowhere near as wide. A box that came
// out 1x1 here would be the bug this whole file exists to catch.
TEST(MonitorBinding, SquareFootageInAWideSequenceIsTallAndNarrow) {
  const auto box = monitor_box(project_with(1000, 1000), "c1", 2.0);
  ASSERT_TRUE(box.has_value());

  EXPECT_DOUBLE_EQ(box->height, 1.0);
  EXPECT_NEAR(box->width, 1080.0 / 1920.0, 1e-9);
}

TEST(MonitorBinding, TheStoredTransformIsWhereTheBoxIs) {
  core::Project p = project_with(1920, 1080);
  p.tracks[0].clips[0].transform = {
      .x = 0.25, .y = 0.75, .scale_x = 0.5, .scale_y = 0.25, .rotation = 30.0};

  const auto box = monitor_box(p, "c1", 2.0);
  ASSERT_TRUE(box.has_value());
  EXPECT_DOUBLE_EQ(box->x, 0.25);
  EXPECT_DOUBLE_EQ(box->y, 0.75);
  EXPECT_DOUBLE_EQ(box->width, 0.5);
  EXPECT_DOUBLE_EQ(box->height, 0.25);
  EXPECT_DOUBLE_EQ(box->rotation, 30.0);
}

TEST(MonitorBinding, AnAnimatedTransformIsEvaluatedAtTheTimeAsked) {
  core::Project p = project_with(1920, 1080);
  p = core::set_keyframe(std::move(p), "c1", core::AnimProp::X, 0.0, 0.0);
  p = core::set_keyframe(std::move(p), "c1", core::AnimProp::X, 4.0, 1.0);

  // The clip starts at 2 s, so halfway through it is timeline time 4.
  const auto box = monitor_box(p, "c1", 4.0);
  ASSERT_TRUE(box.has_value());
  EXPECT_NEAR(box->x, 0.5, 1e-9);
}

TEST(MonitorBinding, ThereIsNoBoxForThingsThatDrawNoPicture) {
  EXPECT_FALSE(monitor_box(project_with(1920, 1080), "nope", 2.0).has_value());

  core::Project audio = project_with(1920, 1080);
  audio.tracks[0].kind = core::TrackKind::Audio;
  audio.tracks[0].clips[0].kind = core::TrackKind::Audio;
  EXPECT_FALSE(monitor_box(audio, "c1", 2.0).has_value());

  // An adjustment layer sets `has_video` — it contributes a filter, not a
  // picture — so handles for moving it would be handles for moving nothing.
  core::Project adjustment = project_with(1920, 1080);
  adjustment.media[0].is_adjustment = true;
  EXPECT_FALSE(monitor_box(adjustment, "c1", 2.0).has_value());
}


// A selected clip the playhead has run past is not in the picture, and handles
// drawn over whatever is would move something the eye cannot see move.
TEST(MonitorBinding, ThereIsNoBoxWhenThePlayheadIsNotOnTheClip) {
  const core::Project p = project_with(1920, 1080);
  // The clip runs from 2 to 6 seconds.
  EXPECT_FALSE(monitor_box(p, "c1", 1.9).has_value());
  EXPECT_TRUE(monitor_box(p, "c1", 2.0).has_value());
  EXPECT_TRUE(monitor_box(p, "c1", 5.9).has_value());
  EXPECT_FALSE(monitor_box(p, "c1", 6.0).has_value());
}

// ------------------------------------------------------------- and back --

TEST(MonitorBinding, ABoxAppliedAndReadBackIsTheSameBox) {
  core::Project p = project_with(1000, 1000);
  const ui::MonitorBox wanted{
      .x = 0.3, .y = 0.6, .width = 0.4, .height = 0.8, .rotation = -45.0};

  p = apply_monitor_box(std::move(p), "c1", wanted, 2.0);
  const auto read = monitor_box(p, "c1", 2.0);
  ASSERT_TRUE(read.has_value());

  EXPECT_NEAR(read->x, wanted.x, 1e-9);
  EXPECT_NEAR(read->y, wanted.y, 1e-9);
  EXPECT_NEAR(read->width, wanted.width, 1e-9);
  EXPECT_NEAR(read->height, wanted.height, 1e-9);
  EXPECT_NEAR(read->rotation, wanted.rotation, 1e-9);
}

// The handles are drawn round a centre; position names the anchor. When the two
// are the same point there is nothing to get wrong, so both of these move it.

TEST(MonitorBinding, TheBoxSurroundsTheLayerWhenTheAnchorHasBeenMoved) {
  core::Project p = project_with(1920, 1080);
  // Anchored at its top left corner and placed in the middle of the frame, the
  // layer hangs down and to the right — so its centre, which is what the
  // handles go round, is half a layer past the middle on each axis.
  p.tracks.front().clips.front().transform.anchor_x = 0.0;
  p.tracks.front().clips.front().transform.anchor_y = 0.0;

  const auto box = monitor_box(p, "c1", 2.0);
  ASSERT_TRUE(box.has_value());
  EXPECT_NEAR(box->x, 1.0, 1e-9);
  EXPECT_NEAR(box->y, 1.0, 1e-9);
}

TEST(MonitorBinding, ADraggedBoxComesBackTheSameWithAMovedAnchor) {
  core::Project p = project_with(1000, 1000);
  p.tracks.front().clips.front().transform.anchor_x = 0.2;
  p.tracks.front().clips.front().transform.anchor_y = 0.9;

  const ui::MonitorBox wanted{
      .x = 0.3, .y = 0.6, .width = 0.4, .height = 0.8, .rotation = -45.0};
  p = apply_monitor_box(std::move(p), "c1", wanted, 2.0);

  const auto read = monitor_box(p, "c1", 2.0);
  ASSERT_TRUE(read.has_value());
  EXPECT_NEAR(read->x, wanted.x, 1e-9);
  EXPECT_NEAR(read->y, wanted.y, 1e-9);
  EXPECT_NEAR(read->rotation, wanted.rotation, 1e-9);
  // And the anchor itself is left where it was: a drag on the handles moves the
  // layer, never the point it turns about.
  EXPECT_DOUBLE_EQ(only_clip(p).transform.anchor_x, 0.2);
}

TEST(MonitorBinding, TheScaleWrittenIsRelativeToTheAspectFitSize) {
  core::Project p = project_with(1000, 1000);
  // Square footage fits to 1080x1080, which is 0.5625 of the canvas width. A
  // box a full canvas width across is therefore scale_x of about 1.78.
  p = apply_monitor_box(std::move(p), "c1",
                        ui::MonitorBox{.x = 0.5, .y = 0.5, .width = 1.0, .height = 1.0}, 2.0);

  EXPECT_NEAR(only_clip(p).transform.scale_x, 1920.0 / 1080.0, 1e-9);
  EXPECT_NEAR(only_clip(p).transform.scale_y, 1.0, 1e-9);
}

// The monitor and the inspector are two controls for one property, so a drag
// on one has to reach the same place as a drag on the other -- keyframes
// included.
TEST(MonitorBinding, ADragOnAnAnimatedClipWritesKeyframesRatherThanTheStoredValue) {
  core::Project p = project_with(1920, 1080);
  p = core::set_keyframe(std::move(p), "c1", core::AnimProp::X, 0.0, 0.5);
  const core::Transform before = only_clip(p).transform;

  p = apply_monitor_box(std::move(p), "c1",
                        ui::MonitorBox{.x = 0.8, .y = 0.5, .width = 1.0, .height = 1.0}, 3.0);

  // The stored x is untouched -- an animated property ignores it -- and there
  // is a keyframe a second into the clip saying where it went.
  EXPECT_DOUBLE_EQ(only_clip(p).transform.x, before.x);
  EXPECT_NEAR(core::animated_transform(only_clip(p), 1.0).x, 0.8, 1e-9);
}

TEST(MonitorBinding, ABoxForAClipThatIsNotThereChangesNothing) {
  const core::Project p = project_with(1920, 1080);
  EXPECT_EQ(apply_monitor_box(p, "nope", ui::MonitorBox{}, 2.0), p);
}

}  // namespace
}  // namespace cutline::editor
