/// What plays, and how loud — decided from the model alone.

#include "cutline/render/mix.hpp"

#include "cutline/core/animate.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include <string>
#include <vector>

namespace cutline::render {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

[[nodiscard]] Media source(std::string id, double duration = 60.0) {
  Media m;
  m.id = std::move(id);
  m.audio_stream_count = 1;
  m.duration = duration;
  return m;
}

[[nodiscard]] Clip clip(std::string id, std::string media_id, double start, double length,
                        double source_in = 0.0) {
  Clip c;
  c.id = std::move(id);
  c.media_id = std::move(media_id);
  c.kind = TrackKind::Audio;
  c.start = start;
  c.source_in = source_in;
  c.source_out = source_in + length;
  return c;
}

[[nodiscard]] Track audio_track(std::string id, std::vector<Clip> clips) {
  Track t;
  t.id = std::move(id);
  t.kind = TrackKind::Audio;
  t.clips = std::move(clips);
  return t;
}

[[nodiscard]] Project project(std::vector<Track> tracks, std::vector<Media> media = {}) {
  Project p;
  p.media = media.empty() ? std::vector<Media>{source("m")} : std::move(media);
  p.sequence().tracks = std::move(tracks);
  return p;
}

// ------------------------------------------------------------------ what plays --

TEST(PlanAudio, AnEmptyProjectPlaysNothing) {
  EXPECT_TRUE(plan_audio(Project{}).empty());
}

TEST(PlanAudio, AnAudioClipIsPlanned) {
  const Project p = project({audio_track("a1", {clip("c", "m", 2.0, 5.0, 1.0)})});
  const auto planned = plan_audio(p);

  ASSERT_EQ(planned.size(), 1u);
  EXPECT_EQ(planned[0].clip->id, "c");
  EXPECT_EQ(planned[0].media->id, "m");
  EXPECT_DOUBLE_EQ(planned[0].start, 2.0);
  EXPECT_DOUBLE_EQ(planned[0].end, 7.0);
  EXPECT_DOUBLE_EQ(planned[0].source_in, 1.0);
  EXPECT_DOUBLE_EQ(planned[0].source_out, 6.0);
}

TEST(PlanAudio, VideoTracksContributeNothing) {
  // An A/V pair is two linked clips in this model, and the audio half lives on
  // an audio track. A video track carrying clips must not be decoded for sound.
  Project p = project({audio_track("a1", {})});
  Track video;
  video.id = "v1";
  video.kind = TrackKind::Video;
  video.clips = {clip("c", "m", 0.0, 5.0)};
  video.clips[0].kind = TrackKind::Video;
  p.sequence().tracks.insert(p.sequence().tracks.begin(), video);

  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, MutedTracksAreSilent) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].muted = true;
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, SoloSilencesEveryOtherTrack) {
  Project p = project({audio_track("a1", {clip("c1", "m", 0.0, 5.0)}),
                       audio_track("a2", {clip("c2", "m", 0.0, 5.0)})});
  p.sequence().tracks[1].solo = true;

  const auto planned = plan_audio(p);
  ASSERT_EQ(planned.size(), 1u);
  EXPECT_EQ(planned[0].clip->id, "c2");
}

TEST(PlanAudio, ASoloedTrackThatIsAlsoMutedStaysSilent) {
  // Mute wins: it is the more explicit statement of the two.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].solo = true;
  p.sequence().tracks[0].muted = true;
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, DisabledClipsAreSkipped) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].clips[0].disabled = true;
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, AClipPinnedToSilenceIsSkipped) {
  // Decoding it would be work with no audible result.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].clips[0].gain = 0.0;
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, AClipThatFadesUpFromSilenceStillPlays) {
  // Automation is checked point by point, not just at the start, or every clip
  // that begins silent would vanish.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p = core::set_gain_keyframe(std::move(p), "c", 0.0, 0.0);
  p = core::set_gain_keyframe(std::move(p), "c", 2.0, 1.0);
  EXPECT_EQ(plan_audio(p).size(), 1u);
}

TEST(PlanAudio, AZeroLengthClipIsSkipped) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 0.0)})});
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, GeneratedMediaHaveNothingToDecode) {
  Media title;
  title.id = "t";
  title.is_graphic = true;
  Project p = project({audio_track("a1", {clip("c", "t", 0.0, 5.0)})}, {title});
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, TheStreamOrdinalIsCarried) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].clips[0].audio_stream = 2;
  ASSERT_EQ(plan_audio(p).size(), 1u);
  EXPECT_EQ(plan_audio(p)[0].audio_stream, 2);
}

TEST(PlanAudio, RetimingIsCarriedRatherThanApplied) {
  // The mixer needs the rate to resample with; baking it into the span here
  // would lose the information it needs.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 8.0)})});
  p.sequence().tracks[0].clips[0].speed = 2.0;
  p.sequence().tracks[0].clips[0].reverse = true;

  const auto planned = plan_audio(p);
  ASSERT_EQ(planned.size(), 1u);
  EXPECT_DOUBLE_EQ(planned[0].speed, 2.0);
  EXPECT_TRUE(planned[0].reverse);
  // Eight seconds of source at double speed occupies four on the timeline.
  EXPECT_DOUBLE_EQ(planned[0].end, 4.0);
}

TEST(PlanAudio, ClipsComeBackInTrackThenStartOrder) {
  const Project p = project({audio_track("a1", {clip("late", "m", 10.0, 2.0),
                                                clip("early", "m", 1.0, 2.0)}),
                             audio_track("a2", {clip("other", "m", 0.0, 2.0)})});
  const auto planned = plan_audio(p);

  ASSERT_EQ(planned.size(), 3u);
  EXPECT_EQ(planned[0].clip->id, "early");
  EXPECT_EQ(planned[1].clip->id, "late");
  EXPECT_EQ(planned[2].clip->id, "other");
  EXPECT_EQ(planned[0].track_index, 0);
  EXPECT_EQ(planned[2].track_index, 1);
}

TEST(PlanAudio, OverlappingClipsAreBothPlanned) {
  // Unlike video, audio on separate tracks sums rather than occluding, so
  // nothing is dropped for being underneath something else.
  const Project p = project({audio_track("a1", {clip("c1", "m", 0.0, 5.0)}),
                             audio_track("a2", {clip("c2", "m", 2.0, 5.0)})});
  EXPECT_EQ(plan_audio(p).size(), 2u);
}

// ------------------------------------------------------------------- gain --

[[nodiscard]] PlannedAudioClip only(const Project& p) {
  const auto planned = plan_audio(p);
  EXPECT_EQ(planned.size(), 1u);
  return planned.at(0);
}

TEST(AudioGain, UnityByDefault) {
  const Project p = project({audio_track("a1", {clip("c", "m", 2.0, 5.0)})});
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 4.0), 1.0);
}

TEST(AudioGain, ConstantGainApplies) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].clips[0].gain = 0.5;
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 2.0), 0.5);
}

TEST(AudioGain, OutsideTheClipIsSilent) {
  const Project p = project({audio_track("a1", {clip("c", "m", 2.0, 5.0)})});
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 1.0), 0.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 8.0), 0.0);
}

TEST(AudioGain, AFadeInRampsFromSilence) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.sequence().tracks[0].clips[0].fade_in = 2.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 0.0), 0.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 1.0), 0.5);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 2.0), 1.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 5.0), 1.0);
}

TEST(AudioGain, AFadeOutRampsToSilence) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.sequence().tracks[0].clips[0].fade_out = 4.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 5.0), 1.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 8.0), 0.5);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 10.0), 0.0);
}

TEST(AudioGain, FadesAreRelativeToTheClipNotTheTimeline) {
  Project p = project({audio_track("a1", {clip("c", "m", 30.0, 10.0)})});
  p.sequence().tracks[0].clips[0].fade_in = 2.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 30.0), 0.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 31.0), 0.5);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 32.0), 1.0);
}

TEST(AudioGain, FadesMultiplyTheClipGainRatherThanReplacingIt) {
  // A clip both faded in and turned down should end up quiet, not merely one of
  // the two. This mirrors how `segment_alpha` treats video opacity.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.sequence().tracks[0].clips[0].gain = 0.5;
  p.sequence().tracks[0].clips[0].fade_in = 2.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 1.0), 0.25);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 5.0), 0.5);
}

TEST(AudioGain, OverlappingFadesStillReachSilenceAtBothEnds) {
  // Fades longer than half the clip overlap in the middle; the result should
  // stay sane rather than exceeding unity or going negative.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 4.0)})});
  p.sequence().tracks[0].clips[0].fade_in = 3.0;
  p.sequence().tracks[0].clips[0].fade_out = 3.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 0.0), 0.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 4.0), 0.0);
  for (double t = 0.0; t <= 4.0; t += 0.25) {
    const double gain = audio_gain_at(only(p), t);
    EXPECT_GE(gain, 0.0) << "at " << t;
    EXPECT_LE(gain, 1.0) << "at " << t;
  }
}

TEST(AudioGain, AutomationOverridesTheConstantGain) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.sequence().tracks[0].clips[0].gain = 1.0;
  p = core::set_gain_keyframe(std::move(p), "c", 0.0, 0.0);
  p = core::set_gain_keyframe(std::move(p), "c", 10.0, 1.0);

  EXPECT_NEAR(audio_gain_at(only(p), 0.0), 0.0, 1e-9);
  EXPECT_NEAR(audio_gain_at(only(p), 5.0), 0.5, 1e-9);
  EXPECT_NEAR(audio_gain_at(only(p), 10.0), 1.0, 1e-9);
}

TEST(AudioGain, AutomationAndFadesCombine) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.sequence().tracks[0].clips[0].fade_out = 2.0;
  p = core::set_gain_keyframe(std::move(p), "c", 0.0, 0.5);
  p = core::set_gain_keyframe(std::move(p), "c", 10.0, 0.5);

  EXPECT_NEAR(audio_gain_at(only(p), 5.0), 0.5, 1e-9);
  EXPECT_NEAR(audio_gain_at(only(p), 9.0), 0.25, 1e-9);
}

// -------------------------------------------------------------------- pan --
//
// Balance, not constant power: one side is let through less and the other is
// left alone. The centre being exactly unity on both sides is the property that
// matters most — it is what makes every project written before there was a
// panner sound the same to the sample.

TEST(AudioPan, CentredIsUnityOnBothSides) {
  const Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  EXPECT_EQ(audio_pan_at(only(p), 2.0), (StereoGain{.left = 1.0, .right = 1.0}));
}

TEST(AudioPan, HardLeftSilencesTheRightAndLeavesTheLeftAlone) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].clips[0].pan = -1.0;
  const StereoGain gain = audio_pan_at(only(p), 2.0);
  EXPECT_DOUBLE_EQ(gain.left, 1.0) << "no boost, ever";
  EXPECT_DOUBLE_EQ(gain.right, 0.0);
}

TEST(AudioPan, HalfRightTrimsTheLeftByHalf) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].clips[0].pan = 0.5;
  const StereoGain gain = audio_pan_at(only(p), 2.0);
  EXPECT_DOUBLE_EQ(gain.left, 0.5);
  EXPECT_DOUBLE_EQ(gain.right, 1.0);
}

TEST(AudioPan, IsAutomatedInClipLocalTime) {
  Project p = project({audio_track("a1", {clip("c", "m", 10.0, 10.0)})});
  p = core::set_keyframe(std::move(p), "c", core::AnimProp::Pan, 0.0, -1.0);
  p = core::set_keyframe(std::move(p), "c", core::AnimProp::Pan, 10.0, 1.0);

  // Timeline 15 is local 5, which is the middle of the sweep.
  EXPECT_NEAR(audio_pan_at(only(p), 10.0).right, 0.0, 1e-9);
  EXPECT_NEAR(audio_pan_at(only(p), 15.0).left, 1.0, 1e-9);
  EXPECT_NEAR(audio_pan_at(only(p), 15.0).right, 1.0, 1e-9);
  EXPECT_NEAR(audio_pan_at(only(p), 20.0).left, 0.0, 1e-9);
}

TEST(AudioPan, DoesNotFade) {
  // The fades belong to the gain. A clip fading out does not also drift across
  // the image, which is what multiplying the ramp in here would do.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.sequence().tracks[0].clips[0].fade_out = 2.0;
  p.sequence().tracks[0].clips[0].pan = -0.5;
  EXPECT_EQ(audio_pan_at(only(p), 2.0), audio_pan_at(only(p), 9.0));
}

// ------------------------------------------------------------ source time --

TEST(AudioSourceTime, TracksTheTimelineThroughTheTrim) {
  const Project p = project({audio_track("a1", {clip("c", "m", 10.0, 5.0, 30.0)})});
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), 10.0), 30.0);
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), 12.5), 32.5);
}

TEST(AudioSourceTime, SpeedScalesHowFastTheSourceIsConsumed) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.sequence().tracks[0].clips[0].speed = 2.0;
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), 2.0), 4.0);
}

TEST(AudioSourceTime, StaysInsideTheTrim) {
  // A rounding error at a boundary must not pull in audio from beyond the trim.
  const Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0, 20.0)})});
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), -1.0), 20.0);
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), 99.0), 25.0);
}


// ----------------------------------------------------------- track mixer --
//
// A track's own fader and panner, applied to everything on it after each clip's
// own. Two levels, because a clip's gain is what that take needed and a track's
// is what the stem needs against the others.

TEST(TrackMixer, TheTrackFaderMultipliesTheClipsOwn) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].gain = 0.5;
  p.sequence().tracks[0].clips[0].gain = 0.5;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 2.0), 0.25);
}

TEST(TrackMixer, AUnityTrackChangesNothing) {
  const Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 2.0), 1.0);
}

TEST(TrackMixer, TheTrackPannerAppliesAfterTheClipsOwn) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].pan = 0.5;  // half right

  const StereoGain gain = audio_pan_at(only(p), 2.0);
  EXPECT_DOUBLE_EQ(gain.left, 0.5);
  EXPECT_DOUBLE_EQ(gain.right, 1.0);
}

TEST(TrackMixer, OppositePansCancelToSilenceRatherThanToCentre) {
  // The property that decides how the two combine. Multiplied, a clip hard left
  // on a track panned hard right is silent, which is what two balances in
  // series do; added, the two pans would cancel and it would play centred,
  // which is not what either control was asked for.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].clips[0].pan = -1.0;
  p.sequence().tracks[0].pan = 1.0;

  const StereoGain gain = audio_pan_at(only(p), 2.0);
  EXPECT_DOUBLE_EQ(gain.left, 0.0);
  EXPECT_DOUBLE_EQ(gain.right, 0.0);
}

TEST(TrackMixer, ATrackPanIsCarriedOnEveryClipThatCameOffIt) {
  // Copied into the plan rather than looked up, so the mixer never reaches back
  // into a project that may have moved on.
  Project p = project({audio_track("a1", {clip("c1", "m", 0.0, 2.0),
                                          clip("c2", "m", 3.0, 2.0)})});
  p.sequence().tracks[0].pan = -1.0;

  const auto planned = plan_audio(p);
  ASSERT_EQ(planned.size(), 2u);
  EXPECT_DOUBLE_EQ(planned[0].track_pan, -1.0);
  EXPECT_DOUBLE_EQ(planned[1].track_pan, -1.0);
}

TEST(TrackMixer, ANonsenseTrackMixIsClampedRatherThanTrusted) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.sequence().tracks[0].gain = 100.0;
  p.sequence().tracks[0].pan = -8.0;

  const auto planned = plan_audio(p);
  ASSERT_EQ(planned.size(), 1u);
  EXPECT_DOUBLE_EQ(planned[0].track_gain, core::kMaxGain);
  EXPECT_DOUBLE_EQ(planned[0].track_pan, -1.0);
}


// ------------------------------------------------------------- crossfades --

/// Two clips meeting at 5 s, each with ten seconds of file either side to
/// lend, the first carrying an audio transition into the second.
[[nodiscard]] Project crossfaded(core::TransitionKind kind, double length = 2.0) {
  Clip first = clip("a", "m", 0.0, 5.0, 10.0);
  first.transition_out = core::Transition{.kind = kind, .duration = length};
  return project({audio_track("a1", {first, clip("b", "m", 5.0, 5.0, 30.0)})});
}

TEST(Crossfade, BothClipsPlayThroughTheOverlapFromTheirHandles) {
  const auto planned = plan_audio(crossfaded(core::TransitionKind::ConstantPower));
  ASSERT_EQ(planned.size(), 2u);
  // Half the transition each side of the cut.
  EXPECT_DOUBLE_EQ(planned[0].end, 6.0);
  EXPECT_DOUBLE_EQ(planned[0].source_out, 16.0);
  EXPECT_DOUBLE_EQ(planned[1].start, 4.0);
  EXPECT_DOUBLE_EQ(planned[1].source_in, 29.0);
  // And each still knows where it really starts and ends.
  EXPECT_DOUBLE_EQ(planned[0].clip_end, 5.0);
  EXPECT_DOUBLE_EQ(planned[1].clip_start, 5.0);
  // Read from the right part of the file throughout the overlap.
  EXPECT_DOUBLE_EQ(audio_source_time_at(planned[0], 5.5), 15.5);
  EXPECT_DOUBLE_EQ(audio_source_time_at(planned[1], 4.5), 29.5);
}

TEST(Crossfade, ConstantPowerHoldsTheLoudnessThroughTheCut) {
  const auto planned = plan_audio(crossfaded(core::TransitionKind::ConstantPower));
  for (const double t : {4.0, 4.5, 5.0, 5.5, 6.0}) {
    const double out = clip_gain_at(planned[0], t);
    const double in = clip_gain_at(planned[1], t);
    EXPECT_NEAR(out * out + in * in, 1.0, 1e-9) << t;
  }
  EXPECT_NEAR(clip_gain_at(planned[0], 5.0), std::sqrt(0.5), 1e-9);
  EXPECT_NEAR(clip_gain_at(planned[1], 4.0), 0.0, 1e-9);
  EXPECT_NEAR(clip_gain_at(planned[0], 6.0), 0.0, 1e-9);
  // Untouched outside the transition.
  EXPECT_DOUBLE_EQ(clip_gain_at(planned[0], 3.0), 1.0);
}

TEST(Crossfade, ConstantGainAddsUpToOne) {
  const auto planned = plan_audio(crossfaded(core::TransitionKind::ConstantGain));
  for (const double t : {4.25, 5.0, 5.75}) {
    EXPECT_NEAR(clip_gain_at(planned[0], t) + clip_gain_at(planned[1], t), 1.0, 1e-9) << t;
  }
}

TEST(Crossfade, AnExponentialFadeIsEvenInDecibels) {
  // A quarter of the way, a quarter of sixty decibels down from full.
  EXPECT_NEAR(20.0 * std::log10(crossfade_gain(core::TransitionKind::ExponentialFade, 0.75, true)),
              -15.0, 1e-9);
  EXPECT_EQ(crossfade_gain(core::TransitionKind::ExponentialFade, 0.0, true), 0.0);
  EXPECT_EQ(crossfade_gain(core::TransitionKind::ExponentialFade, 0.0, false), 1.0);
}

TEST(Crossfade, AClipsOwnFadeAndKeyframesStayWhereTheyWere) {
  Project p = crossfaded(core::TransitionKind::ConstantPower);
  p.sequence().tracks[0].clips[1].fade_in = 1.0;
  const auto planned = plan_audio(p);
  // Its fade in runs from its own start at 5 s, not from where it is first
  // heard at 4 s: half way through it, and through the crossfade's last half.
  EXPECT_NEAR(clip_gain_at(planned[1], 5.5),
              0.5 * crossfade_gain(core::TransitionKind::ConstantPower, 0.75, true), 1e-9);
}

TEST(Crossfade, OnlyAsLongAsBothSidesCanLend) {
  // The second clip starts half a second into its file.
  Clip first = clip("a", "m", 0.0, 5.0, 10.0);
  first.transition_out =
      core::Transition{.kind = core::TransitionKind::ConstantGain, .duration = 2.0};
  const auto planned =
      plan_audio(project({audio_track("a1", {first, clip("b", "m", 5.0, 5.0, 0.5)})}));
  EXPECT_DOUBLE_EQ(planned[0].end, 5.5);
  EXPECT_DOUBLE_EQ(planned[1].start, 4.5);
}

TEST(Crossfade, APictureTransitionOnSoundAndAGapAreBothIgnored) {
  EXPECT_DOUBLE_EQ(plan_audio(crossfaded(core::TransitionKind::Dissolve))[0].end, 5.0);
  Project gap = crossfaded(core::TransitionKind::ConstantPower);
  gap.sequence().tracks[0].clips[1].start = 6.0;
  EXPECT_DOUBLE_EQ(plan_audio(gap)[0].end, 5.0);
}

}  // namespace
}  // namespace cutline::render
