/// The catalogue against the resolver.
///
/// These two have to agree about every effect's name and parameter keys, and
/// nothing but a test says so: a catalogue entry typed "greyscale" would offer
/// a working control that quietly does nothing, because the resolver's if-chain
/// ignores what it does not recognise. So rather than checking the table
/// against itself, each entry is pushed to its extreme and the resolved result
/// has to differ from neutral.

#include "cutline/render/effect_catalog.hpp"

#include "cutline/render/effects.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

namespace cutline::render {
namespace {

/// A clip carrying one effect with every parameter at the far end of its range,
/// which for every effect in the catalogue is a value that does something.
[[nodiscard]] core::Clip with_effect_at_maximum(const EffectSpec& spec) {
  core::ClipEffect effect;
  effect.type = std::string(spec.type);
  for (const EffectParamSpec& param : spec.params) {
    effect.params.emplace(std::string(param.key), param.maximum);
  }

  core::Clip clip;
  clip.effects = {std::move(effect)};
  return clip;
}

[[nodiscard]] core::Clip with_effect_at_defaults(const EffectSpec& spec) {
  core::ClipEffect effect;
  effect.type = std::string(spec.type);
  for (const EffectParamSpec& param : spec.params) {
    effect.params.emplace(std::string(param.key), param.fallback);
  }
  for (const EffectColorSpec& color : spec.colors) {
    effect.colors.emplace(std::string(color.key), std::string(color.fallback));
  }

  core::Clip clip;
  clip.effects = {std::move(effect)};
  return clip;
}

TEST(EffectCatalog, EveryEntryIsAnEffectTheResolverKnows) {
  for (const EffectSpec& spec : effect_catalog()) {
    const EffectParams resolved = resolve_effect_params(with_effect_at_maximum(spec), 0.0);
    EXPECT_FALSE(resolved.is_neutral())
        << spec.type << " resolved to nothing at its maximum, so the resolver does not "
                        "recognise that type";
  }
}

TEST(EffectCatalog, EveryTypeAppearsOnce) {
  std::set<std::string_view> seen;
  for (const EffectSpec& spec : effect_catalog()) {
    EXPECT_TRUE(seen.insert(spec.type).second) << spec.type << " is listed twice";
  }
  EXPECT_EQ(seen.size(), effect_catalog().size());
}

TEST(EffectCatalog, EveryEntryIsNamedAndUsable) {
  for (const EffectSpec& spec : effect_catalog()) {
    EXPECT_FALSE(spec.type.empty());
    EXPECT_FALSE(spec.name.empty()) << spec.type << " has no name to show";
    EXPECT_FALSE(spec.params.empty() && spec.colors.empty())
        << spec.type << " has nothing to adjust";

    for (const EffectParamSpec& param : spec.params) {
      EXPECT_FALSE(param.key.empty()) << spec.type;
      EXPECT_FALSE(param.name.empty()) << spec.type << "." << param.key;
      EXPECT_LT(param.minimum, param.maximum) << spec.type << "." << param.key;
      EXPECT_GE(param.fallback, param.minimum) << spec.type << "." << param.key;
      EXPECT_LE(param.fallback, param.maximum) << spec.type << "." << param.key;
    }
  }
}

/// The effects whose defaults are deliberately not neutral.
///
/// An effect with nothing to set — Invert, Black & White — has to arrive doing
/// its one thing or it is indistinguishable from a button that failed. The
/// rest arrive at the middle of a slider, which is what a slider is for: a
/// Contrast that started at 300% would be the bug.
constexpr std::string_view kStartsVisible[]{"grayscale", "invert", "flip", "vignette",
                                            "chromakey"};

[[nodiscard]] bool starts_visible(std::string_view type) {
  return std::ranges::contains(kStartsVisible, type);
}

TEST(EffectCatalog, AnEffectWithNothingToSetArrivesDoingIt) {
  for (const EffectSpec& spec : effect_catalog()) {
    if (!starts_visible(spec.type)) continue;
    const EffectParams resolved = resolve_effect_params(with_effect_at_defaults(spec), 0.0);
    EXPECT_FALSE(resolved.is_neutral()) << spec.type << " at its defaults does nothing";
  }
}

TEST(EffectCatalog, EveryOtherEffectArrivesNeutral) {
  for (const EffectSpec& spec : effect_catalog()) {
    if (starts_visible(spec.type)) continue;
    const EffectParams resolved = resolve_effect_params(with_effect_at_defaults(spec), 0.0);
    EXPECT_TRUE(resolved.is_neutral())
        << spec.type << " changes the picture the moment it is added";
  }
}

TEST(EffectCatalog, TheDefaultsAreTheOnesTheResolverAssumes) {
  // Where a stored value is absent the resolver falls back to its own default,
  // and for most effects that has to be the catalogue's or a fresh effect would
  // change the moment it is saved and reloaded without its parameters.
  //
  // Vignette and flip are the deliberate exceptions: what a *missing* parameter
  // means and what a *new* effect is given are different questions there, which
  // is why the catalogue calls the field `fallback` rather than `neutral`.
  const std::set<std::string_view> differs{"vignette", "flip"};

  for (const EffectSpec& spec : effect_catalog()) {
    if (differs.contains(spec.type)) continue;

    core::ClipEffect bare;
    bare.type = std::string(spec.type);
    core::Clip clip;
    clip.effects = {bare};

    EXPECT_EQ(resolve_effect_params(clip, 0.0), resolve_effect_params(with_effect_at_defaults(spec), 0.0))
        << spec.type << " means something different with its parameters missing";
  }
}

TEST(FindEffectSpec, FindsWhatTheCatalogueLists) {
  const EffectSpec* found = find_effect_spec("blur");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->name, "Gaussian Blur");
  EXPECT_EQ(found->params.size(), 1u);
}

TEST(FindEffectSpec, AnUnknownTypeIsNullRatherThanAnEmptyEntry) {
  // A project written by a newer version can name an effect this build has
  // never heard of, and the difference between "no such effect" and "an effect
  // with no parameters" is what lets the panel say so.
  EXPECT_EQ(find_effect_spec("holographic-tilt-shift"), nullptr);
  EXPECT_EQ(find_effect_spec(""), nullptr);
}

}  // namespace
}  // namespace cutline::render
