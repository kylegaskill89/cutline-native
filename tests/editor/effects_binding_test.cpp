/// A clip's effect stack, as a panel reads it.

#include "cutline/editor/effects_binding.hpp"

#include "cutline/audio/chain.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

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

// -------------------------------------------------------------- keyframes --

/// A clip with one blur on it, which has a single numeric parameter.
[[nodiscard]] Project blurred() { return add_effect(one_clip(), "c1", "blur"); }

[[nodiscard]] const core::ClipEffect& only_effect(const Project& p) {
  return only_clip(p).effects.front();
}

TEST(SetEffectParameter, WritesTheStoredValueWhenNotAnimated) {
  const Project p = set_effect_parameter(blurred(), "c1", 0, "amount", 12.0, 3.0);
  EXPECT_DOUBLE_EQ(only_effect(p).params.at("amount"), 12.0);
  EXPECT_TRUE(only_effect(p).keyframes.empty()) << "a static parameter gains no keyframes";
}

TEST(SetEffectParameter, WritesAKeyframeWhenAnimated) {
  Project p = set_effect_parameter_animated(blurred(), "c1", 0, "amount", true, 1.0);
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 20.0, 4.0);

  const auto& frames = only_effect(p).keyframes.at("amount");
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_DOUBLE_EQ(frames[1].t, 4.0);
  EXPECT_DOUBLE_EQ(frames[1].v, 20.0);
}

TEST(SetEffectParameterAnimated, TurningItOnKeepsTheValueItHad) {
  // The point of the stopwatch is that nothing about the picture changes when
  // it is pressed — the value it had becomes the first keyframe.
  Project p = set_effect_parameter(blurred(), "c1", 0, "amount", 7.0);
  p = set_effect_parameter_animated(std::move(p), "c1", 0, "amount", true, 2.0);

  const auto& frames = only_effect(p).keyframes.at("amount");
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_DOUBLE_EQ(frames.front().t, 2.0);
  EXPECT_DOUBLE_EQ(frames.front().v, 7.0);
}

TEST(SetEffectParameterAnimated, TurningItOffKeepsTheValueAtThatTime) {
  Project p = set_effect_parameter_animated(blurred(), "c1", 0, "amount", true, 0.0);
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 10.0, 0.0);
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 30.0, 4.0);

  // Halfway between them, so the value being kept is one only the keyframes
  // ever produced.
  p = set_effect_parameter_animated(std::move(p), "c1", 0, "amount", false, 2.0);

  EXPECT_TRUE(only_effect(p).keyframes.empty());
  EXPECT_DOUBLE_EQ(only_effect(p).params.at("amount"), 20.0);
}

TEST(SetEffectParameterAnimated, AskingForWhatIsAlreadyTheCaseChangesNothing) {
  const Project before = blurred();
  EXPECT_EQ(set_effect_parameter_animated(before, "c1", 0, "amount", false, 0.0), before);

  const Project animated = set_effect_parameter_animated(before, "c1", 0, "amount", true, 0.0);
  EXPECT_EQ(set_effect_parameter_animated(animated, "c1", 0, "amount", true, 3.0), animated)
      << "a second press must not add a keyframe";
}

TEST(ToggleEffectKeyframe, AddsOneHoldingTheCurrentValue) {
  Project p = set_effect_parameter_animated(blurred(), "c1", 0, "amount", true, 0.0);
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 40.0, 4.0);
  p = toggle_effect_keyframe(std::move(p), "c1", 0, "amount", 2.0);

  const auto& frames = only_effect(p).keyframes.at("amount");
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_DOUBLE_EQ(frames[1].t, 2.0);
  // Halfway along a linear ramp from 0 to 40, so adding a keyframe changed the
  // shape of the animation not at all — which is what a marker should do.
  EXPECT_DOUBLE_EQ(frames[1].v, 20.0);
}

TEST(ToggleEffectKeyframe, RemovesTheOneAlreadyThere) {
  Project p = set_effect_parameter_animated(blurred(), "c1", 0, "amount", true, 0.0);
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 40.0, 4.0);
  p = toggle_effect_keyframe(std::move(p), "c1", 0, "amount", 4.0);

  ASSERT_EQ(only_effect(p).keyframes.at("amount").size(), 1u);
  EXPECT_DOUBLE_EQ(only_effect(p).keyframes.at("amount").front().t, 0.0);
}

TEST(ToggleEffectKeyframe, DoesNothingToAParameterThatIsNotAnimated) {
  const Project before = blurred();
  EXPECT_EQ(toggle_effect_keyframe(before, "c1", 0, "amount", 1.0), before);
}

TEST(ClipEffects, AnAnimatedRowReadsItsValueAtTheGivenTime) {
  Project p = set_effect_parameter_animated(blurred(), "c1", 0, "amount", true, 0.0);
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 50.0, 4.0);

  // Held by value. A reference bound to a *member* of a temporary is not
  // lifetime-extended, and this one read freed memory until it was noticed.
  const EffectParamRow at_start = clip_effects(p, "c1", 0.0).front().params.front();
  EXPECT_DOUBLE_EQ(at_start.value, 0.0);
  EXPECT_TRUE(at_start.animated);
  EXPECT_TRUE(at_start.keyed_here);

  const EffectParamRow midway = clip_effects(p, "c1", 2.0).front().params.front();
  EXPECT_DOUBLE_EQ(midway.value, 25.0);
  EXPECT_FALSE(midway.keyed_here) << "there is no keyframe at two seconds";
}

TEST(ClipEffects, AStaticRowIgnoresTheTime) {
  const Project p = set_effect_parameter(blurred(), "c1", 0, "amount", 9.0);
  EXPECT_DOUBLE_EQ(clip_effects(p, "c1", 0.0).front().params.front().value, 9.0);
  EXPECT_DOUBLE_EQ(clip_effects(p, "c1", 99.0).front().params.front().value, 9.0);
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

// ---------------------------------------------------------- interpolation --

TEST(EffectInterp, AnAnimatedParameterReportsItsCurve) {
  Project p = set_effect_parameter_animated(blurred(), "c1", 0, "amount", true, 0.0);
  p = set_effect_parameter_interp(std::move(p), "c1", 0, "amount", core::Interp::Ease);

  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.front().params.size(), 1u);
  EXPECT_EQ(rows.front().params.front().interp, core::Interp::Ease);
}

TEST(EffectInterp, SettingItOnSomethingNotAnimatedDoesNothing) {
  const Project before = blurred();
  EXPECT_EQ(set_effect_parameter_interp(before, "c1", 0, "amount", core::Interp::Hold),
            before);
}

TEST(EffectInterp, TurningAnimationOffAndOnAgainDoesNotKeepTheOldCurve) {
  // Nowhere to keep it: switching off drops every keyframe, and the mode lives
  // on them. A fresh animation is linear, which is what the model says.
  Project p = set_effect_parameter_animated(blurred(), "c1", 0, "amount", true, 0.0);
  p = set_effect_parameter_interp(std::move(p), "c1", 0, "amount", core::Interp::Hold);
  p = set_effect_parameter_animated(std::move(p), "c1", 0, "amount", false, 0.0);
  p = set_effect_parameter_animated(std::move(p), "c1", 0, "amount", true, 0.0);

  EXPECT_EQ(clip_effects(p, "c1").front().params.front().interp, core::Interp::Linear);
}

// ------------------------------------------------------------ audio stack --

TEST(AudioEffects, EveryOneInTheRegistryCanBeAdded) {
  // The offer and the registry have to be the same list, or the menu shows
  // something that cannot be added or hides something that can.
  const std::vector<EffectChoice> choices = addable_audio_effects();
  ASSERT_FALSE(choices.empty());

  for (const EffectChoice& choice : choices) {
    const Project p = add_audio_effect(one_clip(), "c1", choice.type);
    EXPECT_EQ(only_clip(p).audio_effects.size(), 1u) << choice.type;
    EXPECT_EQ(only_clip(p).audio_effects.front().type, choice.type);
  }
}

TEST(AudioEffects, AreNamedRatherThanKeyed) {
  const std::vector<EffectChoice> choices = addable_audio_effects();
  const auto compressor =
      std::ranges::find(choices, "compressor", &EffectChoice::type);
  ASSERT_NE(compressor, choices.end());
  EXPECT_EQ(compressor->name, "Compressor");
}

TEST(AudioEffects, ANewOneCarriesEveryParameterAtItsDefault) {
  // Written out rather than left implicit, for the same reason the visual stack
  // does it: the panel reads what is stored, and an empty map would show the
  // registry defaults while the file said nothing.
  const Project p = add_audio_effect(one_clip(), "c1", "compressor");
  const std::vector<EffectRow> rows = clip_audio_effects(p, "c1");

  ASSERT_EQ(rows.size(), 1u);
  ASSERT_FALSE(rows.front().params.empty());
  EXPECT_EQ(only_clip(p).audio_effects.front().params.size(), rows.front().params.size());
  for (const EffectParamRow& param : rows.front().params) {
    EXPECT_DOUBLE_EQ(param.value, param.fallback) << param.key;
  }
}

TEST(AudioEffects, AnUnknownTypeIsNotAdded) {
  // Not a real effect name. This used to say "reverb", picked because the
  // application did not have one — which is a name the application may one day
  // have, and now does.
  const Project before = one_clip();
  EXPECT_EQ(add_audio_effect(before, "c1", "no-such-effect"), before);
}

TEST(AudioEffects, RowsCarryTheRegistrysRangesAndUnits) {
  const Project p = add_audio_effect(one_clip(), "c1", "highpass");
  const std::vector<EffectRow> rows = clip_audio_effects(p, "c1");

  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().name, "High-Pass");
  ASSERT_FALSE(rows.front().params.empty());

  const EffectParamRow& cutoff = rows.front().params.front();
  EXPECT_GT(cutoff.range.maximum, cutoff.range.minimum);
  EXPECT_FALSE(cutoff.suffix.empty()) << "a frequency without its unit is a number";
}

TEST(AudioEffects, AParameterTheStoredEffectDoesNotCarryReadsAsItsDefault) {
  // Missing is not zero. An absent cutoff means the registry default, not DC.
  Project p = add_audio_effect(one_clip(), "c1", "highpass");
  p.tracks[0].clips[0].audio_effects[0].params.clear();

  const std::vector<EffectRow> rows = clip_audio_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  for (const EffectParamRow& param : rows.front().params) {
    EXPECT_DOUBLE_EQ(param.value, param.fallback) << param.key;
  }
}

TEST(AudioEffects, NoneOfThemAnimate) {
  // `AudioClipEffect` holds parameters and nothing else, so a panel must not
  // offer a stopwatch it has nowhere to put the result of.
  Project p = one_clip();
  for (const EffectChoice& choice : addable_audio_effects()) {
    p = add_audio_effect(std::move(p), "c1", choice.type);
  }

  for (const EffectRow& row : clip_audio_effects(p, "c1")) {
    EXPECT_TRUE(row.colors.empty()) << row.type << " has no colours in the registry";
    for (const EffectParamRow& param : row.params) {
      EXPECT_FALSE(param.animated) << row.type << "." << param.key;
      EXPECT_FALSE(param.keyed_here) << row.type << "." << param.key;
    }
  }
}

TEST(AudioEffects, KeepTheirStackOrder) {
  Project p = add_audio_effect(one_clip(), "c1", "highpass");
  p = add_audio_effect(std::move(p), "c1", "compressor");

  const std::vector<EffectRow> rows = clip_audio_effects(p, "c1");
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].type, "highpass");
  EXPECT_EQ(rows[1].type, "compressor");
  EXPECT_EQ(rows[0].index, 0u);
  EXPECT_EQ(rows[1].index, 1u);
}

TEST(AudioEffects, AnEffectThisBuildDoesNotKnowStillShowsUp) {
  // A project written by a newer version should open and play, minus the effect
  // it names — and the stack should say something is there rather than leaving
  // a gap that reads as data lost.
  Project p = one_clip();
  p.tracks[0].clips[0].audio_effects.push_back(
      core::AudioClipEffect{.type = "no-such-effect"});

  const std::vector<EffectRow> rows = clip_audio_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(rows.front().unknown);
  EXPECT_EQ(rows.front().name, "no-such-effect");
  EXPECT_TRUE(rows.front().params.empty());
}

TEST(AudioEffects, AClipWithNoneHasNoRows) {
  EXPECT_TRUE(clip_audio_effects(one_clip(), "c1").empty());
  EXPECT_TRUE(clip_audio_effects(one_clip(), "ghost").empty());
}

// ------------------------------------------------------------- copy/paste --

/// One video clip and one audio clip, which is the shape an A/V pair has here:
/// two linked clips, so a stack copied off one and pasted across both has to do
/// something sensible with each.
[[nodiscard]] Project a_pair() {
  Clip video;
  video.id = "v-clip";
  video.media_id = "m1";
  video.kind = TrackKind::Video;
  video.source_out = 5.0;

  Clip audio;
  audio.id = "a-clip";
  audio.media_id = "m1";
  audio.kind = TrackKind::Audio;
  audio.source_out = 5.0;

  Track vt{.id = "v1", .kind = TrackKind::Video};
  vt.clips = {std::move(video)};
  Track at{.id = "a1", .kind = TrackKind::Audio};
  at.clips = {std::move(audio)};

  Project p;
  p.tracks = {std::move(vt), std::move(at)};
  return p;
}

[[nodiscard]] const Clip& clip_named(const Project& p, std::string_view id) {
  return *core::find_clip(p, id);
}

[[nodiscard]] Project with_a_blur(Project p, std::string_view clip_id, double amount) {
  return core::add_clip_effect(std::move(p), clip_id, "blur", {{"amount", amount}});
}

TEST(EffectClipboard, AFreshOneIsEmpty) {
  const EffectClipboard clipboard;
  EXPECT_TRUE(clipboard.empty());
  EXPECT_EQ(clipboard.size(), 0u);
}

TEST(EffectClipboard, CopyingRemembersWhatItCameFrom) {
  Project p = a_pair();
  p = with_a_blur(std::move(p), "v-clip", 12.0);
  p = core::add_audio_effect(std::move(p), "a-clip", "gain", {{"gain", 3.0}});

  const EffectClipboard look = copy_effects(p, "v-clip");
  EXPECT_EQ(look.kind, TrackKind::Video);
  ASSERT_EQ(look.video.size(), 1u);
  EXPECT_EQ(look.video[0].type, "blur");
  EXPECT_FALSE(look.empty());

  const EffectClipboard filters = copy_effects(p, "a-clip");
  EXPECT_EQ(filters.kind, TrackKind::Audio);
  ASSERT_EQ(filters.audio.size(), 1u);
  EXPECT_EQ(filters.audio[0].type, "gain");
}

TEST(EffectClipboard, CopyingTakesTheKeyframesToo) {
  Project p = with_a_blur(a_pair(), "v-clip", 0.0);
  p = core::set_effect_keyframe(std::move(p), "v-clip", 0, "amount", 1.0, 20.0);

  const EffectClipboard clipboard = copy_effects(p, "v-clip");
  ASSERT_EQ(clipboard.video.size(), 1u);
  const auto keyed = clipboard.video[0].keyframes.find("amount");
  ASSERT_NE(keyed, clipboard.video[0].keyframes.end());
  EXPECT_EQ(keyed->second.size(), 1u);
}

// A clipboard that pointed into the project would go stale the moment the clip
// it came from was trimmed, and undo would make it dangle.
TEST(EffectClipboard, WhatIsCopiedSurvivesTheClipItCameFrom) {
  Project p = with_a_blur(a_pair(), "v-clip", 12.0);
  const EffectClipboard clipboard = copy_effects(p, "v-clip");

  p = core::clear_clip_effects(std::move(p), "v-clip");
  ASSERT_TRUE(clip_named(p, "v-clip").effects.empty());
  EXPECT_EQ(clipboard.video.size(), 1u);
}

TEST(EffectClipboard, CopyingAClipThatIsNotThereGivesNothing) {
  EXPECT_TRUE(copy_effects(a_pair(), "nope").empty());
}

// Copying a clip with nothing on it and pasting is a way of clearing a stack,
// and it is the one anybody would try.
TEST(EffectClipboard, CopyingACleanClipIsHowAStackGetsCleared) {
  Project p = a_pair();
  p.tracks[0].clips.push_back(Clip{.id = "v-clean",
                                   .media_id = "m1",
                                   .kind = TrackKind::Video,
                                   .source_out = 5.0,
                                   .start = 10.0});
  p = with_a_blur(std::move(p), "v-clip", 12.0);

  // Filled, holding nothing: that is different from nothing having been copied,
  // and it is the difference between clearing a stack and a Paste that is
  // greyed out.
  const EffectClipboard blank = copy_effects(p, "v-clean");
  ASSERT_FALSE(blank.empty());
  ASSERT_EQ(blank.size(), 0u);

  const std::vector<std::string> onto{"v-clip"};
  p = paste_effects(std::move(p), onto, blank);
  EXPECT_TRUE(clip_named(p, "v-clip").effects.empty());
}

TEST(EffectClipboard, PastingBeforeAnythingHasBeenCopiedChangesNothing) {
  const Project p = with_a_blur(a_pair(), "v-clip", 12.0);
  const std::vector<std::string> onto{"v-clip"};
  EXPECT_EQ(paste_effects(p, onto, EffectClipboard{}), p);
}

TEST(EffectClipboard, PastingReplacesTheStackRatherThanAddingToIt) {
  Project p = with_a_blur(a_pair(), "v-clip", 12.0);
  const EffectClipboard clipboard = copy_effects(p, "v-clip");

  const std::vector<std::string> onto{"v-clip"};
  p = paste_effects(std::move(p), onto, clipboard);
  p = paste_effects(std::move(p), onto, clipboard);

  // Twice pasted is still one blur. Appending would give three.
  EXPECT_EQ(clip_named(p, "v-clip").effects.size(), 1u);
}

TEST(EffectClipboard, PastingReachesEveryClipNamed) {
  Project p = a_pair();
  p.tracks[0].clips.push_back(Clip{.id = "v-other",
                                   .media_id = "m1",
                                   .kind = TrackKind::Video,
                                   .source_out = 5.0,
                                   .start = 10.0});
  p = with_a_blur(std::move(p), "v-clip", 12.0);

  const EffectClipboard clipboard = copy_effects(p, "v-clip");
  const std::vector<std::string> onto{"v-clip", "v-other"};
  p = paste_effects(std::move(p), onto, clipboard);

  EXPECT_EQ(clip_named(p, "v-other").effects.size(), 1u);
  EXPECT_EQ(clip_named(p, "v-other").effects[0].type, "blur");
}

// Which stack a clip gets is decided by the track it is on. Pasting a video
// look onto a selection that includes the linked audio must leave the audio's
// filters alone rather than emptying them.
TEST(EffectClipboard, AVideoStackPastedAcrossAPairLeavesTheAudioAlone) {
  Project p = with_a_blur(a_pair(), "v-clip", 12.0);
  p = core::add_audio_effect(std::move(p), "a-clip", "gain", {{"gain", 3.0}});

  const EffectClipboard clipboard = copy_effects(p, "v-clip");
  ASSERT_EQ(clipboard.kind, TrackKind::Video);

  const std::vector<std::string> onto{"v-clip", "a-clip"};
  p = paste_effects(std::move(p), onto, clipboard);

  EXPECT_EQ(clip_named(p, "v-clip").effects.size(), 1u);
  EXPECT_EQ(clip_named(p, "a-clip").audio_effects.size(), 1u)
      << "the audio clip was cleared by a video paste";
}

TEST(EffectClipboard, AnAudioStackPastedOntoAnAudioClipArrives) {
  Project p = a_pair();
  p.tracks[1].clips.push_back(Clip{.id = "a-other",
                                   .media_id = "m1",
                                   .kind = TrackKind::Audio,
                                   .source_out = 5.0,
                                   .start = 10.0});
  p = core::add_audio_effect(std::move(p), "a-other", "highpass", {{"frequency", 200.0}});

  const EffectClipboard clipboard = copy_effects(p, "a-other");
  const std::vector<std::string> onto{"a-clip"};
  p = paste_effects(std::move(p), onto, clipboard);

  ASSERT_EQ(clip_named(p, "a-clip").audio_effects.size(), 1u);
  EXPECT_EQ(clip_named(p, "a-clip").audio_effects[0].type, "highpass");
}

TEST(EffectClipboard, PastingOntoNothingChangesNothing) {
  const Project p = with_a_blur(a_pair(), "v-clip", 12.0);
  const EffectClipboard clipboard = copy_effects(p, "v-clip");

  EXPECT_EQ(paste_effects(p, {}, clipboard), p);

  const std::vector<std::string> missing{"nope"};
  EXPECT_EQ(paste_effects(p, missing, clipboard), p);
}

// Every edit returns the project unchanged when it cannot apply, which is what
// lets the session skip the undo entry.
TEST(EffectClipboard, PastingWhatIsAlreadyThereChangesNothing) {
  const Project p = with_a_blur(a_pair(), "v-clip", 12.0);
  const EffectClipboard clipboard = copy_effects(p, "v-clip");

  const std::vector<std::string> onto{"v-clip"};
  EXPECT_EQ(paste_effects(p, onto, clipboard), p);
}

// ----------------------------------------------------------------- reset --

TEST(ResetEffect, PutsEveryParameterBackToItsDefault) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 30.0);
  ASSERT_NE(clip_effects(p, "c1").front().params.front().value, 0.0);

  const Project reset = reset_effect(p, "c1", 0);
  // Held, not bound through: `clip_effects` returns a vector, and a reference
  // into a temporary one dangles at the semicolon. The symptom is a test that
  // reads a plausible number out of freed memory — sometimes the right one.
  const std::vector<EffectRow> rows = clip_effects(reset, "c1");
  const EffectParamRow& row = rows.front().params.front();
  EXPECT_DOUBLE_EQ(row.value, row.fallback);
}

TEST(ResetEffect, ClearsTheKeyframesRatherThanWritingOneAtTheDefault) {
  // Setting an animated parameter writes a keyframe, not the stored value. A
  // reset that did not clear them first would put one keyframe at the playhead
  // holding the default and leave the rest of the curve exactly as it was.
  Project p = add_effect(one_clip(), "c1", "blur");
  p = set_effect_parameter_animated(std::move(p), "c1", 0, "amount", true, 0.0);
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 30.0, 2.0);
  ASSERT_TRUE(clip_effects(p, "c1").front().params.front().animated);

  const Project reset = reset_effect(p, "c1", 0);
  const std::vector<EffectRow> rows = clip_effects(reset, "c1");
  const EffectParamRow& row = rows.front().params.front();
  EXPECT_FALSE(row.animated);
  EXPECT_DOUBLE_EQ(row.value, row.fallback);
}

TEST(ResetEffect, LeavesTheRestOfTheStackAlone) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = add_effect(std::move(p), "c1", "brightness");
  p = set_effect_parameter(std::move(p), "c1", 1, "amount", 40.0);

  const Project reset = reset_effect(p, "c1", 0);
  EXPECT_DOUBLE_EQ(clip_effects(reset, "c1")[1].params.front().value, 40.0);
}

TEST(ResetEffect, DoesNothingForAnEffectThatIsNotThere) {
  const Project p = add_effect(one_clip(), "c1", "blur");
  EXPECT_EQ(reset_effect(p, "c1", 9), p);
  EXPECT_EQ(reset_effect(p, "nope", 0), p);
}

TEST(ResetAudioEffect, PutsEveryParameterBackToItsDefault) {
  // Its own audio clip rather than the shared fixture, which is defined further
  // down with the library tests.
  Project p = one_clip();
  Clip a;
  a.id = "a1";
  a.media_id = "m2";
  a.kind = TrackKind::Audio;
  a.source_out = 5.0;
  Track t;
  t.id = "a-track";
  t.kind = TrackKind::Audio;
  t.clips = {std::move(a)};
  p.tracks.push_back(std::move(t));

  p = add_audio_effect(std::move(p), "a1", "lowpass");
  const double moved = clip_audio_effects(p, "a1").front().params.front().range.maximum;
  p = core::set_audio_effect_param(std::move(p), "a1", 0,
                                   clip_audio_effects(p, "a1").front().params.front().key,
                                   moved);

  const Project reset = reset_audio_effect(p, "a1", 0);
  const std::vector<EffectRow> rows = clip_audio_effects(reset, "a1");
  const EffectParamRow& row = rows.front().params.front();
  EXPECT_DOUBLE_EQ(row.value, row.fallback);
}

// ------------------------------------------------ audio effect keyframes --

/// One audio clip, long enough for keyframes to be spread over.
[[nodiscard]] Project audio_clip_project() {
  Clip c;
  c.id = "a1";
  c.media_id = "m1";
  c.kind = TrackKind::Audio;
  c.source_out = 5.0;

  Track t;
  t.id = "a";
  t.kind = TrackKind::Audio;
  t.clips = {std::move(c)};

  Project p;
  p.tracks = {std::move(t)};
  return p;
}

//
// The same four operations the visual stack has. What is worth testing here is
// the join rather than the keyframe machinery, which `core` already covers.

TEST(AudioEffectKeyframes, TheStopwatchKeepsTheValueItHad) {
  Project p = audio_clip_project();
  p = add_audio_effect(std::move(p), "a1", "lowpass");
  p = core::set_audio_effect_param(std::move(p), "a1", 0, "freq", 4000.0);

  p = set_audio_effect_parameter_animated(std::move(p), "a1", 0, "freq", true, 2.0);

  const std::vector<EffectRow> rows = clip_audio_effects(p, "a1", 2.0);
  const EffectParamRow& row = rows.front().params.front();
  EXPECT_TRUE(row.animated);
  EXPECT_TRUE(row.keyed_here);
  EXPECT_DOUBLE_EQ(row.value, 4000.0) << "pressing the stopwatch must not change the sound";
}

TEST(AudioEffectKeyframes, AnAnimatedParameterIsReadAtTheTimeAsked) {
  Project p = audio_clip_project();
  p = add_audio_effect(std::move(p), "a1", "lowpass");
  p = set_audio_effect_parameter_animated(std::move(p), "a1", 0, "freq", true, 0.0);
  p = set_audio_effect_parameter(std::move(p), "a1", 0, "freq", 800.0, 0.0);
  p = set_audio_effect_parameter(std::move(p), "a1", 0, "freq", 12000.0, 4.0);

  EXPECT_DOUBLE_EQ(clip_audio_effects(p, "a1", 0.0).front().params.front().value, 800.0);
  EXPECT_DOUBLE_EQ(clip_audio_effects(p, "a1", 2.0).front().params.front().value, 6400.0);
}

TEST(AudioEffectKeyframes, SettingAnAnimatedParameterWritesAKeyframe) {
  Project p = audio_clip_project();
  p = add_audio_effect(std::move(p), "a1", "lowpass");
  const double stored = p.tracks.front().clips.front().audio_effects.front().params.at("freq");

  p = set_audio_effect_parameter_animated(std::move(p), "a1", 0, "freq", true, 0.0);
  p = set_audio_effect_parameter(std::move(p), "a1", 0, "freq", 300.0, 3.0);

  const core::AudioClipEffect& effect =
      p.tracks.front().clips.front().audio_effects.front();
  EXPECT_EQ(effect.keyframes.at("freq").size(), 2u);
  EXPECT_DOUBLE_EQ(effect.params.at("freq"), stored) << "the stored value is left alone";
}

TEST(AudioEffectKeyframes, TurningTheStopwatchOffKeepsTheValueAtThatTime) {
  Project p = audio_clip_project();
  p = add_audio_effect(std::move(p), "a1", "lowpass");
  p = set_audio_effect_parameter_animated(std::move(p), "a1", 0, "freq", true, 0.0);
  p = set_audio_effect_parameter(std::move(p), "a1", 0, "freq", 800.0, 0.0);
  p = set_audio_effect_parameter(std::move(p), "a1", 0, "freq", 12000.0, 4.0);

  p = set_audio_effect_parameter_animated(std::move(p), "a1", 0, "freq", false, 2.0);

  const core::AudioClipEffect& effect =
      p.tracks.front().clips.front().audio_effects.front();
  EXPECT_TRUE(effect.keyframes.empty());
  EXPECT_DOUBLE_EQ(effect.params.at("freq"), 6400.0) << "halfway along the sweep";
}

TEST(AudioEffectKeyframes, AMarkerDoesNothingUntilTheStopwatchIsOn) {
  Project p = audio_clip_project();
  p = add_audio_effect(std::move(p), "a1", "lowpass");
  EXPECT_EQ(toggle_audio_effect_keyframe(p, "a1", 0, "freq", 1.0), p);
}

TEST(AudioEffectKeyframes, AMarkerAddsAndTakesAwayWithoutBendingTheCurve) {
  Project p = audio_clip_project();
  p = add_audio_effect(std::move(p), "a1", "lowpass");
  p = set_audio_effect_parameter_animated(std::move(p), "a1", 0, "freq", true, 0.0);
  p = set_audio_effect_parameter(std::move(p), "a1", 0, "freq", 12000.0, 4.0);

  const Project added = toggle_audio_effect_keyframe(p, "a1", 0, "freq", 2.0);
  EXPECT_EQ(added.tracks.front().clips.front().audio_effects.front().keyframes.at("freq").size(),
            3u);
  EXPECT_DOUBLE_EQ(clip_audio_effects(added, "a1", 2.0).front().params.front().value,
                   clip_audio_effects(p, "a1", 2.0).front().params.front().value);

  EXPECT_EQ(toggle_audio_effect_keyframe(added, "a1", 0, "freq", 2.0), p);
}

TEST(AudioEffectKeyframes, ResettingAnEffectClearsThemToo) {
  Project p = audio_clip_project();
  p = add_audio_effect(std::move(p), "a1", "lowpass");
  p = set_audio_effect_parameter_animated(std::move(p), "a1", 0, "freq", true, 0.0);
  p = set_audio_effect_parameter(std::move(p), "a1", 0, "freq", 300.0, 3.0);

  const Project reset = reset_audio_effect(p, "a1", 0);
  EXPECT_TRUE(reset.tracks.front().clips.front().audio_effects.front().keyframes.empty());
}

TEST(AudioEffectKeyframes, AnEffectThatIsNotThereChangesNothing) {
  const Project p = audio_clip_project();
  EXPECT_EQ(set_audio_effect_parameter(p, "a1", 7, "freq", 100.0), p);
  EXPECT_EQ(set_audio_effect_parameter_animated(p, "a1", 7, "freq", true, 0.0), p);
  EXPECT_EQ(toggle_audio_effect_keyframe(p, "a1", 7, "freq", 0.0), p);
  EXPECT_EQ(set_audio_effect_parameter_interp(p, "a1", 7, "freq", core::Interp::Ease), p);
}

TEST(CopyOneEffect, TakesJustThatEffect) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = add_effect(std::move(p), "c1", "vignette");

  const EffectClipboard clipboard = copy_one_effect(p, "c1", 1);
  ASSERT_EQ(clipboard.size(), 1u);
  EXPECT_EQ(clipboard.video.front().type, "vignette");
  EXPECT_FALSE(clipboard.empty());
}

TEST(CopyOneEffect, KeepsItsKeyframes) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = set_effect_parameter_animated(std::move(p), "c1", 0, "amount", true, 0.0);
  p = set_effect_parameter(std::move(p), "c1", 0, "amount", 20.0, 2.0);

  const EffectClipboard clipboard = copy_one_effect(p, "c1", 0);
  ASSERT_EQ(clipboard.size(), 1u);
  EXPECT_FALSE(clipboard.video.front().keyframes.empty());
}

TEST(CopyOneEffect, AnIndexThatNamesNothingLeavesTheClipboardAlone) {
  // Empty rather than filled, so a paste after a failed copy puts nothing
  // anywhere instead of clearing a stack.
  const Project p = add_effect(one_clip(), "c1", "blur");
  EXPECT_TRUE(copy_one_effect(p, "c1", 9).empty());
  EXPECT_TRUE(copy_one_effect(p, "nope", 0).empty());
}

// ---------------------------------------------------------------- library --

/// A video clip and an audio clip, so the library has both kinds to refuse.
[[nodiscard]] Project both_kinds() {
  Project p = one_clip();
  Clip a;
  a.id = "a1";
  a.media_id = "m2";
  a.kind = TrackKind::Audio;
  a.source_out = 5.0;

  Track t;
  t.id = "a-track";
  t.kind = TrackKind::Audio;
  t.clips = {std::move(a)};
  p.tracks.push_back(std::move(t));
  return p;
}

TEST(EffectLibrary, HoldsVideoEffectsAudioEffectsAndTransitionsTogether) {
  // Premiere's Effects panel holds all three, and the person reaching for one
  // does not think of them as three catalogues.
  const std::vector<LibraryEntry> library = effect_library();

  const auto has_prefix = [&library](std::string_view prefix) {
    return std::ranges::any_of(library, [prefix](const LibraryEntry& entry) {
      return entry.id.starts_with(prefix);
    });
  };
  EXPECT_TRUE(has_prefix("video:"));
  EXPECT_TRUE(has_prefix("audio:"));
  EXPECT_TRUE(has_prefix("transition:"));
}

TEST(EffectLibrary, EveryEntryHasAFolderAndAName) {
  for (const LibraryEntry& entry : effect_library()) {
    EXPECT_FALSE(entry.id.empty());
    EXPECT_FALSE(entry.name.empty()) << entry.id;
    EXPECT_FALSE(entry.folder.empty()) << entry.id;
  }
}

TEST(EffectLibrary, EveryEntryCanBeApplied) {
  // The catalogue and the thing that applies from it are two lists that could
  // drift. This is what notices.
  const Project p = both_kinds();
  for (const LibraryEntry& entry : effect_library()) {
    const bool video = entry.id.starts_with("video:");
    const bool audio = entry.id.starts_with("audio:");
    if (!video && !audio) continue;  // transitions need a join, tested below
    const std::string_view clip = video ? "c1" : "a1";
    EXPECT_NE(apply_library_entry(p, clip, entry.id), p) << entry.id;
  }
}

// ------------------------------------------- adding across a selection --

/// Two video clips and one audio clip, so a selection can be mixed.
[[nodiscard]] Project three_clips() {
  Project p = both_kinds();
  Clip second;
  second.id = "c2";
  second.media_id = "m1";
  second.kind = TrackKind::Video;
  second.source_out = 5.0;
  second.start = 5.0;
  p.tracks.front().clips.push_back(std::move(second));
  return p;
}

TEST(AddEffectTo, EveryClipNamedGetsIt) {
  const std::vector<std::string> both{"c1", "c2"};
  const Project p = add_effect_to(three_clips(), both, "blur", false);

  EXPECT_EQ(core::find_clip(p, "c1")->effects.size(), 1u);
  EXPECT_EQ(core::find_clip(p, "c2")->effects.size(), 1u);
}

TEST(AddEffectTo, AClipTheEffectDoesNotSuitIsPassedOver) {
  // The point of the function. `core::add_clip_effect` does not look at a
  // clip's kind, so a plain loop would put a blur on the waveform — where it
  // would sit in the file, draw an fx badge, and never be run by anything.
  const std::vector<std::string> mixed{"c1", "a1"};
  const Project p = add_effect_to(three_clips(), mixed, "blur", false);

  EXPECT_EQ(core::find_clip(p, "c1")->effects.size(), 1u);
  EXPECT_TRUE(core::find_clip(p, "a1")->effects.empty());
}

TEST(AddEffectTo, AnAudioEffectGoesToTheAudioClipAndNoFurther) {
  const std::vector<std::string> mixed{"c1", "a1"};
  const Project p = add_effect_to(three_clips(), mixed, "lowpass", true);

  EXPECT_EQ(core::find_clip(p, "a1")->audio_effects.size(), 1u);
  EXPECT_TRUE(core::find_clip(p, "c1")->audio_effects.empty());
}

TEST(AddEffectTo, AddingTwiceStacksTwo) {
  // Adding appends, unlike pasting, which replaces. Two blurs is a legitimate
  // thing to want and the panel offers no other way to say it.
  const std::vector<std::string> one{"c1"};
  Project p = add_effect_to(three_clips(), one, "blur", false);
  p = add_effect_to(std::move(p), one, "blur", false);

  EXPECT_EQ(core::find_clip(p, "c1")->effects.size(), 2u);
}

TEST(AddEffectTo, NamingNobodyChangesNothing) {
  const Project before = three_clips();
  EXPECT_EQ(add_effect_to(before, {}, "blur", false), before);
}

TEST(AddEffectTo, AnUnknownTypeChangesNothing) {
  const Project before = three_clips();
  const std::vector<std::string> one{"c1"};
  EXPECT_EQ(add_effect_to(before, one, "no-such-effect", false), before);
}

TEST(AddEffectTo, AClipThatIsNotThereIsSkippedAndTheRestStillGetIt) {
  const std::vector<std::string> some{"gone", "c1"};
  const Project p = add_effect_to(three_clips(), some, "blur", false);

  EXPECT_EQ(core::find_clip(p, "c1")->effects.size(), 1u);
}

TEST(ClearEffectsOn, EveryClipNamedIsStripped) {
  Project p = three_clips();
  const std::vector<std::string> both{"c1", "c2"};
  p = add_effect_to(std::move(p), both, "blur", false);
  p = clear_effects_on(std::move(p), both);

  EXPECT_TRUE(core::find_clip(p, "c1")->effects.empty());
  EXPECT_TRUE(core::find_clip(p, "c2")->effects.empty());
}

TEST(ClearEffectsOn, BothStacksGo) {
  // "Clear the effects" on a clip means all of them, and the button sits next
  // to neither stack.
  Project p = three_clips();
  const std::vector<std::string> audio{"a1"};
  p = add_effect_to(std::move(p), audio, "lowpass", true);
  p = clear_effects_on(std::move(p), audio);

  EXPECT_TRUE(core::find_clip(p, "a1")->audio_effects.empty());
}

TEST(ClearEffectsOn, AClipNotNamedKeepsWhatItHad) {
  Project p = three_clips();
  const std::vector<std::string> both{"c1", "c2"};
  p = add_effect_to(std::move(p), both, "blur", false);
  p = clear_effects_on(std::move(p), std::vector<std::string>{"c1"});

  EXPECT_TRUE(core::find_clip(p, "c1")->effects.empty());
  EXPECT_EQ(core::find_clip(p, "c2")->effects.size(), 1u);
}

TEST(ClearEffectsOn, ClearingWhatIsAlreadyCleanChangesNothing) {
  const Project before = three_clips();
  const std::vector<std::string> both{"c1", "c2"};
  EXPECT_EQ(clear_effects_on(before, both), before);
}

TEST(ApplyLibraryEntry, RefusesAVideoEffectOnAnAudioClip) {
  const Project p = both_kinds();
  EXPECT_EQ(apply_library_entry(p, "a1", "video:blur"), p);
  EXPECT_FALSE(library_entry_fits(p, "a1", "video:blur"));
}

TEST(ApplyLibraryEntry, RefusesAnAudioEffectOnAVideoClip) {
  const Project p = both_kinds();
  EXPECT_EQ(apply_library_entry(p, "c1", "audio:lowpass"), p);
}

TEST(ApplyLibraryEntry, RefusesATransitionWhereNothingAbutsTheClip) {
  // A transition needs a cut to sit on. Offering one at the end of a track
  // would be a control that silently did nothing.
  const Project p = one_clip();
  EXPECT_FALSE(library_entry_fits(p, "c1", "transition:dissolve"));
  EXPECT_EQ(apply_library_entry(p, "c1", "transition:dissolve"), p);
}

/// Two clips meeting at a cut, both trimmed to the whole of their source — so
/// there are no handles for an overlapping transition to borrow.
[[nodiscard]] Project abutting() {
  Project p = one_clip();
  Clip next;
  next.id = "c2";
  next.media_id = "m1";
  next.source_out = 5.0;
  next.start = 5.0;
  p.tracks.front().clips.push_back(std::move(next));
  return p;
}

TEST(ApplyLibraryEntry, AddsADipToBlackWhereThereIsAJoin) {
  // Dip to black fades out and then in, sequentially, so it needs no handles
  // and always works where there is a cut.
  const Project p = abutting();
  ASSERT_TRUE(library_entry_fits(p, "c1", "transition:dip-black"));

  const Project after = apply_library_entry(p, "c1", "transition:dip-black");
  ASSERT_TRUE(only_clip(after).transition_out.has_value());
  EXPECT_GT(only_clip(after).transition_out->duration, 0.0);
}

TEST(ApplyLibraryEntry, RefusesAnOverlappingTransitionWithNoHandlesToBorrow) {
  // Both clips use the whole of their footage, so a dissolve has nothing to
  // overlap into and would resolve to nothing at all. The library greys it
  // rather than offering a control that silently does nothing.
  const Project p = abutting();
  EXPECT_FALSE(library_entry_fits(p, "c1", "transition:dissolve"));
  EXPECT_EQ(apply_library_entry(p, "c1", "transition:dissolve"), p);
}

TEST(ApplyLibraryEntry, AnIdThisBuildDoesNotKnowChangesNothing) {
  // Which is what a library written by a newer version looks like.
  const Project p = both_kinds();
  EXPECT_EQ(apply_library_entry(p, "c1", "video:not-an-effect"), p);
  EXPECT_EQ(apply_library_entry(p, "c1", "nonsense"), p);
  EXPECT_EQ(apply_library_entry(p, "c1", ""), p);
}

TEST(ApplyLibraryEntry, AClipThatIsNotThereChangesNothing) {
  const Project p = both_kinds();
  EXPECT_EQ(apply_library_entry(p, "nope", "video:blur"), p);
}


// ------------------------------------------------------------------ mask --
//
// A mask belongs to one effect. The panel reads and writes percentages; the
// model keeps fractions of the layer, so a mask survives the clip being scaled.

TEST(EffectMask, AnEffectStartsWithNone) {
  const Project p = add_effect(one_clip(), "c1", "blur");
  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().mask.shape, core::MaskShape::None);
}

TEST(EffectMask, IsShownInPercentagesAndStoredInFractions) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = set_effect_mask(std::move(p), "c1", 0,
                      EffectMaskRow{.shape = core::MaskShape::Ellipse,
                                    .x = 25.0,
                                    .y = 75.0,
                                    .width = 10.0,
                                    .height = 20.0,
                                    .rotation = 30.0,
                                    .feather = 5.0,
                                    .opacity = 50.0,
                                    .inverted = true});

  const core::Mask& stored = only_clip(p).effects.front().mask;
  EXPECT_DOUBLE_EQ(stored.x, 0.25);
  EXPECT_DOUBLE_EQ(stored.feather, 0.05);
  EXPECT_DOUBLE_EQ(stored.opacity, 0.5);
  EXPECT_DOUBLE_EQ(stored.rotation, 30.0) << "degrees are degrees either way";
  EXPECT_TRUE(stored.inverted);

  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  EXPECT_DOUBLE_EQ(rows.front().mask.x, 25.0);
  EXPECT_DOUBLE_EQ(rows.front().mask.opacity, 50.0);
}

TEST(EffectMask, ClearingItLeavesTheEffectApplyingEverywhere) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = set_effect_mask(std::move(p), "c1", 0, EffectMaskRow{.shape = core::MaskShape::Ellipse});
  ASSERT_TRUE(only_clip(p).effects.front().mask.active());

  p = clear_effect_mask(std::move(p), "c1", 0);
  EXPECT_FALSE(only_clip(p).effects.front().mask.active());
}

TEST(EffectMask, EachEffectHasItsOwn) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = add_effect(std::move(p), "c1", "contrast");
  p = set_effect_mask(std::move(p), "c1", 1,
                      EffectMaskRow{.shape = core::MaskShape::Rectangle, .x = 10.0});

  const std::vector<EffectRow> rows = clip_effects(p, "c1");
  EXPECT_EQ(rows[0].mask.shape, core::MaskShape::None);
  EXPECT_EQ(rows[1].mask.shape, core::MaskShape::Rectangle);
  EXPECT_DOUBLE_EQ(rows[1].mask.x, 10.0);
}

TEST(EffectMask, ANonsenseMaskIsClampedRatherThanRefused) {
  Project p = add_effect(one_clip(), "c1", "blur");
  p = set_effect_mask(std::move(p), "c1", 0,
                      EffectMaskRow{.shape = core::MaskShape::Ellipse,
                                    .width = -30.0,
                                    .feather = -10.0,
                                    .opacity = 400.0});

  const core::Mask& stored = only_clip(p).effects.front().mask;
  EXPECT_DOUBLE_EQ(stored.width, 0.0);
  EXPECT_DOUBLE_EQ(stored.feather, 0.0);
  EXPECT_DOUBLE_EQ(stored.opacity, 1.0);
}

TEST(EffectMask, AnEffectThatIsNotThereChangesNothing) {
  const Project p = add_effect(one_clip(), "c1", "blur");
  EXPECT_EQ(set_effect_mask(p, "c1", 7, EffectMaskRow{.shape = core::MaskShape::Ellipse}), p);
  EXPECT_EQ(set_effect_mask(p, "nope", 0, EffectMaskRow{.shape = core::MaskShape::Ellipse}), p);
}

TEST(EffectMask, EveryShapeIsOfferedAndNamed) {
  EXPECT_EQ(mask_shapes().size(), 4u);
  for (const core::MaskShape shape : mask_shapes()) {
    EXPECT_FALSE(mask_shape_name(shape).empty());
  }
}

TEST(EffectMask, ChoosingFreeDrawGivesAShapeToPullAbout) {
  // Premiere hands you a pen and an empty frame. A rectangle you can drag from
  // the first moment is the same feature with nothing to learn first, and the
  // first corner you move makes it yours.
  Project p = add_effect(one_clip(), "c1", "blur");
  p = set_effect_mask(std::move(p), "c1", 0,
                      EffectMaskRow{.shape = core::MaskShape::Path, .width = 30.0,
                                    .height = 20.0});

  const core::Mask& mask = only_clip(p).effects[0].mask;
  EXPECT_EQ(mask.shape, core::MaskShape::Path);
  ASSERT_EQ(mask.points.size(), 4u);
  EXPECT_DOUBLE_EQ(mask.points[0].x, -0.3);
  EXPECT_DOUBLE_EQ(mask.points[0].y, -0.2);
  EXPECT_DOUBLE_EQ(mask.points[2].x, 0.3);
  EXPECT_DOUBLE_EQ(mask.points[2].y, 0.2);
}

TEST(EffectMask, ThePathSurvivesAnEditToEveryOtherNumber) {
  // The panel's row does not carry the corners, so rewriting the mask from it
  // would erase a drawn path every time its feather was touched.
  Project p = add_effect(one_clip(), "c1", "blur");
  p = set_effect_mask(std::move(p), "c1", 0, EffectMaskRow{.shape = core::MaskShape::Path});

  core::Mask drawn = only_clip(p).effects[0].mask;
  drawn.points = {core::MaskPoint{-0.4, -0.1}, core::MaskPoint{0.2, -0.3},
                  core::MaskPoint{0.1, 0.4}};
  p = core::set_effect_mask(std::move(p), "c1", 0, drawn);

  p = set_effect_mask(std::move(p), "c1", 0,
                      EffectMaskRow{.shape = core::MaskShape::Path, .feather = 10.0});

  const core::Mask& after = only_clip(p).effects[0].mask;
  ASSERT_EQ(after.points.size(), 3u) << "the corners were thrown away";
  EXPECT_DOUBLE_EQ(after.points[1].x, 0.2);
  EXPECT_DOUBLE_EQ(after.feather, 0.1);
}


// ------------------------------------------------------------ role presets --

/// One audio clip on an audio track, which is what a role applies to.
[[nodiscard]] Project one_audio_clip() {
  Clip c;
  c.id = "a";
  c.media_id = "m1";
  c.kind = TrackKind::Audio;
  c.source_out = 5.0;

  Track t;
  t.id = "a1";
  t.kind = TrackKind::Audio;
  t.clips = {std::move(c)};

  Project p;
  p.tracks = {std::move(t)};
  return p;
}

[[nodiscard]] const Clip& audio_clip_of(const Project& p) { return p.tracks.front().clips.front(); }

TEST(RolePreset, TheRoleIsSetAndTheProcessingComesWithIt) {
  const Project p = apply_role_preset(one_audio_clip(), "a", core::AudioRole::Dialogue);
  EXPECT_EQ(audio_clip_of(p).role, core::AudioRole::Dialogue);
  EXPECT_FALSE(audio_clip_of(p).audio_effects.empty())
      << "saying what a clip is should be followed by giving it what that usually needs";
}

TEST(RolePreset, EveryParameterIsWrittenOutEvenTheOnesThePresetIgnores) {
  const Project p = apply_role_preset(one_audio_clip(), "a", core::AudioRole::Dialogue);
  const auto band = std::ranges::find(audio_clip_of(p).audio_effects, "eqband",
                                      &core::AudioClipEffect::type);
  ASSERT_NE(band, audio_clip_of(p).audio_effects.end());
  // Three in the registry: frequency, gain and Q. The preset names all three
  // here, so the one that matters is that the count matches the registry rather
  // than the preset.
  const audio::AudioEffectDef* def = audio::audio_effect_def("eqband");
  ASSERT_NE(def, nullptr);
  EXPECT_EQ(band->params.size(), def->params.size());
}

TEST(RolePreset, PressingItTwiceDoesNotStackItTwice) {
  Project p = apply_role_preset(one_audio_clip(), "a", core::AudioRole::Dialogue);
  const std::size_t once = audio_clip_of(p).audio_effects.size();
  p = apply_role_preset(std::move(p), "a", core::AudioRole::Dialogue);
  EXPECT_EQ(audio_clip_of(p).audio_effects.size(), once);
}

TEST(RolePreset, AnEffectSomebodyTunedIsLeftAlone) {
  Project p = add_audio_effect(one_audio_clip(), "a", "compressor");
  p = set_audio_effect_parameter(std::move(p), "a", 0, "ratio", 8.0);
  p = apply_role_preset(std::move(p), "a", core::AudioRole::Dialogue);

  const auto compressor = std::ranges::find(audio_clip_of(p).audio_effects, "compressor",
                                            &core::AudioClipEffect::type);
  ASSERT_NE(compressor, audio_clip_of(p).audio_effects.end());
  EXPECT_DOUBLE_EQ(compressor->params.at("ratio"), 8.0)
      << "a preset is a starting point for a clip that has none, not an opinion about work "
         "already done";
  EXPECT_EQ(std::ranges::count(audio_clip_of(p).audio_effects, "compressor",
                               &core::AudioClipEffect::type),
            1);
}

TEST(RolePreset, NoRoleMeansNoProcessing) {
  const Project p = apply_role_preset(one_audio_clip(), "a", core::AudioRole::None);
  EXPECT_EQ(audio_clip_of(p).role, core::AudioRole::None);
  EXPECT_TRUE(audio_clip_of(p).audio_effects.empty());
}

TEST(RolePreset, ThePictureHalfOfAPairTakesNeither) {
  const Project before = one_clip();
  EXPECT_EQ(apply_role_preset(before, "c1", core::AudioRole::Dialogue), before)
      << "a role is about sound";
}

}  // namespace
}  // namespace cutline::editor
