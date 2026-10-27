/// What a tracking run leaves on the mask: keyframes, as far as the method
/// follows the picture.

#include "cutline/editor/mask_tracking.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace cutline::editor {
namespace {

[[nodiscard]] core::Project masked(core::Mask mask) {
  core::Clip c;
  c.id = "c1";
  c.media_id = "m1";
  c.source_out = 5.0;
  core::ClipEffect blur{.type = "blur"};
  blur.mask = std::move(mask);
  c.effects = {blur};
  core::Track t;
  t.id = "v1";
  t.kind = core::TrackKind::Video;
  t.clips = {c};
  core::Project p;
  p.sequence().tracks = {t};
  return p;
}

[[nodiscard]] const core::ClipEffect& effect_of(const core::Project& p) {
  return p.sequence().tracks[0].clips[0].effects[0];
}

/// Two frames: where it started and where it went.
[[nodiscard]] std::vector<core::TrackedMask> run(core::Mask start) {
  core::Mask moved = start;
  moved.x += 0.1;
  moved.rotation += 5.0;
  moved.width *= 2.0;
  for (core::MaskPoint& p : moved.points) p.x *= 2.0;
  return {core::TrackedMask{.t = 1.0, .mask = start}, core::TrackedMask{.t = 1.04, .mask = moved}};
}

TEST(ApplyTrackedMasks, PositionKeysOnlyThePosition) {
  const core::Mask mask{.shape = core::MaskShape::Ellipse, .tracking = core::MaskTracking::Position};
  const core::Project p = apply_tracked_masks(masked(mask), "c1", 0, run(mask));
  const core::ClipEffect& e = effect_of(p);
  ASSERT_EQ(e.keyframes.at("mask.x").size(), 2u);
  EXPECT_DOUBLE_EQ(e.keyframes.at("mask.x")[1].v, 0.6);
  EXPECT_FALSE(e.keyframes.contains("mask.rotation"));
  EXPECT_FALSE(e.keyframes.contains("mask.width"));
}

TEST(ApplyTrackedMasks, AllThreeKeysRotationAndSizeToo) {
  const core::Mask mask{.shape = core::MaskShape::Rectangle};
  const core::Project p = apply_tracked_masks(masked(mask), "c1", 0, run(mask));
  const core::ClipEffect& e = effect_of(p);
  EXPECT_DOUBLE_EQ(e.keyframes.at("mask.rotation")[1].v, 5.0);
  EXPECT_DOUBLE_EQ(e.keyframes.at("mask.width")[1].v, 0.5);
}

TEST(ApplyTrackedMasks, APathsScaleIsKeyedAsItsPath) {
  const core::Mask mask{.shape = core::MaskShape::Path,
                        .points = {core::MaskPoint{.x = -0.1}, core::MaskPoint{.x = 0.1},
                                   core::MaskPoint{.y = 0.1}}};
  const core::Project p = apply_tracked_masks(masked(mask), "c1", 0, run(mask));
  const core::Mask& keyed = effect_of(p).mask;
  ASSERT_EQ(keyed.path_keys.size(), 2u);
  EXPECT_DOUBLE_EQ(keyed.path_keys[1].points[1].x, 0.2);
  EXPECT_FALSE(effect_of(p).keyframes.contains("mask.width"));
}

TEST(MaskTrackingMethod, IsKeptOnTheMask) {
  const core::Project p = set_mask_tracking(masked(core::Mask{.shape = core::MaskShape::Ellipse}),
                                            "c1", 0, core::MaskTracking::PositionRotation);
  EXPECT_EQ(effect_of(p).mask.tracking, core::MaskTracking::PositionRotation);
}

TEST(MaskPathStopwatch, KeysAndUnkeysAtThePlayhead) {
  core::Project p = masked(core::Mask{.shape = core::MaskShape::Path,
                                      .points = {core::MaskPoint{.x = -0.1},
                                                 core::MaskPoint{.x = 0.1},
                                                 core::MaskPoint{.y = 0.1}}});
  p = set_mask_path_animated(std::move(p), "c1", 0, true, 1.0);
  EXPECT_EQ(effect_of(p).mask.path_keys.size(), 1u);
  p = toggle_mask_path_key(std::move(p), "c1", 0, 2.0);
  EXPECT_EQ(effect_of(p).mask.path_keys.size(), 2u);
  p = toggle_mask_path_key(std::move(p), "c1", 0, 2.0);
  EXPECT_EQ(effect_of(p).mask.path_keys.size(), 1u);
  p = set_mask_path_animated(std::move(p), "c1", 0, false, 1.0);
  EXPECT_TRUE(effect_of(p).mask.path_keys.empty());
}

}  // namespace
}  // namespace cutline::editor
