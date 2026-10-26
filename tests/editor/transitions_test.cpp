/// Transitions, as the panel sees them.
///
/// The model already renders all four kinds. What is tested here is the part
/// the interface needs and the model does not provide: whether a transition
/// would do anything at all, and how long it could be if it did.
///
/// That question is not idle. A dissolve overlaps the two clips, and overlapping
/// means borrowing unused source from each side. A clip trimmed to the last
/// frame of its footage has none to lend, and the resolver quietly skips it —
/// so a panel that offered the control anyway would move a slider and change
/// no pixels.

#include "cutline/editor/transitions.hpp"

#include "cutline/editor/commands.hpp"
#include "cutline/editor/effects_binding.hpp"
#include "cutline/editor/session.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/segments.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;
using core::TransitionKind;

/// Two abutting clips with plenty of handle either side: the media runs sixty
/// seconds and each clip uses five of the middle of it.
[[nodiscard]] Project abutting() {
  Project project;
  project.sequence().fps = 30.0;
  project.media.push_back(
      Media{.id = "m1", .name = "wide.mp4", .duration = 60.0, .has_video = true});

  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {
      Clip{.id = "a", .media_id = "m1", .source_in = 10.0, .source_out = 15.0, .start = 0.0},
      Clip{.id = "b", .media_id = "m1", .source_in = 30.0, .source_out = 35.0, .start = 5.0},
  };
  project.sequence().tracks.push_back(std::move(video));
  return project;
}

/// The same, with a gap between the two.
[[nodiscard]] Project with_a_gap() {
  Project project = abutting();
  project.sequence().tracks[0].clips[1].start = 8.0;
  return project;
}

/// Abutting, but each clip is trimmed to the very ends of its footage, so
/// neither has anything to lend.
[[nodiscard]] Project without_handles() {
  Project project;
  project.sequence().fps = 30.0;
  project.media.push_back(Media{.id = "m1", .duration = 10.0, .has_video = true});
  project.media.push_back(Media{.id = "m2", .duration = 10.0, .has_video = true});

  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {
      // Runs to the last frame of m1: no tail.
      Clip{.id = "a", .media_id = "m1", .source_in = 5.0, .source_out = 10.0, .start = 0.0},
      // Starts at the first frame of m2: no head.
      Clip{.id = "b", .media_id = "m2", .source_in = 0.0, .source_out = 5.0, .start = 5.0},
  };
  project.sequence().tracks.push_back(std::move(video));
  return project;
}

// ---------------------------------------------------------------- naming --

TEST(Transitions, EveryKindIsNamedAsPremiereNamesIt) {
  EXPECT_EQ(transition_name(TransitionKind::Dissolve), "Cross Dissolve");
  EXPECT_EQ(transition_name(TransitionKind::DipBlack), "Dip to Black");
  EXPECT_EQ(transition_name(TransitionKind::Push), "Push");
  EXPECT_EQ(transition_name(TransitionKind::Slide), "Slide");
}

// ------------------------------------------------------------- the join --

TEST(Transitions, AClipWithAnotherAfterItHasAJoin) {
  EXPECT_TRUE(clip_transition(abutting(), "a").joins);
}

TEST(Transitions, TheLastClipOnATrackHasNone) {
  // The model would keep a transition there and the renderer would ignore it.
  const TransitionRow row = clip_transition(abutting(), "b");
  EXPECT_FALSE(row.joins);
  EXPECT_DOUBLE_EQ(row.longest, 0.0);
}

TEST(Transitions, AGapIsNotAJoin) {
  EXPECT_FALSE(clip_transition(with_a_gap(), "a").joins);
  EXPECT_DOUBLE_EQ(longest_transition(with_a_gap(), "a", TransitionKind::Dissolve), 0.0);
}

TEST(Transitions, AClipThatIsNotThereHasNothing) {
  EXPECT_EQ(clip_transition(abutting(), "ghost"), TransitionRow{});
}

// ------------------------------------------------------------ how long --

TEST(Transitions, IsBoundedByTheClipsThemselves) {
  // Half sits either side of the cut, and neither half may swallow its clip.
  // Both clips are five seconds, so ten is the ceiling.
  const Project project = abutting();
  EXPECT_DOUBLE_EQ(longest_transition(project, "a", TransitionKind::DipBlack), 10.0);
}

TEST(Transitions, AnOverlappingKindIsBoundedByTheHandlesToo) {
  // The clips use 10 to 15 and 30 to 35 of a sixty-second media, so there is
  // far more handle than clip and the clips are the tighter limit.
  const Project project = abutting();
  EXPECT_DOUBLE_EQ(longest_transition(project, "a", TransitionKind::Dissolve), 10.0);

  // Now trim the handles to half a second each, and they become the limit.
  Project tight = project;
  tight.media[0].duration = 15.5;                  // half a second past a's out
  tight.sequence().tracks[0].clips[1].source_in = 0.5;        // half a second of head on b
  tight.sequence().tracks[0].clips[1].source_out = 5.5;
  EXPECT_DOUBLE_EQ(longest_transition(tight, "a", TransitionKind::Dissolve), 1.0);
}

TEST(Transitions, WithNoHandlesOnlyDipToBlackIsPossible) {
  // The whole reason this layer exists. A dissolve here resolves to nothing at
  // all, and the resolver skips it in silence.
  const Project project = without_handles();
  EXPECT_DOUBLE_EQ(longest_transition(project, "a", TransitionKind::Dissolve), 0.0);
  EXPECT_DOUBLE_EQ(longest_transition(project, "a", TransitionKind::Push), 0.0);
  EXPECT_GT(longest_transition(project, "a", TransitionKind::DipBlack), 0.0);

  const TransitionRow row = clip_transition(project, "a");
  EXPECT_TRUE(row.joins);
  EXPECT_TRUE(row.handles_exhausted);
}

TEST(Transitions, AStillHasAllTheHandleAnyoneCouldWant) {
  // A title has no source to run out of, so refusing a dissolve onto one would
  // be an arbitrary rule about generated media.
  Project project = abutting();
  project.media.push_back(Media{.id = "t1", .has_video = true, .is_graphic = true});
  project.sequence().tracks[0].clips[1].media_id = "t1";

  EXPECT_FALSE(clip_transition(project, "a").handles_exhausted);
  EXPECT_GT(longest_transition(project, "a", TransitionKind::Dissolve), 0.0);
}

// -------------------------------------------------------------- setting --

TEST(Transitions, SettingOneStoresIt) {
  const Project after = set_transition(abutting(), "a", TransitionKind::Dissolve, 2.0);
  const core::Clip* clip = core::find_clip(after, "a");

  ASSERT_TRUE(clip->transition_out.has_value());
  EXPECT_EQ(clip->transition_out->kind, TransitionKind::Dissolve);
  EXPECT_DOUBLE_EQ(clip->transition_out->duration, 2.0);
}

TEST(Transitions, TooLongIsClampedRatherThanRefused) {
  // The way a trim stops at the end of its source instead of snapping back.
  const Project after = set_transition(abutting(), "a", TransitionKind::Dissolve, 500.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "a")->transition_out->duration, 10.0);
}

TEST(Transitions, AKindTheJoinCannotManageClearsInstead) {
  // Storing a transition the renderer would skip is worse than storing none:
  // the timeline would draw it and the picture would not do it.
  const Project after = set_transition(without_handles(), "a", TransitionKind::Dissolve, 1.0);
  EXPECT_FALSE(core::find_clip(after, "a")->transition_out.has_value());
}

TEST(Transitions, NoneClearsIt) {
  const Project on = set_transition(abutting(), "a", TransitionKind::Push, 1.0);
  ASSERT_TRUE(core::find_clip(on, "a")->transition_out.has_value());

  const Project off = set_transition(on, "a", std::nullopt, 0.0);
  EXPECT_FALSE(core::find_clip(off, "a")->transition_out.has_value());
}

TEST(Transitions, ZeroLengthIsTheSameAsNone) {
  // A transition of no duration is a cut, which the model already says.
  const Project after = set_transition(abutting(), "a", TransitionKind::Dissolve, 0.0);
  EXPECT_FALSE(core::find_clip(after, "a")->transition_out.has_value());
}

TEST(Transitions, ANewOneIsASecondOrAsMuchAsFits) {
  EXPECT_DOUBLE_EQ(default_transition_length(abutting(), "a", TransitionKind::Dissolve), 1.0);

  // A join with only half a second of handle gets half a second.
  Project tight = abutting();
  tight.media[0].duration = 15.25;
  tight.sequence().tracks[0].clips[1].source_in = 0.25;
  tight.sequence().tracks[0].clips[1].source_out = 5.25;
  EXPECT_DOUBLE_EQ(default_transition_length(tight, "a", TransitionKind::Dissolve), 0.5);
}

// ------------------------------------------------------- and it renders --

TEST(Transitions, WhatIsSetIsWhatTheResolverActuallyDoes) {
  // The loop closed. Everything above is about describing a transition; this
  // checks the description was not a fiction, by asking the model's own
  // resolver what the picture does.
  const Project project = set_transition(abutting(), "a", TransitionKind::Dissolve, 2.0);
  const std::vector<core::VideoSeg> segs = core::resolve_video_segments(
      project.sequence().tracks[0], [&project](std::string_view id) {
        const auto media = std::ranges::find(project.media, id, &core::Media::id);
        return media == project.media.end() ? 0.0 : media->duration;
      });

  ASSERT_EQ(segs.size(), 2u);
  EXPECT_GT(segs[1].x_in, 0.0) << "the incoming clip should be fading in";
  EXPECT_LT(segs[1].start, 5.0) << "and starting early, having borrowed a handle";
}

TEST(Transitions, WhatTheLayerRefusesIsWhatTheResolverWouldHaveSkipped) {
  // The agreement that matters: this says a dissolve is impossible here, and
  // the resolver — asked to do one anyway — does nothing.
  Project forced = without_handles();
  forced.sequence().tracks[0].clips[0].transition_out =
      core::Transition{.kind = TransitionKind::Dissolve, .duration = 1.0};

  const std::vector<core::VideoSeg> segs = core::resolve_video_segments(
      forced.sequence().tracks[0], [&forced](std::string_view id) {
        const auto media = std::ranges::find(forced.media, id, &core::Media::id);
        return media == forced.media.end() ? 0.0 : media->duration;
      });

  ASSERT_EQ(segs.size(), 2u);
  EXPECT_DOUBLE_EQ(segs[1].x_in, 0.0);
  EXPECT_DOUBLE_EQ(segs[1].start, 5.0) << "nothing was borrowed, so nothing moved";
  EXPECT_DOUBLE_EQ(longest_transition(forced, "a", TransitionKind::Dissolve), 0.0);
}


// ------------------------------------------------------------------ audio --

/// The same two clips as `abutting`, on an audio track.
[[nodiscard]] Project abutting_sound() {
  Project project = abutting();
  project.media[0].audio_stream_count = 1;
  Track& track = project.sequence().tracks[0];
  track.kind = TrackKind::Audio;
  for (Clip& clip : track.clips) clip.kind = TrackKind::Audio;
  return project;
}

TEST(AudioTransitions, EachTrackOffersItsOwnKindsWithPremieresDefaultFirst) {
  const auto video = transition_kinds_for(TrackKind::Video);
  const auto audio = transition_kinds_for(TrackKind::Audio);
  ASSERT_EQ(video.size(), 4u);
  ASSERT_EQ(audio.size(), 3u);
  EXPECT_EQ(audio[0], TransitionKind::ConstantPower);
  EXPECT_EQ(default_transition_kind(TrackKind::Audio), TransitionKind::ConstantPower);
  EXPECT_EQ(default_transition_kind(TrackKind::Video), TransitionKind::Dissolve);
  for (const TransitionKind kind : audio) EXPECT_TRUE(core::is_audio_transition(kind));
  EXPECT_EQ(transition_name(TransitionKind::ExponentialFade), "Exponential Fade");
  EXPECT_EQ(transition_from_id("constant-gain"), TransitionKind::ConstantGain);
}

TEST(AudioTransitions, TheSoundsKindsGoOnSoundAndThePicturesOnPictures) {
  EXPECT_GT(longest_transition(abutting_sound(), "a", TransitionKind::ConstantPower), 0.0);
  EXPECT_EQ(longest_transition(abutting_sound(), "a", TransitionKind::Dissolve), 0.0);
  EXPECT_EQ(longest_transition(abutting(), "a", TransitionKind::ConstantPower), 0.0);
}

TEST(AudioTransitions, ACrossfadeNeedsBothSidesToLend) {
  // Five seconds of tail on the first and fifteen of head on the second: the
  // smaller decides, unlike a dissolve, which can hold a frame.
  Project p = abutting_sound();
  p.sequence().tracks[0].clips[0].source_in = 50.0;
  p.sequence().tracks[0].clips[0].source_out = 55.0;
  EXPECT_DOUBLE_EQ(longest_transition(p, "a", TransitionKind::ConstantGain), 10.0);
}

TEST(AudioTransitions, EachKindIsPutDownAtItsOwnPreferredLength) {
  const TransitionLengths lengths{.video = 0.5, .audio = 2.0};
  EXPECT_DOUBLE_EQ(lengths.for_kind(TransitionKind::Dissolve), 0.5);
  EXPECT_DOUBLE_EQ(lengths.for_kind(TransitionKind::ConstantPower), 2.0);
  const Project p = apply_library_entry(abutting_sound(), "a", "transition:constant-power",
                                        lengths);
  const auto row = clip_transition(p, "a");
  EXPECT_TRUE(row.present);
  EXPECT_DOUBLE_EQ(row.duration, 2.0);
}

TEST(AudioTransitions, TheLibraryFilesThemUnderAudioTransitions) {
  bool found = false;
  for (const LibraryEntry& entry : effect_library()) {
    if (entry.id == "transition:constant-power") {
      EXPECT_EQ(entry.folder, "Audio Transitions");
      found = true;
    }
    if (entry.id == "transition:dissolve") EXPECT_EQ(entry.folder, "Video Transitions");
  }
  EXPECT_TRUE(found);
}

TEST(ApplyTransition, TheNearestCutOnEachTargetedTrackGetsTheDefault) {
  Project p = abutting();
  p.sequence().tracks[0].targeted = true;
  p = apply_transition_at(std::move(p), 4.2, TrackKind::Video, {.video = 1.5});
  const auto row = clip_transition(p, "a");
  ASSERT_TRUE(row.present);
  EXPECT_EQ(row.kind, TransitionKind::Dissolve);
  EXPECT_DOUBLE_EQ(row.duration, 1.5);
  // Video's key leaves sound alone, and a track not targeted is not touched.
  EXPECT_FALSE(clip_transition(apply_transition_at(abutting_sound(), 4.2, TrackKind::Video, {}),
                               "a")
                   .present);
  EXPECT_FALSE(clip_transition(apply_transition_at(abutting(), 4.2, TrackKind::Video, {}), "a")
                   .present);
}

TEST(ApplyTransition, OneAlreadyThereIsKept) {
  Project p = set_transition(abutting(), "a", TransitionKind::Push, 0.5);
  p.sequence().tracks[0].targeted = true;
  p = apply_transition_at(std::move(p), 5.0, TrackKind::Video, {});
  EXPECT_EQ(clip_transition(p, "a").kind, TransitionKind::Push);
}

TEST(ApplyTransition, TheCommandsUseTheLengthsTheSessionWasGiven) {
  Project p = abutting_sound();
  p.sequence().tracks[0].targeted = true;
  Session session(std::move(p));
  session.set_playhead(5.0);
  session.set_transition_lengths({.video = 1.0, .audio = 0.25});
  EXPECT_FALSE(can_run(session, Command::ApplyVideoTransition));
  ASSERT_TRUE(can_run(session, Command::ApplyAudioTransition));
  EXPECT_TRUE(run(session, Command::ApplyAudioTransition));
  EXPECT_DOUBLE_EQ(clip_transition(session.project(), "a").duration, 0.25);
  // And once it is there, there is nothing left for the key to do.
  EXPECT_FALSE(can_run(session, Command::ApplyAudioTransition));
}

TEST(ApplyTransition, DefaultsToSelectionGoAtBothEndsOfEachClip) {
  // Three clips in a row; selecting the middle one puts transitions at both
  // of its cuts.
  Project p = abutting_sound();
  Clip third = p.sequence().tracks[0].clips[1];
  third.id = "c";
  third.start = 10.0;
  third.source_in = 40.0;
  third.source_out = 45.0;
  p.sequence().tracks[0].clips.push_back(third);
  const std::vector<std::string> selected{"b"};
  p = apply_default_transitions(std::move(p), selected, {});
  EXPECT_EQ(clip_transition(p, "a").kind, TransitionKind::ConstantPower);
  EXPECT_TRUE(clip_transition(p, "a").present);
  EXPECT_TRUE(clip_transition(p, "b").present);
}

}  // namespace
}  // namespace cutline::editor
