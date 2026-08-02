/// Between a clip's transform and the box the handles are drawn on.
///
/// The whole of this is one conversion each way, and the reason it needs
/// testing is that the two systems look alike: both are about where a layer is,
/// and one of them is a scale relative to the media while the other is a
/// fraction of the canvas. Getting them the wrong way round produces a picture
/// that is only wrong when the media is not the shape of the frame.

#include "cutline/editor/monitor_binding.hpp"

#include "cutline/core/animate.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/query.hpp"
#include "cutline/editor/inspector.hpp"

#include <gtest/gtest.h>

#include <vector>

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


// -------------------------------------------------------- masks on screen --
//
// A mask is stored in fractions of the *layer* and drawn in fractions of the
// *canvas*, and this is the only layer that knows both. What is worth pinning
// down is the round trip, and that it survives the layer being moved, scaled
// and turned — because that is exactly when the two spaces stop agreeing.

[[nodiscard]] core::Project with_mask(int media_w, int media_h, core::Mask mask) {
  core::Project p = project_with(media_w, media_h);
  core::ClipEffect blur;
  blur.type = "blur";
  blur.params["amount"] = 4.0;
  blur.mask = mask;
  p.tracks.front().clips.front().effects = {blur};
  return p;
}

TEST(MaskOverlays, AMaskInTheMiddleOfACentredLayerIsInTheMiddleOfTheFrame) {
  const core::Project p =
      with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse});

  const std::vector<MaskOverlayRef> masks = mask_overlays(p, "c1", 2.0);
  ASSERT_EQ(masks.size(), 1u);
  EXPECT_EQ(masks[0].effect, 0u);
  EXPECT_NEAR(masks[0].overlay.x, 0.5, 1e-9);
  EXPECT_NEAR(masks[0].overlay.y, 0.5, 1e-9);
  EXPECT_NEAR(masks[0].overlay.width, 0.25, 1e-9) << "a quarter of a full-frame layer";
}

TEST(MaskOverlays, AMaskFollowsTheLayerWhenItMoves) {
  core::Project p = with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse});
  p.tracks.front().clips.front().transform.x = 0.25;

  const std::vector<MaskOverlayRef> masks = mask_overlays(p, "c1", 2.0);
  ASSERT_EQ(masks.size(), 1u);
  EXPECT_NEAR(masks[0].overlay.x, 0.25, 1e-9);
}

TEST(MaskOverlays, AMaskShrinksWithTheLayer) {
  core::Project p = with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse});
  p.tracks.front().clips.front().transform.scale_x = 0.5;
  p.tracks.front().clips.front().transform.scale_y = 0.5;

  const std::vector<MaskOverlayRef> masks = mask_overlays(p, "c1", 2.0);
  ASSERT_EQ(masks.size(), 1u);
  EXPECT_NEAR(masks[0].overlay.width, 0.125, 1e-9);
  EXPECT_NEAR(masks[0].overlay.height, 0.125, 1e-9);
}

TEST(MaskOverlays, AMaskTurnsWithTheLayerAndKeepsItsOwnTurn) {
  core::Project p =
      with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Rectangle, .rotation = 20.0});
  p.tracks.front().clips.front().transform.rotation = 30.0;

  const std::vector<MaskOverlayRef> masks = mask_overlays(p, "c1", 2.0);
  ASSERT_EQ(masks.size(), 1u);
  EXPECT_NEAR(masks[0].overlay.rotation, 50.0, 1e-9);
}

TEST(MaskOverlays, AMaskOffTheLayersCentreSwingsRoundWithIt) {
  // A quarter turn takes a mask that sat to the right of the layer's middle
  // round to below it.
  core::Project p = with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse,
                                                     .x = 0.75,
                                                     .y = 0.5});
  p.tracks.front().clips.front().transform.rotation = 90.0;

  const std::vector<MaskOverlayRef> masks = mask_overlays(p, "c1", 2.0);
  ASSERT_EQ(masks.size(), 1u);
  EXPECT_NEAR(masks[0].overlay.x, 0.5, 1e-9);
  EXPECT_GT(masks[0].overlay.y, 0.5);
}

TEST(MaskOverlays, AnUnmaskedOrDisabledEffectDrawsNothing) {
  core::Project plain = with_mask(1920, 1080, core::Mask{});
  EXPECT_TRUE(mask_overlays(plain, "c1", 2.0).empty()) << "no shape, no overlay";

  core::Project off = with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse});
  off.tracks.front().clips.front().effects.front().enabled = false;
  EXPECT_TRUE(mask_overlays(off, "c1", 2.0).empty()) << "a disabled effect masks nothing";
}

TEST(MaskOverlays, ADraggedOverlayComesBackTheSame) {
  // The property everything else rests on. Anything the drag can produce has to
  // survive the trip back into layer space and out again, or a mask would creep
  // every time it was touched.
  core::Project p = with_mask(1000, 1000, core::Mask{.shape = core::MaskShape::Ellipse});
  core::Clip& clip = p.tracks.front().clips.front();
  clip.transform.x = 0.3;
  clip.transform.y = 0.7;
  clip.transform.scale_x = 0.6;
  clip.transform.scale_y = 1.4;
  clip.transform.rotation = -35.0;

  const ui::MaskOverlay wanted{
      .shape = 1, .x = 0.42, .y = 0.61, .width = 0.11, .height = 0.09, .rotation = 12.0};

  const core::Project after = apply_mask_overlay(p, "c1", 0, wanted, 2.0);
  const std::vector<MaskOverlayRef> masks = mask_overlays(after, "c1", 2.0);
  ASSERT_EQ(masks.size(), 1u);

  EXPECT_NEAR(masks[0].overlay.x, wanted.x, 1e-9);
  EXPECT_NEAR(masks[0].overlay.y, wanted.y, 1e-9);
  EXPECT_NEAR(masks[0].overlay.width, wanted.width, 1e-9);
  EXPECT_NEAR(masks[0].overlay.height, wanted.height, 1e-9);
  EXPECT_NEAR(masks[0].overlay.rotation, wanted.rotation, 1e-9);
}

TEST(MaskOverlays, ADragKeepsWhatItWasNotAsking) {
  // Feather, opacity and inversion are not on the picture, so a drag must not
  // quietly reset them to whatever a default-constructed mask holds.
  core::Project p = with_mask(1920, 1080,
                              core::Mask{.shape = core::MaskShape::Rectangle,
                                         .feather = 0.2,
                                         .opacity = 0.4,
                                         .inverted = true});

  const ui::MaskOverlay moved{.shape = 2, .x = 0.3, .y = 0.3};
  const core::Project after = apply_mask_overlay(p, "c1", 0, moved, 2.0);

  const core::Mask& mask = after.tracks.front().clips.front().effects.front().mask;
  EXPECT_DOUBLE_EQ(mask.feather, 0.2);
  EXPECT_DOUBLE_EQ(mask.opacity, 0.4);
  EXPECT_TRUE(mask.inverted);
  EXPECT_EQ(mask.shape, core::MaskShape::Rectangle);
}

TEST(MaskOverlays, AnEffectThatIsNotThereChangesNothing) {
  const core::Project p =
      with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse});
  EXPECT_EQ(apply_mask_overlay(p, "c1", 7, ui::MaskOverlay{}, 2.0), p);
  EXPECT_EQ(apply_mask_overlay(p, "nope", 0, ui::MaskOverlay{}, 2.0), p);
}

// ------------------------------------------------------ an animated mask --

TEST(MaskOverlays, AnAnimatedMaskIsDrawnWhereItIsRatherThanWhereItWasStored) {
  // The outline on the picture and the shape the renderer uses have to be the
  // same shape. Reading the stored mask instead meant that the moment one of its
  // numbers was animated the outline stopped meaning anything.
  core::Project p = with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse});
  p = core::set_effect_keyframe(std::move(p), "c1", 0, "mask.x", 0.0, 0.25);
  p = core::set_effect_keyframe(std::move(p), "c1", 0, "mask.x", 2.0, 0.75);

  // The clip starts at 2 s, so these are clip-local 0 and 2.
  const std::vector<MaskOverlayRef> at_start = mask_overlays(p, "c1", 2.0);
  ASSERT_EQ(at_start.size(), 1u);
  EXPECT_NEAR(at_start[0].overlay.x, 0.25, 1e-9);

  const std::vector<MaskOverlayRef> at_end = mask_overlays(p, "c1", 4.0);
  ASSERT_EQ(at_end.size(), 1u);
  EXPECT_NEAR(at_end[0].overlay.x, 0.75, 1e-9);
}

TEST(MaskOverlays, DraggingAnAnimatedMaskWritesAKeyframe) {
  // Found on screen: dragging the shape moved the outline and the render
  // ignored it, because the drag wrote the stored number and the animation
  // overrode it on the way out. A drag on the picture and a drag on a number
  // have to be the same edit, which is what going through `set_effect_parameter`
  // buys — the same rule `apply_monitor_box` already followed.
  core::Project p = with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse});
  p = core::set_effect_keyframe(std::move(p), "c1", 0, "mask.x", 0.0, 0.5);

  ui::MaskOverlay moved = mask_overlays(p, "c1", 4.0).front().overlay;
  moved.x = 0.8;
  p = apply_mask_overlay(std::move(p), "c1", 0, moved, 4.0);

  const std::vector<core::Keyframe>& keys =
      p.tracks.front().clips.front().effects[0].keyframes.at("mask.x");
  ASSERT_EQ(keys.size(), 2u) << "the drag did not leave a keyframe behind";
  EXPECT_DOUBLE_EQ(keys[1].t, 2.0) << "at the moment it was dragged, clip-local";
  EXPECT_NEAR(keys[1].v, 0.8, 1e-9);

  // And the one it was already holding is untouched.
  EXPECT_DOUBLE_EQ(keys[0].t, 0.0);
  EXPECT_NEAR(keys[0].v, 0.5, 1e-9);
}

TEST(MaskOverlays, DraggingAMaskThatIsNotAnimatedStillJustMovesIt) {
  core::Project p = with_mask(1920, 1080, core::Mask{.shape = core::MaskShape::Ellipse});

  ui::MaskOverlay moved = mask_overlays(p, "c1", 4.0).front().overlay;
  moved.x = 0.8;
  p = apply_mask_overlay(std::move(p), "c1", 0, moved, 4.0);

  const core::ClipEffect& effect = p.tracks.front().clips.front().effects[0];
  EXPECT_TRUE(effect.keyframes.empty()) << "a drag invented an animation";
  EXPECT_NEAR(effect.mask.x, 0.8, 1e-9);
}

}  // namespace
}  // namespace cutline::editor
