/// A clip's effect stack, as a panel reads it.

#include "cutline/editor/effects_binding.hpp"

#include "cutline/core/effects.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Project;
using core::Track;
using core::TrackKind;

[[nodiscard]] Project one_clip() {
  Clip c;
  c.id = "c1";
  c.media_id = "m1";
  c.source_out = 5.0;

  Track t;
  t.id = "v1";
  t.kind = TrackKind::Video;
  t.clips = {std::move(c)};

  Project p;
  p.tracks = {std::move(t)};
  return p;
}

[[nodiscard]] const Clip& only_clip(const Project& p) { return p.tracks.front().clips.front(); }

// ------------------------------------------------------------------ adding --

TEST(AddEffect, PutsTheEffectOnTheClip) {
  const Project p = add_effect(one_clip(), "c1", "blur");
  ASSERT_EQ(only_clip(p).effects.size(), 1u);
  EXPECT_EQ(only_clip(p).effects.front().type, "blur");
  EXPECT_TRUE(only_clip(p).effects.front().enabled);
}

TEST(AddEffect, WritesEveryParameterRatherThanOnlyTheChangedOnes) {
  // What is stored is what a reopened project shows. An effect added with an
  // empty map would look right until it was saved.
  const Project p = add_effect(one_clip(), "c1", "crop");
  const core::ClipEffect& effect = only_clip(p).effects.front();
  EXPECT_EQ(effect.params.size(), 4u);
  EXPECT_TRUE(effect.params.contains("left"));
  EXPECT_TRUE(effect.params.contains("bottom"));
}

TEST(AddEffect, WritesTheColourOfAnEffectThatHasOne) {
  const Project p = add_effect(one_clip(), "c1", "chromakey");
  const core::ClipEffect& effect = only_clip(p).effects.front();
  EXPECT_EQ(effect.colors.at("color"), "#00d000");
}

TEST(AddEffect, AnUnknownTypeChangesNothing) {
  const Project before = one_clip();
  EXPECT_EQ(add_effect(before, "c1", "holographic-tilt-shift"), before);
}

TEST(AddEffect, AMissingClipChangesNothing) {
  const Project before = one_clip();
  EXPECT_EQ(add_effect(before, "nope", "blur"), before);
}

TEST(AddEffect, AppendsRatherThanReplacing) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = add_effect(std::move(p), "c1", "hue");

  ASSERT_EQ(only_clip(p).effects.size(), 2u);
  EXPECT_EQ(only_clip(p).effects[0].type, "blur");
  EXPECT_EQ(only_clip(p).effects[1].type, "hue");
}

// ------------------------------------------------------------------- rows --

TEST(ClipEffects, AnEmptyStackHasNoRows) {
  EXPECT_TRUE(clip_effects(one_clip(), "c1").empty());
}

TEST(ClipEffects, AMissingClipHasNoRows) {
  EXPECT_TRUE(clip_effects(one_clip(), "nope").empty());
}

TEST(ClipEffects, RowsFollowStackOrderAndCarryTheirIndex) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = add_effect(std::move(p), "c1", "vignette");

  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].type, "blur");
  EXPECT_EQ(rows[0].index, 0u);
  EXPECT_EQ(rows[1].type, "vignette");
  EXPECT_EQ(rows[1].index, 1u);
}

TEST(ClipEffects, ARowIsNamedForReadingRatherThanForKeying) {
  const Project p = add_effect(one_clip(), "c1", "grayscale");
  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().name, "Black & White");
  EXPECT_FALSE(rows.front().unknown);
}

TEST(ClipEffects, ParametersCarryTheirRangeAndValue) {
  const Project p = add_effect(one_clip(), "c1", "vignette");
  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_EQ(rows.front().params.size(), 1u);

  const EffectParamRow& amount = rows.front().params.front();
  EXPECT_EQ(amount.key, "amount");
  EXPECT_DOUBLE_EQ(amount.value, 40.0);
  EXPECT_DOUBLE_EQ(amount.fallback, 40.0);
  EXPECT_DOUBLE_EQ(amount.range.minimum, 0.0);
  EXPECT_DOUBLE_EQ(amount.range.maximum, 100.0);
  EXPECT_EQ(amount.suffix, "%");
  EXPECT_FALSE(amount.toggle);
}

TEST(ClipEffects, AParameterTheEffectDoesNotCarryReadsAsItsDefault) {
  // Only what was changed is written, and the resolver fills the rest in. The
  // panel has to do the same, or a hand-written project would show a crop with
  // four sliders at zero whatever it actually says.
  Project p = one_clip();
  core::ClipEffect bare;
  bare.type = "contrast";
  p.tracks.front().clips.front().effects = {bare};

  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_EQ(rows.front().params.size(), 1u);
  EXPECT_DOUBLE_EQ(rows.front().params.front().value, 100.0) << "not zero";
}

TEST(ClipEffects, ToggleParametersAreMarkedAsSuch) {
  const Project p = add_effect(one_clip(), "c1", "flip");
  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.front().params.size(), 2u);
  EXPECT_TRUE(rows.front().params[0].toggle);
  EXPECT_DOUBLE_EQ(rows.front().params[0].value, 1.0) << "a new flip flips";
  EXPECT_TRUE(rows.front().params[1].toggle);
  EXPECT_DOUBLE_EQ(rows.front().params[1].value, 0.0);
}

TEST(ClipEffects, AColourParameterIsReportedSeparately) {
  const Project p = add_effect(one_clip(), "c1", "chromakey");
  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.front().colors.size(), 1u);
  EXPECT_EQ(rows.front().colors.front().key, "color");
  EXPECT_EQ(rows.front().colors.front().value, "#00d000");
}

TEST(ClipEffects, ADisabledEffectStillHasARow) {
  // It stays in the stack, inert, and the panel is where it gets turned back
  // on — so leaving it out would make it unreachable.
  Project p = add_effect(one_clip(), "c1", "blur");
  p = core::toggle_clip_effect(std::move(p), "c1", 0);

  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(rows.front().enabled);
}

TEST(ClipEffects, AnAnimatedParameterIsMarked) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = core::set_effect_keyframe(std::move(p), "c1", 0, "amount", 0.0, 5.0);

  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.front().params.size(), 1u);
  EXPECT_TRUE(rows.front().params.front().animated);
}

TEST(ClipEffects, AnUnknownEffectIsShownAsItselfRatherThanDropped) {
  Project p = one_clip();
  core::ClipEffect future;
  future.type = "holographic-tilt-shift";
  p.tracks.front().clips.front().effects = {future};

  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(rows.front().unknown);
  EXPECT_EQ(rows.front().name, "holographic-tilt-shift");
  EXPECT_TRUE(rows.front().params.empty());
}

// ----------------------------------------------------------------- offers --

TEST(AddableEffects, OffersTheWholeCatalogue) {
  const std::vector<EffectChoice> choices = addable_effects();
  EXPECT_GE(choices.size(), 11u);
  EXPECT_TRUE(std::ranges::any_of(choices, [](const EffectChoice& c) { return c.type == "blur"; }));
}

TEST(AddableEffects, EveryOfferCanActuallyBeAdded) {
  for (const EffectChoice& choice : addable_effects()) {
    const Project p = add_effect(one_clip(), "c1", choice.type);
    ASSERT_EQ(only_clip(p).effects.size(), 1u) << choice.type << " could not be added";
    EXPECT_EQ(only_clip(p).effects.front().type, choice.type);
  }
}

TEST(AddableEffects, EachOneIsNamedAndFiled) {
  for (const EffectChoice& choice : addable_effects()) {
    EXPECT_FALSE(choice.name.empty()) << choice.type;
    EXPECT_FALSE(choice.category.empty()) << choice.type;
  }
}

}  // namespace
}  // namespace cutline::editor
