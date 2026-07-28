#include "cutline/render/effects.hpp"

#include "cutline/core/keyframe.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

namespace cutline::render {
namespace {

using core::Clip;
using core::ClipEffect;

Clip with_effects(std::vector<ClipEffect> effects) {
  Clip c;
  c.id = "c";
  c.effects = std::move(effects);
  return c;
}

ClipEffect effect(std::string type, std::map<std::string, double> params = {}) {
  ClipEffect e;
  e.type = std::move(type);
  e.params = std::move(params);
  return e;
}

// ------------------------------------------------------------ hex parsing --

TEST(ParseHexColor, ReadsSixDigits) {
  const EffectColor c = parse_hex_color("#00d000");
  EXPECT_FLOAT_EQ(c.r, 0.0f);
  EXPECT_NEAR(c.g, 208.0f / 255.0f, 1e-6);
  EXPECT_FLOAT_EQ(c.b, 0.0f);
}

TEST(ParseHexColor, WorksWithoutTheHash) {
  EXPECT_EQ(parse_hex_color("ff0000"), parse_hex_color("#ff0000"));
}

TEST(ParseHexColor, ExpandsThreeDigitShorthand) {
  // #abc means #aabbcc, so the shorthand and the long form must agree.
  EXPECT_EQ(parse_hex_color("#f0c"), parse_hex_color("#ff00cc"));
}

TEST(ParseHexColor, IsCaseInsensitive) {
  EXPECT_EQ(parse_hex_color("#ABCDEF"), parse_hex_color("#abcdef"));
}

TEST(ParseHexColor, FallsBackRatherThanFailing) {
  // A malformed colour in a project file should not make a clip disappear.
  const EffectColor fallback{0.25f, 0.5f, 0.75f};
  EXPECT_EQ(parse_hex_color("not a colour", fallback), fallback);
  EXPECT_EQ(parse_hex_color("#12345", fallback), fallback);
  EXPECT_EQ(parse_hex_color("#gggggg", fallback), fallback);
  EXPECT_EQ(parse_hex_color("", fallback), fallback);
}

TEST(ParseHexColor, WhiteAndBlackLandExactlyOnTheEnds) {
  const EffectColor white = parse_hex_color("#ffffff");
  EXPECT_FLOAT_EQ(white.r, 1.0f);
  EXPECT_FLOAT_EQ(white.g, 1.0f);
  EXPECT_FLOAT_EQ(white.b, 1.0f);

  const EffectColor black = parse_hex_color("#000000");
  EXPECT_FLOAT_EQ(black.r, 0.0f);
}

// ------------------------------------------------------------------ empty --

TEST(ResolveEffectParams, AnEmptyStackIsNeutral) {
  const EffectParams p = resolve_effect_params(with_effects({}), 0.0);
  EXPECT_TRUE(p.is_neutral());
  EXPECT_FLOAT_EQ(p.brightness, 0.0f);
  EXPECT_FLOAT_EQ(p.contrast, 1.0f);
  EXPECT_FLOAT_EQ(p.saturation, 1.0f);
}

TEST(ResolveEffectParams, ADisabledEffectContributesNothing) {
  ClipEffect e = effect("brightness", {{"amount", 50.0}});
  e.enabled = false;
  EXPECT_TRUE(resolve_effect_params(with_effects({e}), 0.0).is_neutral());
}

TEST(ResolveEffectParams, AnUnknownEffectIsIgnoredRatherThanRejected) {
  // A project written by a newer version should still open, minus whatever it
  // cannot draw.
  const EffectParams p =
      resolve_effect_params(with_effects({effect("time-warp", {{"amount", 3.0}})}), 0.0);
  EXPECT_TRUE(p.is_neutral());
}

// -------------------------------------------------------------- unit maths --

TEST(ResolveEffectParams, BrightnessScalesToTheFFmpegRange) {
  // eq=brightness=amount/100
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("brightness", {{"amount", 50.0}})}), 0.0)
          .brightness,
      0.5f);
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("brightness", {{"amount", -100.0}})}), 0.0)
          .brightness,
      -1.0f);
}

TEST(ResolveEffectParams, ContrastIsAPercentage) {
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("contrast", {{"amount", 200.0}})}), 0.0).contrast,
      2.0f);
}

TEST(ResolveEffectParams, SaturationIsAPercentage) {
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("saturation", {{"amount", 0.0}})}), 0.0)
          .saturation,
      0.0f);
}

TEST(ResolveEffectParams, HueStaysInDegrees) {
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("hue", {{"angle", -90.0}})}), 0.0).hue_degrees,
      -90.0f);
}

TEST(ResolveEffectParams, BlackAndWhiteIsASaturationMultiplier) {
  // hue=s=1-amount/100, so full strength means zero saturation.
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("grayscale", {{"amount", 100.0}})}), 0.0)
          .saturation,
      0.0f);
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("grayscale", {{"amount", 40.0}})}), 0.0)
          .saturation,
      0.6f);
}

TEST(ResolveEffectParams, VignetteConvertsPercentToRadians) {
  // vignette=a=(amount/100)*(pi/2)
  const EffectParams p =
      resolve_effect_params(with_effects({effect("vignette", {{"amount", 50.0}})}), 0.0);
  EXPECT_NEAR(p.vignette, std::numbers::pi / 4.0, 1e-6);
}

TEST(ResolveEffectParams, VignetteIsCappedAtAQuarterTurn) {
  const EffectParams p =
      resolve_effect_params(with_effects({effect("vignette", {{"amount", 100.0}})}), 0.0);
  EXPECT_NEAR(p.vignette, std::numbers::pi / 2.0, 1e-6);
}

TEST(ResolveEffectParams, CropConvertsPercentToFractions) {
  const EffectParams p = resolve_effect_params(
      with_effects({effect("crop", {{"left", 10.0}, {"top", 20.0}, {"right", 5.0}})}), 0.0);
  EXPECT_FLOAT_EQ(p.crop_left, 0.1f);
  EXPECT_FLOAT_EQ(p.crop_top, 0.2f);
  EXPECT_FLOAT_EQ(p.crop_right, 0.05f);
  EXPECT_FLOAT_EQ(p.crop_bottom, 0.0f);
}

// -------------------------------------------------------------- defaulting --

TEST(ResolveEffectParams, AMissingParameterUsesTheRegistryDefaultNotZero) {
  // An absent contrast means 100%, not black. This is the difference between a
  // sensible default and a broken frame.
  EXPECT_FLOAT_EQ(resolve_effect_params(with_effects({effect("contrast")}), 0.0).contrast, 1.0f);
  EXPECT_FLOAT_EQ(resolve_effect_params(with_effects({effect("saturation")}), 0.0).saturation,
                  1.0f);
}

TEST(ResolveEffectParams, ANonFiniteParameterFallsBackToTheDefault) {
  const EffectParams p = resolve_effect_params(
      with_effects({effect("contrast", {{"amount", std::nan("")}})}), 0.0);
  EXPECT_FLOAT_EQ(p.contrast, 1.0f);
}

// ---------------------------------------------------------------- toggles --

TEST(ResolveEffectParams, InvertDefaultsToOnWhenPresent) {
  EXPECT_TRUE(resolve_effect_params(with_effects({effect("invert")}), 0.0).invert);
}

TEST(ResolveEffectParams, TwoInvertsCancel) {
  // Chaining `negate,negate` is the identity, so stacking two must be too.
  const EffectParams p =
      resolve_effect_params(with_effects({effect("invert"), effect("invert")}), 0.0);
  EXPECT_FALSE(p.invert);
}

TEST(ResolveEffectParams, FlipDefaultsToHorizontalOff) {
  // The registry's default for `horizontal` is 1, but an absent key means the
  // user never enabled it, so the fallback here is off.
  const EffectParams p = resolve_effect_params(with_effects({effect("flip")}), 0.0);
  EXPECT_FALSE(p.flip_x);
  EXPECT_FALSE(p.flip_y);
}

TEST(ResolveEffectParams, FlipReadsBothAxes) {
  const EffectParams p = resolve_effect_params(
      with_effects({effect("flip", {{"horizontal", 1.0}, {"vertical", 1.0}})}), 0.0);
  EXPECT_TRUE(p.flip_x);
  EXPECT_TRUE(p.flip_y);
}

// ------------------------------------------------------------- chroma key --

TEST(ResolveEffectParams, ChromaKeyScalesItsPercentages) {
  const EffectParams p = resolve_effect_params(
      with_effects({effect("chromakey", {{"similarity", 30.0}, {"blend", 10.0}})}), 0.0);
  EXPECT_TRUE(p.chroma_key);
  EXPECT_FLOAT_EQ(p.chroma_similarity, 0.3f);
  EXPECT_FLOAT_EQ(p.chroma_blend, 0.1f);
}

TEST(ResolveEffectParams, ChromaKeyUsesTheRegistryGreenWhenNoColourIsSet) {
  const EffectParams p = resolve_effect_params(with_effects({effect("chromakey")}), 0.0);
  EXPECT_EQ(p.chroma_color, parse_hex_color("#00d000"));
}

TEST(ResolveEffectParams, ChromaKeyReadsItsColour) {
  ClipEffect e = effect("chromakey");
  e.colors["color"] = "#0000ff";

  const EffectParams p = resolve_effect_params(with_effects({e}), 0.0);
  EXPECT_EQ(p.chroma_color, parse_hex_color("#0000ff"));
}

// -------------------------------------------------------------- stacking --

TEST(ResolveEffectParams, BlurIsCarriedInPixels) {
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("blur", {{"amount", 12.5}})}), 0.0).blur_sigma,
      12.5f);
}

TEST(ResolveEffectParams, BlurCountsAsNonNeutralBecauseItCostsExtraPasses) {
  EXPECT_FALSE(
      resolve_effect_params(with_effects({effect("blur", {{"amount", 4.0}})}), 0.0).is_neutral());
}

TEST(ResolveEffectParams, AZeroBlurStaysNeutral) {
  EXPECT_TRUE(
      resolve_effect_params(with_effects({effect("blur", {{"amount", 0.0}})}), 0.0).is_neutral());
}

TEST(ResolveEffectParams, ANegativeBlurIsClampedRatherThanInverted) {
  EXPECT_FLOAT_EQ(
      resolve_effect_params(with_effects({effect("blur", {{"amount", -5.0}})}), 0.0).blur_sigma,
      0.0f);
}

TEST(ResolveEffectParams, StackedContrastsMultiply) {
  // Chaining two eq fragments applies both, so the stack must too.
  const EffectParams p = resolve_effect_params(
      with_effects({effect("contrast", {{"amount", 200.0}}),
                    effect("contrast", {{"amount", 150.0}})}),
      0.0);
  EXPECT_FLOAT_EQ(p.contrast, 3.0f);
}

TEST(ResolveEffectParams, StackedBrightnessesSum) {
  const EffectParams p = resolve_effect_params(
      with_effects({effect("brightness", {{"amount", 20.0}}),
                    effect("brightness", {{"amount", 30.0}})}),
      0.0);
  EXPECT_FLOAT_EQ(p.brightness, 0.5f);
}

TEST(ResolveEffectParams, BlackAndWhiteBeatsALaterSaturation) {
  // Multiplying by zero is absorbing: once the colour is gone, a later
  // saturation cannot bring it back. Chaining the fragments behaved the same.
  const EffectParams p = resolve_effect_params(
      with_effects({effect("grayscale", {{"amount", 100.0}}),
                    effect("saturation", {{"amount", 300.0}})}),
      0.0);
  EXPECT_FLOAT_EQ(p.saturation, 0.0f);
}

TEST(ResolveEffectParams, SaturationNeverGoesNegative) {
  const EffectParams p = resolve_effect_params(
      with_effects({effect("grayscale", {{"amount", 200.0}})}), 0.0);
  EXPECT_GE(p.saturation, 0.0f);
}

TEST(ResolveEffectParams, StackedCropsCannotInvertTheRectangle) {
  // Three 45% crops on one axis would leave a negative width. The kept region
  // is floored instead, so the clip stays visible rather than vanishing.
  const EffectParams p = resolve_effect_params(
      with_effects({effect("crop", {{"left", 45.0}, {"right", 45.0}}),
                    effect("crop", {{"left", 45.0}, {"right", 45.0}})}),
      0.0);
  EXPECT_LE(p.crop_left + p.crop_right, 1.0f);
  EXPECT_GT(1.0f - p.crop_left - p.crop_right, 0.0f);
}

// ------------------------------------------------------------- keyframes --

TEST(ResolveEffectParams, AnimatedParametersResolveAtTheGivenTime) {
  ClipEffect e = effect("brightness", {{"amount", 0.0}});
  e.keyframes["amount"] = {{.t = 0.0, .v = 0.0}, {.t = 2.0, .v = 100.0}};

  const Clip clip = with_effects({e});
  EXPECT_FLOAT_EQ(resolve_effect_params(clip, 0.0).brightness, 0.0f);
  EXPECT_FLOAT_EQ(resolve_effect_params(clip, 1.0).brightness, 0.5f);
  EXPECT_FLOAT_EQ(resolve_effect_params(clip, 2.0).brightness, 1.0f);
}

TEST(ResolveEffectParams, AnAnimatedParameterOverridesItsStaticValue) {
  ClipEffect e = effect("contrast", {{"amount", 300.0}});
  e.keyframes["amount"] = {{.t = 0.0, .v = 100.0}, {.t = 1.0, .v = 100.0}};

  EXPECT_FLOAT_EQ(resolve_effect_params(with_effects({e}), 0.5).contrast, 1.0f);
}

}  // namespace
}  // namespace cutline::render
