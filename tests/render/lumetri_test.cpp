/// Lumetri Color as a plan: its LUTs, its curves, and which passes a grade
/// becomes. What the passes do to a picture is in the engine's render tests.

#include "cutline/render/lumetri.hpp"

#include "cutline/render/effect_catalog.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <string>

namespace cutline::render {
namespace {

constexpr std::string_view kSwapRedAndBlue = R"(# red and blue change places
TITLE "Swap"
LUT_3D_SIZE 2
0 0 0
0 0 1
0 1 0
0 1 1
1 0 0
1 0 1
1 1 0
1 1 1
)";

[[nodiscard]] core::ClipEffect lumetri(std::map<std::string, double> params = {}) {
  return core::ClipEffect{.type = "lumetri", .params = std::move(params)};
}

[[nodiscard]] std::vector<EffectPass> plan(const core::ClipEffect& effect) {
  std::vector<EffectPass> passes;
  plan_lumetri(effect, passes);
  return passes;
}

TEST(Cube, ACubeIsReadWithItsTitleAndRedChangingFastest) {
  const auto lut = parse_cube(kSwapRedAndBlue);
  ASSERT_TRUE(lut.has_value()) << lut.error();
  EXPECT_EQ(lut->title, "Swap");
  EXPECT_EQ(lut->size, 2);
  EXPECT_TRUE(lut->three_d);
  ASSERT_EQ(lut->rgb.size(), 24u);
  // The second entry is red one, green and blue nought, and it holds blue.
  EXPECT_EQ(lut->rgb[3], 0.0f);
  EXPECT_EQ(lut->rgb[5], 1.0f);
}

TEST(Cube, AOneDimensionalLutAndADomainAreRead) {
  const auto lut = parse_cube(
      "LUT_1D_SIZE 3\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n0 0 0\n0.5 0.5 0.5\n1 1 1\n");
  ASSERT_TRUE(lut.has_value()) << lut.error();
  EXPECT_FALSE(lut->three_d);
  EXPECT_EQ(lut->size, 3);
  EXPECT_EQ(lut->domain_max[1], 2.0f);
}

TEST(Cube, KeywordsOtherApplicationsAddArePassedOver) {
  const auto lut = parse_cube("LUT_IN_VIDEO_RANGE\nLUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
  ASSERT_TRUE(lut.has_value()) << lut.error();
}

TEST(Cube, AFileWithTheWrongNumberOfEntriesIsRefusedSayingSo) {
  const auto lut = parse_cube("LUT_3D_SIZE 2\n0 0 0\n1 1 1\n");
  ASSERT_FALSE(lut.has_value());
  EXPECT_NE(lut.error().find("8 entries"), std::string::npos);
}

TEST(Cube, AFileWithNoSizeOrABadLineIsRefused) {
  EXPECT_FALSE(parse_cube("0 0 0\n1 1 1\n").has_value());
  EXPECT_FALSE(parse_cube("LUT_3D_SIZE 1\n0 0 0\n").has_value());
  EXPECT_FALSE(parse_cube("LUT_1D_SIZE 2\n0 0\n1 1 1\n").has_value());
  EXPECT_FALSE(parse_cube("LUT_1D_SIZE 2\nDOMAIN_MIN 1 1 1\nDOMAIN_MAX 1 1 1\n0 0 0\n1 1 1\n")
                   .has_value());
}

TEST(Cube, AFileIsReadOnceAndAgainWhenItChanges) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("cutline_lut_" + std::to_string(_getpid()) + ".cube");
  std::ofstream(path) << kSwapRedAndBlue;
  const auto first = load_lut(path);
  ASSERT_TRUE(first.has_value()) << first.error();
  const auto again = load_lut(path);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(first->get(), again->get());

  std::ofstream(path) << "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n";
  std::filesystem::last_write_time(
      path, std::filesystem::last_write_time(path) + std::chrono::seconds(5));
  const auto changed = load_lut(path);
  ASSERT_TRUE(changed.has_value());
  EXPECT_FALSE((*changed)->three_d);
  std::filesystem::remove(path);

  EXPECT_FALSE(load_lut(path).has_value());
}

TEST(Curves, ACurveWithNoPointsIsStraight) {
  const std::vector<float> diagonal = curve_table({}, CurveRest::Diagonal, false);
  ASSERT_EQ(diagonal.size(), kCurveSamples);
  EXPECT_FLOAT_EQ(diagonal.front(), 0.0f);
  EXPECT_FLOAT_EQ(diagonal.back(), 1.0f);
  EXPECT_NEAR(diagonal[128], 128.0f / 255.0f, 1e-6);
  for (const float y : curve_table({}, CurveRest::Middle, true)) EXPECT_FLOAT_EQ(y, 0.5f);
}

TEST(Curves, ACurvePassesThroughItsPointsAndIsPinnedAtTheEnds) {
  const std::vector<core::CurvePoint> points{{.x = 0.5, .y = 0.75}};
  const std::vector<float> table = curve_table(points, CurveRest::Diagonal, false);
  EXPECT_FLOAT_EQ(table.front(), 0.0f);
  EXPECT_FLOAT_EQ(table.back(), 1.0f);
  // 0.5 falls between two samples, so the nearest is close rather than exact.
  EXPECT_NEAR(table[128], 0.75f, 0.01f);
}

TEST(Curves, ACurveNeverOvershootsBetweenItsPoints) {
  // A step: flat, a sudden rise, flat. A natural spline rings either side of
  // it; this must not.
  const std::vector<core::CurvePoint> points{
      {.x = 0.0, .y = 0.2}, {.x = 0.45, .y = 0.2}, {.x = 0.55, .y = 0.8}, {.x = 1.0, .y = 0.8}};
  const std::vector<float> table = curve_table(points, CurveRest::Diagonal, false);
  for (std::size_t i = 0; i < table.size(); ++i) {
    EXPECT_GE(table[i], 0.2f - 1e-6f) << i;
    EXPECT_LE(table[i], 0.8f + 1e-6f) << i;
    if (i > 0) EXPECT_GE(table[i], table[i - 1] - 1e-6f) << i;
  }
}

TEST(Curves, AHueCurveGoesRound) {
  // A dip at red, which is at both ends of the hue line.
  const std::vector<core::CurvePoint> points{
      {.x = 0.0, .y = 0.2}, {.x = 0.2, .y = 0.5}, {.x = 0.8, .y = 0.5}};
  const std::vector<float> table = curve_table(points, CurveRest::Middle, true);
  EXPECT_NEAR(table.front(), 0.2f, 1e-5f);
  EXPECT_NEAR(table.back(), 0.2f, 1e-5f);
  EXPECT_NEAR(table[128], 0.5f, 1e-5f);
}

TEST(Wheels, AWheelChangesColourAndNotLuma) {
  for (const auto [x, y] : {std::pair{1.0, 0.0}, {0.0, 1.0}, {-0.7, 0.7}}) {
    const std::array<float, 3> offset = wheel_offset(x, y);
    EXPECT_NEAR(offset[0] * 0.2126f + offset[1] * 0.7152f + offset[2] * 0.0722f, 0.0f, 1e-4f);
  }
  // Up is towards red, as on a vectorscope, and right towards blue.
  EXPECT_GT(wheel_offset(0.0, 1.0)[0], 0.0f);
  EXPECT_GT(wheel_offset(1.0, 0.0)[2], 0.0f);
}

TEST(LumetriPlan, AGradeLeftAsItWasAddedDoesNothing) {
  core::ClipEffect effect = lumetri();
  for (const EffectParamSpec& spec : find_effect_spec("lumetri")->params) {
    effect.params[std::string(spec.key)] = spec.fallback;
  }
  EXPECT_TRUE(plan(effect).empty());
}

TEST(LumetriPlan, ABasicCorrectionIsOneGradePass) {
  const std::vector<EffectPass> passes = plan(lumetri({{"exposure", 1.0}}));
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].kind, EffectPassKind::Lumetri);
  EXPECT_EQ(static_cast<int>(passes[0].values[0]), kLumetriBasic);
  EXPECT_FLOAT_EQ(passes[0].data[kExposure], 2.0f);
  // No LUTs and no curves.
  EXPECT_EQ(passes[0].values[1], -1.0f);
  EXPECT_EQ(passes[0].values[3], -1.0f);
}

TEST(LumetriPlan, ASectionSwitchedOffContributesNothing) {
  EXPECT_TRUE(plan(lumetri({{"exposure", 1.0}, {"basic", 0.0}})).empty());
  EXPECT_TRUE(plan(lumetri({{"vig_amount", -2.0}, {"vignette", 0.0}})).empty());
}

TEST(LumetriPlan, BasicCreativeCurvesAndWheelsShareOnePass) {
  core::ClipEffect effect = lumetri({{"contrast", 20.0}, {"vibrance", 30.0},
                                     {"midtones_luma", 10.0}});
  effect.curves["rgb_master"] = {{.x = 0.5, .y = 0.6}};
  const std::vector<EffectPass> passes = plan(effect);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(static_cast<int>(passes[0].values[0]),
            kLumetriBasic | kLumetriCreative | kLumetriCurves | kLumetriWheels);
  // The curves follow the settings, every table there, and only the RGB ones
  // flagged as having anything to do.
  EXPECT_EQ(passes[0].values[3], static_cast<float>(kLumetriSettings));
  EXPECT_EQ(passes[0].data.size(), kLumetriSettings + kLumetriCurveKeys.size() * kCurveSamples);
  EXPECT_EQ(passes[0].values[4], 1.0f);
  EXPECT_EQ(passes[0].values[5], 0.0f);
}

TEST(LumetriPlan, CreativeSharpenSplitsTheGradeWhereItRuns) {
  core::ClipEffect effect = lumetri({{"exposure", 0.5}, {"sharpen", 40.0}});
  effect.curves["hue_sat"] = {{.x = 0.0, .y = 0.8}};
  const std::vector<EffectPass> passes = plan(effect);
  ASSERT_EQ(passes.size(), 3u);
  EXPECT_EQ(static_cast<int>(passes[0].values[0]), kLumetriBasic);
  EXPECT_EQ(passes[1].kind, EffectPassKind::Sharpen);
  EXPECT_EQ(static_cast<int>(passes[2].values[0]), kLumetriCurves);

  // Below nought it softens instead.
  const std::vector<EffectPass> soft = plan(lumetri({{"sharpen", -40.0}}));
  ASSERT_EQ(soft.size(), 1u);
  EXPECT_EQ(soft[0].kind, EffectPassKind::Blur);
}

TEST(LumetriPlan, ALutThatCannotBeReadIsLeftOutAndTheRestStillApplies) {
  core::ClipEffect effect = lumetri({{"saturation", 50.0}});
  effect.files["input_lut"] = "C:/nowhere/at/all.cube";
  const std::vector<EffectPass> passes = plan(effect);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].values[1], -1.0f);
}

TEST(LumetriPlan, ALookIsCarriedInTheData) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("cutline_look_" + std::to_string(_getpid()) + ".cube");
  std::ofstream(path) << kSwapRedAndBlue;
  core::ClipEffect effect = lumetri();
  effect.files["look"] = path.string();
  const std::vector<EffectPass> passes = plan(effect);
  std::filesystem::remove(path);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(static_cast<int>(passes[0].values[0]), kLumetriCreative);
  const auto at = static_cast<std::size_t>(passes[0].values[2]);
  EXPECT_EQ(at, kLumetriSettings);
  EXPECT_EQ(passes[0].data[at], 2.0f);
  EXPECT_EQ(passes[0].data.size(), kLumetriSettings + kLutHeader + 24);
}

TEST(LumetriPlan, TheSecondaryRunsOnlyWithACorrectionOrTheMaskShown) {
  EXPECT_TRUE(plan(lumetri({{"key_hue_lo", 90.0}, {"key_hue_hi", 150.0}})).empty());
  const std::vector<EffectPass> passes =
      plan(lumetri({{"key_hue_lo", 330.0}, {"key_hue_hi", 30.0}, {"sec_saturation", 0.0}}));
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].kind, EffectPassKind::LumetriSecondary);
  // 330 to 30 goes round through red: centred on nought, sixty degrees wide.
  EXPECT_NEAR(passes[0].data[kKeyHue], 0.0f, 1e-6f);
  EXPECT_NEAR(passes[0].data[kKeyHue + 1], 30.0f / 360.0f, 1e-6f);
  EXPECT_EQ(plan(lumetri({{"show_mask", 1.0}})).size(), 1u);
}

TEST(LumetriPlan, TheVignetteIsItsOwnPass) {
  const std::vector<EffectPass> passes = plan(lumetri({{"vig_amount", -2.5}}));
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].kind, EffectPassKind::LumetriVignette);
  EXPECT_FLOAT_EQ(passes[0].values[0], -0.5f);
}

TEST(LumetriPlan, ItIsReachedThroughTheEffectStack) {
  core::Clip clip;
  clip.source_out = 5.0;
  clip.effects = {lumetri({{"exposure", 1.0}})};
  const std::vector<EffectPass> passes = plan_effect_passes(clip, 0.0);
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].kind, EffectPassKind::Lumetri);
}

}  // namespace
}  // namespace cutline::render
