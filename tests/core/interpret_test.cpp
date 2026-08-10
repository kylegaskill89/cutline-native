/// Interpret Footage: playing a source at a rate the file did not claim.
///
/// Two things have to be true at once and they pull against each other. The
/// source gets longer, because the same frames at a slower rate take more time.
/// And the clips already cut from it keep showing the *same frames*, which
/// means their ranges move too — and then the sequence has to absorb the fact
/// that they are all a different length.

#include "cutline/core/interpret.hpp"

#include "cutline/core/query.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

/// 60 fps footage, four seconds of it, with two seconds cut onto a track and
/// another clip butted up behind it.
///
/// The second clip is from a *different* source, so it is not conformed itself
/// and anything that happens to it is the ripple rather than the conform.
[[nodiscard]] Project cut_from_sixty() {
  Project p;
  p.sequence().fps = 24.0;
  p.media = {
      Media{.id = "fast", .name = "shot.mp4", .duration = 4.0, .has_video = true, .fps = 60.0},
      Media{.id = "other", .name = "b.mp4", .duration = 30.0, .has_video = true, .fps = 24.0},
  };

  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {
      Clip{.id = "c1", .media_id = "fast", .source_in = 0.0, .source_out = 2.0, .start = 0.0},
      Clip{.id = "c2", .media_id = "other", .source_in = 0.0, .source_out = 1.0, .start = 2.0},
  };
  p.sequence().tracks.push_back(std::move(video));
  return p;
}

[[nodiscard]] const Media& source(const Project& p, std::string_view id) {
  for (const Media& m : p.media) {
    if (m.id == id) return m;
  }
  static const Media nothing;
  return nothing;
}

}  // namespace

// ----------------------------------------------------------------- the rate --

TEST(Interpret, AnUntouchedSourceIsPlayedAtItsOwnRate) {
  const Project p = cut_from_sixty();
  EXPECT_DOUBLE_EQ(conform_speed(source(p, "fast")), 1.0);
  EXPECT_FALSE(is_conformed(source(p, "fast")));
  EXPECT_DOUBLE_EQ(playback_fps(source(p, "fast")).value_or(0.0), 60.0);
}

TEST(Interpret, SixtyShownAtTwentyFourReadsTheFileAtFourTenths) {
  // The one number the whole feature is. Ten seconds of source is four seconds
  // of file, which is what makes every frame of it a real one.
  const Project p = interpret_media(cut_from_sixty(), "fast", 24.0);
  EXPECT_DOUBLE_EQ(conform_speed(source(p, "fast")), 0.4);
  EXPECT_TRUE(is_conformed(source(p, "fast")));
  EXPECT_DOUBLE_EQ(playback_fps(source(p, "fast")).value_or(0.0), 24.0);
}

TEST(Interpret, TheSourceGetsLonger) {
  const Project p = interpret_media(cut_from_sixty(), "fast", 24.0);
  EXPECT_DOUBLE_EQ(source(p, "fast").duration, 10.0);
  EXPECT_DOUBLE_EQ(source(p, "fast").file_duration.value_or(0.0), 4.0);
}

TEST(Interpret, ClearingPutsTheLengthBackExactly) {
  // Restored from what was kept rather than divided back, so a source that has
  // been conformed and un-conformed is the length the file actually is and not
  // two roundings away from it.
  Project p = interpret_media(cut_from_sixty(), "fast", 23.976);
  p = interpret_media(std::move(p), "fast", 47.952);
  p = interpret_media(std::move(p), "fast", std::nullopt);

  EXPECT_DOUBLE_EQ(source(p, "fast").duration, 4.0);
  EXPECT_FALSE(source(p, "fast").file_duration.has_value());
  EXPECT_FALSE(source(p, "fast").assumed_fps.has_value());
  EXPECT_DOUBLE_EQ(find_clip(p, "c1")->source_out, 2.0);
}

TEST(Interpret, ARateMatchingTheFileIsNotAConformButTheAbsenceOfOne) {
  // Stored as nothing rather than as 60, so `is_conformed` never has to ask
  // twice and no file is written claiming an override that does nothing.
  Project p = interpret_media(cut_from_sixty(), "fast", 24.0);
  p = interpret_media(std::move(p), "fast", 60.0);

  EXPECT_FALSE(source(p, "fast").assumed_fps.has_value());
  EXPECT_FALSE(is_conformed(source(p, "fast")));
  EXPECT_DOUBLE_EQ(source(p, "fast").duration, 4.0);
}

TEST(Interpret, AlreadyAtThisRateChangesNothing) {
  const Project before = interpret_media(cut_from_sixty(), "fast", 24.0);
  EXPECT_EQ(interpret_media(before, "fast", 24.0), before);
}

// ------------------------------------------------------------ what refuses --

TEST(Interpret, ASourceWithNoRateCannotBeConformed) {
  // There is nothing to conform *from*: a file whose rate never probed has no
  // ratio to form, and guessing one would be inventing the very number this is
  // about.
  Project p = cut_from_sixty();
  p.media[0].fps.reset();
  EXPECT_FALSE(can_interpret(p.media[0]));
  EXPECT_EQ(interpret_media(p, "fast", 24.0), p);
}

TEST(Interpret, StillsAndGeneratedSourcesCannotBe) {
  Project p = cut_from_sixty();
  p.media.push_back(Media{.id = "card", .name = "card.png", .duration = 5.0,
                          .has_video = true, .is_image = true, .fps = 60.0});
  p.media.push_back(Media{.id = "matte", .name = "Colour", .duration = 5.0,
                          .is_color = true, .fps = 60.0});

  EXPECT_FALSE(can_interpret(p.media[2])) << "a still has no frames to re-time";
  EXPECT_FALSE(can_interpret(p.media[3]));
  EXPECT_EQ(interpret_media(p, "card", 24.0), p);
  EXPECT_EQ(interpret_media(p, "matte", 24.0), p);
}

TEST(Interpret, AnAbsurdRateIsRefused) {
  const Project before = cut_from_sixty();
  EXPECT_EQ(interpret_media(before, "fast", 0.0), before);
  EXPECT_EQ(interpret_media(before, "fast", -24.0), before);
  EXPECT_EQ(interpret_media(before, "fast", 100000.0), before);
}

TEST(Interpret, ASourceThatIsNotThereChangesNothing) {
  const Project before = cut_from_sixty();
  EXPECT_EQ(interpret_media(before, "nobody", 24.0), before);
}

// ----------------------------------------------------------- and the clips --

TEST(Interpret, ClipsKeepTheirFrames) {
  // A clip on the first two seconds of 60 fps footage is on frames 0 to 120.
  // At 24 those frames end at five seconds, and that is where the clip's range
  // has to end — leaving it at two would have the clip jump to different
  // footage, which is not what changing a playback rate means.
  const Project p = interpret_media(cut_from_sixty(), "fast", 24.0);
  const Clip* clip = find_clip(p, "c1");
  ASSERT_NE(clip, nullptr);
  EXPECT_DOUBLE_EQ(clip->source_in, 0.0);
  EXPECT_DOUBLE_EQ(clip->source_out, 5.0);
  EXPECT_DOUBLE_EQ(clip_duration(*clip), 5.0);
}

TEST(Interpret, TheSequenceRipplesToFit) {
  // Without this the conformed clip runs three seconds into the one behind it.
  const Project p = interpret_media(cut_from_sixty(), "fast", 24.0);
  const Clip* behind = find_clip(p, "c2");
  ASSERT_NE(behind, nullptr);
  EXPECT_DOUBLE_EQ(behind->start, 5.0);
  EXPECT_DOUBLE_EQ(behind->source_out, 1.0) << "the ripple retimed a clip it only had to move";
}

TEST(Interpret, ConformingUpwardsClosesTheSequenceBackUp) {
  // The other direction: 60 played at 120 is half as long, and what follows
  // comes back to meet it rather than leaving a gap.
  const Project p = interpret_media(cut_from_sixty(), "fast", 120.0);
  EXPECT_DOUBLE_EQ(source(p, "fast").duration, 2.0);
  EXPECT_DOUBLE_EQ(find_clip(p, "c1")->source_out, 1.0);
  EXPECT_DOUBLE_EQ(find_clip(p, "c2")->start, 1.0);
}

TEST(Interpret, AnUnusedSourceIsJustALength) {
  Project p = cut_from_sixty();
  p.sequence().tracks.clear();
  const Project after = interpret_media(std::move(p), "fast", 24.0);
  EXPECT_DOUBLE_EQ(source(after, "fast").duration, 10.0);
}

TEST(Interpret, TheMarksOnTheSourceMoveWithIt) {
  // They belong to the asset and are in the same seconds its clips are, so a
  // mark left behind would name a different frame afterwards.
  Project p = cut_from_sixty();
  p.media[0].in_point = 1.0;
  p.media[0].out_point = 3.0;

  const Project after = interpret_media(std::move(p), "fast", 24.0);
  EXPECT_DOUBLE_EQ(source(after, "fast").in_point.value_or(0.0), 2.5);
  EXPECT_DOUBLE_EQ(source(after, "fast").out_point.value_or(0.0), 7.5);
}

TEST(Interpret, AHeldFrameStaysTheSameFrame) {
  Project p = cut_from_sixty();
  find_clip(p, "c1")->hold = 1.0;
  const Project after = interpret_media(std::move(p), "fast", 24.0);
  EXPECT_DOUBLE_EQ(find_clip(after, "c1")->hold.value_or(0.0), 2.5);
}

TEST(Interpret, FadesAreNotLeftLongerThanTheClip) {
  // Conforming upwards shortens a clip out from under its own fades, exactly
  // as a retime can.
  Project p = cut_from_sixty();
  Clip* clip = find_clip(p, "c1");
  clip->fade_in = 1.0;
  clip->fade_out = 0.8;

  const Project after = interpret_media(std::move(p), "fast", 240.0);
  const Clip* result = find_clip(after, "c1");
  ASSERT_NE(result, nullptr);
  const double length = clip_duration(*result);
  EXPECT_LE(result->fade_in, length + 1e-9);
  EXPECT_LE(result->fade_in + result->fade_out, length + 1e-9);
}

// ------------------------------------------------------------- and the file --

TEST(Interpret, AConformSurvivesBeingSavedAndOpened) {
  const Project before = interpret_media(cut_from_sixty(), "fast", 23.976);
  const auto read = from_json(to_json(before));

  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_EQ(read->project, before);
  EXPECT_DOUBLE_EQ(source(read->project, "fast").assumed_fps.value_or(0.0), 23.976);
  EXPECT_DOUBLE_EQ(source(read->project, "fast").file_duration.value_or(0.0), 4.0);
}

TEST(Interpret, AProjectWithNoConformWritesNeitherField) {
  // So a file written now opens in a build that predates the feature, and one
  // written before it gains nothing on the way through.
  const std::string text = to_json(cut_from_sixty());
  EXPECT_EQ(text.find("assumed_fps"), std::string::npos);
  EXPECT_EQ(text.find("file_duration"), std::string::npos);
}

}  // namespace cutline::core
