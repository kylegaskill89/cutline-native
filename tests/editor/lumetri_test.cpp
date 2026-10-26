/// What the Lumetri Color panel does to a project: adding the effect when it
/// is first used, its eyedroppers, Auto, and the looks that come with Cutline.

#include "cutline/editor/lumetri.hpp"

#include "cutline/core/query.hpp"
#include "cutline/render/lumetri.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <process.h>

namespace cutline::editor {
namespace {

[[nodiscard]] core::Project one_clip() {
  core::Project p;
  core::Clip clip;
  clip.id = "c1";
  clip.source_out = 5.0;
  core::Track track;
  track.id = "v1";
  track.kind = core::TrackKind::Video;
  track.clips = {clip};
  p.sequence().tracks = {track};
  return p;
}

[[nodiscard]] const core::Clip& clip_of(const core::Project& p) {
  return *core::find_clip(p, "c1");
}

[[nodiscard]] double linear(int code) {
  const double c = code / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

TEST(LumetriPanel, TheFirstChangeAddsTheEffectAndTheNextUsesIt) {
  core::Project p = one_clip();
  EXPECT_FALSE(lumetri_on(p, "c1").has_value());
  p = set_lumetri_param(std::move(p), "c1", "exposure", 1.5, 0.0);
  ASSERT_EQ(clip_of(p).effects.size(), 1u);
  EXPECT_EQ(clip_of(p).effects[0].type, "lumetri");
  EXPECT_DOUBLE_EQ(clip_of(p).effects[0].params.at("exposure"), 1.5);
  // Every other control at its default, as an effect added any other way.
  EXPECT_DOUBLE_EQ(clip_of(p).effects[0].params.at("saturation"), 100.0);

  p = set_lumetri_params(std::move(p), "c1", {{"contrast", 20.0}, {"whites", -10.0}}, 0.0);
  ASSERT_EQ(clip_of(p).effects.size(), 1u);
  EXPECT_DOUBLE_EQ(clip_of(p).effects[0].params.at("contrast"), 20.0);
  EXPECT_DOUBLE_EQ(clip_of(p).effects[0].params.at("exposure"), 1.5);
}

TEST(LumetriPanel, CurvesAndLutsGoOnTheSameEffect) {
  core::Project p = one_clip();
  p = set_lumetri_curve(std::move(p), "c1", "rgb_master", {{.x = 0.5, .y = 0.7}});
  p = set_lumetri_file(std::move(p), "c1", "look", "C:/Looks/Warm.cube");
  ASSERT_EQ(clip_of(p).effects.size(), 1u);
  EXPECT_EQ(clip_of(p).effects[0].curves.at("rgb_master").size(), 1u);
  EXPECT_EQ(clip_of(p).effects[0].files.at("look"), "C:/Looks/Warm.cube");
}

TEST(LumetriPanel, AClipThatIsNotThereIsLeftAlone) {
  const core::Project p = set_lumetri_param(one_clip(), "nope", "exposure", 1.0, 0.0);
  EXPECT_TRUE(clip_of(p).effects.empty());
}

TEST(LumetriPanel, WhatTheEyedroppersSeeIsThePictureWithoutTheGrade) {
  core::Project p = set_lumetri_param(one_clip(), "c1", "exposure", 1.0, 0.0);
  EXPECT_FALSE(clip_of(without_lumetri(p, "c1")).effects[0].enabled);
  const core::Project partly = without_lumetri(p, "c1", {"secondary", "vignette"});
  EXPECT_TRUE(clip_of(partly).effects[0].enabled);
  EXPECT_EQ(clip_of(partly).effects[0].params.at("secondary"), 0.0);
  EXPECT_EQ(clip_of(partly).effects[0].params.at("basic"), 1.0);
}

TEST(WhiteBalance, AGreyNeedsNoCorrection) {
  const auto [temperature, tint] = white_balance_for(128, 128, 128);
  EXPECT_NEAR(temperature, 0.0, 1e-9);
  EXPECT_NEAR(tint, 0.0, 1e-9);
}

TEST(WhiteBalance, ABluishWhiteIsWarmedAndAGreenishOneMadeMoreMagenta) {
  EXPECT_GT(white_balance_for(180, 190, 220).first, 10.0);
  EXPECT_LT(white_balance_for(230, 200, 170).first, -10.0);
  EXPECT_GT(white_balance_for(170, 210, 170).second, 10.0);
}

TEST(WhiteBalance, TheCorrectionMakesThePickedColourNeutral) {
  // The grade's gains for these sliders, times the picked light, come out the
  // same in all three channels.
  const int r = 200;
  const int g = 205;
  const int b = 225;
  const auto [temperature, tint] = white_balance_for(r, g, b);
  const double t = temperature / 100.0;
  const double n = tint / 100.0;
  const double red = linear(r) * std::exp2(0.35 * t + 0.1 * n);
  const double green = linear(g) * std::exp2(-0.2 * n);
  const double blue = linear(b) * std::exp2(-0.35 * t + 0.1 * n);
  EXPECT_NEAR(red / green, 1.0, 1e-6);
  EXPECT_NEAR(blue / green, 1.0, 1e-6);
}

TEST(SetColour, AKeyIsCentredOnThePickedHueAndGoesRoundAtRed) {
  std::map<std::string, double> key;
  for (const auto& [name, value] : key_for(220, 30, 30)) key[name] = value;
  EXPECT_NEAR(key.at("key_hue_lo"), 340.0, 1e-9);
  EXPECT_NEAR(key.at("key_hue_hi"), 20.0, 1e-9);
  EXPECT_GT(key.at("key_sat_hi"), key.at("key_sat_lo"));
}

TEST(SetColour, AGreyKeysEveryHue) {
  std::map<std::string, double> key;
  for (const auto& [name, value] : key_for(120, 120, 122)) key[name] = value;
  EXPECT_EQ(key.at("key_hue_lo"), 0.0);
  EXPECT_EQ(key.at("key_hue_hi"), 360.0);
}

[[nodiscard]] std::vector<std::uint8_t> ramp(int from, int to) {
  std::vector<std::uint8_t> pixels;
  for (int i = 0; i < 256; ++i) {
    const auto v = static_cast<std::uint8_t>(from + (to - from) * i / 255);
    pixels.insert(pixels.end(), {v, v, v, 255});
  }
  return pixels;
}

[[nodiscard]] double setting(const std::vector<std::pair<std::string, double>>& values,
                             std::string_view key) {
  for (const auto& [name, value] : values) {
    if (name == key) return value;
  }
  return std::nan("");
}

TEST(AutoTone, ADarkPictureIsBroughtUpAndABrightOneDown) {
  const std::vector<std::uint8_t> dark = ramp(0, 90);
  EXPECT_GT(setting(auto_tone(dark.data(), 256, 1), "exposure"), 0.5);
  const std::vector<std::uint8_t> bright = ramp(150, 255);
  EXPECT_LT(setting(auto_tone(bright.data(), 256, 1), "exposure"), -0.3);
}

TEST(AutoTone, APictureAlreadySpreadAcrossTheRangeIsLeftNearlyAlone) {
  const std::vector<std::uint8_t> full = ramp(0, 255);
  const auto values = auto_tone(full.data(), 256, 1);
  // Its middle is a little over the 0.46 Auto aims for, so a little under a
  // third of a stop down.
  EXPECT_NEAR(setting(values, "exposure"), -0.27, 0.05);
  EXPECT_NEAR(setting(values, "whites"), 0.0, 25.0);
  EXPECT_NEAR(setting(values, "blacks"), 0.0, 25.0);
}

/// A ramp from black to white with a cast added to the brighter half.
[[nodiscard]] std::vector<std::uint8_t> ramp_with_cast(int red, int blue) {
  std::vector<std::uint8_t> pixels;
  for (int i = 0; i < 256; ++i) {
    const int bright = i > 128 ? 1 : 0;
    pixels.insert(pixels.end(),
                  {static_cast<std::uint8_t>(std::clamp(i + red * bright, 0, 255)),
                   static_cast<std::uint8_t>(i),
                   static_cast<std::uint8_t>(std::clamp(i + blue * bright, 0, 255)), 255});
  }
  return pixels;
}

TEST(ColourMatch, APictureMatchedToItselfIsLeftAlone) {
  const std::vector<std::uint8_t> picture = ramp_with_cast(20, -20);
  const auto values =
      colour_match(picture.data(), 256, 1, picture.data(), 256, 1, 100.0);
  for (const auto& [name, value] : values) {
    EXPECT_EQ(value, name == "saturation" ? 100.0 : 0.0) << name;
  }
}

TEST(ColourMatch, AWarmerReferenceTurnsTheHighlightsWheelTowardsRed) {
  const std::vector<std::uint8_t> warm = ramp_with_cast(25, -25);
  const std::vector<std::uint8_t> neutral = ramp_with_cast(0, 0);
  const auto values = colour_match(warm.data(), 256, 1, neutral.data(), 256, 1, 100.0);
  // Up the wheel is towards red and left away from blue.
  EXPECT_GT(setting(values, "highlights_y"), 10.0);
  EXPECT_LT(setting(values, "highlights_x"), -10.0);
  // The shadows had no cast to match, so move much less.
  EXPECT_LT(std::abs(setting(values, "shadows_y")), std::abs(setting(values, "highlights_y")));
}

TEST(ColourMatch, AMoreColourfulReferenceAsksForMoreSaturation) {
  const std::vector<std::uint8_t> strong = ramp_with_cast(40, -40);
  const std::vector<std::uint8_t> weak = ramp_with_cast(10, -10);
  EXPECT_GT(setting(colour_match(strong.data(), 256, 1, weak.data(), 256, 1, 100.0),
                    "saturation"),
            150.0);
}

TEST(ColourMatch, ADarkerReferenceLowersTheLuma) {
  const std::vector<std::uint8_t> dark = ramp(0, 160);
  const std::vector<std::uint8_t> full = ramp(0, 255);
  const auto values = colour_match(dark.data(), 256, 1, full.data(), 256, 1, 100.0);
  EXPECT_LT(setting(values, "highlights_luma"), -10.0);
  EXPECT_LT(setting(values, "midtones_luma"), 0.0);
}

TEST(Looks, TheLooksThatComeWithCutlineAreWrittenAndRead) {
  const std::filesystem::path folder = std::filesystem::temp_directory_path() /
                                       ("cutline_looks_" + std::to_string(_getpid()));
  std::filesystem::remove_all(folder);
  // One of the owner's own, which must be left as it is.
  std::filesystem::create_directories(folder);
  std::ofstream(folder / "Warm Film.cube") << "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n";

  write_built_in_looks(folder);
  const std::vector<std::filesystem::path> looks = looks_in(folder);
  EXPECT_EQ(looks.size(), built_in_look_names().size());
  for (const std::filesystem::path& look : looks) {
    const auto lut = render::load_lut(look);
    ASSERT_TRUE(lut.has_value()) << look << ": " << lut.error();
    if (look.stem() == "Warm Film") {
      EXPECT_FALSE((*lut)->three_d);
    } else {
      EXPECT_EQ((*lut)->size, 33);
    }
  }
  EXPECT_TRUE(std::ranges::is_sorted(
      looks, {}, [](const std::filesystem::path& p) { return p.stem().string(); }));
  std::filesystem::remove_all(folder);
}

}  // namespace
}  // namespace cutline::editor
