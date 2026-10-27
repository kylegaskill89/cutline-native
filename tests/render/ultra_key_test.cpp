/// Ultra Key as a plan: which passes its settings become and what they carry.
/// What the passes do to a picture is in the engine's render tests.

#include "cutline/render/ultra_key.hpp"

#include "cutline/render/effect_catalog.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace cutline::render {
namespace {

[[nodiscard]] core::ClipEffect ultra_key(std::map<std::string, double> params = {}) {
  return core::ClipEffect{.type = "ultrakey", .params = std::move(params)};
}

[[nodiscard]] std::vector<EffectPass> plan(const core::ClipEffect& effect) {
  std::vector<EffectPass> passes;
  plan_ultra_key(effect, passes);
  return passes;
}

TEST(UltraKey, ItIsInTheCatalogueUnderKeyingWithPremieresSections) {
  const EffectSpec* spec = find_effect_spec("ultrakey");
  ASSERT_NE(spec, nullptr);
  EXPECT_EQ(spec->name, "Ultra Key");
  EXPECT_EQ(spec->category, EffectCategory::Keying);
  ASSERT_EQ(spec->colors.size(), 1u);
  EXPECT_EQ(spec->colors[0].name, "Key Color");
  ASSERT_GE(spec->params.size(), 2u);
  EXPECT_EQ(spec->params[0].name, "Output");
  EXPECT_EQ(spec->params[0].choices.size(), 3u);
  EXPECT_EQ(spec->params[1].name, "Setting");
  EXPECT_EQ(spec->params[1].choices.size(), 4u);
  std::vector<std::string_view> groups;
  for (const EffectParamSpec& param : spec->params) {
    if (!param.group.empty() && (groups.empty() || groups.back() != param.group)) {
      groups.push_back(param.group);
    }
  }
  EXPECT_EQ(groups, (std::vector<std::string_view>{"Matte Generation", "Matte Cleanup",
                                                   "Spill Suppression", "Color Correction"}));
}

TEST(UltraKey, TheDefaultsAreAdobesDocumentedOnes) {
  const EffectSpec* spec = find_effect_spec("ultrakey");
  ASSERT_NE(spec, nullptr);
  std::map<std::string_view, double> fallback;
  for (const EffectParamSpec& param : spec->params) fallback[param.key] = param.fallback;
  EXPECT_EQ(fallback["transparency"], 45.0);
  EXPECT_EQ(fallback["highlight"], 10.0);
  EXPECT_EQ(fallback["shadow"], 50.0);
  EXPECT_EQ(fallback["tolerance"], 50.0);
  EXPECT_EQ(fallback["pedestal"], 10.0);
  EXPECT_EQ(fallback["choke"], 0.0);
  // And the Default setting is the same five numbers.
  EXPECT_EQ(ultra_key_preset(UltraKeySetting::Default),
            (std::array<double, 5>{45.0, 10.0, 50.0, 50.0, 10.0}));
}

TEST(UltraKey, AnUntouchedKeyIsOnePassCarryingItsSettings) {
  const std::vector<EffectPass> passes = plan(ultra_key());
  ASSERT_EQ(passes.size(), 1u);
  EXPECT_EQ(passes[0].kind, EffectPassKind::UltraKey);
  ASSERT_EQ(passes[0].data.size(), kUltraKeySettings);
  EXPECT_FLOAT_EQ(passes[0].data[kUltraTransparency], 0.45f);
  EXPECT_FLOAT_EQ(passes[0].data[kUltraSpill], 1.0f);
  EXPECT_FLOAT_EQ(passes[0].data[kUltraSaturation], 1.0f);
  EXPECT_NEAR(passes[0].data[kUltraKeyColour + 1], 208.0f / 255.0f, 1e-6f);
}

TEST(UltraKey, TheKeyColourIsTheOneChosen) {
  core::ClipEffect effect = ultra_key();
  effect.colors["color"] = "#0000ff";
  const std::vector<EffectPass> passes = plan(effect);
  EXPECT_FLOAT_EQ(passes[0].data[kUltraKeyColour], 0.0f);
  EXPECT_FLOAT_EQ(passes[0].data[kUltraKeyColour + 2], 1.0f);
}

TEST(UltraKey, ChokeSoftenAndContrastEachAddTheirPassInOrder) {
  const std::vector<EffectPass> passes =
      plan(ultra_key({{"choke", 100.0}, {"soften", 50.0}, {"contrast", 20.0}}));
  ASSERT_EQ(passes.size(), 4u);
  EXPECT_EQ(passes[1].kind, EffectPassKind::MatteChoke);
  EXPECT_FLOAT_EQ(passes[1].values[0], 4.0f);
  EXPECT_EQ(passes[2].kind, EffectPassKind::MatteSoften);
  EXPECT_FLOAT_EQ(passes[2].values[0], 5.0f);
  EXPECT_EQ(passes[3].kind, EffectPassKind::UltraKeyOutput);
  EXPECT_FLOAT_EQ(passes[3].values[0], 4.0f);
}

TEST(UltraKey, AnOutputOtherThanCompositeNeedsTheLastPass) {
  const std::vector<EffectPass> passes = plan(ultra_key({{"output", 1.0}}));
  ASSERT_EQ(passes.size(), 2u);
  EXPECT_EQ(passes[1].kind, EffectPassKind::UltraKeyOutput);
  EXPECT_FLOAT_EQ(passes[1].values[2], 1.0f);
}

}  // namespace
}  // namespace cutline::render
