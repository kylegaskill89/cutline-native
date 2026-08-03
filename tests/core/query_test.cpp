#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::core {
namespace {

/// A four-second video clip drawn from source [2, 6), placed at timeline 10.
Clip make_clip(std::string id = "c1") {
  Clip c;
  c.id = std::move(id);
  c.media_id = "m1";
  c.kind = TrackKind::Video;
  c.source_in = 2.0;
  c.source_out = 6.0;
  c.start = 10.0;
  return c;
}

Track make_track(std::string id, TrackKind kind) {
  Track t;
  t.id = std::move(id);
  t.kind = kind;
  return t;
}

// ------------------------------------------------------------ clip timing --

TEST(ClipTiming, DurationFollowsSourceSpanAndSpeed) {
  Clip c = make_clip();
  EXPECT_DOUBLE_EQ(source_span(c), 4.0);
  EXPECT_DOUBLE_EQ(clip_duration(c), 4.0);
  EXPECT_DOUBLE_EQ(clip_end(c), 14.0);

  c.speed = 2.0;
  EXPECT_DOUBLE_EQ(clip_duration(c), 2.0);
  EXPECT_DOUBLE_EQ(clip_end(c), 12.0);

  c.speed = 0.5;
  EXPECT_DOUBLE_EQ(clip_duration(c), 8.0);
  EXPECT_DOUBLE_EQ(clip_end(c), 18.0);
}

TEST(ClipTiming, SpeedIsGuardedPositive) {
  Clip c = make_clip();
  c.speed = 0.0;
  EXPECT_DOUBLE_EQ(clip_speed(c), 1.0);
  c.speed = -3.0;
  EXPECT_DOUBLE_EQ(clip_speed(c), 1.0);
}

TEST(ClipTiming, SourceTimeAdvancesWithTheTimeline) {
  const Clip c = make_clip();
  EXPECT_DOUBLE_EQ(source_time_at(c, 10.0), 2.0);
  EXPECT_DOUBLE_EQ(source_time_at(c, 12.0), 4.0);
  EXPECT_DOUBLE_EQ(source_time_at(c, 14.0), 6.0);
}

TEST(ClipTiming, ReverseWalksTheSourceBackwards) {
  Clip c = make_clip();
  c.reverse = true;
  EXPECT_DOUBLE_EQ(source_time_at(c, 10.0), 6.0);
  EXPECT_DOUBLE_EQ(source_time_at(c, 12.0), 4.0);
  EXPECT_DOUBLE_EQ(source_time_at(c, 14.0), 2.0);
}

TEST(ClipTiming, SpeedScalesSourceConsumption) {
  Clip c = make_clip();
  c.speed = 2.0;
  EXPECT_DOUBLE_EQ(source_time_at(c, 11.0), 4.0);
}

TEST(SubSource, MapsATimelineWindowBackToSource) {
  const Clip c = make_clip();
  EXPECT_EQ(clip_sub_source(c, 10.0, 11.0), (SourceRange{2.0, 3.0}));
  EXPECT_EQ(clip_sub_source(c, 11.0, 13.0), (SourceRange{3.0, 5.0}));
}

// Reverse swaps which end of the source a timeline window maps to, which is
// what keeps a split of a reversed clip frame-correct.
TEST(SubSource, IsReverseAware) {
  Clip c = make_clip();
  c.reverse = true;
  EXPECT_EQ(clip_sub_source(c, 10.0, 11.0), (SourceRange{5.0, 6.0}));
  EXPECT_EQ(clip_sub_source(c, 13.0, 14.0), (SourceRange{2.0, 3.0}));
}

TEST(SubSource, AccountsForSpeed) {
  Clip c = make_clip();
  c.speed = 2.0;
  EXPECT_EQ(clip_sub_source(c, 10.0, 11.0), (SourceRange{2.0, 4.0}));
}

TEST(TimelineDuration, IsTheFurthestClipEndAcrossTracks) {
  Project p;
  EXPECT_DOUBLE_EQ(timeline_duration(p), 0.0);

  Track v = make_track("v1", TrackKind::Video);
  v.clips.push_back(make_clip("a"));  // ends at 14

  Track a = make_track("a1", TrackKind::Audio);
  Clip late = make_clip("b");
  late.start = 20.0;  // ends at 24
  a.clips.push_back(late);

  p.tracks = {v, a};
  EXPECT_DOUBLE_EQ(timeline_duration(p), 24.0);
}

// -------------------------------------------------------------- animation --

TEST(Animation, StaticTransformIsUsedWithoutKeyframes) {
  Clip c = make_clip();
  c.transform = {.x = 0.25, .y = 0.75, .scale_x = 2.0, .scale_y = 3.0, .rotation = 45.0};
  const Transform tr = animated_transform(c, 1.0);
  EXPECT_EQ(tr, c.transform);
}

TEST(Animation, KeyframesOverrideOnlyTheirOwnProperty) {
  Clip c = make_clip();
  c.transform.x = 0.25;
  c.transform.y = 0.75;
  c.keyframes[anim_prop_index(AnimProp::X)] = {{.t = 0.0, .v = 0.0}, {.t = 2.0, .v = 1.0}};

  const Transform tr = animated_transform(c, 1.0);
  EXPECT_DOUBLE_EQ(tr.x, 0.5);   // animated
  EXPECT_DOUBLE_EQ(tr.y, 0.75);  // untouched static value
  EXPECT_TRUE(is_animated(c, AnimProp::X));
  EXPECT_FALSE(is_animated(c, AnimProp::Y));
}

TEST(Animation, OpacityIsClampedToUnitRange) {
  Clip c = make_clip();
  c.keyframes[anim_prop_index(AnimProp::Opacity)] = {{.t = 0.0, .v = -5.0},
                                                     {.t = 2.0, .v = 7.0}};
  EXPECT_DOUBLE_EQ(animated_opacity(c, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(animated_opacity(c, 2.0), 1.0);
}

TEST(Animation, StaticOpacityPassesThroughUnclamped) {
  Clip c = make_clip();
  c.opacity = 0.4;
  EXPECT_DOUBLE_EQ(animated_opacity(c, 1.0), 0.4);
}

TEST(Animation, AnimatedValueDispatchesPerProperty) {
  Clip c = make_clip();
  c.transform = {.x = 0.1, .y = 0.2, .scale_x = 0.3, .scale_y = 0.4, .rotation = 5.0};
  c.opacity = 0.6;
  EXPECT_DOUBLE_EQ(animated_value(c, AnimProp::X, 0.0), 0.1);
  EXPECT_DOUBLE_EQ(animated_value(c, AnimProp::Y, 0.0), 0.2);
  EXPECT_DOUBLE_EQ(animated_value(c, AnimProp::ScaleX, 0.0), 0.3);
  EXPECT_DOUBLE_EQ(animated_value(c, AnimProp::ScaleY, 0.0), 0.4);
  EXPECT_DOUBLE_EQ(animated_value(c, AnimProp::Rotation, 0.0), 5.0);
  EXPECT_DOUBLE_EQ(animated_value(c, AnimProp::Opacity, 0.0), 0.6);
}

// ------------------------------------------------------------------ audio --

TEST(Gain, ConstantGainWithoutAutomation) {
  Clip c = make_clip();
  c.gain = 0.5;
  EXPECT_FALSE(is_gain_animated(c));
  EXPECT_DOUBLE_EQ(gain_at(c, 1.0), 0.5);
}

TEST(Gain, AutomationOverridesConstantAndClamps) {
  Clip c = make_clip();
  c.gain = 0.5;
  c.gain_keyframes = {{.t = 0.0, .v = -1.0}, {.t = 2.0, .v = 10.0}};
  EXPECT_TRUE(is_gain_animated(c));
  EXPECT_DOUBLE_EQ(gain_at(c, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(gain_at(c, 2.0), kMaxGain);
}

TEST(TrackAudibility, MutedTracksAreSilent) {
  Project p;
  Track a = make_track("a1", TrackKind::Audio);
  a.muted = true;
  p.tracks = {a};
  EXPECT_FALSE(is_track_audible(p, p.tracks[0]));
}

TEST(TrackAudibility, WithoutSoloEveryUnmutedTrackPlays) {
  Project p;
  p.tracks = {make_track("a1", TrackKind::Audio), make_track("a2", TrackKind::Audio)};
  EXPECT_TRUE(is_track_audible(p, p.tracks[0]));
  EXPECT_TRUE(is_track_audible(p, p.tracks[1]));
}

TEST(TrackAudibility, SoloSilencesEveryOtherTrack) {
  Project p;
  Track a1 = make_track("a1", TrackKind::Audio);
  Track a2 = make_track("a2", TrackKind::Audio);
  a2.solo = true;
  p.tracks = {a1, a2};
  EXPECT_FALSE(is_track_audible(p, p.tracks[0]));
  EXPECT_TRUE(is_track_audible(p, p.tracks[1]));
}

TEST(TrackAudibility, MuteBeatsSoloOnTheSameTrack) {
  Project p;
  Track a = make_track("a1", TrackKind::Audio);
  a.solo = true;
  a.muted = true;
  p.tracks = {a};
  EXPECT_FALSE(is_track_audible(p, p.tracks[0]));
}

// Only audio solo suppresses other audio; a soloed video track is irrelevant.
TEST(TrackAudibility, VideoSoloDoesNotSuppressAudio) {
  Project p;
  Track v = make_track("v1", TrackKind::Video);
  v.solo = true;
  p.tracks = {v, make_track("a1", TrackKind::Audio)};
  EXPECT_TRUE(is_track_audible(p, p.tracks[1]));
}

// ---------------------------------------------------------------- lookups --

Project two_track_project() {
  Project p;
  Track v = make_track("v1", TrackKind::Video);
  v.clips.push_back(make_clip("c1"));
  Track a = make_track("a1", TrackKind::Audio);
  a.clips.push_back(make_clip("c2"));
  p.tracks = {v, a};
  return p;
}

TEST(Lookups, FindClipSearchesEveryTrack) {
  const Project p = two_track_project();
  ASSERT_NE(find_clip(p, "c2"), nullptr);
  EXPECT_EQ(find_clip(p, "c2")->id, "c2");
  EXPECT_EQ(find_clip(p, "nope"), nullptr);
}

TEST(Lookups, TrackOfClipIdentifiesTheOwner) {
  const Project p = two_track_project();
  ASSERT_NE(track_of_clip(p, "c2"), nullptr);
  EXPECT_EQ(track_of_clip(p, "c2")->id, "a1");
  EXPECT_EQ(track_of_clip(p, "nope"), nullptr);
}

TEST(Lookups, UnlinkedClipIsItsOwnGroup) {
  const Project p = two_track_project();
  EXPECT_EQ(group_members(p, "c1"), std::vector<std::string>{"c1"});
}

TEST(Lookups, LinkedClipsReportEveryMember) {
  Project p = two_track_project();
  p.tracks[0].clips[0].group_id = "g1";
  p.tracks[1].clips[0].group_id = "g1";
  const std::vector<std::string> expected{"c1", "c2"};
  EXPECT_EQ(group_members(p, "c1"), expected);
  EXPECT_EQ(group_members(p, "c2"), expected);
}

TEST(Lookups, GroupOfAnUnknownClipIsEmpty) {
  const Project p = two_track_project();
  EXPECT_TRUE(group_members(p, "nope").empty());
}

// The clip's own end is exclusive, so an abutting clip owns the boundary.
TEST(Lookups, ClipAtTimeIsHalfOpen) {
  Track t = make_track("v1", TrackKind::Video);
  t.clips.push_back(make_clip("c1"));  // [10, 14)

  EXPECT_EQ(clip_at_time(t, 9.9), nullptr);
  ASSERT_NE(clip_at_time(t, 10.0), nullptr);
  EXPECT_EQ(clip_at_time(t, 10.0)->id, "c1");
  ASSERT_NE(clip_at_time(t, 13.999), nullptr);
  EXPECT_EQ(clip_at_time(t, 14.0), nullptr);
}

// ------------------------------------------------------------------ media --

TEST(MediaKinds, GeneratedMediaHaveNoSourceFile) {
  Media text;
  text.is_text = true;
  EXPECT_TRUE(is_generated_media(text));
  EXPECT_TRUE(is_still_like(text));

  Media image;
  image.is_image = true;
  EXPECT_FALSE(is_generated_media(image));  // an image is a real file
  EXPECT_TRUE(is_still_like(image));

  Media video;
  video.has_video = true;
  EXPECT_FALSE(is_generated_media(video));
  EXPECT_FALSE(is_still_like(video));
}

// ------------------------------------------------------------ time remap --

/// A five-second clip of a ten-second source, starting on the timeline at two.
[[nodiscard]] Clip remappable() {
  Clip c;
  c.id = "c";
  c.media_id = "m";
  c.start = 2.0;
  c.source_in = 0.0;
  c.source_out = 5.0;
  return c;
}

[[nodiscard]] std::vector<Keyframe>& speed_curve(Clip& c) {
  return c.keyframes[anim_prop_index(AnimProp::Speed)];
}

TEST(TimeRemap, AClipWithNoCurveIsNotRemapped) {
  Clip c = remappable();
  EXPECT_FALSE(is_time_remapped(c));
  c.speed = 2.0;
  EXPECT_FALSE(is_time_remapped(c)) << "fast is not the same as remapped";
  EXPECT_DOUBLE_EQ(speed_at(c, 1.0), 2.0);
  EXPECT_DOUBLE_EQ(source_offset_at(c, 1.0), 2.0);
}

TEST(TimeRemap, AFlatCurveMatchesAConstantSpeed) {
  // The integral of a constant is the constant times the time, and this is the
  // case that has to be exact rather than close: it is what every unremapped
  // clip in every project goes through.
  Clip c = remappable();
  speed_curve(c) = {{.t = 0.0, .v = 2.0}, {.t = 4.0, .v = 2.0}};

  EXPECT_TRUE(is_time_remapped(c));
  EXPECT_DOUBLE_EQ(speed_at(c, 1.5), 2.0);
  EXPECT_NEAR(source_offset_at(c, 1.0), 2.0, 1e-9);
  EXPECT_NEAR(source_offset_at(c, 2.5), 5.0, 1e-9);
}

TEST(TimeRemap, ARampConsumesTheAreaUnderIt) {
  // One to three over two seconds is a trapezoid: the average of the ends times
  // the width, which is four seconds of source in two of timeline.
  Clip c = remappable();
  speed_curve(c) = {{.t = 0.0, .v = 1.0}, {.t = 2.0, .v = 3.0}};

  EXPECT_NEAR(source_offset_at(c, 2.0), 4.0, 1e-6);
  // And halfway along it has consumed a quarter less than half of that: the
  // area of the smaller trapezoid, one to two over one second.
  EXPECT_NEAR(source_offset_at(c, 1.0), 1.5, 1e-6);
}

TEST(TimeRemap, ACurveCannotStopTheClipDead) {
  // A speed of zero would be a freeze, and the model has never had one: every
  // other path through it divides by the speed. So a curve is held to the same
  // floor a typed speed is, and a ramp aimed at zero arrives at a crawl rather
  // than at a stop.
  Clip c = remappable();
  speed_curve(c) = {{.t = 0.0, .v = 0.0}, {.t = 2.0, .v = 0.0}};

  EXPECT_DOUBLE_EQ(speed_at(c, 1.0), kMinSpeed);
  EXPECT_NEAR(source_offset_at(c, 2.0), 2.0 * kMinSpeed, 1e-9);
}

TEST(TimeRemap, ARampIsSlowAtTheStartAndFastAtTheEnd) {
  // The claim that makes the feature worth having, stated as the shape of the
  // curve rather than as one number.
  Clip c = remappable();
  speed_curve(c) = {{.t = 0.0, .v = 0.1}, {.t = 4.0, .v = 4.0}};

  const double first = source_offset_at(c, 1.0);
  const double last = source_offset_at(c, 4.0) - source_offset_at(c, 3.0);
  EXPECT_LT(first, last) << "the last second should consume far more source";
}

TEST(TimeRemap, TheOffsetOnlyEverGoesForwards) {
  // Speed is clamped to the same bounds a typed one is, so a curve that
  // overshoots below zero cannot wind the source backwards — which would be
  // reverse, and reverse is a different flag.
  Clip c = remappable();
  speed_curve(c) = {{.t = 0.0, .v = 1.0}, {.t = 2.0, .v = 0.0}, {.t = 4.0, .v = 1.0}};

  double previous = 0.0;
  for (double t = 0.0; t <= 4.0; t += 0.25) {
    const double now = source_offset_at(c, t);
    EXPECT_GE(now, previous - 1e-9) << "went backwards at " << t;
    previous = now;
  }
}

TEST(TimeRemap, SourceTimeFollowsTheCurveAndTheClipsPlaceOnTheTimeline) {
  Clip c = remappable();
  speed_curve(c) = {{.t = 0.0, .v = 1.0}, {.t = 2.0, .v = 3.0}};

  // Timeline 2.0 is clip-local zero: the first frame of the source.
  EXPECT_NEAR(source_time_at(c, 2.0), 0.0, 1e-9);
  EXPECT_NEAR(source_time_at(c, 4.0), 4.0, 1e-6);
}

TEST(TimeRemap, ReverseStillCountsBackFromTheOutPoint) {
  Clip c = remappable();
  c.reverse = true;
  speed_curve(c) = {{.t = 0.0, .v = 1.0}, {.t = 4.0, .v = 1.0}};

  EXPECT_NEAR(source_time_at(c, 2.0), 5.0, 1e-9) << "the out point, at the start";
  EXPECT_NEAR(source_time_at(c, 4.0), 3.0, 1e-6);
}

}  // namespace
}  // namespace cutline::core
