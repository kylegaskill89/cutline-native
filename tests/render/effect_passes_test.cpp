/// A clip's effect stack as a list of passes.
///
/// The thing worth pinning down is the *order*. The flat resolver folded a
/// stack into shared fields, so moving an effect up or down changed nothing;
/// these are operations in the order they are written down, and most of what is
/// below says so in one way or another.

#include "cutline/render/effect_passes.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::render {
namespace {

using core::Clip;
using core::ClipEffect;

[[nodiscard]] ClipEffect effect(std::string type, std::map<std::string, double> params = {}) {
  ClipEffect e;
  e.type = std::move(type);
  e.params = std::move(params);
  return e;
}

[[nodiscard]] Clip clip_with(std::vector<ClipEffect> effects) {
  Clip c;
  c.id = "c1";
  c.source_out = 5.0;
  c.effects = std::move(effects);
  return c;
}

[[nodiscard]] std::vector<EffectPassKind> kinds(const std::vector<EffectPass>& passes) {
  std::vector<EffectPassKind> out;
  out.reserve(passes.size());
  for (const EffectPass& pass : passes) out.push_back(pass.kind);
  return out;
}

// ------------------------------------------------------------------- plan --

TEST(EffectPasses, AnEmptyStackIsNoPasses) {
  EXPECT_TRUE(plan_effect_passes(clip_with({}), 0.0).empty());
}

TEST(EffectPasses, ANeutralEffectIsNoPass) {
  // A contrast of 100% and a brightness of nothing are effects somebody added
  // and has not touched. Running a pass for each would cost two round trips
  // through a scratch target to change nothing at all.
  const std::vector<EffectPass> passes = plan_effect_passes(
      clip_with({effect("contrast", {{"amount", 100.0}}),
                 effect("brightness", {{"amount", 0.0}}),
                 effect("blur", {{"amount", 0.0}})}),
      0.0);
  EXPECT_TRUE(passes.empty());
}

TEST(EffectPasses, EachEffectBecomesOnePass) {
  const std::vector<EffectPass> passes = plan_effect_passes(
      clip_with({effect("brightness", {{"amount", 10.0}}),
                 effect("hue", {{"angle", 30.0}}),
                 effect("contrast", {{"amount", 150.0}})}),
      0.0);

  ASSERT_EQ(passes.size(), 3u);
  EXPECT_NEAR(pass_brightness(passes[0]), 0.1f, 1e-6);
  EXPECT_NEAR(pass_hue_radians(passes[1]), 0.5235987, 1e-5);
  EXPECT_NEAR(pass_contrast(passes[2]), 1.5f, 1e-6);
}

TEST(EffectPasses, TheOrderIsTheStackOrder) {
  // The whole point. Under the flat resolver these two stacks produced an
  // identical struct, so moving an effect changed nothing about the picture.
  const std::vector<EffectPassKind> one =
      kinds(plan_effect_passes(clip_with({effect("blur", {{"amount", 4.0}}),
                                          effect("contrast", {{"amount", 150.0}})}),
                               0.0));
  const std::vector<EffectPassKind> other =
      kinds(plan_effect_passes(clip_with({effect("contrast", {{"amount", 150.0}}),
                                          effect("blur", {{"amount", 4.0}})}),
                               0.0));

  EXPECT_EQ(one, (std::vector{EffectPassKind::Blur, EffectPassKind::Color}));
  EXPECT_EQ(other, (std::vector{EffectPassKind::Color, EffectPassKind::Blur}));
  EXPECT_NE(one, other);
}

TEST(EffectPasses, TwoContrastsAreTwoPassesRatherThanOneProduct) {
  const std::vector<EffectPass> passes = plan_effect_passes(
      clip_with({effect("contrast", {{"amount", 150.0}}),
                 effect("contrast", {{"amount", 150.0}})}),
      0.0);

  ASSERT_EQ(passes.size(), 2u);
  EXPECT_NEAR(pass_contrast(passes[0]), 1.5f, 1e-6);
  EXPECT_NEAR(pass_contrast(passes[1]), 1.5f, 1e-6) << "not folded into 2.25";
}

TEST(EffectPasses, ADisabledEffectContributesNothing) {
  ClipEffect off = effect("contrast", {{"amount", 200.0}});
  off.enabled = false;
  EXPECT_TRUE(plan_effect_passes(clip_with({off}), 0.0).empty());
}

TEST(EffectPasses, AnUnknownEffectIsSkippedRatherThanRefused) {
  // A project written by a newer version should still play, minus what this
  // build cannot draw.
  const std::vector<EffectPass> passes = plan_effect_passes(
      clip_with({effect("holographic-tilt-shift", {{"amount", 3.0}}),
                 effect("brightness", {{"amount", 10.0}})}),
      0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].kind, EffectPassKind::Color);
}

TEST(EffectPasses, KeyframesAreResolvedAtTheTimeAsked) {
  Clip c = clip_with({effect("brightness", {{"amount", 0.0}})});
  c.effects[0].keyframes["amount"] = {{.t = 0.0, .v = 0.0}, {.t = 4.0, .v = 100.0}};

  EXPECT_TRUE(plan_effect_passes(c, 0.0).empty()) << "nothing to do at the start";
  const std::vector<EffectPass> midway = plan_effect_passes(c, 2.0);
  ASSERT_EQ(midway.size(), 1u);
  EXPECT_NEAR(pass_brightness(midway[0]), 0.5f, 1e-6);
}

// ------------------------------------------------------------------ shapes --

TEST(EffectPasses, GrayscaleIsASaturationMultiplier) {
  const std::vector<EffectPass> passes =
      plan_effect_passes(clip_with({effect("grayscale", {{"amount", 100.0}})}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_NEAR(pass_saturation(passes[0]), 0.0f, 1e-6);
}

TEST(EffectPasses, InvertIsItsOwnPassAndTwoOfThemCancel) {
  const std::vector<EffectPass> passes = plan_effect_passes(
      clip_with({effect("invert", {{"on", 1.0}}), effect("invert", {{"on", 1.0}})}), 0.0);
  EXPECT_EQ(kinds(passes), (std::vector{EffectPassKind::Invert, EffectPassKind::Invert}));
}

TEST(EffectPasses, FlipCarriesItsAxesAsMultipliers) {
  const std::vector<EffectPass> passes = plan_effect_passes(
      clip_with({effect("flip", {{"horizontal", 1.0}, {"vertical", 0.0}})}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_TRUE(pass_flips_x(passes[0]));
  EXPECT_FALSE(pass_flips_y(passes[0]));
}

TEST(EffectPasses, AFlipWithNeitherAxisIsNoPass) {
  EXPECT_TRUE(plan_effect_passes(clip_with({effect("flip")}), 0.0).empty());
}

TEST(EffectPasses, VignetteIsCappedAtAQuarterTurn) {
  const std::vector<EffectPass> passes =
      plan_effect_passes(clip_with({effect("vignette", {{"amount", 400.0}})}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_NEAR(pass_vignette(passes[0]), 1.5707963, 1e-5);
}

TEST(EffectPasses, OneCropCannotInvertItsOwnRectangle) {
  const std::vector<EffectPass> passes =
      plan_effect_passes(clip_with({effect("crop", {{"left", 80.0}, {"right", 80.0}})}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  const std::array<float, 4> crop = pass_crop(passes[0]);
  EXPECT_NEAR(crop[0] + crop[2], 0.99f, 1e-5) << "one percent of the frame survives";
}

TEST(EffectPasses, TwoCropsAreTwoPassesAndNeitherIsScaledBack) {
  // Each takes its share of what the last one left, which is what a stack of
  // two crops means. The flat resolver added them and scaled the total to fit.
  const std::vector<EffectPass> passes = plan_effect_passes(
      clip_with({effect("crop", {{"left", 60.0}}), effect("crop", {{"left", 60.0}})}), 0.0);

  ASSERT_EQ(passes.size(), 2u);
  EXPECT_NEAR(pass_crop(passes[0])[0], 0.6f, 1e-5);
  EXPECT_NEAR(pass_crop(passes[1])[0], 0.6f, 1e-5);
}

TEST(EffectPasses, ChromaKeyCarriesItsColourAndTolerances) {
  ClipEffect key = effect("chromakey", {{"similarity", 40.0}, {"blend", 20.0}});
  key.colors["color"] = "#ff0000";

  const std::vector<EffectPass> passes = plan_effect_passes(clip_with({key}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(pass_key_color(passes[0]), (EffectColor{1.0f, 0.0f, 0.0f}));
  EXPECT_NEAR(pass_similarity(passes[0]), 0.4f, 1e-5);
  EXPECT_NEAR(pass_blend(passes[0]), 0.2f, 1e-5);
}

TEST(EffectPasses, AChromaKeyWithNoColourTakesTheRegistryGreen) {
  const std::vector<EffectPass> passes =
      plan_effect_passes(clip_with({effect("chromakey")}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(pass_key_color(passes[0]), kDefaultKeyColor);
}

TEST(EffectPasses, ABlurIsOnePassCarryingItsSigma) {
  // One here and two draws in the compositor: how wide a tap is depends on the
  // target's size, which this layer does not know.
  const std::vector<EffectPass> passes =
      plan_effect_passes(clip_with({effect("blur", {{"amount", 6.0}})}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].kind, EffectPassKind::Blur);
  EXPECT_NEAR(pass_sigma(passes[0]), 6.0f, 1e-6);
}

TEST(EffectPasses, EveryKindPacksIntoTheSharedBudget) {
  // The property the whole design rests on: a pass is a fixed, shared block, so
  // adding an effect costs no permanent room. If a new kind ever needed a ninth
  // float this would still compile — hence the assertion on the size itself.
  static_assert(kPassValues == 8);
  const EffectPass pass = chroma_key_pass(kDefaultKeyColor, 0.3f, 0.1f);
  EXPECT_EQ(pass.values.size(), kPassValues);
}


// ------------------------------------------------------------------- mask --

TEST(EffectPasses, APassIsUnmaskedUnlessTheEffectSaysOtherwise) {
  const std::vector<EffectPass> passes =
      plan_effect_passes(clip_with({effect("brightness", {{"amount", 10.0}})}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_FALSE(passes[0].mask.active());
}

TEST(EffectPasses, AMaskReachesThePassThatCarriesIt) {
  ClipEffect masked = effect("brightness", {{"amount", 10.0}});
  masked.mask = core::Mask{.shape = core::MaskShape::Ellipse,
                           .x = 0.25,
                           .y = 0.75,
                           .width = 0.1,
                           .height = 0.2,
                           .rotation = 90.0,
                           .feather = 0.05,
                           .opacity = 0.5,
                           .inverted = true};

  const std::vector<EffectPass> passes = plan_effect_passes(clip_with({masked}), 0.0);
  ASSERT_EQ(passes.size(), 1u);

  const PassMask& mask = passes[0].mask;
  EXPECT_TRUE(mask.active());
  EXPECT_NEAR(mask.x, 0.25f, 1e-6);
  EXPECT_NEAR(mask.width, 0.1f, 1e-6);
  EXPECT_NEAR(mask.feather, 0.05f, 1e-6);
  EXPECT_NEAR(mask.opacity, 0.5f, 1e-6);
  EXPECT_NEAR(mask.inverted, 1.0f, 1e-6);
  // Resolved to a cosine and a sine here so the shader has none to do per pixel.
  EXPECT_NEAR(mask.cos_rotation, 0.0f, 1e-6);
  EXPECT_NEAR(mask.sin_rotation, 1.0f, 1e-6);
}

TEST(EffectPasses, BothHalvesOfABlurCarryTheSameMask) {
  // A blur is one pass here and two draws in the compositor, and both axes have
  // to agree about where the blur is or the mask would smear across its own
  // edge on one of them.
  ClipEffect masked = effect("blur", {{"amount", 5.0}});
  masked.mask = core::Mask{.shape = core::MaskShape::Rectangle};

  const std::vector<EffectPass> passes = plan_effect_passes(clip_with({masked}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_TRUE(passes[0].mask.active());
}

TEST(EffectPasses, EachEffectKeepsItsOwnMask) {
  ClipEffect first = effect("brightness", {{"amount", 10.0}});
  first.mask = core::Mask{.shape = core::MaskShape::Ellipse, .x = 0.25};
  ClipEffect second = effect("contrast", {{"amount", 150.0}});
  second.mask = core::Mask{.shape = core::MaskShape::Rectangle, .x = 0.75};

  const std::vector<EffectPass> passes = plan_effect_passes(clip_with({first, second}), 0.0);
  ASSERT_EQ(passes.size(), 2u);
  EXPECT_NEAR(passes[0].mask.x, 0.25f, 1e-6);
  EXPECT_NEAR(passes[1].mask.x, 0.75f, 1e-6);
}

TEST(EffectPasses, AMaskWithNoShapeIsNoMask) {
  ClipEffect masked = effect("brightness", {{"amount", 10.0}});
  masked.mask = core::Mask{.shape = core::MaskShape::None, .feather = 0.5};

  const std::vector<EffectPass> passes = plan_effect_passes(clip_with({masked}), 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_FALSE(passes[0].mask.active());
}

}  // namespace
}  // namespace cutline::render
