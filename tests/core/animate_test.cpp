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
  p.sequence().tracks = {v};
  return p;
}

const Clip& only_clip(const Project& p) { return p.sequence().tracks[0].clips[0]; }

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
  p.sequence().tracks[0].clips[0].gain = 0.5;
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

// -------------------------------------------------------- animating a mask --

/// A clip with one effect carrying an ellipse.
[[nodiscard]] Project masked_project() {
  Project p = one_clip_project();
  p = add_clip_effect(std::move(p), "c1", "blur", {{"amount", 4.0}});
  return set_effect_mask(std::move(p), "c1", 0,
                         Mask{.shape = MaskShape::Ellipse, .x = 0.25, .width = 0.1});
}

[[nodiscard]] const ClipEffect& only_effect(const Project& p) { return only_clip(p).effects[0]; }

TEST(MaskAnimation, AMaskNumberIsSetOnTheMaskRatherThanTheParameters) {
  // The mask is the one home for the value. A parameter map holding a second
  // copy would be two truths about one number, and whichever the renderer read
  // would be the wrong one half the time.
  Project p = masked_project();
  p = set_clip_effect_param(std::move(p), "c1", 0, "mask.width", 0.4);

  EXPECT_DOUBLE_EQ(only_effect(p).mask.width, 0.4);
  EXPECT_FALSE(only_effect(p).params.contains("mask.width"));
  EXPECT_DOUBLE_EQ(effect_param_at(only_effect(p), "mask.width", 0.0), 0.4);
}

TEST(MaskAnimation, KeyframesOnAMaskResolveOntoTheMask) {
  Project p = masked_project();
  p = set_effect_keyframe(std::move(p), "c1", 0, "mask.x", 0.0, 0.2);
  p = set_effect_keyframe(std::move(p), "c1", 0, "mask.x", 2.0, 0.8);

  ASSERT_TRUE(is_effect_param_animated(only_effect(p), "mask.x"));

  const std::vector<ClipEffect> at_start = resolved_effects(only_clip(p), 0.0);
  ASSERT_EQ(at_start.size(), 1u);
  EXPECT_DOUBLE_EQ(at_start[0].mask.x, 0.2);
  // Folded onto the mask, not left in the parameters where nothing reads it.
  EXPECT_FALSE(at_start[0].params.contains("mask.x"));

  const std::vector<ClipEffect> midway = resolved_effects(only_clip(p), 1.0);
  EXPECT_DOUBLE_EQ(midway[0].mask.x, 0.5);

  const std::vector<ClipEffect> at_end = resolved_effects(only_clip(p), 2.0);
  EXPECT_DOUBLE_EQ(at_end[0].mask.x, 0.8);
}

TEST(MaskAnimation, AnAnimatedNumberLeavesTheOthersAlone) {
  Project p = masked_project();
  p = set_effect_keyframe(std::move(p), "c1", 0, "mask.x", 0.0, 0.9);

  const std::vector<ClipEffect> resolved = resolved_effects(only_clip(p), 0.0);
  ASSERT_EQ(resolved.size(), 1u);
  EXPECT_DOUBLE_EQ(resolved[0].mask.x, 0.9);
  EXPECT_DOUBLE_EQ(resolved[0].mask.width, 0.1) << "an unanimated number moved";
  EXPECT_EQ(resolved[0].mask.shape, MaskShape::Ellipse);
}

TEST(MaskAnimation, TheShapeAndTheInversionAreNotNumbers) {
  // A keyframe between two of them would have to mean something halfway
  // between an ellipse and a rectangle.
  EXPECT_EQ(mask_param_field("mask.shape"), nullptr);
  EXPECT_EQ(mask_param_field("mask.inverted"), nullptr);
  EXPECT_EQ(mask_param_field("amount"), nullptr);
  EXPECT_NE(mask_param_field("mask.feather"), nullptr);
}

TEST(MaskAnimation, EveryOfferedKeyNamesAField) {
  // The list a panel builds its rows from and the mapping a value is written
  // through are two lists, and this is what keeps them one.
  for (const std::string_view key : mask_param_keys()) {
    EXPECT_NE(mask_param_field(key), nullptr) << key;
  }
}

TEST(MaskAnimation, AMaskKeyframeCountsAsOneOnTheClip) {
  // Which is what draws the mark on the clip in the timeline, and what tells
  // the keyframe panel there is a lane to show.
  Project p = masked_project();
  EXPECT_FALSE(clip_has_effect_keyframes(only_clip(p)));

  p = set_effect_keyframe(std::move(p), "c1", 0, "mask.feather", 1.5, 0.3);
  EXPECT_TRUE(clip_has_effect_keyframes(only_clip(p)));

  const std::vector<double> times = effect_keyframe_times(only_clip(p));
  ASSERT_EQ(times.size(), 1u);
  EXPECT_DOUBLE_EQ(times[0], 1.5);
}

// ------------------------------------------------------- track automation --

Project one_audio_track() {
  Project p;
  Track a;
  a.id = "a1";
  a.kind = TrackKind::Audio;
  a.gain = 0.5;
  p.sequence().tracks = {a};
  return p;
}

const Track& only_track(const Project& p) { return p.sequence().tracks[0]; }

TEST(TrackAutomation, AKeyframeIsSetAtATimelineTime) {
  Project p = one_audio_track();
  p = set_track_gain_keyframe(std::move(p), "a1", 4.0, 0.25);

  ASSERT_EQ(only_track(p).gain_keyframes.size(), 1u);
  EXPECT_DOUBLE_EQ(only_track(p).gain_keyframes[0].t, 4.0);
  EXPECT_DOUBLE_EQ(only_track(p).gain_keyframes[0].v, 0.25);
}

TEST(TrackAutomation, SettingOneTwiceAtTheSameTimeReplacesIt) {
  Project p = one_audio_track();
  p = set_track_gain_keyframe(std::move(p), "a1", 2.0, 0.5);
  p = set_track_gain_keyframe(std::move(p), "a1", 2.0, 0.75);

  ASSERT_EQ(only_track(p).gain_keyframes.size(), 1u);
  EXPECT_DOUBLE_EQ(only_track(p).gain_keyframes[0].v, 0.75);
}

TEST(TrackAutomation, ValuesAreClampedToWhatAFaderCanReach) {
  Project p = one_audio_track();
  p = set_track_gain_keyframe(std::move(p), "a1", 0.0, -5.0);
  p = set_track_pan_keyframe(std::move(p), "a1", 0.0, 9.0);

  EXPECT_DOUBLE_EQ(only_track(p).gain_keyframes[0].v, 0.0);
  EXPECT_DOUBLE_EQ(only_track(p).pan_keyframes[0].v, 1.0);
}

TEST(TrackAutomation, NamingATrackThatIsNotThereChangesNothing) {
  const Project before = one_audio_track();
  EXPECT_EQ(set_track_gain_keyframe(before, "nobody", 1.0, 0.5), before);
  EXPECT_EQ(set_track_automation(before, "nobody", AutomationMode::Write), before);
}

TEST(TrackAutomation, OffMeansTheCurveIsNotRead) {
  // Off does not throw the curve away — it stops it being followed, so a fader
  // that was automated can go back to being a plain fader and then back again.
  Project p = one_audio_track();
  p = set_track_gain_keyframe(std::move(p), "a1", 0.0, 1.0);
  ASSERT_TRUE(is_track_gain_animated(only_track(p)));

  p = set_track_automation(std::move(p), "a1", AutomationMode::Off);
  EXPECT_FALSE(is_track_gain_animated(only_track(p)));
  EXPECT_FALSE(only_track(p).gain_keyframes.empty()) << "the curve is kept";
  EXPECT_DOUBLE_EQ(track_gain_at(only_track(p), 0.0), 0.5) << "the constant is used";

  p = set_track_automation(std::move(p), "a1", AutomationMode::Read);
  EXPECT_DOUBLE_EQ(track_gain_at(only_track(p), 0.0), 1.0);
}

TEST(TrackAutomation, WriteLatchAndTouchAllFollowTheCurveToo) {
  // They differ in when they *record*, not in what they play back.
  for (const AutomationMode mode :
       {AutomationMode::Write, AutomationMode::Latch, AutomationMode::Touch}) {
    Project p = one_audio_track();
    p = set_track_gain_keyframe(std::move(p), "a1", 0.0, 1.0);
    p = set_track_automation(std::move(p), "a1", mode);
    EXPECT_TRUE(is_track_gain_animated(only_track(p)));
  }
}

TEST(TrackPass, APassBecomesTheCurve) {
  Project p = one_audio_track();
  const std::vector<Keyframe> pass{{.t = 1.0, .v = 1.0}, {.t = 2.0, .v = 0.5}};
  p = write_track_gain_pass(std::move(p), "a1", pass);

  ASSERT_EQ(only_track(p).gain_keyframes.size(), 2u);
  EXPECT_DOUBLE_EQ(only_track(p).gain_keyframes[1].v, 0.5);
}

TEST(TrackPass, ItReplacesOnlyWhatItCovers) {
  // Punching in: riding the fader through one passage must not disturb what was
  // set either side of it.
  Project p = one_audio_track();
  p = set_track_gain_keyframe(std::move(p), "a1", 0.0, 1.0);
  p = set_track_gain_keyframe(std::move(p), "a1", 5.0, 1.0);
  p = set_track_gain_keyframe(std::move(p), "a1", 10.0, 1.0);

  const std::vector<Keyframe> pass{{.t = 4.0, .v = 0.2}, {.t = 6.0, .v = 0.3}};
  p = write_track_gain_pass(std::move(p), "a1", pass);

  const std::vector<Keyframe>& keys = only_track(p).gain_keyframes;
  ASSERT_EQ(keys.size(), 4u);
  EXPECT_DOUBLE_EQ(keys[0].t, 0.0) << "before the pass, untouched";
  EXPECT_DOUBLE_EQ(keys[1].t, 4.0);
  EXPECT_DOUBLE_EQ(keys[2].t, 6.0);
  EXPECT_DOUBLE_EQ(keys[3].t, 10.0) << "after the pass, untouched";
}

TEST(TrackPass, AKeyframeExactlyUnderEitherEndIsReplaced) {
  // Inclusive at both ends, or a point would be left sitting a fraction of a
  // frame from the one that replaced it.
  Project p = one_audio_track();
  p = set_track_gain_keyframe(std::move(p), "a1", 2.0, 1.0);
  p = set_track_gain_keyframe(std::move(p), "a1", 4.0, 1.0);

  const std::vector<Keyframe> pass{{.t = 2.0, .v = 0.1}, {.t = 4.0, .v = 0.2}};
  p = write_track_gain_pass(std::move(p), "a1", pass);

  const std::vector<Keyframe>& keys = only_track(p).gain_keyframes;
  ASSERT_EQ(keys.size(), 2u);
  EXPECT_DOUBLE_EQ(keys[0].v, 0.1);
  EXPECT_DOUBLE_EQ(keys[1].v, 0.2);
}

TEST(TrackPass, AnEmptyPassChangesNothing) {
  // What a mode that was armed and never touched produces.
  Project p = one_audio_track();
  p = set_track_gain_keyframe(std::move(p), "a1", 1.0, 0.5);
  const Project before = p;

  EXPECT_EQ(write_track_gain_pass(before, "a1", {}), before);
}

TEST(TrackPass, TheResultIsStillSortedByTime) {
  Project p = one_audio_track();
  p = set_track_gain_keyframe(std::move(p), "a1", 9.0, 1.0);

  const std::vector<Keyframe> pass{{.t = 1.0, .v = 0.4}, {.t = 3.0, .v = 0.6}};
  p = write_track_gain_pass(std::move(p), "a1", pass);

  const std::vector<Keyframe>& keys = only_track(p).gain_keyframes;
  for (std::size_t i = 1; i < keys.size(); ++i) EXPECT_LT(keys[i - 1].t, keys[i].t);
}

TEST(TrackPass, ClearingLeavesTheModeAlone) {
  // Clearing a pass to record another one is the ordinary reason to clear.
  Project p = one_audio_track();
  p = set_track_automation(std::move(p), "a1", AutomationMode::Latch);
  p = set_track_gain_keyframe(std::move(p), "a1", 1.0, 0.5);
  p = clear_track_gain_keyframes(std::move(p), "a1");

  EXPECT_TRUE(only_track(p).gain_keyframes.empty());
  EXPECT_EQ(only_track(p).automation, AutomationMode::Latch);
}

TEST(MasterAutomation, ThePassBecomesTheCurveAndOffIgnoresIt) {
  Project p = one_audio_track();
  p.sequence().master_gain = 0.5;
  const std::vector<Keyframe> pass{{.t = 0.0, .v = 1.0}, {.t = 2.0, .v = 0.25}};
  p = write_master_gain_pass(std::move(p), pass);

  ASSERT_EQ(p.sequence().master_gain_keyframes.size(), 2u);
  EXPECT_TRUE(is_master_gain_animated(p));
  EXPECT_DOUBLE_EQ(master_gain_at(p, 0.0), 1.0);

  p = set_master_automation(std::move(p), AutomationMode::Off);
  EXPECT_FALSE(is_master_gain_animated(p));
  EXPECT_DOUBLE_EQ(master_gain_at(p, 0.0), 0.5) << "the constant, and the curve is kept";
  EXPECT_EQ(p.sequence().master_gain_keyframes.size(), 2u);
}

TEST(MasterAutomation, APassReplacesOnlyWhatItCovers) {
  // The same rule as a track's, and it is the same code — punching in on the
  // master means what it means anywhere else.
  Project p;
  p = write_master_gain_pass(std::move(p), std::vector<Keyframe>{{.t = 0.0, .v = 1.0},
                                                                 {.t = 10.0, .v = 1.0}});
  p = write_master_gain_pass(std::move(p), std::vector<Keyframe>{{.t = 4.0, .v = 0.2},
                                                                 {.t = 6.0, .v = 0.3}});

  ASSERT_EQ(p.sequence().master_gain_keyframes.size(), 4u);
  EXPECT_DOUBLE_EQ(p.sequence().master_gain_keyframes[0].t, 0.0);
  EXPECT_DOUBLE_EQ(p.sequence().master_gain_keyframes[3].t, 10.0);
}

TEST(MasterAutomation, ClearingLeavesTheModeAlone) {
  Project p = set_master_automation(Project{}, AutomationMode::Touch);
  p = write_master_gain_pass(std::move(p), std::vector<Keyframe>{{.t = 1.0, .v = 0.5}});
  p = clear_master_gain_keyframes(std::move(p));

  EXPECT_TRUE(p.sequence().master_gain_keyframes.empty());
  EXPECT_EQ(p.sequence().master_automation, AutomationMode::Touch);
}

}  // namespace
}  // namespace cutline::core
