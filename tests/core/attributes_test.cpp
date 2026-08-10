/// Paste Attributes: some of what was copied, onto clips already on the
/// timeline.
///
/// The interesting behaviour is all about what does *not* travel. A paste that
/// moves everything is the plain paste this is an alternative to, so every test
/// here is as much about the fields left alone as about the ones carried.

#include "cutline/core/attributes.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

/// Two shots, each an A/V pair, so "each half takes from its own kind" has
/// something to be true of.
///
/// The first is dressed: moved, faded, given an effect, turned up and panned.
/// The second is plain, and is what everything is pasted onto.
[[nodiscard]] Project two_shots() {
  Project p;
  p.sequence().fps = 30.0;
  p.media.push_back(
      Media{.id = "m1", .name = "take.mp4", .duration = 60.0, .has_video = true,
            .audio_stream_count = 1});

  Clip source_video{.id = "v-from", .media_id = "m1", .source_in = 0.0, .source_out = 2.0,
                    .start = 0.0};
  source_video.kind = TrackKind::Video;
  source_video.group_id = "g1";
  source_video.transform = Transform{.x = 0.25, .y = 0.75, .scale_x = 2.0, .scale_y = 2.0,
                                     .rotation = 30.0};
  source_video.opacity = 0.5;
  source_video.fade_in = 0.4;
  source_video.blend = BlendMode::Screen;
  source_video.effects.push_back(ClipEffect{.type = "video:blur"});
  source_video.keyframes[anim_prop_index(AnimProp::X)] = {Keyframe{.t = 0.0, .v = 0.0},
                                                          Keyframe{.t = 2.0, .v = 1.0}};
  source_video.keyframes[anim_prop_index(AnimProp::Opacity)] = {Keyframe{.t = 0.0, .v = 0.0},
                                                                Keyframe{.t = 1.0, .v = 1.0}};

  Clip source_audio{.id = "a-from", .media_id = "m1", .source_in = 0.0, .source_out = 2.0,
                    .start = 0.0};
  source_audio.kind = TrackKind::Audio;
  source_audio.group_id = "g1";
  source_audio.gain = 0.25;
  source_audio.pan = -0.8;
  source_audio.role = AudioRole::Dialogue;
  source_audio.channel_map = {1, 1};
  source_audio.fade_in = 0.3;
  source_audio.gain_keyframes = {Keyframe{.t = 0.0, .v = 0.0}, Keyframe{.t = 2.0, .v = 1.0}};
  source_audio.audio_effects.push_back(AudioClipEffect{.type = "audio:highpass"});

  // Four seconds rather than two, so a stretch has something to scale by.
  Clip onto_video{.id = "v-onto", .media_id = "m1", .source_in = 0.0, .source_out = 4.0,
                  .start = 10.0};
  onto_video.kind = TrackKind::Video;
  onto_video.group_id = "g2";

  Clip onto_audio{.id = "a-onto", .media_id = "m1", .source_in = 0.0, .source_out = 4.0,
                  .start = 10.0};
  onto_audio.kind = TrackKind::Audio;
  onto_audio.group_id = "g2";

  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {source_video, onto_video};
  Track audio{.id = "a1", .kind = TrackKind::Audio};
  audio.clips = {source_audio, onto_audio};

  p.sequence().tracks.push_back(std::move(video));
  p.sequence().tracks.push_back(std::move(audio));
  return p;
}

/// What a copy of the dressed shot looks like on the clipboard.
[[nodiscard]] std::vector<ClipCopy> copied(const Project& p) {
  const std::vector<std::string> both{"v-from", "a-from"};
  return copy_clips(p, both);
}

[[nodiscard]] std::vector<std::string> targets() { return {"v-onto", "a-onto"}; }

}  // namespace

// ------------------------------------------------------------ what travels --

TEST(PasteAttributes, MotionTravelsWithItsKeyframes) {
  // Both, and as one group. A shot moved without its animation jumps back to
  // wherever the first keyframe puts it the moment it plays, which is a worse
  // answer than not having moved it.
  const Project before = two_shots();
  const Project after =
      paste_attributes(before, copied(before), targets(), ClipAttributes{.motion = true});

  const Clip* onto = find_clip(after, "v-onto");
  ASSERT_NE(onto, nullptr);
  EXPECT_DOUBLE_EQ(onto->transform.x, 0.25);
  EXPECT_DOUBLE_EQ(onto->transform.scale_x, 2.0);
  EXPECT_DOUBLE_EQ(onto->transform.rotation, 30.0);
  EXPECT_EQ(onto->keyframes[anim_prop_index(AnimProp::X)].size(), 2u);
}

TEST(PasteAttributes, OpacityBringsTheFadesWithIt) {
  // Premiere writes a video fade *as* opacity keyframes, so the two belong to
  // one tick or the group means something different here than it does there.
  const Project before = two_shots();
  const Project after =
      paste_attributes(before, copied(before), targets(), ClipAttributes{.opacity = true});

  const Clip* onto = find_clip(after, "v-onto");
  ASSERT_NE(onto, nullptr);
  EXPECT_DOUBLE_EQ(onto->opacity, 0.5);
  EXPECT_DOUBLE_EQ(onto->fade_in, 0.4);
  EXPECT_EQ(onto->keyframes[anim_prop_index(AnimProp::Opacity)].size(), 2u);
}

TEST(PasteAttributes, TheSoundGroupsTravelToTheSoundHalf) {
  const Project before = two_shots();
  const Project after = paste_attributes(
      before, copied(before), targets(),
      ClipAttributes{.volume = true, .pan = true, .channels = true, .role = true,
                     .audio_effects = true});

  const Clip* onto = find_clip(after, "a-onto");
  ASSERT_NE(onto, nullptr);
  EXPECT_DOUBLE_EQ(onto->gain, 0.25);
  EXPECT_DOUBLE_EQ(onto->pan, -0.8);
  EXPECT_DOUBLE_EQ(onto->fade_in, 0.3);
  EXPECT_EQ(onto->role, AudioRole::Dialogue);
  EXPECT_EQ(onto->channel_map, std::vector<int>({1, 1}));
  EXPECT_EQ(onto->audio_effects.size(), 1u);
  EXPECT_EQ(onto->gain_keyframes.size(), 2u);
}

TEST(PasteAttributes, EachHalfTakesFromItsOwnKind) {
  // A shot on this timeline is two linked clips, so copying one copies both and
  // selecting another selects two more. Picture groups have to reach the video
  // halves and sound groups the audio ones, or "make this shot like that one"
  // needs doing twice.
  const Project before = two_shots();
  const Project after = paste_attributes(before, copied(before), targets(),
                                         ClipAttributes{.motion = true, .volume = true});

  const Clip* picture = find_clip(after, "v-onto");
  const Clip* sound = find_clip(after, "a-onto");
  ASSERT_NE(picture, nullptr);
  ASSERT_NE(sound, nullptr);
  EXPECT_DOUBLE_EQ(picture->transform.x, 0.25) << "the picture missed its motion";
  EXPECT_DOUBLE_EQ(sound->gain, 0.25) << "the sound missed its level";
  // And neither took the other's.
  EXPECT_DOUBLE_EQ(picture->gain, 1.0);
  EXPECT_DOUBLE_EQ(sound->transform.x, 0.5);
}

TEST(PasteAttributes, EffectsReplaceRatherThanAppend) {
  // The same bargain `paste_effects` strikes, and for the same reason: pasting
  // twice must not apply everything twice.
  Project before = two_shots();
  Clip* onto = find_clip(before, "v-onto");
  ASSERT_NE(onto, nullptr);
  onto->effects.push_back(ClipEffect{.type = "video:invert"});

  const Project after =
      paste_attributes(before, copied(before), targets(), ClipAttributes{.effects = true});
  const Clip* result = find_clip(after, "v-onto");
  ASSERT_NE(result, nullptr);
  ASSERT_EQ(result->effects.size(), 1u);
  EXPECT_EQ(result->effects[0].type, "video:blur");
}

// -------------------------------------------------------- what stays put --

TEST(PasteAttributes, NothingTickedChangesNothing) {
  const Project before = two_shots();
  EXPECT_EQ(paste_attributes(before, copied(before), targets(), ClipAttributes{}), before);
}

TEST(PasteAttributes, WhatIsNotTickedIsLeftAlone) {
  // The whole point of the command. Motion travelling and quietly bringing the
  // grade with it would make this the plain paste it exists to be narrower
  // than.
  const Project before = two_shots();
  const Project after =
      paste_attributes(before, copied(before), targets(), ClipAttributes{.motion = true});

  const Clip* onto = find_clip(after, "v-onto");
  ASSERT_NE(onto, nullptr);
  EXPECT_DOUBLE_EQ(onto->opacity, 1.0);
  EXPECT_DOUBLE_EQ(onto->fade_in, 0.0);
  EXPECT_EQ(onto->blend, BlendMode::Normal);
  EXPECT_TRUE(onto->effects.empty());
  EXPECT_TRUE(onto->keyframes[anim_prop_index(AnimProp::Opacity)].empty());
}

TEST(PasteAttributes, AGroupWhoseKindIsNotOnTheClipboardChangesNothing) {
  // Copying a video-only clip and asking for Volume has to be a no-op rather
  // than a silence: there is no level on the clipboard to have meant.
  const Project before = two_shots();
  const std::vector<std::string> picture_only{"v-from"};
  const std::vector<ClipCopy> clipboard = copy_clips(before, picture_only);

  const Project after = paste_attributes(before, clipboard, targets(),
                                         ClipAttributes{.motion = true, .volume = true});
  const Clip* sound = find_clip(after, "a-onto");
  ASSERT_NE(sound, nullptr);
  EXPECT_DOUBLE_EQ(sound->gain, 1.0);
  EXPECT_DOUBLE_EQ(sound->fade_in, 0.0);
  // And the half that *was* on the clipboard still travelled.
  EXPECT_DOUBLE_EQ(find_clip(after, "v-onto")->transform.x, 0.25);
}

TEST(PasteAttributes, AClipIsNeverPastedOntoItself) {
  // It would be a no-op in every group, but going through the motions makes an
  // edit out of a gesture that changed nothing — and one undo entry that undoes
  // nothing is one press of Ctrl+Z that appears to do nothing.
  const Project before = two_shots();
  const std::vector<std::string> itself{"v-from", "a-from"};
  EXPECT_EQ(paste_attributes(before, copied(before), itself,
                             ClipAttributes{.motion = true, .volume = true}),
            before);
}

TEST(PasteAttributes, AnEmptyClipboardOrSelectionChangesNothing) {
  const Project before = two_shots();
  EXPECT_EQ(paste_attributes(before, {}, targets(), ClipAttributes{.motion = true}), before);
  EXPECT_EQ(paste_attributes(before, copied(before), {}, ClipAttributes{.motion = true}),
            before);
}

TEST(PasteAttributes, AClipThatIsNotThereIsSkipped) {
  const Project before = two_shots();
  const std::vector<std::string> ghost{"nobody", "v-onto"};
  const Project after =
      paste_attributes(before, copied(before), ghost, ClipAttributes{.motion = true});
  EXPECT_DOUBLE_EQ(find_clip(after, "v-onto")->transform.x, 0.25);
}

// ------------------------------------------------------------- stretching --

TEST(PasteAttributes, KeyframeTimesAreLeftAloneUnlessAsked) {
  // Premiere's default, and the safer one: an unscaled animation is
  // recognisably the move that was copied.
  const Project before = two_shots();
  const Project after =
      paste_attributes(before, copied(before), targets(), ClipAttributes{.motion = true});

  const Clip* onto = find_clip(after, "v-onto");
  ASSERT_NE(onto, nullptr);
  const std::vector<Keyframe>& keys = onto->keyframes[anim_prop_index(AnimProp::X)];
  ASSERT_EQ(keys.size(), 2u);
  EXPECT_DOUBLE_EQ(keys[1].t, 2.0) << "the times were stretched without being asked";
}

TEST(PasteAttributes, StretchingScalesTheTimesToTheClipLandedOn) {
  // Two seconds of animation copied onto a four-second clip becomes four
  // seconds of it. Premiere's "Scale Attributes Time".
  const Project before = two_shots();
  const Project after = paste_attributes(
      before, copied(before), targets(),
      ClipAttributes{.motion = true, .volume = true, .scale_to_length = true});

  const std::vector<Keyframe>& picture =
      find_clip(after, "v-onto")->keyframes[anim_prop_index(AnimProp::X)];
  ASSERT_EQ(picture.size(), 2u);
  EXPECT_DOUBLE_EQ(picture[1].t, 4.0);

  // The sound half is scaled by its own pair of lengths, not the picture's.
  const std::vector<Keyframe>& sound = find_clip(after, "a-onto")->gain_keyframes;
  ASSERT_EQ(sound.size(), 2u);
  EXPECT_DOUBLE_EQ(sound[1].t, 4.0);
}

TEST(PasteAttributes, StretchingCarriesTheEasingWithTheTimes) {
  // A keyframe's handles are offsets in the same units as its time. Leaving
  // them behind keeps the easing of the length the move was drawn at, which
  // reads as a different move rather than as the same one made slower.
  Project before = two_shots();
  Clip* from = find_clip(before, "v-from");
  ASSERT_NE(from, nullptr);
  from->keyframes[anim_prop_index(AnimProp::X)][0].out_x = 0.5;

  const Project after = paste_attributes(before, copied(before), targets(),
                                         ClipAttributes{.motion = true, .scale_to_length = true});
  const std::vector<Keyframe>& keys =
      find_clip(after, "v-onto")->keyframes[anim_prop_index(AnimProp::X)];
  ASSERT_FALSE(keys.empty());
  EXPECT_DOUBLE_EQ(keys[0].out_x, 1.0);
}

TEST(PasteAttributes, AZeroLengthClipLeavesTheTimesAlone) {
  // Multiplying by zero is not a scaled animation, it is a lost one: every
  // keyframe piled onto the head of the clip.
  Project before = two_shots();
  Clip* onto = find_clip(before, "v-onto");
  ASSERT_NE(onto, nullptr);
  onto->source_out = onto->source_in;

  const Project after = paste_attributes(before, copied(before), targets(),
                                         ClipAttributes{.motion = true, .scale_to_length = true});
  const std::vector<Keyframe>& keys =
      find_clip(after, "v-onto")->keyframes[anim_prop_index(AnimProp::X)];
  ASSERT_EQ(keys.size(), 2u);
  EXPECT_DOUBLE_EQ(keys[1].t, 2.0);
}

}  // namespace cutline::core
