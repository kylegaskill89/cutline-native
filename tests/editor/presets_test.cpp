/// Named effect stacks, saved once and applied everywhere.
///
/// A preset holds *copies* of the effects with the values they had, not a
/// reference to anything, because the only reason to have one is that it
/// applies whole to a project it was not made in.

#include "cutline/editor/presets.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Project;
using core::Track;
using core::TrackKind;

[[nodiscard]] Project two_clips() {
  Clip video;
  video.id = "v";
  video.media_id = "m1";
  video.source_out = 5.0;

  Clip audio;
  audio.id = "a";
  audio.media_id = "m2";
  audio.kind = TrackKind::Audio;
  audio.source_out = 5.0;

  Track picture{.id = "v1", .kind = TrackKind::Video};
  picture.clips = {std::move(video)};
  Track sound{.id = "a1", .kind = TrackKind::Audio};
  sound.clips = {std::move(audio)};

  Project p;
  p.sequence().tracks = {std::move(picture), std::move(sound)};
  return p;
}

[[nodiscard]] core::ClipEffect blur(double amount) {
  core::ClipEffect e;
  e.type = "blur";
  e.params["amount"] = amount;
  return e;
}

[[nodiscard]] Project with_blur(double amount = 6.0) {
  Project p = two_clips();
  p.sequence().tracks[0].clips[0].effects = {blur(amount)};
  return p;
}

[[nodiscard]] const Clip& picture_clip(const Project& p) { return p.sequence().tracks[0].clips[0]; }
[[nodiscard]] const Clip& sound_clip(const Project& p) { return p.sequence().tracks[1].clips[0]; }

// ------------------------------------------------------------------ saving --

TEST(Presets, SavingTakesWhatIsOnTheClip) {
  Presets presets;
  ASSERT_TRUE(save_preset(presets, with_blur(), "v", "Soft"));

  ASSERT_EQ(presets.named.size(), 1u);
  EXPECT_EQ(presets.named[0].name, "Soft");
  ASSERT_EQ(presets.named[0].video.size(), 1u);
  EXPECT_DOUBLE_EQ(presets.named[0].video[0].params.at("amount"), 6.0);
}

TEST(Presets, SavingOverOneKeepsItsPlace) {
  // A preset you have just refined jumping to the end of the list is one you
  // then have to go and find.
  Presets presets;
  ASSERT_TRUE(save_preset(presets, with_blur(2.0), "v", "First"));
  ASSERT_TRUE(save_preset(presets, with_blur(3.0), "v", "Second"));
  ASSERT_TRUE(save_preset(presets, with_blur(9.0), "v", "First"));

  ASSERT_EQ(presets.named.size(), 2u);
  EXPECT_EQ(presets.named[0].name, "First");
  EXPECT_DOUBLE_EQ(presets.named[0].video[0].params.at("amount"), 9.0);
}

TEST(Presets, ANamelessOrEmptyPresetIsRefused) {
  Presets presets;
  EXPECT_FALSE(save_preset(presets, with_blur(), "v", ""));
  // A clip with nothing on it: applying that preset would do nothing, which
  // looks exactly like the button not working.
  EXPECT_FALSE(save_preset(presets, two_clips(), "v", "Nothing"));
  EXPECT_FALSE(save_preset(presets, with_blur(), "nope", "Missing"));
  EXPECT_TRUE(presets.named.empty());
}

TEST(Presets, RemovingOneSaysWhetherItWasThere) {
  Presets presets;
  ASSERT_TRUE(save_preset(presets, with_blur(), "v", "Soft"));
  EXPECT_TRUE(remove_preset(presets, "Soft"));
  EXPECT_FALSE(remove_preset(presets, "Soft"));
  EXPECT_TRUE(presets.named.empty());
}

// ---------------------------------------------------------------- applying --

TEST(Presets, ApplyingAddsRatherThanReplacing) {
  // The opposite of what pasting a clipboard does, and the difference is what
  // each gesture means: a paste is "make this look like that", a preset is a
  // thing you reach for and put on. Two presets on one clip is ordinary.
  Presets presets;
  ASSERT_TRUE(save_preset(presets, with_blur(4.0), "v", "Soft"));

  Project p = with_blur(1.0);
  p = apply_preset(std::move(p), "v", presets.named[0]);

  ASSERT_EQ(picture_clip(p).effects.size(), 2u);
  EXPECT_DOUBLE_EQ(picture_clip(p).effects[0].params.at("amount"), 1.0);
  EXPECT_DOUBLE_EQ(picture_clip(p).effects[1].params.at("amount"), 4.0);
}

TEST(Presets, OnlyTheHalfThatFitsIsApplied) {
  // A preset carrying both is what a clip carrying both produced. Putting a
  // colour correction on a waveform because the same preset also held an
  // equaliser is not what anybody meant.
  EffectPreset both;
  both.name = "Everything";
  both.video = {blur(3.0)};
  core::AudioClipEffect filter;
  filter.type = "lowpass";
  both.audio = {filter};

  Project p = two_clips();
  p = apply_preset(std::move(p), "v", both);
  p = apply_preset(std::move(p), "a", both);

  EXPECT_EQ(picture_clip(p).effects.size(), 1u);
  EXPECT_TRUE(picture_clip(p).audio_effects.empty());
  EXPECT_TRUE(sound_clip(p).effects.empty());
  EXPECT_EQ(sound_clip(p).audio_effects.size(), 1u);
}

TEST(Presets, APresetWithNothingForThisClipChangesNothing) {
  EffectPreset sound_only;
  sound_only.name = "Sound";
  core::AudioClipEffect filter;
  filter.type = "lowpass";
  sound_only.audio = {filter};

  const Project before = two_clips();
  EXPECT_EQ(apply_preset(before, "v", sound_only), before);
  EXPECT_EQ(apply_preset(before, "nope", sound_only), before);
  EXPECT_EQ(apply_preset(before, "v", EffectPreset{}), before);
}

TEST(Presets, KeyframesAndMasksComeWithIt) {
  // What makes a preset worth saving rather than a set of numbers worth
  // writing down.
  Project p = two_clips();
  core::ClipEffect shaped = blur(2.0);
  shaped.keyframes["amount"] = {{.t = 0.0, .v = 0.0}, {.t = 2.0, .v = 8.0}};
  shaped.mask = core::Mask{.shape = core::MaskShape::Ellipse, .feather = 0.1};
  p.sequence().tracks[0].clips[0].effects = {shaped};

  Presets presets;
  ASSERT_TRUE(save_preset(presets, p, "v", "Shaped"));

  const Project applied = apply_preset(two_clips(), "v", presets.named[0]);
  ASSERT_EQ(picture_clip(applied).effects.size(), 1u);
  EXPECT_EQ(picture_clip(applied).effects[0].keyframes.at("amount").size(), 2u);
  EXPECT_EQ(picture_clip(applied).effects[0].mask.shape, core::MaskShape::Ellipse);
}

// ------------------------------------------------------------- persistence --

TEST(Presets, RoundTripThroughItsFile) {
  Presets presets;
  Project p = two_clips();
  core::ClipEffect shaped = blur(5.0);
  shaped.keyframes["amount"] = {{.t = 0.0, .v = 1.0, .e = core::Interp::Ease},
                                {.t = 3.0, .v = 9.0}};
  shaped.mask = core::Mask{.shape = core::MaskShape::Rectangle, .x = 0.3, .inverted = true};
  core::AudioClipEffect filter;
  filter.type = "lowpass";
  filter.params["freq"] = 800.0;
  p.sequence().tracks[0].clips[0].effects = {shaped};
  p.sequence().tracks[0].clips[0].audio_effects = {filter};
  ASSERT_TRUE(save_preset(presets, p, "v", "Everything"));

  const auto read = presets_from_json(to_json(presets));
  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_EQ(*read, presets);
}

TEST(Presets, AMissingOrEmptyFileIsNotAnError) {
  const auto read = presets_from_json(R"({"version": 1})");
  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_TRUE(read->named.empty());
}

TEST(Presets, ANewerFileIsRefusedRatherThanHalfRead) {
  const auto read = presets_from_json(R"({"version": 9999, "presets": []})");
  ASSERT_FALSE(read.has_value());
  EXPECT_NE(read.error().find("newer version"), std::string::npos);
}

TEST(Presets, RubbishIsRefused) {
  EXPECT_FALSE(presets_from_json("not json").has_value());
  EXPECT_FALSE(presets_from_json("[1, 2, 3]").has_value());
}

TEST(Presets, AnEntryWithNoNameOrNothingInItIsDropped) {
  // Neither can be offered or found again, so there is nothing to keep.
  const auto read = presets_from_json(R"({
    "version": 1,
    "presets": [
      {"name": "", "stacks": {"effects": [{"type": "blur"}]}},
      {"name": "Empty", "stacks": {"effects": []}},
      {"name": "Good", "stacks": {"effects": [{"type": "blur"}]}}
    ]
  })");

  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_EQ(read->named.size(), 1u);
  EXPECT_EQ(read->named[0].name, "Good");
}

TEST(Presets, FindsOneByName) {
  Presets presets;
  ASSERT_TRUE(save_preset(presets, with_blur(), "v", "Soft"));
  ASSERT_NE(find_preset(presets, "Soft"), nullptr);
  EXPECT_EQ(find_preset(presets, "Hard"), nullptr);
}

}  // namespace
}  // namespace cutline::editor
