/// Transform as a plan: when it is a pass, and the moments a shutter divides
/// into. What the pass does to a picture is in the engine's render tests.

#include "cutline/render/transform_effect.hpp"

#include "cutline/render/effect_catalog.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace cutline::render {
namespace {

[[nodiscard]] core::ClipEffect transform(std::map<std::string, double> params = {}) {
  return core::ClipEffect{.type = "transform", .params = std::move(params)};
}

[[nodiscard]] std::vector<EffectPass> plan(const core::ClipEffect& effect, double t = 0.0,
                                           double frame = 1.0 / 24.0) {
  std::vector<EffectPass> passes;
  plan_transform(effect, t, frame, passes);
  return passes;
}

/// Position x animated from a quarter to three quarters over a second.
[[nodiscard]] core::ClipEffect sliding(std::map<std::string, double> params) {
  core::ClipEffect effect = transform(std::move(params));
  effect.keyframes["position_x"] = {core::Keyframe{.t = 0.0, .v = 0.25},
                                    core::Keyframe{.t = 1.0, .v = 0.75}};
  return effect;
}

TEST(TransformEffect, ItIsInTheCatalogueWithPremieresControlsInOrder) {
  const EffectSpec* spec = find_effect_spec("transform");
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(spec->name, "Transform");
  std::vector<std::string_view> names;
  for (const EffectParamSpec& param : spec->params) {
    if (param.name.ends_with(" Y")) continue;
    names.push_back(param.name);
  }
  EXPECT_EQ(names, (std::vector<std::string_view>{
                       "Anchor Point", "Position", "Uniform Scale", "Scale Height",
                       "Scale Width", "Skew", "Skew Axis", "Rotation", "Opacity",
                       "Use Composition's Shutter Angle", "Shutter Angle", "Sampling"}));
  EXPECT_TRUE(spec->params[0].pairs_next);
  EXPECT_EQ(spec->params[0].unit, EffectParamUnit::LayerWidth);
  EXPECT_EQ(spec->params[1].unit, EffectParamUnit::LayerHeight);
}

TEST(TransformEffect, AtItsDefaultsItIsNoPass) { EXPECT_TRUE(plan(transform()).empty()); }

TEST(TransformEffect, ARotationIsOnePassOfOneMoment) {
  const std::vector<EffectPass> passes = plan(transform({{"rotation", 90.0}}));
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].kind, EffectPassKind::Transform);
  ASSERT_EQ(passes[0].data.size(), kTransformHeader + kSampleSize);
  EXPECT_FLOAT_EQ(passes[0].data[kTransformSamples], 1.0f);
  EXPECT_NEAR(passes[0].data[kTransformHeader + kSampleRotation], 1.5707963f, 1e-6f);
}

TEST(TransformEffect, UniformScaleMakesScaleHeightTheScaleBothWays) {
  const auto uniform = plan(transform({{"scale_height", 50.0}, {"scale_width", 200.0}}));
  EXPECT_FLOAT_EQ(uniform[0].data[kTransformHeader + kSampleScale], 0.5f);
  const auto apart = plan(
      transform({{"uniform", 0.0}, {"scale_height", 50.0}, {"scale_width", 200.0}}));
  EXPECT_FLOAT_EQ(apart[0].data[kTransformHeader + kSampleScale], 2.0f);
  EXPECT_FLOAT_EQ(apart[0].data[kTransformHeader + kSampleScale + 1], 0.5f);
}

TEST(TransformEffect, OpacityAloneIsAPass) {
  const auto passes = plan(transform({{"opacity", 50.0}}));
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_FLOAT_EQ(passes[0].data[kTransformOpacity], 0.5f);
}

TEST(TransformEffect, AShutterOnSomethingMovingIsSixteenMomentsAcrossIt) {
  const auto passes =
      plan(sliding({{"composition_shutter", 0.0}, {"shutter_angle", 180.0}}), 0.5);
  ASSERT_EQ(passes.size(), 1u);
  ASSERT_EQ(passes[0].data[kTransformSamples], static_cast<float>(kMaxShutterSamples));
  // Half a frame either side of a 24 fps frame at half a unit a second.
  const float first = passes[0].data[kTransformHeader + kSamplePosition];
  const float last =
      passes[0].data[kTransformHeader + (kMaxShutterSamples - 1) * kSampleSize + kSamplePosition];
  EXPECT_LT(first, 0.5f);
  EXPECT_GT(last, 0.5f);
  EXPECT_NEAR(last - first, 0.5 * (0.5 / 24.0) * (15.0 / 16.0), 1e-5);
}

TEST(TransformEffect, TheCompositionsShutterAngleMeansNoBlurAsInPremiere) {
  // A quarter of the way, off the centre where the slide is no transform.
  const auto passes = plan(sliding({{"shutter_angle", 180.0}}), 0.25);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_FLOAT_EQ(passes[0].data[kTransformSamples], 1.0f);
}

TEST(TransformEffect, AShutterOnSomethingStillIsOneMoment) {
  const auto passes = plan(
      transform({{"rotation", 10.0}, {"composition_shutter", 0.0}, {"shutter_angle", 360.0}}));
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_FLOAT_EQ(passes[0].data[kTransformSamples], 1.0f);
}

}  // namespace
}  // namespace cutline::render
