/// A clip's effect stack, as a panel reads it.

#include "cutline/editor/effects_binding.hpp"

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
  const Project before = one_clip();
  EXPECT_EQ(add_audio_effect(before, "c1", "reverb"), before);
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
  p.tracks[0].clips[0].audio_effects.push_back(core::AudioClipEffect{.type = "reverb"});

  const std::vector<EffectRow> rows = clip_audio_effects(p, "c1");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_TRUE(rows.front().unknown);
  EXPECT_EQ(rows.front().name, "reverb");
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

}  // namespace
}  // namespace cutline::editor
