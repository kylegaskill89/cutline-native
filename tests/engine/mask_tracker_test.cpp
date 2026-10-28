/// Mask tracking through the renderer: a mask over a picture that moves,
/// carried along with it. The tracker's own arithmetic is tested on its own in
/// the track tests; this is the join between it, the clip and the mask.

#include "cutline/engine/mask_tracker.hpp"

#include "cutline/engine/frame_renderer.hpp"

#include "cutline/core/nesting.hpp"
#include "cutline/core/sequences.hpp"

#include <gtest/gtest.h>

#include <format>
#include <random>

namespace cutline::engine {
namespace {

using core::Clip;
using core::Media;
using core::Project;

constexpr int kWidth = 320;
constexpr int kHeight = 180;

std::shared_ptr<gpu::Device> shared_device() {
  static std::shared_ptr<gpu::Device> device = [] {
    auto created = gpu::Device::create({.allow_software = true});
    return created ? *created : nullptr;
  }();
  return device;
}

/// Squares of many sizes and shades scattered over the frame: corners
/// everywhere, nothing repeating.
[[nodiscard]] Media scatter() {
  Media m;
  m.id = "g";
  m.is_graphic = true;
  core::Graphic graphic;
  std::mt19937 random(3);
  std::uniform_real_distribution<double> place(0.05, 0.95);
  std::uniform_real_distribution<double> size(6.0, 22.0);
  std::uniform_int_distribution<int> shade(40, 255);
  for (int i = 0; i < 60; ++i) {
    core::GraphicLayer layer;
    layer.id = "s" + std::to_string(i);
    layer.kind = core::GraphicKind::Rectangle;
    layer.width = size(random);
    layer.height = size(random);
    layer.x = place(random);
    layer.y = place(random);
    layer.style.fill = std::format("#{:02x}{:02x}{:02x}", shade(random), shade(random),
                                   shade(random));
    graphic.layers.push_back(std::move(layer));
  }
  m.graphic = std::move(graphic);
  return m;
}

/// A nest of the scattered squares sliding right a tenth of the frame a
/// second, with a rectangle mask in the middle of the effect on the clip that
/// holds it.
[[nodiscard]] Project sliding(core::MaskTracking tracking, double speed = 0.1) {
  Project p;
  p.sequence().canvas_w = kWidth;
  p.sequence().canvas_h = kHeight;
  p.sequence().fps = 24.0;
  p.media = {scatter()};
  p = core::add_sequence(std::move(p), "Inner");
  core::Sequence& inner = p.sequences[1];
  inner.canvas_w = kWidth;
  inner.canvas_h = kHeight;
  inner.fps = 24.0;
  Clip squares;
  squares.id = "squares";
  squares.media_id = "g";
  squares.source_out = 5.0;
  squares.keyframes[static_cast<std::size_t>(core::AnimProp::X)] = {
      core::Keyframe{.t = 0.0, .v = 0.5}, core::Keyframe{.t = 5.0, .v = 0.5 + 5.0 * speed}};
  inner.tracks[0].clips = {squares};

  Media nest;
  nest.id = "nest";
  nest.sequence_id = inner.id;
  p.media.push_back(nest);
  p = core::sync_nested_media(std::move(p));

  Clip outer;
  outer.id = "outer";
  outer.media_id = "nest";
  outer.source_out = 5.0;
  core::ClipEffect effect{.type = "blur", .params = {{"amount", 10.0}}};
  effect.mask = core::Mask{.shape = core::MaskShape::Rectangle,
                           .x = 0.5,
                           .y = 0.5,
                           .width = 0.2,
                           .height = 0.25,
                           .tracking = tracking};
  outer.effects = {effect};
  core::Track track;
  track.id = "v1";
  track.kind = core::TrackKind::Video;
  track.clips = {outer};
  p.sequence().tracks = {track};
  return p;
}

class MaskTrackerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
    // The shot is a graphic, and a build without the text library draws
    // graphics as nothing: a blank shot has nothing to follow.
    if (!can_draw_text()) GTEST_SKIP() << "this build cannot draw the graphic the shot is made of";
  }
};

TEST(MaskOutline, ARectangleIsItsFourCornersInPixels) {
  const core::Mask mask{.shape = core::MaskShape::Rectangle, .x = 0.5, .y = 0.5,
                        .width = 0.25, .height = 0.1};
  const auto outline = mask_outline(mask, 200.0, 100.0);
  ASSERT_EQ(outline.size(), 4u);
  EXPECT_DOUBLE_EQ(outline[0].x, 50.0);
  EXPECT_DOUBLE_EQ(outline[0].y, 40.0);
  EXPECT_DOUBLE_EQ(outline[2].x, 150.0);
  EXPECT_DOUBLE_EQ(outline[2].y, 60.0);
}

TEST(MovedMask, EachMethodMovesOnlyWhatItFollows) {
  const track::Similarity motion{.origin = {100.0, 50.0}, .dx = 20.0, .dy = 10.0,
                                 .rotation = 0.1, .scale = 1.5};
  core::Mask mask{.shape = core::MaskShape::Ellipse, .x = 0.5, .y = 0.5, .width = 0.2,
                  .height = 0.2};
  mask.tracking = core::MaskTracking::Position;
  const core::Mask placed = moved_mask(mask, motion, 200.0, 100.0);
  EXPECT_DOUBLE_EQ(placed.x, 0.6);
  EXPECT_DOUBLE_EQ(placed.y, 0.6);
  EXPECT_EQ(placed.rotation, 0.0);
  EXPECT_EQ(placed.width, 0.2);

  mask.tracking = core::MaskTracking::PositionScaleRotation;
  const core::Mask all = moved_mask(mask, motion, 200.0, 100.0);
  EXPECT_NEAR(all.rotation, 5.7296, 1e-3);
  EXPECT_NEAR(all.width, 0.3, 1e-12);
}

TEST_F(MaskTrackerTest, AMaskIsCarriedAlongWithThePictureUnderIt) {
  const Project p = sliding(core::MaskTracking::Position);
  const auto result = track_mask(shared_device(), p,
                                 MaskTrackRequest{.clip_id = "outer", .effect = 0, .from = 0.5});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_TRUE(result->stopped.empty()) << result->stopped;
  // From half a second to the last frame before five, one entry a frame.
  ASSERT_EQ(result->frames.size(), 108u);
  EXPECT_DOUBLE_EQ(result->frames.front().mask.x, 0.5);
  // A tenth of the frame a second, for a second.
  const core::TrackedMask& later = result->frames[24];
  EXPECT_NEAR(later.t, 1.5, 1e-9);
  EXPECT_NEAR(later.mask.x, 0.6, 0.004);
  EXPECT_NEAR(later.mask.y, 0.5, 0.004);
}

TEST_F(MaskTrackerTest, OneFrameBackwardIsOneStepTheOtherWay) {
  const Project p = sliding(core::MaskTracking::Position);
  const auto result = track_mask(
      shared_device(), p,
      MaskTrackRequest{.clip_id = "outer", .effect = 0, .from = 2.0, .backward = true,
                       .one_frame = true});
  ASSERT_TRUE(result.has_value()) << result.error();
  ASSERT_EQ(result->frames.size(), 2u);
  EXPECT_NEAR(result->frames[1].t, 2.0 - 1.0 / 24.0, 1e-9);
  EXPECT_NEAR(result->frames[1].mask.x, 0.5 - 0.1 / 24.0, 0.002);
}

TEST_F(MaskTrackerTest, AStillPictureLeavesTheMaskWhereItIs) {
  const Project p = sliding(core::MaskTracking::PositionScaleRotation, 0.0);
  const auto result = track_mask(
      shared_device(), p,
      MaskTrackRequest{.clip_id = "outer", .effect = 0, .from = 1.0, .one_frame = true});
  ASSERT_TRUE(result.has_value()) << result.error();
  ASSERT_EQ(result->frames.size(), 2u);
  EXPECT_NEAR(result->frames[1].mask.x, 0.5, 1e-3);
  EXPECT_NEAR(result->frames[1].mask.rotation, 0.0, 0.05);
  EXPECT_NEAR(result->frames[1].mask.width, 0.2, 1e-3);
}

TEST_F(MaskTrackerTest, StoppingKeepsWhatWasTracked) {
  const Project p = sliding(core::MaskTracking::Position);
  const auto result = track_mask(shared_device(), p,
                                 MaskTrackRequest{.clip_id = "outer", .effect = 0, .from = 0.0},
                                 [](int done, int) { return done < 5; });
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(result->frames.size(), 6u);
  EXPECT_FALSE(result->stopped.empty());
}

TEST_F(MaskTrackerTest, AnEffectWithoutAMaskIsRefused) {
  Project p = sliding(core::MaskTracking::Position);
  p.sequence().tracks[0].clips[0].effects[0].mask = core::Mask{};
  EXPECT_FALSE(
      track_mask(shared_device(), p, MaskTrackRequest{.clip_id = "outer", .effect = 0})
          .has_value());
}

}  // namespace
}  // namespace cutline::engine
