#include "cutline/core/animate.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

Project one_clip_project() {
  Project p;
  Clip c;
  c.id = "c1";
  c.media_id = "m1";
  c.source_in = 0.0;
  c.source_out = 5.0;

  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.clips = {c};
  p.tracks = {v};
  return p;
}

const Clip& only_clip(const Project& p) { return p.tracks[0].clips[0]; }

const std::vector<Keyframe>& kfs_of(const Project& p, AnimProp prop) {
  return only_clip(p).keyframes[anim_prop_index(prop)];
}

// ------------------------------------------------------ transform keyframes --

TEST(SetKeyframe, AddsKeyframesInTimeOrder) {
  Project p = one_clip_project();
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 2.0, 0.8);
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 0.0, 0.2);

  const std::vector<Keyframe>& kfs = kfs_of(p, AnimProp::X);
  ASSERT_EQ(kfs.size(), 2u);
  EXPECT_DOUBLE_EQ(kfs[0].t, 0.0);
  EXPECT_DOUBLE_EQ(kfs[0].v, 0.2);
  EXPECT_DOUBLE_EQ(kfs[1].t, 2.0);
}

TEST(SetKeyframe, ReplacesAKeyframeAtTheSameTime) {
  Project p = one_clip_project();
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 1.0, 0.2);
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 1.0, 0.9);

  ASSERT_EQ(kfs_of(p, AnimProp::X).size(), 1u);
  EXPECT_DOUBLE_EQ(kfs_of(p, AnimProp::X)[0].v, 0.9);
}

TEST(SetKeyframe, ReplacingKeepsThatKeyframesInterpolation) {
  Project p = one_clip_project();
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 1.0, 0.2);
  p = set_keyframe_interp(std::move(p), "c1", AnimProp::X, Interp::Hold);
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 1.0, 0.9);

  EXPECT_EQ(kfs_of(p, AnimProp::X)[0].e, Interp::Hold);
}

// A property animated with "ease" should stay eased as points are added.
TEST(SetKeyframe, NewKeyframesInheritThePropertysMode) {
  Project p = one_clip_project();
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 0.0, 0.0);
  p = set_keyframe_interp(std::move(p), "c1", AnimProp::X, Interp::Ease);
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 2.0, 1.0);

  EXPECT_EQ(kfs_of(p, AnimProp::X)[1].e, Interp::Ease);
  EXPECT_EQ(keyframe_interp_of(only_clip(p), AnimProp::X), Interp::Ease);
}

TEST(KeyframeInterp, DefaultsToLinear) {
  const Project p = one_clip_project();
  EXPECT_EQ(keyframe_interp_of(only_clip(p), AnimProp::X), Interp::Linear);
}

TEST(RemoveKeyframe, RemovesTheOneNearTheGivenTime) {
  Project p = one_clip_project();
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 0.0, 0.0);
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 2.0, 1.0);

  p = remove_keyframe_at(std::move(p), "c1", AnimProp::X, 2.0);
  ASSERT_EQ(kfs_of(p, AnimProp::X).size(), 1u);
  EXPECT_DOUBLE_EQ(kfs_of(p, AnimProp::X)[0].t, 0.0);
}

TEST(RemoveKeyframe, LeavesDistantKeyframesAlone) {
  Project p = one_clip_project();
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 2.0, 1.0);
  p = remove_keyframe_at(std::move(p), "c1", AnimProp::X, 2.5);
  EXPECT_EQ(kfs_of(p, AnimProp::X).size(), 1u);
}

TEST(ClearKeyframes, TurnsAnimationOffForThatProperty) {
  Project p = one_clip_project();
  p = set_keyframe(std::move(p), "c1", AnimProp::X, 0.0, 0.0);
  p = set_keyframe(std::move(p), "c1", AnimProp::Y, 0.0, 0.0);

  p = clear_keyframes(std::move(p), "c1", AnimProp::X);
  EXPECT_FALSE(is_animated(only_clip(p), AnimProp::X));
  EXPECT_TRUE(is_animated(only_clip(p), AnimProp::Y));
}

TEST(KeyframeOps, UnknownClipsAreNoOps) {
  const Project before = one_clip_project();
  EXPECT_EQ(set_keyframe(before, "nope", AnimProp::X, 0.0, 1.0), before);
  EXPECT_EQ(remove_keyframe_at(before, "nope", AnimProp::X, 0.0), before);
  EXPECT_EQ(clear_keyframes(before, "nope", AnimProp::X), before);
}

// ------------------------------------------------------- volume automation --

TEST(GainKeyframes, ClampToTheAllowedRange) {
  Project p = one_clip_project();
  p = set_gain_keyframe(std::move(p), "c1", 0.0, -3.0);
  p = set_gain_keyframe(std::move(p), "c1", 1.0, 99.0);

  EXPECT_DOUBLE_EQ(only_clip(p).gain_keyframes[0].v, 0.0);
  EXPECT_DOUBLE_EQ(only_clip(p).gain_keyframes[1].v, kMaxGain);
}

// DIVERGENCE: the reference discarded a gain keyframe's interpolation on every
// edit, which made eased volume automation unreachable.
TEST(GainKeyframes, KeepInterpolationAcrossEdits) {
  Project p = one_clip_project();
  p = set_gain_keyframe(std::move(p), "c1", 0.0, 0.5);
  p = set_gain_keyframe_interp(std::move(p), "c1", Interp::Ease);
  p = set_gain_keyframe(std::move(p), "c1", 0.0, 0.8);  // re-edit the same point

  EXPECT_EQ(only_clip(p).gain_keyframes[0].e, Interp::Ease);
  EXPECT_EQ(gain_keyframe_interp_of(only_clip(p)), Interp::Ease);
  EXPECT_DOUBLE_EQ(only_clip(p).gain_keyframes[0].v, 0.8);
}

TEST(GainKeyframes, MoveRelocatesAPoint) {
  Project p = one_clip_project();
  p = set_gain_keyframe(std::move(p), "c1", 1.0, 0.5);
  p = move_gain_keyframe(std::move(p), "c1", 1.0, 3.0, 0.7);

  ASSERT_EQ(only_clip(p).gain_keyframes.size(), 1u);
  EXPECT_DOUBLE_EQ(only_clip(p).gain_keyframes[0].t, 3.0);
  EXPECT_DOUBLE_EQ(only_clip(p).gain_keyframes[0].v, 0.7);
}

TEST(GainKeyframes, ClearReturnsToConstantGain) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].gain = 0.5;
  p = set_gain_keyframe(std::move(p), "c1", 0.0, 1.0);
  p = clear_gain_keyframes(std::move(p), "c1");

  EXPECT_FALSE(is_gain_animated(only_clip(p)));
  EXPECT_DOUBLE_EQ(gain_at(only_clip(p), 0.0), 0.5);
}

// ------------------------------------------------------------ effect stack --

Project with_effect(std::string type = "blur") {
  Project p = one_clip_project();
  return add_clip_effect(std::move(p), "c1", std::move(type), {{"amount", 5.0}});
}

TEST(EffectStack, AddAppendsInOrder) {
  Project p = with_effect("blur");
  p = add_clip_effect(std::move(p), "c1", "hue", {{"angle", 30.0}});

  ASSERT_EQ(only_clip(p).effects.size(), 2u);
  EXPECT_EQ(only_clip(p).effects[0].type, "blur");
  EXPECT_EQ(only_clip(p).effects[1].type, "hue");
  EXPECT_TRUE(only_clip(p).effects[0].enabled);
}

TEST(EffectStack, RemoveDropsTheEntry) {
  Project p = with_effect();
  p = remove_clip_effect(std::move(p), "c1", 0);
  EXPECT_TRUE(only_clip(p).effects.empty());
}

TEST(EffectStack, RemoveIgnoresAnOutOfRangeIndex) {
  const Project before = with_effect();
  EXPECT_EQ(remove_clip_effect(before, "c1", 7), before);
}

TEST(EffectStack, MoveReordersTheStack) {
  Project p = with_effect("blur");
  p = add_clip_effect(std::move(p), "c1", "hue", {});

  p = move_clip_effect(std::move(p), "c1", 1, -1);
  EXPECT_EQ(only_clip(p).effects[0].type, "hue");
  EXPECT_EQ(only_clip(p).effects[1].type, "blur");
}

TEST(EffectStack, MoveStopsAtTheEnds) {
  const Project before = with_effect();
  EXPECT_EQ(move_clip_effect(before, "c1", 0, -1), before);
  EXPECT_EQ(move_clip_effect(before, "c1", 0, 1), before);
}

TEST(EffectStack, ToggleFlipsEnabled) {
  Project p = with_effect();
  p = toggle_clip_effect(std::move(p), "c1", 0);
  EXPECT_FALSE(only_clip(p).effects[0].enabled);
  p = toggle_clip_effect(std::move(p), "c1", 0);
  EXPECT_TRUE(only_clip(p).effects[0].enabled);
}

TEST(EffectStack, AppendCopiesAStackForPaste) {
  Project p = with_effect("blur");
  const std::vector<ClipEffect> copied = only_clip(p).effects;

  p = append_clip_effects(std::move(p), "c1", copied);
  ASSERT_EQ(only_clip(p).effects.size(), 2u);
  EXPECT_EQ(only_clip(p).effects[1].type, "blur");
}

TEST(EffectStack, ClearRemovesEverything) {
  Project p = with_effect();
  p = clear_clip_effects(std::move(p), "c1");
  EXPECT_TRUE(only_clip(p).effects.empty());
}

TEST(EffectStack, SetParamAndColour) {
  Project p = with_effect();
  p = set_clip_effect_param(std::move(p), "c1", 0, "amount", 12.0);
  p = set_clip_effect_color(std::move(p), "c1", 0, "key", "#00d000");

  EXPECT_DOUBLE_EQ(only_clip(p).effects[0].params.at("amount"), 12.0);
  EXPECT_EQ(only_clip(p).effects[0].colors.at("key"), "#00d000");
}

// -------------------------------------------------- effect param animation --

TEST(EffectParams, ReadTheStaticValueWhenNotAnimated) {
  const Project p = with_effect();
  const ClipEffect& e = only_clip(p).effects[0];
  EXPECT_FALSE(is_effect_param_animated(e, "amount"));
  EXPECT_DOUBLE_EQ(effect_param_at(e, "amount", 3.0), 5.0);
  EXPECT_DOUBLE_EQ(effect_param_at(e, "missing", 3.0), 0.0);
}

TEST(EffectParams, AnimationOverridesTheStaticValue) {
  Project p = with_effect();
  p = set_effect_keyframe(std::move(p), "c1", 0, "amount", 0.0, 0.0);
  p = set_effect_keyframe(std::move(p), "c1", 0, "amount", 2.0, 10.0);

  const ClipEffect& e = only_clip(p).effects[0];
  EXPECT_TRUE(is_effect_param_animated(e, "amount"));
  EXPECT_DOUBLE_EQ(effect_param_at(e, "amount", 1.0), 5.0);
  EXPECT_DOUBLE_EQ(effect_param_at(e, "amount", 0.0), 0.0);
}

// The renderer reads params only, so resolving must leave nothing animated.
TEST(ResolvedEffects, FoldAnimationIntoPlainValues) {
  Project p = with_effect();
  p = set_effect_keyframe(std::move(p), "c1", 0, "amount", 0.0, 0.0);
  p = set_effect_keyframe(std::move(p), "c1", 0, "amount", 2.0, 10.0);

  const std::vector<ClipEffect> resolved = resolved_effects(only_clip(p), 1.0);
  ASSERT_EQ(resolved.size(), 1u);
  EXPECT_DOUBLE_EQ(resolved[0].params.at("amount"), 5.0);
  EXPECT_TRUE(resolved[0].keyframes.empty());
  EXPECT_EQ(resolved[0].type, "blur");
}

TEST(ResolvedEffects, PassStaticStacksThrough) {
  const Project p = with_effect();
  const std::vector<ClipEffect> resolved = resolved_effects(only_clip(p), 9.0);
  ASSERT_EQ(resolved.size(), 1u);
  EXPECT_DOUBLE_EQ(resolved[0].params.at("amount"), 5.0);
}

TEST(EffectKeyframes, ReportPresenceAndTimes) {
  Project p = with_effect();
  EXPECT_FALSE(clip_has_effect_keyframes(only_clip(p)));

  p = set_effect_keyframe(std::move(p), "c1", 0, "amount", 1.5, 3.0);
  EXPECT_TRUE(clip_has_effect_keyframes(only_clip(p)));
  EXPECT_EQ(effect_keyframe_times(only_clip(p)), std::vector<double>{1.5});
}

TEST(EffectKeyframes, InterpolationModeRoundTrips) {
  Project p = with_effect();
  p = set_effect_keyframe(std::move(p), "c1", 0, "amount", 0.0, 0.0);
  EXPECT_EQ(effect_keyframe_interp_of(only_clip(p).effects[0], "amount"), Interp::Linear);

  p = set_effect_keyframe_interp(std::move(p), "c1", 0, "amount", Interp::Hold);
  EXPECT_EQ(effect_keyframe_interp_of(only_clip(p).effects[0], "amount"), Interp::Hold);
}

// Removing the last keyframe must un-animate the parameter, so it falls back to
// its static value rather than evaluating an empty list.
TEST(EffectKeyframes, RemovingTheLastOneRestoresTheStaticValue) {
  Project p = with_effect();
  p = set_effect_keyframe(std::move(p), "c1", 0, "amount", 1.0, 42.0);
  p = remove_effect_keyframe_at(std::move(p), "c1", 0, "amount", 1.0);

  const ClipEffect& e = only_clip(p).effects[0];
  EXPECT_FALSE(is_effect_param_animated(e, "amount"));
  EXPECT_DOUBLE_EQ(effect_param_at(e, "amount", 1.0), 5.0);
}

TEST(EffectKeyframes, ClearRemovesOnlyThatParameter) {
  Project p = with_effect();
  p = set_effect_keyframe(std::move(p), "c1", 0, "amount", 1.0, 42.0);
  p = set_effect_keyframe(std::move(p), "c1", 0, "other", 1.0, 7.0);

  p = clear_effect_keyframes(std::move(p), "c1", 0, "amount");
  const ClipEffect& e = only_clip(p).effects[0];
  EXPECT_FALSE(is_effect_param_animated(e, "amount"));
  EXPECT_TRUE(is_effect_param_animated(e, "other"));
}

// ------------------------------------------------------------ audio stack --

TEST(AudioEffects, AddRemoveToggleAndReorder) {
  Project p = one_clip_project();
  p = add_audio_effect(std::move(p), "c1", "highpass", {{"freq", 100.0}});
  p = add_audio_effect(std::move(p), "c1", "compressor", {{"ratio", 4.0}});

  ASSERT_EQ(only_clip(p).audio_effects.size(), 2u);
  EXPECT_EQ(only_clip(p).audio_effects[0].type, "highpass");

  p = move_audio_effect(std::move(p), "c1", 0, 1);
  EXPECT_EQ(only_clip(p).audio_effects[0].type, "compressor");

  p = toggle_audio_effect(std::move(p), "c1", 0);
  EXPECT_FALSE(only_clip(p).audio_effects[0].enabled);

  p = set_audio_effect_param(std::move(p), "c1", 0, "ratio", 8.0);
  EXPECT_DOUBLE_EQ(only_clip(p).audio_effects[0].params.at("ratio"), 8.0);

  p = remove_audio_effect(std::move(p), "c1", 0);
  ASSERT_EQ(only_clip(p).audio_effects.size(), 1u);
  EXPECT_EQ(only_clip(p).audio_effects[0].type, "highpass");
}

TEST(AudioEffects, OutOfRangeIndicesAreNoOps) {
  const Project before = one_clip_project();
  EXPECT_EQ(remove_audio_effect(before, "c1", 0), before);
  EXPECT_EQ(toggle_audio_effect(before, "c1", 3), before);
  EXPECT_EQ(set_audio_effect_param(before, "c1", 3, "x", 1.0), before);
}

}  // namespace
}  // namespace cutline::core
