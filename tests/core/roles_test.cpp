#include "cutline/core/roles.hpp"

#include "cutline/core/query.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace cutline::core {
namespace {

/// A sequence with two audio tracks: dialogue on the first, music on the
/// second.
Project sequence() {
  Project p;
  for (const char* id : {"a1", "a2"}) {
    Track t;
    t.id = id;
    t.kind = TrackKind::Audio;
    p.tracks.push_back(t);
  }
  return p;
}

Clip audio_clip(std::string id, double start, double length, AudioRole role) {
  Clip c;
  c.id = std::move(id);
  c.media_id = "m1";
  c.kind = TrackKind::Audio;
  c.source_in = 0.0;
  c.source_out = length;
  c.start = start;
  c.role = role;
  return c;
}

/// The gain the curve on a clip comes to at clip-local time `t`.
double curve_at(const Project& p, std::string_view clip_id, double t) {
  const Clip* c = find_clip(p, clip_id);
  return c == nullptr ? 0.0 : eval_keyframes(c->gain_keyframes, t);
}

// ------------------------------------------------------------------ names --

TEST(RoleName, EveryRoleHasOneAndNoneIsNotBlank) {
  EXPECT_EQ(role_name(AudioRole::None), "None") << "a blank row in a menu reads as a broken one";
  EXPECT_EQ(role_name(AudioRole::Dialogue), "Dialogue");
  EXPECT_EQ(role_name(AudioRole::Music), "Music");
  EXPECT_EQ(role_name(AudioRole::Effects), "SFX") << "Premiere's name for it";
  EXPECT_EQ(role_name(AudioRole::Ambience), "Ambience");
  EXPECT_EQ(kAudioRoles.size(), 5u);
  EXPECT_EQ(kAudioRoles.front(), AudioRole::None) << "what a clip starts as comes first";
}

// -------------------------------------------------------------- assigning --

TEST(SetClipRole, ItIsSetOnTheClip) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("c1", 0.0, 4.0, AudioRole::None));
  p = set_clip_role(std::move(p), "c1", AudioRole::Dialogue);
  EXPECT_EQ(find_clip(p, "c1")->role, AudioRole::Dialogue);
}

TEST(SetClipRole, ThePictureHalfOfAPairHasNoRole) {
  Project p = sequence();
  Clip video = audio_clip("v", 0.0, 4.0, AudioRole::None);
  video.kind = TrackKind::Video;
  p.tracks[0].clips.push_back(video);

  p = set_clip_role(std::move(p), "v", AudioRole::Dialogue);
  EXPECT_EQ(find_clip(p, "v")->role, AudioRole::None) << "a role is about sound";
}

TEST(SetTrackRole, ItReachesEveryClipOnTheLane) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("c1", 0.0, 2.0, AudioRole::None));
  p.tracks[0].clips.push_back(audio_clip("c2", 4.0, 2.0, AudioRole::None));
  p.tracks[1].clips.push_back(audio_clip("c3", 0.0, 8.0, AudioRole::None));

  p = set_track_role(std::move(p), "a1", AudioRole::Dialogue);
  EXPECT_EQ(find_clip(p, "c1")->role, AudioRole::Dialogue);
  EXPECT_EQ(find_clip(p, "c2")->role, AudioRole::Dialogue);
  EXPECT_EQ(find_clip(p, "c3")->role, AudioRole::None) << "the other lane is untouched";
}

// -------------------------------------------------------------- the spans --

TEST(RoleSpans, TwoOverlappingLinesAreOneStretchOfSpeech) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("c1", 2.0, 4.0, AudioRole::Dialogue));
  p.tracks[1].clips.push_back(audio_clip("c2", 5.0, 4.0, AudioRole::Dialogue));

  const auto spans = role_spans(p, AudioRole::Dialogue);
  ASSERT_EQ(spans.size(), 1u) << "ducking under each separately would pump between the sentences";
  EXPECT_DOUBLE_EQ(spans[0].start, 2.0);
  EXPECT_DOUBLE_EQ(spans[0].end, 9.0);
}

TEST(RoleSpans, ARealGapStaysTwoStretches) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("c1", 0.0, 2.0, AudioRole::Dialogue));
  p.tracks[0].clips.push_back(audio_clip("c2", 10.0, 2.0, AudioRole::Dialogue));

  const auto spans = role_spans(p, AudioRole::Dialogue);
  ASSERT_EQ(spans.size(), 2u);
  EXPECT_DOUBLE_EQ(spans[1].start, 10.0);
}

TEST(RoleSpans, WhatIsNotHeardIsNotCounted) {
  Project p = sequence();
  Clip disabled = audio_clip("c1", 0.0, 2.0, AudioRole::Dialogue);
  disabled.disabled = true;
  p.tracks[0].clips.push_back(disabled);
  p.tracks[1].clips.push_back(audio_clip("c2", 4.0, 2.0, AudioRole::Dialogue));
  p.tracks[1].muted = true;

  EXPECT_TRUE(role_spans(p, AudioRole::Dialogue).empty())
      << "nothing should get out of the way of something nobody hears";
}

TEST(RoleSpans, NoneIsNotARoleToLookFor) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("c1", 0.0, 2.0, AudioRole::None));
  EXPECT_TRUE(role_spans(p, AudioRole::None).empty());
}

// --------------------------------------------------------------- ducking --

/// Dialogue from 4 to 8, music underneath it for the whole twenty seconds.
Project bed_under_speech() {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("speech", 4.0, 4.0, AudioRole::Dialogue));
  p.tracks[1].clips.push_back(audio_clip("music", 0.0, 20.0, AudioRole::Music));
  return p;
}

TEST(Ducking, TheMusicIsDownUnderTheSpeechAndUpEitherSide) {
  const Project p = duck_clip(bed_under_speech(), "music",
                              DuckSettings{.amount_db = -12.0, .fade = 1.0});
  const double ducked = std::pow(10.0, -12.0 / 20.0);

  EXPECT_NEAR(curve_at(p, "music", 0.0), 1.0, 1e-9) << "before the line";
  EXPECT_NEAR(curve_at(p, "music", 5.0), ducked, 1e-9) << "one second in, fully down";
  EXPECT_NEAR(curve_at(p, "music", 7.0), ducked, 1e-9) << "still down at the end of the line";
  EXPECT_NEAR(curve_at(p, "music", 9.0), 1.0, 1e-9) << "back up a second after it";
  EXPECT_NEAR(curve_at(p, "music", 19.0), 1.0, 1e-9);
}

TEST(Ducking, TheRampTakesTheFadeDuration) {
  const Project p =
      duck_clip(bed_under_speech(), "music", DuckSettings{.amount_db = -12.0, .fade = 2.0});
  const double ducked = std::pow(10.0, -12.0 / 20.0);
  const double half = (1.0 + ducked) / 2.0;
  EXPECT_NEAR(curve_at(p, "music", 5.0), half, 1e-9) << "halfway down, one second into a two "
                                                        "second ramp";
  EXPECT_NEAR(curve_at(p, "music", 6.0), ducked, 1e-9);
}

TEST(Ducking, ANegativePositionHasTheMusicAlreadyDownWhenTheWordsLand) {
  const Project p = duck_clip(bed_under_speech(), "music",
                              DuckSettings{.amount_db = -12.0, .fade = 1.0, .position = -1.0});
  const double ducked = std::pow(10.0, -12.0 / 20.0);
  EXPECT_NEAR(curve_at(p, "music", 4.0), ducked, 1e-9) << "down by the time the line starts";
}

TEST(Ducking, TheUnduckedLevelIsTheClipsOwnGain) {
  Project p = bed_under_speech();
  find_clip(p, "music")->gain = 0.5;
  p = duck_clip(std::move(p), "music", DuckSettings{.amount_db = -12.0, .fade = 1.0});

  EXPECT_NEAR(curve_at(p, "music", 0.0), 0.5, 1e-9) << "a clip set quiet stays that quiet";
  EXPECT_NEAR(curve_at(p, "music", 6.0), 0.5 * std::pow(10.0, -12.0 / 20.0), 1e-9);
}

TEST(Ducking, TwoLinesCloseTogetherStayDownBetweenThem) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("one", 4.0, 2.0, AudioRole::Dialogue));
  p.tracks[0].clips.push_back(audio_clip("two", 7.0, 2.0, AudioRole::Dialogue));
  p.tracks[1].clips.push_back(audio_clip("music", 0.0, 20.0, AudioRole::Music));

  p = duck_clip(std::move(p), "music", DuckSettings{.amount_db = -12.0, .fade = 2.0});
  const double ducked = std::pow(10.0, -12.0 / 20.0);
  EXPECT_NEAR(curve_at(p, "music", 6.5), ducked, 1e-9)
      << "coming up and going straight back down is what pumping is";
}

TEST(Ducking, ADuckThatStartsBeforeTheClipArrivesAlreadyDown) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("speech", 0.0, 10.0, AudioRole::Dialogue));
  p.tracks[1].clips.push_back(audio_clip("music", 4.0, 10.0, AudioRole::Music));

  p = duck_clip(std::move(p), "music", DuckSettings{.amount_db = -12.0, .fade = 1.0});
  EXPECT_NEAR(curve_at(p, "music", 0.0), std::pow(10.0, -12.0 / 20.0), 1e-9)
      << "the ramp happened before this clip began, so it starts at the bottom";
}

TEST(Ducking, NothingToDuckUnderLeavesThePlainFader) {
  Project p = sequence();
  p.tracks[1].clips.push_back(audio_clip("music", 0.0, 20.0, AudioRole::Music));
  p = duck_clip(std::move(p), "music", DuckSettings{});
  EXPECT_TRUE(find_clip(p, "music")->gain_keyframes.empty())
      << "a flat curve says what no curve says, and the one that is not there can still be drawn "
         "on by hand";
}

TEST(Ducking, ItReplacesWhateverWasThereRatherThanLayeringOverIt) {
  Project p = bed_under_speech();
  find_clip(p, "music")->gain_keyframes = {Keyframe{.t = 0.0, .v = 0.1},
                                           Keyframe{.t = 20.0, .v = 0.9}};
  p = duck_clip(std::move(p), "music", DuckSettings{.amount_db = -12.0, .fade = 1.0});
  EXPECT_NEAR(curve_at(p, "music", 0.0), 1.0, 1e-9) << "the clip's gain, not the old curve";
}

TEST(DuckRole, EveryClipCarryingTheRoleIsDucked) {
  Project p = bed_under_speech();
  p.tracks[1].clips.push_back(audio_clip("more_music", 24.0, 4.0, AudioRole::Music));
  p.tracks[0].clips.push_back(audio_clip("more_speech", 25.0, 1.0, AudioRole::Dialogue));

  p = duck_role(std::move(p), AudioRole::Music, DuckSettings{.amount_db = -12.0, .fade = 0.5});
  EXPECT_FALSE(find_clip(p, "music")->gain_keyframes.empty());
  EXPECT_FALSE(find_clip(p, "more_music")->gain_keyframes.empty());
  EXPECT_TRUE(find_clip(p, "speech")->gain_keyframes.empty()) << "the dialogue is left alone";
}

TEST(DuckRole, DuckingNothingChangesNothing) {
  const Project before = bed_under_speech();
  EXPECT_EQ(duck_role(before, AudioRole::None, DuckSettings{}), before);
}

// ------------------------------------------------------------------- file --

TEST(RoleFile, ARoleSurvivesARoundTrip) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("c1", 0.0, 4.0, AudioRole::Ambience));

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(find_clip(loaded->project, "c1")->role, AudioRole::Ambience);
}

TEST(RoleFile, AClipWithoutOneWritesNothing) {
  Project p = sequence();
  p.tracks[0].clips.push_back(audio_clip("c1", 0.0, 4.0, AudioRole::None));
  EXPECT_EQ(to_json(p).find("\"role\""), std::string::npos);
}

}  // namespace
}  // namespace cutline::core
