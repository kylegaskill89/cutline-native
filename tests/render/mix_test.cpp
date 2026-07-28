/// What plays, and how loud — decided from the model alone.

#include "cutline/render/mix.hpp"

#include "cutline/core/animate.hpp"

#include <gtest/gtest.h>

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
  p.tracks = std::move(tracks);
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
  p.tracks.insert(p.tracks.begin(), video);

  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, MutedTracksAreSilent) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.tracks[0].muted = true;
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, SoloSilencesEveryOtherTrack) {
  Project p = project({audio_track("a1", {clip("c1", "m", 0.0, 5.0)}),
                       audio_track("a2", {clip("c2", "m", 0.0, 5.0)})});
  p.tracks[1].solo = true;

  const auto planned = plan_audio(p);
  ASSERT_EQ(planned.size(), 1u);
  EXPECT_EQ(planned[0].clip->id, "c2");
}

TEST(PlanAudio, ASoloedTrackThatIsAlsoMutedStaysSilent) {
  // Mute wins: it is the more explicit statement of the two.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.tracks[0].solo = true;
  p.tracks[0].muted = true;
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, DisabledClipsAreSkipped) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.tracks[0].clips[0].disabled = true;
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, AClipPinnedToSilenceIsSkipped) {
  // Decoding it would be work with no audible result.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.tracks[0].clips[0].gain = 0.0;
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
  title.is_text = true;
  Project p = project({audio_track("a1", {clip("c", "t", 0.0, 5.0)})}, {title});
  EXPECT_TRUE(plan_audio(p).empty());
}

TEST(PlanAudio, TheStreamOrdinalIsCarried) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0)})});
  p.tracks[0].clips[0].audio_stream = 2;
  ASSERT_EQ(plan_audio(p).size(), 1u);
  EXPECT_EQ(plan_audio(p)[0].audio_stream, 2);
}

TEST(PlanAudio, RetimingIsCarriedRatherThanApplied) {
  // The mixer needs the rate to resample with; baking it into the span here
  // would lose the information it needs.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 8.0)})});
  p.tracks[0].clips[0].speed = 2.0;
  p.tracks[0].clips[0].reverse = true;

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
  p.tracks[0].clips[0].gain = 0.5;
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 2.0), 0.5);
}

TEST(AudioGain, OutsideTheClipIsSilent) {
  const Project p = project({audio_track("a1", {clip("c", "m", 2.0, 5.0)})});
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 1.0), 0.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 8.0), 0.0);
}

TEST(AudioGain, AFadeInRampsFromSilence) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.tracks[0].clips[0].fade_in = 2.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 0.0), 0.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 1.0), 0.5);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 2.0), 1.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 5.0), 1.0);
}

TEST(AudioGain, AFadeOutRampsToSilence) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.tracks[0].clips[0].fade_out = 4.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 5.0), 1.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 8.0), 0.5);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 10.0), 0.0);
}

TEST(AudioGain, FadesAreRelativeToTheClipNotTheTimeline) {
  Project p = project({audio_track("a1", {clip("c", "m", 30.0, 10.0)})});
  p.tracks[0].clips[0].fade_in = 2.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 30.0), 0.0);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 31.0), 0.5);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 32.0), 1.0);
}

TEST(AudioGain, FadesMultiplyTheClipGainRatherThanReplacingIt) {
  // A clip both faded in and turned down should end up quiet, not merely one of
  // the two. This mirrors how `segment_alpha` treats video opacity.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.tracks[0].clips[0].gain = 0.5;
  p.tracks[0].clips[0].fade_in = 2.0;

  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 1.0), 0.25);
  EXPECT_DOUBLE_EQ(audio_gain_at(only(p), 5.0), 0.5);
}

TEST(AudioGain, OverlappingFadesStillReachSilenceAtBothEnds) {
  // Fades longer than half the clip overlap in the middle; the result should
  // stay sane rather than exceeding unity or going negative.
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 4.0)})});
  p.tracks[0].clips[0].fade_in = 3.0;
  p.tracks[0].clips[0].fade_out = 3.0;

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
  p.tracks[0].clips[0].gain = 1.0;
  p = core::set_gain_keyframe(std::move(p), "c", 0.0, 0.0);
  p = core::set_gain_keyframe(std::move(p), "c", 10.0, 1.0);

  EXPECT_NEAR(audio_gain_at(only(p), 0.0), 0.0, 1e-9);
  EXPECT_NEAR(audio_gain_at(only(p), 5.0), 0.5, 1e-9);
  EXPECT_NEAR(audio_gain_at(only(p), 10.0), 1.0, 1e-9);
}

TEST(AudioGain, AutomationAndFadesCombine) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.tracks[0].clips[0].fade_out = 2.0;
  p = core::set_gain_keyframe(std::move(p), "c", 0.0, 0.5);
  p = core::set_gain_keyframe(std::move(p), "c", 10.0, 0.5);

  EXPECT_NEAR(audio_gain_at(only(p), 5.0), 0.5, 1e-9);
  EXPECT_NEAR(audio_gain_at(only(p), 9.0), 0.25, 1e-9);
}

// ------------------------------------------------------------ source time --

TEST(AudioSourceTime, TracksTheTimelineThroughTheTrim) {
  const Project p = project({audio_track("a1", {clip("c", "m", 10.0, 5.0, 30.0)})});
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), 10.0), 30.0);
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), 12.5), 32.5);
}

TEST(AudioSourceTime, SpeedScalesHowFastTheSourceIsConsumed) {
  Project p = project({audio_track("a1", {clip("c", "m", 0.0, 10.0)})});
  p.tracks[0].clips[0].speed = 2.0;
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), 2.0), 4.0);
}

TEST(AudioSourceTime, StaysInsideTheTrim) {
  // A rounding error at a boundary must not pull in audio from beyond the trim.
  const Project p = project({audio_track("a1", {clip("c", "m", 0.0, 5.0, 20.0)})});
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), -1.0), 20.0);
  EXPECT_DOUBLE_EQ(audio_source_time_at(only(p), 99.0), 25.0);
}

}  // namespace
}  // namespace cutline::render
