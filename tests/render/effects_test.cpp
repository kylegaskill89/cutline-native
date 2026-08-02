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

}  // namespace
}  // namespace cutline::render
