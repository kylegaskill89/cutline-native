#include "cutline/core/properties.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

using Ids = std::vector<std::string>;

Project one_clip_project() {
  Project p;
  Media m;
  m.id = "m1";
  m.duration = 20.0;
  m.has_video = true;
  p.media = {m};

  Clip c;
  c.id = "c1";
  c.media_id = "m1";
  c.source_in = 0.0;
  c.source_out = 10.0;

  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.clips = {c};
  p.tracks = {v};
  return p;
}

const Clip& only_clip(const Project& p) { return p.tracks[0].clips[0]; }

// -------------------------------------------------------- clip properties --

TEST(ClipProperties, EnableAndDisable) {
  Project p = one_clip_project();
  p = set_clips_enabled(std::move(p), Ids{"c1"}, false);
  EXPECT_TRUE(only_clip(p).disabled);
  p = set_clips_enabled(std::move(p), Ids{"c1"}, true);
  EXPECT_FALSE(only_clip(p).disabled);
}

TEST(ClipProperties, BlendMode) {
  Project p = one_clip_project();
  p = set_clip_blend(std::move(p), "c1", BlendMode::Screen);
  EXPECT_EQ(only_clip(p).blend, BlendMode::Screen);
  p = set_clip_blend(std::move(p), "c1", BlendMode::Normal);
  EXPECT_EQ(only_clip(p).blend, BlendMode::Normal);
}

TEST(ClipProperties, GainIsClamped) {
  Project p = one_clip_project();
  EXPECT_DOUBLE_EQ(set_clip_gain(p, "c1", -1.0).tracks[0].clips[0].gain, 0.0);
  EXPECT_DOUBLE_EQ(set_clip_gain(p, "c1", 99.0).tracks[0].clips[0].gain, kMaxGain);
  EXPECT_DOUBLE_EQ(set_clip_gain(p, "c1", 0.5).tracks[0].clips[0].gain, 0.5);
}

TEST(ClipProperties, OpacityIsClamped) {
  const Project p = one_clip_project();
  EXPECT_DOUBLE_EQ(set_clip_opacity(p, "c1", -1.0).tracks[0].clips[0].opacity, 0.0);
  EXPECT_DOUBLE_EQ(set_clip_opacity(p, "c1", 5.0).tracks[0].clips[0].opacity, 1.0);
}

TEST(ClipProperties, TransformIsReplacedWholesale) {
  Project p = one_clip_project();
  Transform t = only_clip(p).transform;
  t.x = 0.25;
  t.rotation = 90.0;
  p = set_clip_transform(std::move(p), "c1", t);

  EXPECT_DOUBLE_EQ(only_clip(p).transform.x, 0.25);
  EXPECT_DOUBLE_EQ(only_clip(p).transform.rotation, 90.0);
  EXPECT_DOUBLE_EQ(only_clip(p).transform.y, 0.5);  // untouched default
}

// A zero-length transition is just a cut, so setting one clears it.
TEST(ClipProperties, TransitionSetAndClear) {
  Project p = one_clip_project();
  p = set_clip_transition(std::move(p), "c1",
                          Transition{.kind = TransitionKind::Push, .duration = 1.0});
  ASSERT_TRUE(only_clip(p).transition_out.has_value());
  EXPECT_EQ(only_clip(p).transition_out->kind, TransitionKind::Push);

  p = set_clip_transition(std::move(p), "c1",
                          Transition{.kind = TransitionKind::Push, .duration = 0.0});
  EXPECT_FALSE(only_clip(p).transition_out.has_value());

  p = set_clip_transition(std::move(p), "c1",
                          Transition{.kind = TransitionKind::Push, .duration = 1.0});
  p = set_clip_transition(std::move(p), "c1", std::nullopt);
  EXPECT_FALSE(only_clip(p).transition_out.has_value());
}

// ------------------------------------------------------------------- fades --

TEST(Fades, AreClampedToTheClipLength) {
  Project p = one_clip_project();  // ten seconds long
  p = set_clip_fade(std::move(p), "c1", ClipEdge::In, 99.0);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_in, 10.0);

  p = set_clip_fade(std::move(p), "c1", ClipEdge::In, -5.0);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_in, 0.0);
}

// The two fades share the clip's length and may not overlap.
TEST(Fades, LeaveRoomForEachOther) {
  Project p = one_clip_project();
  p = set_clip_fade(std::move(p), "c1", ClipEdge::In, 7.0);
  p = set_clip_fade(std::move(p), "c1", ClipEdge::Out, 9.0);

  EXPECT_DOUBLE_EQ(only_clip(p).fade_in, 7.0);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_out, 3.0);
}

// ------------------------------------------------------------------ speed --

TEST(Speed, RetimesWithoutChangingSource) {
  Project p = one_clip_project();
  p = set_clip_speed(std::move(p), "c1", 2.0);

  EXPECT_DOUBLE_EQ(only_clip(p).speed, 2.0);
  EXPECT_DOUBLE_EQ(only_clip(p).source_out, 10.0);
  EXPECT_DOUBLE_EQ(clip_duration(only_clip(p)), 5.0);
}

TEST(Speed, IsClampedToTheAllowedRange) {
  const Project p = one_clip_project();
  EXPECT_DOUBLE_EQ(set_clip_speed(p, "c1", 0.0).tracks[0].clips[0].speed, kMinSpeed);
  EXPECT_DOUBLE_EQ(set_clip_speed(p, "c1", 1e6).tracks[0].clips[0].speed, kMaxSpeed);
}

TEST(Speed, SetsReverseOnlyWhenAsked) {
  Project p = one_clip_project();
  p = set_clip_speed(std::move(p), "c1", 2.0);
  EXPECT_FALSE(only_clip(p).reverse);

  p = set_clip_speed(std::move(p), "c1", 2.0, true);
  EXPECT_TRUE(only_clip(p).reverse);

  p = set_clip_speed(std::move(p), "c1", 3.0);  // no reverse argument
  EXPECT_TRUE(only_clip(p).reverse);            // so it is left alone
}

// Speeding a clip up shortens it, which can leave its fades longer than it is.
TEST(Speed, ReclampsFadesToTheNewDuration) {
  Project p = one_clip_project();
  p = set_clip_fade(std::move(p), "c1", ClipEdge::In, 6.0);
  p = set_clip_fade(std::move(p), "c1", ClipEdge::Out, 4.0);

  p = set_clip_speed(std::move(p), "c1", 4.0);  // ten seconds becomes two and a half

  EXPECT_DOUBLE_EQ(clip_duration(only_clip(p)), 2.5);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_in, 2.5);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_out, 0.0);
}

// ------------------------------------------------------- generated media --

TEST(GeneratedMedia, TextSpecOnlyAppliesToTitles) {
  Project p = one_clip_project();
  Media title;
  title.id = "t1";
  title.is_text = true;
  title.text = TextSpec{};
  p.media.push_back(title);

  TextSpec spec;
  spec.content = "Hello";
  spec.font_size = 42.0;
  p = set_text_spec(std::move(p), "t1", spec);
  EXPECT_EQ(p.media[1].text->content, "Hello");

  // The footage media is not a title, so it is untouched.
  const Project before = p;
  EXPECT_EQ(set_text_spec(before, "m1", spec), before);
}

TEST(GeneratedMedia, MatteColourAndGradient) {
  Project p = one_clip_project();
  Media matte;
  matte.id = "k1";
  matte.is_color = true;
  matte.color = kDefaultMatteColor;
  p.media.push_back(matte);

  p = set_matte_color(std::move(p), "k1", "#ff0000");
  EXPECT_EQ(p.media[1].color, "#ff0000");

  p = set_matte_gradient(std::move(p), "k1", MatteGradient{.color2 = "#0000ff", .angle = 90.0});
  ASSERT_TRUE(p.media[1].gradient.has_value());
  EXPECT_DOUBLE_EQ(p.media[1].gradient->angle, 90.0);

  p = set_matte_gradient(std::move(p), "k1", std::nullopt);
  EXPECT_FALSE(p.media[1].gradient.has_value());
}

TEST(GeneratedMedia, MatteSettersIgnoreOtherMedia) {
  const Project before = one_clip_project();
  EXPECT_EQ(set_matte_color(before, "m1", "#ff0000"), before);
  EXPECT_EQ(set_matte_gradient(before, "m1", MatteGradient{}), before);
}

// ------------------------------------------------------------------ tracks --

// Video tracks are stored top-first, so a new one goes to the front and becomes
// the topmost compositing layer.
TEST(Tracks, VideoTracksAreAddedOnTop) {
  Project p = one_clip_project();
  p = add_video_track(std::move(p));
  EXPECT_EQ(p.tracks[0].kind, TrackKind::Video);
  EXPECT_TRUE(p.tracks[0].clips.empty());
  EXPECT_EQ(p.tracks[1].id, "v1");
}

TEST(Tracks, AudioTracksAreAddedAtTheBottom) {
  Project p = one_clip_project();
  p = add_audio_track(std::move(p));
  EXPECT_EQ(p.tracks.back().kind, TrackKind::Audio);
}

TEST(Tracks, LabelIsTrimmedAndClearable) {
  Project p = one_clip_project();
  p = set_track_label(std::move(p), "v1", "  Overlay  ");
  EXPECT_EQ(p.tracks[0].label, "Overlay");

  p = set_track_label(std::move(p), "v1", "   ");
  EXPECT_EQ(p.tracks[0].label, "");
}

TEST(Tracks, PatchTouchesOnlyWhatItSets) {
  Project p = one_clip_project();
  p = update_track(std::move(p), "v1", TrackPropsPatch{.hidden = true});
  EXPECT_TRUE(p.tracks[0].hidden);
  EXPECT_FALSE(p.tracks[0].locked);

  p = update_track(std::move(p), "v1", TrackPropsPatch{.locked = true});
  EXPECT_TRUE(p.tracks[0].hidden);  // still set
  EXPECT_TRUE(p.tracks[0].locked);
}

TEST(Tracks, RemoveTakesItsClipsWithIt) {
  Project p = one_clip_project();
  p = remove_track(std::move(p), "v1");
  EXPECT_TRUE(p.tracks.empty());
  EXPECT_EQ(find_clip(p, "c1"), nullptr);
}

TEST(Tracks, UnknownTracksAreNoOps) {
  const Project before = one_clip_project();
  EXPECT_EQ(set_track_label(before, "nope", "x"), before);
  EXPECT_EQ(update_track(before, "nope", TrackPropsPatch{.muted = true}), before);
  EXPECT_EQ(remove_track(before, "nope"), before);
}

// ----------------------------------------------------------------- markers --

Project marked_project() {
  Project p = one_clip_project();
  p = add_marker(std::move(p), 5.0, "five");
  p = add_marker(std::move(p), 1.0, "one");
  p = add_marker(std::move(p), 3.0, "three");
  return p;
}

TEST(Markers, AreKeptInTimeOrder) {
  const Project p = marked_project();
  ASSERT_EQ(p.markers.size(), 3u);
  EXPECT_DOUBLE_EQ(p.markers[0].time, 1.0);
  EXPECT_DOUBLE_EQ(p.markers[1].time, 3.0);
  EXPECT_DOUBLE_EQ(p.markers[2].time, 5.0);
}

TEST(Markers, NearFindsTheClosestWithinTolerance) {
  const Project p = marked_project();
  ASSERT_NE(marker_near(p, 3.2, 0.5), nullptr);
  EXPECT_EQ(marker_near(p, 3.2, 0.5)->label, "three");
  EXPECT_EQ(marker_near(p, 3.2, 0.1), nullptr);
}

TEST(Markers, NavigateForwardsAndBackwards) {
  const Project p = marked_project();
  ASSERT_NE(next_marker(p, 1.0), nullptr);
  EXPECT_DOUBLE_EQ(next_marker(p, 1.0)->time, 3.0);
  EXPECT_EQ(next_marker(p, 5.0), nullptr);

  ASSERT_NE(previous_marker(p, 5.0), nullptr);
  EXPECT_DOUBLE_EQ(previous_marker(p, 5.0)->time, 3.0);
  EXPECT_EQ(previous_marker(p, 1.0), nullptr);
}

TEST(Markers, RemoveAndClear) {
  Project p = marked_project();
  const std::string id = p.markers[1].id;

  p = remove_marker(std::move(p), id);
  EXPECT_EQ(p.markers.size(), 2u);

  p = clear_markers(std::move(p));
  EXPECT_TRUE(p.markers.empty());
}

// --------------------------------------------------------------- factories --

TEST(EmptyProject, HasTheRequestedLanesAndNoMedia) {
  reset_ids();
  const Project p = empty_project();

  ASSERT_EQ(p.tracks.size(), 3u);
  EXPECT_EQ(p.tracks[0].kind, TrackKind::Video);
  EXPECT_EQ(p.tracks[1].kind, TrackKind::Audio);
  EXPECT_EQ(p.tracks[2].kind, TrackKind::Audio);
  EXPECT_TRUE(p.media.empty());
  EXPECT_EQ(p.canvas_w, 1920);
  EXPECT_EQ(p.canvas_h, 1080);
  EXPECT_DOUBLE_EQ(p.fps, 30.0);
}

TEST(EmptyProject, HonoursACustomTrackCount) {
  const Project p = empty_project(2, 1);
  ASSERT_EQ(p.tracks.size(), 3u);
  EXPECT_EQ(p.tracks[0].kind, TrackKind::Video);
  EXPECT_EQ(p.tracks[1].kind, TrackKind::Video);
  EXPECT_EQ(p.tracks[2].kind, TrackKind::Audio);
}

}  // namespace
}  // namespace cutline::core
