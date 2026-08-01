#include "cutline/core/layout.hpp"

#include "cutline/core/animate.hpp"

#include <gtest/gtest.h>

namespace cutline::core {
namespace {

constexpr double kCanvasW = 1920.0;
constexpr double kCanvasH = 1080.0;

Media video_media(int width, int height) {
  Media m;
  m.id = "m";
  m.has_video = true;
  m.width = width;
  m.height = height;
  return m;
}

Clip clip_at(double start, double duration) {
  Clip c;
  c.id = "c";
  c.media_id = "m";
  c.start = start;
  c.source_in = 0.0;
  c.source_out = duration;
  return c;
}

VideoSeg seg_for(const Clip& c) {
  VideoSeg s;
  s.clip = &c;
  s.start = c.start;
  s.end = c.start + (c.source_out - c.source_in);
  s.source_in = c.source_in;
  s.source_out = c.source_out;
  return s;
}

// ------------------------------------------------------------ natural size --

TEST(NaturalSize, MatchingAspectFillsTheCanvasExactly) {
  const Media m = video_media(3840, 2160);
  const Size size = natural_size(&m, kCanvasW, kCanvasH);
  EXPECT_DOUBLE_EQ(size.width, 1920.0);
  EXPECT_DOUBLE_EQ(size.height, 1080.0);
}

TEST(NaturalSize, TallerThanTheCanvasIsLimitedByHeight) {
  // 1000x1000 into 1920x1080: the height is the binding constraint, so the
  // result is square and letterboxed left and right, never cropped.
  const Media m = video_media(1000, 1000);
  const Size size = natural_size(&m, kCanvasW, kCanvasH);
  EXPECT_DOUBLE_EQ(size.width, 1080.0);
  EXPECT_DOUBLE_EQ(size.height, 1080.0);
}

TEST(NaturalSize, WiderThanTheCanvasIsLimitedByWidth) {
  const Media m = video_media(4000, 1000);
  const Size size = natural_size(&m, kCanvasW, kCanvasH);
  EXPECT_DOUBLE_EQ(size.width, 1920.0);
  EXPECT_DOUBLE_EQ(size.height, 480.0);
}

TEST(NaturalSize, SmallMediaIsScaledUpNotLeftAtNativeSize) {
  // Scale 1 means "fit the canvas", not "native pixels" — a 320x180 clip fills
  // the frame rather than sitting tiny in the middle.
  const Media m = video_media(320, 180);
  const Size size = natural_size(&m, kCanvasW, kCanvasH);
  EXPECT_DOUBLE_EQ(size.width, 1920.0);
  EXPECT_DOUBLE_EQ(size.height, 1080.0);
}

TEST(NaturalSize, GeneratedMediaWithoutDimensionsCoversTheCanvas) {
  Media matte;
  matte.id = "matte";
  matte.is_color = true;
  const Size size = natural_size(&matte, kCanvasW, kCanvasH);
  EXPECT_DOUBLE_EQ(size.width, kCanvasW);
  EXPECT_DOUBLE_EQ(size.height, kCanvasH);
}

TEST(NaturalSize, MissingMediaCoversTheCanvas) {
  const Size size = natural_size(nullptr, kCanvasW, kCanvasH);
  EXPECT_DOUBLE_EQ(size.width, kCanvasW);
  EXPECT_DOUBLE_EQ(size.height, kCanvasH);
}

TEST(NaturalSize, TextUsesItsMeasurementRatherThanStoredDimensions) {
  Media title;
  title.id = "t";
  title.is_text = true;
  title.text = TextSpec{};
  const Size size = natural_size(&title, kCanvasW, kCanvasH, {600.0, 120.0});
  EXPECT_DOUBLE_EQ(size.width, 600.0);
  EXPECT_DOUBLE_EQ(size.height, 120.0);
}

TEST(NaturalSize, UnmeasuredTextFallsBackRatherThanCollapsing) {
  // A title that has not been laid out yet must not come out zero-sized; it
  // would silently vanish from the composite.
  Media title;
  title.id = "t";
  title.is_text = true;
  title.text = TextSpec{};
  const Size size = natural_size(&title, kCanvasW, kCanvasH);
  EXPECT_GT(size.width, 0.0);
  EXPECT_GT(size.height, 0.0);
}

// --------------------------------------------------------------- layer box --

TEST(LayerBox, DefaultTransformCentresAndFits) {
  const Media m = video_media(3840, 2160);
  const Clip c = clip_at(0.0, 5.0);
  const LayerBox box = layer_box(c, &m, kCanvasW, kCanvasH, 0.0);

  EXPECT_DOUBLE_EQ(box.center_x, 960.0);
  EXPECT_DOUBLE_EQ(box.center_y, 540.0);
  EXPECT_DOUBLE_EQ(box.width, 1920.0);
  EXPECT_DOUBLE_EQ(box.height, 1080.0);
  EXPECT_DOUBLE_EQ(box.rotation_deg, 0.0);
}

TEST(LayerBox, PositionIsAFractionOfTheCanvas) {
  const Media m = video_media(1920, 1080);
  Clip c = clip_at(0.0, 5.0);
  c.transform = Transform{.x = 0.25, .y = 0.75};

  const LayerBox box = layer_box(c, &m, kCanvasW, kCanvasH, 0.0);
  EXPECT_DOUBLE_EQ(box.center_x, 480.0);
  EXPECT_DOUBLE_EQ(box.center_y, 810.0);
}

TEST(LayerBox, ScaleIsPerAxisAndMultipliesTheFittedSize) {
  const Media m = video_media(1920, 1080);
  Clip c = clip_at(0.0, 5.0);
  c.transform = Transform{.scale_x = 0.5, .scale_y = 2.0};

  const LayerBox box = layer_box(c, &m, kCanvasW, kCanvasH, 0.0);
  EXPECT_DOUBLE_EQ(box.width, 960.0);
  EXPECT_DOUBLE_EQ(box.height, 2160.0);
}

TEST(LayerBox, GeometryIsIndependentOfExportResolution) {
  // The same project rendered at 1080p and at 4K must place the clip at the
  // same *fraction* of the frame. This is the whole reason positions are
  // stored as fractions and scale is relative to a canvas fit.
  const Media m = video_media(1280, 720);
  Clip c = clip_at(0.0, 5.0);
  c.transform = Transform{.x = 0.3, .y = 0.6, .scale_x = 0.5, .scale_y = 0.5};

  const LayerBox hd = layer_box(c, &m, 1920.0, 1080.0, 0.0);
  const LayerBox uhd = layer_box(c, &m, 3840.0, 2160.0, 0.0);

  EXPECT_DOUBLE_EQ(uhd.center_x / 3840.0, hd.center_x / 1920.0);
  EXPECT_DOUBLE_EQ(uhd.center_y / 2160.0, hd.center_y / 1080.0);
  EXPECT_DOUBLE_EQ(uhd.width / 3840.0, hd.width / 1920.0);
  EXPECT_DOUBLE_EQ(uhd.height / 2160.0, hd.height / 1080.0);
}

// ------------------------------------------------------------ anchor point --
//
// The anchor is the point of the layer that Position places and that scale and
// rotation happen about. Everything below the transform still sees a centred
// rectangle, so these tests are about one thing: where that centre ends up.

TEST(AnchorPoint, TheMiddleOfTheLayerLeavesEverythingAsItWas) {
  const Media m = video_media(1920, 1080);
  Clip plain = clip_at(0.0, 5.0);
  plain.transform = Transform{.x = 0.3, .scale_x = 0.5, .rotation = 30.0};

  Clip spelt_out = plain;
  spelt_out.transform.anchor_x = 0.5;
  spelt_out.transform.anchor_y = 0.5;

  EXPECT_EQ(layer_box(plain, &m, kCanvasW, kCanvasH, 0.0),
            layer_box(spelt_out, &m, kCanvasW, kCanvasH, 0.0));
}

TEST(AnchorPoint, PositionPlacesTheAnchorRatherThanTheCentre) {
  // Anchor at the layer's top left, position in the middle of the frame: the
  // layer hangs down and to the right of the centre by half its size.
  const Media m = video_media(1920, 1080);
  Clip c = clip_at(0.0, 5.0);
  c.transform = Transform{.scale_x = 0.5, .scale_y = 0.5, .anchor_x = 0.0, .anchor_y = 0.0};

  const LayerBox box = layer_box(c, &m, kCanvasW, kCanvasH, 0.0);
  EXPECT_DOUBLE_EQ(box.width, 960.0);
  EXPECT_DOUBLE_EQ(box.height, 540.0);
  EXPECT_DOUBLE_EQ(box.center_x, 960.0 + 480.0);
  EXPECT_DOUBLE_EQ(box.center_y, 540.0 + 270.0);
}

TEST(AnchorPoint, RotationSwingsTheLayerAboutIt) {
  // A quarter turn about the layer's top left corner. The anchor stays put in
  // the middle of the frame and the centre, which was down-right of it, comes
  // round to down-left — which is the thing that could not be expressed at all
  // before there was an anchor.
  const Media m = video_media(1920, 1080);
  Clip c = clip_at(0.0, 5.0);
  c.transform = Transform{
      .scale_x = 0.5, .scale_y = 0.5, .rotation = 90.0, .anchor_x = 0.0, .anchor_y = 0.0};

  const LayerBox box = layer_box(c, &m, kCanvasW, kCanvasH, 0.0);
  EXPECT_NEAR(box.center_x, 960.0 - 270.0, 1e-9);
  EXPECT_NEAR(box.center_y, 540.0 + 480.0, 1e-9);
}

TEST(AnchorPoint, ScaleGrowsAwayFromIt) {
  // Pinned by its left edge, a layer scaled up stays pinned there rather than
  // spreading both ways.
  const Media m = video_media(1920, 1080);
  Clip c = clip_at(0.0, 5.0);
  c.transform = Transform{.anchor_x = 0.0};

  const LayerBox small = layer_box(c, &m, kCanvasW, kCanvasH, 0.0);
  c.transform.scale_x = 2.0;
  const LayerBox large = layer_box(c, &m, kCanvasW, kCanvasH, 0.0);

  const double left = small.center_x - small.width * 0.5;
  EXPECT_DOUBLE_EQ(large.center_x - large.width * 0.5, left);
}

TEST(AnchorPoint, IsKeyframeableLikeTheRestOfTheTransform) {
  const Media m = video_media(1920, 1080);
  Project p;
  p.media = {m};
  Track track;
  track.id = "v1";
  track.kind = TrackKind::Video;
  track.clips = {clip_at(0.0, 4.0)};
  p.tracks = {track};

  p = set_keyframe(p, "c", AnimProp::AnchorX, 0.0, 0.5);
  p = set_keyframe(p, "c", AnimProp::AnchorX, 4.0, 0.0);
  p = set_keyframe_interp(p, "c", AnimProp::AnchorX, Interp::Linear);

  const Clip& c = p.tracks[0].clips[0];
  // Halfway along, the anchor is a quarter of the way across the layer, so the
  // centre sits a quarter of the layer's width to the right of it.
  EXPECT_DOUBLE_EQ(layer_box(c, &m, kCanvasW, kCanvasH, 0.0).center_x, 960.0);
  EXPECT_DOUBLE_EQ(layer_box(c, &m, kCanvasW, kCanvasH, 2.0).center_x, 960.0 + 480.0);
  EXPECT_DOUBLE_EQ(layer_box(c, &m, kCanvasW, kCanvasH, 4.0).center_x, 960.0 + 960.0);
}

TEST(LayerBox, KeyframesAreEvaluatedInClipLocalTime) {
  const Media m = video_media(1920, 1080);
  Project p;
  p.media = {m};
  Track track;
  track.id = "v1";
  track.kind = TrackKind::Video;
  track.clips = {clip_at(10.0, 4.0)};
  p.tracks = {track};

  // Animate x from 0 to 1 over the clip's four seconds. The keyframe times are
  // local, so timeline 12 is local 2 — halfway.
  p = set_keyframe(p, "c", AnimProp::X, 0.0, 0.0);
  p = set_keyframe(p, "c", AnimProp::X, 4.0, 1.0);
  p = set_keyframe_interp(p, "c", AnimProp::X, Interp::Linear);

  const Clip& c = p.tracks[0].clips[0];
  EXPECT_DOUBLE_EQ(layer_box(c, &m, kCanvasW, kCanvasH, 10.0).center_x, 0.0);
  EXPECT_DOUBLE_EQ(layer_box(c, &m, kCanvasW, kCanvasH, 12.0).center_x, 960.0);
  EXPECT_DOUBLE_EQ(layer_box(c, &m, kCanvasW, kCanvasH, 14.0).center_x, 1920.0);
}

// ----------------------------------------------------------- segment alpha --

TEST(SegmentAlpha, PlainClipIsFullyOpaque) {
  const Clip c = clip_at(0.0, 5.0);
  EXPECT_DOUBLE_EQ(segment_alpha(seg_for(c), 2.5), 1.0);
}

TEST(SegmentAlpha, OpacityScalesTheWholeSegment) {
  Clip c = clip_at(0.0, 5.0);
  c.opacity = 0.4;
  EXPECT_DOUBLE_EQ(segment_alpha(seg_for(c), 2.5), 0.4);
}

TEST(SegmentAlpha, FadeInRampsLinearlyFromZero) {
  Clip c = clip_at(0.0, 5.0);
  c.fade_in = 2.0;
  const VideoSeg s = seg_for(c);

  EXPECT_DOUBLE_EQ(segment_alpha(s, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 1.0), 0.5);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 2.0), 1.0);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 3.0), 1.0);
}

TEST(SegmentAlpha, FadeOutRampsToZeroAtTheEnd) {
  Clip c = clip_at(0.0, 5.0);
  c.fade_out = 2.0;
  const VideoSeg s = seg_for(c);

  EXPECT_DOUBLE_EQ(segment_alpha(s, 2.0), 1.0);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 4.0), 0.5);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 5.0), 0.0);
}

TEST(SegmentAlpha, FadesAndOpacityMultiply) {
  Clip c = clip_at(0.0, 5.0);
  c.opacity = 0.5;
  c.fade_in = 2.0;
  EXPECT_DOUBLE_EQ(segment_alpha(seg_for(c), 1.0), 0.25);
}

TEST(SegmentAlpha, TransitionRampAppliesWithoutAManualFade) {
  const Clip c = clip_at(0.0, 5.0);
  VideoSeg s = seg_for(c);
  s.x_in = 1.0;
  EXPECT_DOUBLE_EQ(segment_alpha(s, 0.5), 0.5);
}

TEST(SegmentAlpha, TheLongerOfFadeAndTransitionWinsRatherThanBothApplying) {
  // A one-second dissolve on a clip that also has a two-second fade must ramp
  // over two seconds, not over one and then again over the other.
  Clip c = clip_at(0.0, 5.0);
  c.fade_in = 2.0;
  VideoSeg s = seg_for(c);
  s.x_in = 1.0;

  EXPECT_DOUBLE_EQ(segment_alpha(s, 1.0), 0.5);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 2.0), 1.0);
}

TEST(SegmentAlpha, RampsRunOverTheSegmentNotTheClip) {
  // A transition widens the segment past the clip's own bounds by borrowing
  // handles. The fade belongs to what is drawn, so it starts where the segment
  // starts.
  const Clip c = clip_at(4.0, 5.0);
  VideoSeg s = seg_for(c);
  s.start = 3.0;  // one second of borrowed head handle
  s.x_in = 1.0;

  EXPECT_DOUBLE_EQ(segment_alpha(s, 3.0), 0.0);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 3.5), 0.5);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 4.0), 1.0);
}

TEST(SegmentAlpha, NeverGoesNegativeOutsideTheSegment) {
  Clip c = clip_at(0.0, 5.0);
  c.fade_out = 2.0;
  const VideoSeg s = seg_for(c);
  EXPECT_DOUBLE_EQ(segment_alpha(s, 6.0), 0.0);
}

}  // namespace
}  // namespace cutline::core
