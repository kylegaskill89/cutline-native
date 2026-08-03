#include "cutline/core/edit.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

using Ids = std::vector<std::string>;

/// One video track and two audio lanes, all empty.
Project empty_project() {
  Project p;
  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  Track a1;
  a1.id = "a1";
  a1.kind = TrackKind::Audio;
  Track a2;
  a2.id = "a2";
  a2.kind = TrackKind::Audio;
  p.tracks = {v, a1, a2};
  return p;
}

/// Ten seconds of footage with video and two audio streams.
Media footage() {
  Media m;
  m.id = "m1";
  m.name = "footage.mp4";
  m.duration = 10.0;
  m.has_video = true;
  m.audio_stream_count = 2;
  return m;
}

/// A single clip on one video track, source [5, 10) at timeline `start`, drawn
/// from a twenty-second media so both handles exist.
Project one_clip_project(double start = 0.0) {
  Project p;
  Media m;
  m.id = "m1";
  m.duration = 20.0;
  m.has_video = true;
  p.media = {m};

  Clip c;
  c.id = "c1";
  c.media_id = "m1";
  c.source_in = 5.0;
  c.source_out = 10.0;
  c.start = start;

  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.clips = {c};
  p.tracks = {v};
  return p;
}

const Track& track_by_id(const Project& p, std::string_view id) {
  for (const Track& t : p.tracks) {
    if (t.id == id) return t;
  }
  throw std::runtime_error("no such track");
}

Track& track_by_id_mut(Project& p, std::string_view id) {
  for (Track& t : p.tracks) {
    if (t.id == id) return t;
  }
  throw std::runtime_error("no such track");
}

// ------------------------------------------------------------------ media --

TEST(AddMedia, AppendsToThePool) {
  const Project p = add_media(empty_project(), footage());
  ASSERT_EQ(p.media.size(), 1u);
  EXPECT_EQ(p.media[0].id, "m1");
}

TEST(PlacedLength, UsesTheRangeForFootage) {
  const Media m = footage();
  EXPECT_DOUBLE_EQ(placed_length(m), 10.0);
  EXPECT_DOUBLE_EQ(placed_length(m, PlacementRange{.in = 2.0, .out = 5.0}), 3.0);
  // Clamped to the media's own bounds.
  EXPECT_DOUBLE_EQ(placed_length(m, PlacementRange{.in = -4.0, .out = 99.0}), 10.0);
}

TEST(PlacedLength, StillsIgnoreTheRange) {
  Media image;
  image.is_image = true;
  image.duration = 5.0;
  EXPECT_DOUBLE_EQ(placed_length(image, PlacementRange{.in = 1.0, .out = 2.0}), 5.0);
}

// -------------------------------------------------------------- placement --

TEST(PlaceMedia, PlacesVideoAndOneClipPerAudioStream) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);

  ASSERT_EQ(track_by_id(p, "v1").clips.size(), 1u);
  ASSERT_EQ(track_by_id(p, "a1").clips.size(), 1u);
  ASSERT_EQ(track_by_id(p, "a2").clips.size(), 1u);

  EXPECT_EQ(track_by_id(p, "a1").clips[0].audio_stream, 0);
  EXPECT_EQ(track_by_id(p, "a2").clips[0].audio_stream, 1);
  EXPECT_DOUBLE_EQ(track_by_id(p, "v1").clips[0].source_out, 10.0);
}

TEST(PlaceMedia, EverythingPlacedSharesOneGroup) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);

  const std::optional<std::string>& group = track_by_id(p, "v1").clips[0].group_id;
  ASSERT_TRUE(group.has_value());
  EXPECT_EQ(track_by_id(p, "a1").clips[0].group_id, group);
  EXPECT_EQ(track_by_id(p, "a2").clips[0].group_id, group);
  EXPECT_EQ(group_members(p, track_by_id(p, "v1").clips[0].id).size(), 3u);
}

// The rule that was a real bug: streams must not pile onto one lane.
TEST(PlaceMedia, EachStreamTakesALaneOfItsOwn) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);

  EXPECT_EQ(track_by_id(p, "a1").clips.size(), 1u);
  EXPECT_EQ(track_by_id(p, "a2").clips.size(), 1u);
}

TEST(PlaceMedia, TheSoundGoesWhereThePictureWentEvenIfTheLaneIsBusy) {
  // The bug this replaced a rule to fix. Video is pushed onto its track
  // whatever is already there, and the audio used to go hunting for a lane that
  // was free — so a second placement over the first left two clips overlapping
  // on V1 and their sound on lanes nowhere near it.
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  p = place_media(std::move(p), "m1", 0.0);  // the same span, twice

  EXPECT_EQ(p.tracks.size(), 3u) << "no lanes were invented";
  EXPECT_EQ(track_by_id(p, "v1").clips.size(), 2u);
  EXPECT_EQ(track_by_id(p, "a1").clips.size(), 2u);
  EXPECT_EQ(track_by_id(p, "a2").clips.size(), 2u);
}

TEST(PlaceMedia, TheSecondVideoLanesSoundLandsOnTheSecondAudioLane) {
  // V1 with A1 and V2 with A2, which is the pairing every editor shows and the
  // one anybody reading a timeline assumes.
  Project p = empty_project();
  Track v2{.id = "v2", .kind = TrackKind::Video};
  p.tracks.insert(p.tracks.begin(), v2);  // stored topmost first, so V2 is V1's senior

  Media mono = footage();
  mono.audio_stream_count = 1;
  p = add_media(std::move(p), mono);

  p = place_media(std::move(p), "m1", 0.0, "v2");
  EXPECT_TRUE(track_by_id(p, "a1").clips.empty());
  EXPECT_EQ(track_by_id(p, "a2").clips.size(), 1u);
}

TEST(PlaceMedia, LanesAreMadeWhenThereAreTooFew) {
  Project p;
  Track v{.id = "v1", .kind = TrackKind::Video};
  p.tracks = {v};  // no audio lanes at all
  p = add_media(std::move(p), footage());

  p = place_media(std::move(p), "m1", 0.0);
  std::size_t lanes = 0;
  for (const Track& t : p.tracks) {
    if (t.kind == TrackKind::Audio) ++lanes;
  }
  EXPECT_EQ(lanes, 2u) << "one per stream";
}

TEST(PlaceMedia, TwoPlacementsApartShareTheSameLanes) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  p = place_media(std::move(p), "m1", 20.0);

  EXPECT_EQ(p.tracks.size(), 3u);
  EXPECT_EQ(track_by_id(p, "a1").clips.size(), 2u);
  EXPECT_EQ(track_by_id(p, "a2").clips.size(), 2u);
}

TEST(PlaceMedia, HonoursASourceRange) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0, {}, PlacementRange{.in = 2.0, .out = 5.0});

  const Clip& c = track_by_id(p, "v1").clips[0];
  EXPECT_DOUBLE_EQ(c.source_in, 2.0);
  EXPECT_DOUBLE_EQ(c.source_out, 5.0);
  EXPECT_DOUBLE_EQ(clip_end(c), 3.0);
}

TEST(PlaceMedia, TargetsAChosenVideoTrack) {
  Project p = empty_project();
  Track v2;
  v2.id = "v2";
  v2.kind = TrackKind::Video;
  p.tracks.insert(p.tracks.begin() + 1, v2);
  p = add_media(std::move(p), footage());

  p = place_media(std::move(p), "m1", 0.0, "v2");
  EXPECT_TRUE(track_by_id(p, "v1").clips.empty());
  EXPECT_EQ(track_by_id(p, "v2").clips.size(), 1u);
}

TEST(PlaceMedia, UnknownMediaIsANoOp) {
  const Project before = add_media(empty_project(), footage());
  const Project after = place_media(before, "nope", 0.0);
  EXPECT_EQ(before, after);
}

// ----------------------------------------------------------------- splits --

TEST(SplitAt, CutsASpanningClipInTwo) {
  Project p = one_clip_project();
  p = split_at(std::move(p), 2.0, Ids{"c1"});

  const Track& v = track_by_id(p, "v1");
  ASSERT_EQ(v.clips.size(), 2u);
  EXPECT_DOUBLE_EQ(v.clips[0].start, 0.0);
  EXPECT_DOUBLE_EQ(v.clips[0].source_in, 5.0);
  EXPECT_DOUBLE_EQ(v.clips[0].source_out, 7.0);
  EXPECT_DOUBLE_EQ(v.clips[1].start, 2.0);
  EXPECT_DOUBLE_EQ(v.clips[1].source_in, 7.0);
  EXPECT_DOUBLE_EQ(v.clips[1].source_out, 10.0);
}

TEST(SplitAt, IgnoresClipsTheCutDoesNotSpan) {
  const Project before = one_clip_project();
  EXPECT_EQ(split_at(before, 0.0, Ids{"c1"}).tracks[0].clips.size(), 1u);   // at the start
  EXPECT_EQ(split_at(before, 5.0, Ids{"c1"}).tracks[0].clips.size(), 1u);   // at the end
  EXPECT_EQ(split_at(before, 99.0, Ids{"c1"}).tracks[0].clips.size(), 1u);  // beyond
}

TEST(SplitAt, OnlyCutsListedClips) {
  Project p = one_clip_project();
  p = split_at(std::move(p), 2.0, Ids{"other"});
  EXPECT_EQ(p.tracks[0].clips.size(), 1u);
}

// A reversed clip plays its source backwards, so the left piece keeps the
// *later* source range and the right piece takes the earlier one.
TEST(SplitAt, IsReverseAware) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].reverse = true;
  p = split_at(std::move(p), 2.0, Ids{"c1"});

  const Track& v = track_by_id(p, "v1");
  ASSERT_EQ(v.clips.size(), 2u);
  EXPECT_DOUBLE_EQ(v.clips[0].source_in, 8.0);
  EXPECT_DOUBLE_EQ(v.clips[0].source_out, 10.0);
  EXPECT_DOUBLE_EQ(v.clips[1].source_in, 5.0);
  EXPECT_DOUBLE_EQ(v.clips[1].source_out, 8.0);
}

TEST(SplitAt, RightPiecesFormTheirOwnGroup) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  const std::string video_id = track_by_id(p, "v1").clips[0].id;
  const Ids members{group_members(p, video_id)};

  p = split_at(std::move(p), 4.0, members);

  const Clip& left = track_by_id(p, "v1").clips[0];
  const Clip& right = track_by_id(p, "v1").clips[1];
  ASSERT_TRUE(left.group_id.has_value());
  ASSERT_TRUE(right.group_id.has_value());
  EXPECT_NE(*left.group_id, *right.group_id);
  // The right-hand halves stay linked to each other across tracks.
  EXPECT_EQ(group_members(p, right.id).size(), 3u);
}

// A cut is not an edge of the original clip, so the right piece's animation
// must travel with its new origin. The reference left keyframes at their
// original offsets, shifting the animation by the length of the left piece.
TEST(SplitAt, RebasesKeyframesOntoTheNewOrigin) {
  Project p = one_clip_project();
  Clip& c = p.tracks[0].clips[0];
  // x ramps 0 -> 1 across the clip's five seconds.
  c.keyframes[anim_prop_index(AnimProp::X)] = {{.t = 0.0, .v = 0.0}, {.t = 4.0, .v = 1.0}};

  p = split_at(std::move(p), 2.0, Ids{"c1"});
  const Clip& right = p.tracks[0].clips[1];

  const std::vector<Keyframe>& kfs = right.keyframes[anim_prop_index(AnimProp::X)];
  ASSERT_EQ(kfs.size(), 2u);
  EXPECT_DOUBLE_EQ(kfs[0].t, -2.0);
  EXPECT_DOUBLE_EQ(kfs[1].t, 2.0);

  // The same timeline moment still yields the same value across the cut.
  EXPECT_DOUBLE_EQ(animated_value(right, AnimProp::X, 3.0 - right.start), 0.75);
}

TEST(SplitAt, RebasesGainAndEffectKeyframes) {
  Project p = one_clip_project();
  Clip& c = p.tracks[0].clips[0];
  c.gain_keyframes = {{.t = 0.0, .v = 0.0}, {.t = 4.0, .v = 1.0}};
  ClipEffect blur;
  blur.type = "blur";
  blur.keyframes["amount"] = {{.t = 1.0, .v = 10.0}};
  c.effects = {blur};

  p = split_at(std::move(p), 2.0, Ids{"c1"});
  const Clip& right = p.tracks[0].clips[1];

  EXPECT_DOUBLE_EQ(right.gain_keyframes[0].t, -2.0);
  EXPECT_DOUBLE_EQ(right.effects[0].keyframes.at("amount")[0].t, -1.0);
  // The left piece keeps its origin, so its keyframes do not move.
  EXPECT_DOUBLE_EQ(p.tracks[0].clips[0].gain_keyframes[0].t, 0.0);
}

TEST(SplitAt, FadesStayOnTheEdgesThatOwnThem) {
  Project p = one_clip_project();
  Clip& c = p.tracks[0].clips[0];
  c.fade_in = 1.0;
  c.fade_out = 1.5;

  p = split_at(std::move(p), 2.0, Ids{"c1"});

  EXPECT_DOUBLE_EQ(p.tracks[0].clips[0].fade_in, 1.0);
  EXPECT_DOUBLE_EQ(p.tracks[0].clips[0].fade_out, 0.0);  // no fade at the cut
  EXPECT_DOUBLE_EQ(p.tracks[0].clips[1].fade_in, 0.0);
  EXPECT_DOUBLE_EQ(p.tracks[0].clips[1].fade_out, 1.5);
}

TEST(SplitAt, TheOutTransitionFollowsTheOutEdge) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].transition_out =
      Transition{.kind = TransitionKind::Dissolve, .duration = 1.0};

  p = split_at(std::move(p), 2.0, Ids{"c1"});

  EXPECT_FALSE(p.tracks[0].clips[0].transition_out.has_value());
  ASSERT_TRUE(p.tracks[0].clips[1].transition_out.has_value());
  EXPECT_DOUBLE_EQ(p.tracks[0].clips[1].transition_out->duration, 1.0);
}

TEST(SplitAt, UnlinkedClipsProduceUnlinkedPieces) {
  Project p = one_clip_project();
  p = split_at(std::move(p), 2.0, Ids{"c1"});
  EXPECT_FALSE(p.tracks[0].clips[0].group_id.has_value());
  EXPECT_FALSE(p.tracks[0].clips[1].group_id.has_value());
}

// ------------------------------------------------------------------ moves --

TEST(MoveClips, ShiftsAlongTheTimeline) {
  Project p = one_clip_project(2.0);
  p = move_clips(std::move(p), Ids{"c1"}, 3.0);
  EXPECT_DOUBLE_EQ(p.tracks[0].clips[0].start, 5.0);
}

TEST(MoveClips, ClampsTheWholeSetAgainstZero) {
  Project p = one_clip_project(2.0);
  p = move_clips(std::move(p), Ids{"c1"}, -10.0);
  EXPECT_DOUBLE_EQ(p.tracks[0].clips[0].start, 0.0);
}

TEST(MoveClips, MovesBetweenTracksOfTheSameKind) {
  Project p = one_clip_project();
  Track v2;
  v2.id = "v2";
  v2.kind = TrackKind::Video;
  p.tracks.push_back(v2);

  p = move_clips(std::move(p), Ids{"c1"}, 0.0, 1);
  EXPECT_TRUE(track_by_id(p, "v1").clips.empty());
  EXPECT_EQ(track_by_id(p, "v2").clips.size(), 1u);
}

TEST(MoveClips, StaysPutAtTheEdgeOfTheTrackStack) {
  Project p = one_clip_project();
  p = move_clips(std::move(p), Ids{"c1"}, 0.0, -1);  // already the top video track
  EXPECT_EQ(track_by_id(p, "v1").clips.size(), 1u);
}

// Dragging a video clip upward must not also shuffle its linked audio between
// audio lanes, which is what restrict_kind exists to prevent.
TEST(MoveClips, RestrictKindLimitsVerticalMovement) {
  // V2 above V1, which is the order tracks are stored in — topmost first — and
  // the order the lane names assume. Placed explicitly on V1, so its sound
  // lands on A1 and A2 and the move can be watched leaving them alone.
  Project p = empty_project();
  Track v2;
  v2.id = "v2";
  v2.kind = TrackKind::Video;
  p.tracks.insert(p.tracks.begin(), v2);
  p = add_media(std::move(p), footage());
  p = place_media(std::move(p), "m1", 0.0, "v1");

  const std::string video_id = track_by_id(p, "v1").clips[0].id;
  const Ids members{group_members(p, video_id)};
  p = move_clips(std::move(p), members, 0.0, -1, TrackKind::Video);

  EXPECT_TRUE(track_by_id(p, "v1").clips.empty());
  EXPECT_EQ(track_by_id(p, "v2").clips.size(), 1u);
  EXPECT_EQ(track_by_id(p, "a1").clips.size(), 1u);  // audio stayed
  EXPECT_EQ(track_by_id(p, "a2").clips.size(), 1u);
}

TEST(MoveClips, UnknownClipsAreANoOp) {
  const Project before = one_clip_project();
  EXPECT_EQ(move_clips(before, Ids{"nope"}, 5.0), before);
}

// ------------------------------------------------------------------ trims --

TEST(SetClipEdge, TrimmingTheInEdgeConsumesSource) {
  Project p = one_clip_project();
  p = set_clip_edge(std::move(p), "c1", ClipEdge::In, 1.0);

  const Clip& c = p.tracks[0].clips[0];
  EXPECT_DOUBLE_EQ(c.start, 1.0);
  EXPECT_DOUBLE_EQ(c.source_in, 6.0);
  EXPECT_DOUBLE_EQ(c.source_out, 10.0);
}

TEST(SetClipEdge, ExtendingTheInEdgeIsBoundedBySource) {
  Project p = one_clip_project(3.0);
  p.tracks[0].clips[0].source_in = 1.0;  // only one second of head available
  p = set_clip_edge(std::move(p), "c1", ClipEdge::In, 0.0);

  const Clip& c = p.tracks[0].clips[0];
  EXPECT_DOUBLE_EQ(c.start, 2.0);
  EXPECT_DOUBLE_EQ(c.source_in, 0.0);
}

TEST(SetClipEdge, TheInEdgeCannotCrossZero) {
  Project p = one_clip_project(1.0);
  p = set_clip_edge(std::move(p), "c1", ClipEdge::In, -5.0);
  EXPECT_DOUBLE_EQ(p.tracks[0].clips[0].start, 0.0);
}

TEST(SetClipEdge, ExtendingTheOutEdgeConsumesTailSource) {
  Project p = one_clip_project();
  p = set_clip_edge(std::move(p), "c1", ClipEdge::Out, 8.0);

  const Clip& c = p.tracks[0].clips[0];
  EXPECT_DOUBLE_EQ(c.source_out, 13.0);
  EXPECT_DOUBLE_EQ(clip_end(c), 8.0);
}

TEST(SetClipEdge, TheOutEdgeStopsAtTheNextClip) {
  Project p = one_clip_project();
  Clip neighbour;
  neighbour.id = "c2";
  neighbour.media_id = "m1";
  neighbour.source_in = 0.0;
  neighbour.source_out = 2.0;
  neighbour.start = 6.0;
  p.tracks[0].clips.push_back(neighbour);

  p = set_clip_edge(std::move(p), "c1", ClipEdge::Out, 99.0);
  EXPECT_DOUBLE_EQ(clip_end(p.tracks[0].clips[0]), 6.0);
}

TEST(SetClipEdge, TrimIsBoundedByTheMinimumClipLength) {
  Project p = one_clip_project();
  p = set_clip_edge(std::move(p), "c1", ClipEdge::Out, 0.0);
  // The clamp reaches the limit through `source_out + (kMinClip - duration)`,
  // so the result carries a few ULP of cancellation error rather than landing
  // exactly on kMinClip.
  EXPECT_NEAR(clip_duration(p.tracks[0].clips[0]), kMinClip, 1e-12);
}

TEST(SetClipEdge, TheWholeLinkedGroupTrimsTogether) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  const std::string video_id = track_by_id(p, "v1").clips[0].id;

  p = set_clip_edge(std::move(p), video_id, ClipEdge::In, 2.0);

  EXPECT_DOUBLE_EQ(track_by_id(p, "v1").clips[0].start, 2.0);
  EXPECT_DOUBLE_EQ(track_by_id(p, "a1").clips[0].start, 2.0);
  EXPECT_DOUBLE_EQ(track_by_id(p, "a2").clips[0].start, 2.0);
  EXPECT_DOUBLE_EQ(track_by_id(p, "a1").clips[0].source_in, 2.0);
}

// Reverse anchors the timeline edges to the opposite source edges.
TEST(SetClipEdge, IsReverseAware) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].reverse = true;
  p = set_clip_edge(std::move(p), "c1", ClipEdge::In, 1.0);

  const Clip& c = p.tracks[0].clips[0];
  EXPECT_DOUBLE_EQ(c.start, 1.0);
  EXPECT_DOUBLE_EQ(c.source_in, 5.0);
  EXPECT_DOUBLE_EQ(c.source_out, 9.0);
}

TEST(SetClipEdge, UnknownClipIsANoOp) {
  const Project before = one_clip_project();
  EXPECT_EQ(set_clip_edge(before, "nope", ClipEdge::In, 1.0), before);
}

// --------------------------------------------------------------- deletion --

TEST(RemoveClips, LeavesAGap) {
  Project p = one_clip_project();
  p = remove_clips(std::move(p), Ids{"c1"});
  EXPECT_TRUE(p.tracks[0].clips.empty());
}

TEST(RippleDelete, ClosesTheGapAcrossEveryTrack) {
  Project p = one_clip_project();
  Clip second;
  second.id = "c2";
  second.media_id = "m1";
  second.source_in = 0.0;
  second.source_out = 5.0;
  second.start = 5.0;
  Clip third = second;
  third.id = "c3";
  third.start = 10.0;
  p.tracks[0].clips.push_back(second);
  p.tracks[0].clips.push_back(third);

  p = ripple_delete(std::move(p), Ids{"c2"});

  const Track& v = p.tracks[0];
  ASSERT_EQ(v.clips.size(), 2u);
  EXPECT_EQ(v.clips[0].id, "c1");
  EXPECT_DOUBLE_EQ(v.clips[0].start, 0.0);
  EXPECT_EQ(v.clips[1].id, "c3");
  EXPECT_DOUBLE_EQ(v.clips[1].start, 5.0);  // pulled left by the removed span
}

TEST(RippleDelete, TakesTheWholeLinkedGroup) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  const std::string video_id = track_by_id(p, "v1").clips[0].id;

  p = ripple_delete(std::move(p), Ids{video_id});

  for (const Track& t : p.tracks) EXPECT_TRUE(t.clips.empty());
}

TEST(RippleDelete, UnknownClipsAreANoOp) {
  const Project before = one_clip_project();
  EXPECT_EQ(ripple_delete(before, Ids{"nope"}), before);
}

// ---------------------------------------------------------------- linking --

TEST(Linking, LinkJoinsClipsIntoOneGroup) {
  Project p = one_clip_project();
  Clip other;
  other.id = "c2";
  other.media_id = "m1";
  other.source_out = 2.0;
  other.start = 6.0;
  p.tracks[0].clips.push_back(other);

  p = link_clips(std::move(p), Ids{"c1", "c2"});
  EXPECT_EQ(group_members(p, "c1").size(), 2u);
}

TEST(Linking, LinkingFewerThanTwoClipsIsANoOp) {
  const Project before = one_clip_project();
  EXPECT_EQ(link_clips(before, Ids{"c1"}), before);
  EXPECT_EQ(link_clips(before, Ids{}), before);
}

TEST(Linking, UnlinkClipsDetachesOnlyTheNamedOnes) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  const std::string audio_id = track_by_id(p, "a1").clips[0].id;

  p = unlink_clips(std::move(p), Ids{audio_id});
  EXPECT_EQ(group_members(p, audio_id).size(), 1u);
  EXPECT_EQ(group_members(p, track_by_id(p, "v1").clips[0].id).size(), 2u);
}

TEST(Linking, UnlinkGroupDetachesEveryMember) {
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  const std::string video_id = track_by_id(p, "v1").clips[0].id;

  p = unlink_group(std::move(p), video_id);
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) EXPECT_FALSE(c.group_id.has_value());
  }
}

TEST(Linking, UnlinkGroupOnAnUnlinkedClipIsANoOp) {
  const Project before = one_clip_project();
  EXPECT_EQ(unlink_group(before, "c1"), before);
}

// --------------------------------------------------------------------- ids --

TEST(NewId, IsMonotonicAndResettable) {
  reset_ids();
  EXPECT_EQ(new_id("clip"), "clip_1");
  EXPECT_EQ(new_id("clip"), "clip_2");
  EXPECT_EQ(new_id("grp"), "grp_3");
  reset_ids();
  EXPECT_EQ(new_id("clip"), "clip_1");
}

// ---------------------------------------------------------- sync lock --

TEST(SyncLock, APinnedTrackDoesNotMoveWhenSomethingElseRipples) {
  // What sync lock is for: a music bed, a title at a fixed time, a bar of tone
  // at the head — an edit elsewhere opens the sequence up and these hold.
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  track_by_id_mut(p, "a2").sync_locked = false;

  const double pinned = track_by_id(p, "a2").clips[0].start;
  p = ripple_insert(std::move(p), 0.0, 5.0);

  EXPECT_DOUBLE_EQ(track_by_id(p, "v1").clips[0].start, 5.0);
  EXPECT_DOUBLE_EQ(track_by_id(p, "a1").clips[0].start, 5.0);
  EXPECT_DOUBLE_EQ(track_by_id(p, "a2").clips[0].start, pinned);
}

TEST(SyncLock, IsOnByDefault) {
  // The ordinary case is that an insert opens the whole sequence and nothing
  // goes out of step with anything.
  const Track fresh;
  EXPECT_TRUE(fresh.sync_locked);
}

TEST(SyncLock, APinnedTracksOwnClipIsStillDeleted) {
  // Deleting something is not an edit *elsewhere*. What sync lock decides is
  // whether the rest of the track closes up behind it.
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  p = place_media(std::move(p), "m1", 20.0);
  track_by_id_mut(p, "a2").sync_locked = false;

  const std::string doomed = track_by_id(p, "a2").clips[0].id;
  const double later = track_by_id(p, "a2").clips[1].start;
  p = ripple_delete(std::move(p), Ids{doomed});

  ASSERT_EQ(track_by_id(p, "a2").clips.size(), 1u);
  EXPECT_DOUBLE_EQ(track_by_id(p, "a2").clips[0].start, later) << "and did not close up";
}

TEST(SyncLock, ARippleTrimStillTrimsThePinnedTracksOwnGroup) {
  // The clip being trimmed is what the gesture is on. Refusing to move it
  // would mean the trim silently did half of itself.
  Project p = add_media(empty_project(), footage());
  p = place_media(std::move(p), "m1", 0.0);
  track_by_id_mut(p, "a1").sync_locked = false;

  const std::string video_id = track_by_id(p, "v1").clips[0].id;
  p = ripple_trim_edge(std::move(p), video_id, ClipEdge::Out, 4.0);

  EXPECT_DOUBLE_EQ(clip_end(track_by_id(p, "v1").clips[0]), 4.0);
  EXPECT_DOUBLE_EQ(clip_end(track_by_id(p, "a1").clips[0]), 4.0)
      << "its linked sound was trimmed with it";
}

}  // namespace
}  // namespace cutline::core
