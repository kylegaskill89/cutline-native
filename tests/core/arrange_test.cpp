/// Tests for the arrangement tools: ripple insert, insert and overwrite
/// editing, rate stretch, slip, and slide.

#include "cutline/core/edit.hpp"

#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

using Ids = std::vector<std::string>;

Media media_of(std::string id, double duration, bool has_video = true) {
  Media m;
  m.id = std::move(id);
  m.duration = duration;
  m.has_video = has_video;
  return m;
}

Clip clip_of(std::string id, double start, double source_in, double source_out) {
  Clip c;
  c.id = std::move(id);
  c.media_id = "m1";
  c.start = start;
  c.source_in = source_in;
  c.source_out = source_out;
  return c;
}

/// One video track holding three abutting five-second clips over [0, 15),
/// each drawn from a twenty-second media so handles exist on both sides.
Project three_clip_project() {
  Project p;
  p.media = {media_of("m1", 20.0)};

  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.clips = {clip_of("a", 0.0, 0.0, 5.0), clip_of("b", 5.0, 5.0, 10.0),
             clip_of("c", 10.0, 10.0, 15.0)};
  p.tracks = {v};
  return p;
}

const Track& video(const Project& p) { return p.tracks[0]; }

// -------------------------------------------------------- layered moves --

/// Two video lanes and two audio lanes, with a two-stream media placed on the
/// lower video lane.
Project layered_project() {
  Project p;
  Media m;
  m.id = "m1";
  m.duration = 10.0;
  m.has_video = true;
  m.audio_stream_count = 2;
  p.media = {m};

  Track v1;
  v1.id = "v1";
  v1.kind = TrackKind::Video;
  Track v2;
  v2.id = "v2";
  v2.kind = TrackKind::Video;
  Track a1;
  a1.id = "a1";
  a1.kind = TrackKind::Audio;
  Track a2;
  a2.id = "a2";
  a2.kind = TrackKind::Audio;
  p.tracks = {v1, v2, a1, a2};
  return p;
}

std::size_t audio_lane_count(const Project& p) {
  std::size_t n = 0;
  for (const Track& t : p.tracks) {
    if (t.kind == TrackKind::Audio) ++n;
  }
  return n;
}

// With the lanes already free of anyone else's audio, the reflow is content to
// leave the clips where they are.
TEST(LayeredMove, KeepsAudioOnLanesItAlreadyOwns) {
  Project p = layered_project();
  p = place_media(std::move(p), "m1", 0.0, "v2");
  const std::string video_id = p.tracks[1].clips[0].id;
  const Ids members{group_members(p, video_id)};

  p = move_clips_layered(std::move(p), members, 0.0, -1);

  EXPECT_EQ(p.tracks[0].clips.size(), 1u);  // moved up to v1
  EXPECT_EQ(audio_lane_count(p), 2u);       // no new lanes needed
}

// A second layer's audio may not land on lanes the first layer occupies.
TEST(LayeredMove, GivesTheSecondLayerItsOwnAudioLanes) {
  Project p = layered_project();
  p = place_media(std::move(p), "m1", 0.0, "v2");  // first placement fills a1 and a2
  p = place_media(std::move(p), "m1", 20.0, "v2");

  const std::string second_video = p.tracks[1].clips[1].id;
  const Ids members{group_members(p, second_video)};

  p = move_clips_layered(std::move(p), members, 0.0, -1);

  EXPECT_EQ(audio_lane_count(p), 4u);  // two fresh lanes for the raised layer
  for (const Track& t : p.tracks) {
    if (t.kind != TrackKind::Audio) continue;
    // No lane may end up mixing two different groups.
    for (const Clip& c : t.clips) {
      EXPECT_EQ(c.group_id, t.clips[0].group_id);
    }
  }
}

TEST(LayeredMove, DoesNothingExtraWithoutALayerChange) {
  Project p = layered_project();
  p = place_media(std::move(p), "m1", 0.0, "v2");
  const std::string video_id = p.tracks[1].clips[0].id;
  const Ids members{group_members(p, video_id)};

  const Project before = p;
  p = move_clips_layered(std::move(p), members, 5.0, 0);  // slide only

  EXPECT_EQ(audio_lane_count(p), audio_lane_count(before));
  EXPECT_DOUBLE_EQ(p.tracks[1].clips[0].start, 5.0);
}

// --------------------------------------------------------- ripple insert --

TEST(RippleInsert, ShiftsEverythingFromTheInsertPoint) {
  Project p = three_clip_project();
  p = ripple_insert(std::move(p), 5.0, 3.0);

  ASSERT_EQ(video(p).clips.size(), 3u);
  EXPECT_DOUBLE_EQ(video(p).clips[0].start, 0.0);   // before the point, untouched
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 8.0);
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 13.0);
}

TEST(RippleInsert, SplitsWhateverSpansThePoint) {
  Project p = three_clip_project();
  p = ripple_insert(std::move(p), 2.0, 3.0);

  ASSERT_EQ(video(p).clips.size(), 4u);
  EXPECT_DOUBLE_EQ(video(p).clips[0].start, 0.0);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 2.0);
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 5.0);  // the right half, pushed right
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 8.0);
}

// ------------------------------------------------------------ insert edit --

TEST(InsertMediaAt, RipplesOpenThenPlaces) {
  Project p = three_clip_project();
  p = add_media(std::move(p), media_of("m2", 4.0));
  p = insert_media_at(std::move(p), "m2", 5.0);

  // a [0,5), the inserted clip [5,9), then b and c pushed right by four.
  ASSERT_EQ(video(p).clips.size(), 4u);
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 5.0);
  EXPECT_EQ(video(p).clips[1].media_id, "m2");
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[1]), 9.0);
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 9.0);
}

TEST(InsertMediaAt, UnknownMediaIsANoOp) {
  const Project before = three_clip_project();
  EXPECT_EQ(insert_media_at(before, "nope", 5.0), before);
}

// --------------------------------------------------------- overwrite edit --

TEST(OverwriteMediaAt, CarvesOutWhatItLandsOn) {
  Project p;
  p.media = {media_of("m1", 20.0), media_of("m2", 4.0)};
  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.clips = {clip_of("a", 0.0, 0.0, 10.0)};
  p.tracks = {v};

  p = overwrite_media_at(std::move(p), "m2", 3.0);

  // The covered clip becomes [0,3) and [7,10) with the new clip between them.
  ASSERT_EQ(video(p).clips.size(), 3u);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 3.0);
  EXPECT_DOUBLE_EQ(video(p).clips[0].source_out, 3.0);

  EXPECT_EQ(video(p).clips[1].media_id, "m2");
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 3.0);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[1]), 7.0);

  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 7.0);
  EXPECT_DOUBLE_EQ(video(p).clips[2].source_in, 7.0);
}

TEST(OverwriteMediaAt, DropsFullyCoveredClips) {
  Project p = three_clip_project();
  p = add_media(std::move(p), media_of("m2", 10.0));
  p = overwrite_media_at(std::move(p), "m2", 0.0);  // covers a and b exactly

  ASSERT_EQ(video(p).clips.size(), 2u);
  EXPECT_EQ(video(p).clips[0].media_id, "m2");
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 10.0);
  EXPECT_EQ(video(p).clips[1].id, "c");
}

TEST(OverwriteMediaAt, LeavesUncoveredClipsAlone) {
  Project p = three_clip_project();
  p = add_media(std::move(p), media_of("m2", 2.0));
  p = overwrite_media_at(std::move(p), "m2", 20.0);  // past everything

  EXPECT_EQ(video(p).clips.size(), 4u);
  EXPECT_DOUBLE_EQ(video(p).clips[0].start, 0.0);
}

// ------------------------------------------------------------ rate stretch --

TEST(RateStretch, DraggingTheOutEdgeChangesSpeedNotSource) {
  Project p = three_clip_project();
  p = rate_stretch_edge(std::move(p), "a", ClipEdge::Out, 10.0);

  const Clip& a = video(p).clips[0];
  EXPECT_DOUBLE_EQ(a.speed, 0.5);        // five seconds of source over ten
  EXPECT_DOUBLE_EQ(a.source_in, 0.0);    // source untouched
  EXPECT_DOUBLE_EQ(a.source_out, 5.0);
  EXPECT_DOUBLE_EQ(clip_end(a), 10.0);
}

TEST(RateStretch, DraggingTheInEdgeKeepsTheTailAnchored) {
  Project p = three_clip_project();
  p = rate_stretch_edge(std::move(p), "b", ClipEdge::In, 0.0);

  const Clip& b = *find_clip(p, "b");
  EXPECT_DOUBLE_EQ(b.speed, 0.5);
  EXPECT_DOUBLE_EQ(b.start, 0.0);
  EXPECT_DOUBLE_EQ(clip_end(b), 10.0);  // the tail stayed put
}

TEST(RateStretch, SpeedIsClampedToTheAllowedRange) {
  Project p = three_clip_project();
  p = rate_stretch_edge(std::move(p), "a", ClipEdge::Out, 0.0);  // degenerate drag
  EXPECT_LE(video(p).clips[0].speed, kMaxSpeed);
  EXPECT_GE(video(p).clips[0].speed, kMinSpeed);
}

TEST(RateStretch, UnknownClipIsANoOp) {
  const Project before = three_clip_project();
  EXPECT_EQ(rate_stretch_edge(before, "nope", ClipEdge::Out, 10.0), before);
}

// -------------------------------------------------------------------- slip --

TEST(Slip, ShiftsTheSourceWithoutMovingTheClip) {
  Project p = three_clip_project();
  p = slip_clip(std::move(p), "a", 2.0);

  const Clip& a = video(p).clips[0];
  EXPECT_DOUBLE_EQ(a.source_in, 2.0);
  EXPECT_DOUBLE_EQ(a.source_out, 7.0);
  EXPECT_DOUBLE_EQ(a.start, 0.0);  // unmoved
  EXPECT_DOUBLE_EQ(clip_duration(a), 5.0);
}

TEST(Slip, IsClampedToTheMediaBounds) {
  Project p = three_clip_project();
  p = slip_clip(std::move(p), "a", -10.0);  // would run before the media starts
  EXPECT_DOUBLE_EQ(video(p).clips[0].source_in, 0.0);

  p = slip_clip(std::move(p), "c", 99.0);  // would run past the media end
  EXPECT_DOUBLE_EQ(find_clip(p, "c")->source_out, 20.0);
}

TEST(Slip, StillsHaveNoSourceToSlip) {
  Project p;
  Media image;
  image.id = "img";
  image.is_image = true;
  image.duration = 5.0;
  p.media = {image};

  Clip c = clip_of("s1", 0.0, 0.0, 5.0);
  c.media_id = "img";
  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.clips = {c};
  p.tracks = {v};

  const Project before = p;
  EXPECT_EQ(slip_clip(before, "s1", 2.0), before);
}

// ------------------------------------------------------------------- slide --

// Sliding grows the previous clip and shrinks the next, so the neighbours keep
// their positions and the sequence length does not change.
TEST(Slide, TradesTimeWithBothNeighbours) {
  Project p = three_clip_project();
  const double length_before = timeline_duration(p);

  p = slide_clip(std::move(p), "b", 1.0);

  EXPECT_DOUBLE_EQ(find_clip(p, "b")->start, 6.0);
  EXPECT_DOUBLE_EQ(clip_end(*find_clip(p, "a")), 6.0);   // previous grew
  EXPECT_DOUBLE_EQ(find_clip(p, "a")->source_out, 6.0);
  EXPECT_DOUBLE_EQ(find_clip(p, "c")->start, 11.0);      // next shrank
  EXPECT_DOUBLE_EQ(find_clip(p, "c")->source_in, 11.0);
  EXPECT_DOUBLE_EQ(timeline_duration(p), length_before);
}

TEST(Slide, LeavesTheSlidClipsSourceAlone) {
  Project p = three_clip_project();
  p = slide_clip(std::move(p), "b", 1.0);

  const Clip& b = *find_clip(p, "b");
  EXPECT_DOUBLE_EQ(b.source_in, 5.0);
  EXPECT_DOUBLE_EQ(b.source_out, 10.0);
}

TEST(Slide, IsClampedByTheNeighboursLimits) {
  Project p = three_clip_project();
  p = slide_clip(std::move(p), "b", 99.0);

  // The next clip must keep at least the minimum length.
  EXPECT_NEAR(clip_duration(*find_clip(p, "c")), kMinClip, 1e-9);
}

TEST(Slide, NeedsSomethingToSlideAgainst) {
  Project p;
  p.media = {media_of("m1", 20.0)};
  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.clips = {clip_of("lonely", 0.0, 0.0, 5.0)};
  p.tracks = {v};

  const Project before = p;
  EXPECT_EQ(slide_clip(before, "lonely", 1.0), before);
}

TEST(Slide, UnknownClipIsANoOp) {
  const Project before = three_clip_project();
  EXPECT_EQ(slide_clip(before, "nope", 1.0), before);
}

// ------------------------------------------------- ripple trim and roll --

TEST(RippleTrim, ShorteningATailPullsEverythingAfterItAlong) {
  Project p = three_clip_project();
  p = ripple_trim_edge(std::move(p), "a", ClipEdge::Out, 3.0);

  ASSERT_EQ(video(p).clips.size(), 3u);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 3.0);
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 3.0) << "b closed the gap";
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 8.0);
}

TEST(RippleTrim, LengtheningATailPushesEverythingAfterItAlong) {
  Project p = three_clip_project();
  p = ripple_trim_edge(std::move(p), "a", ClipEdge::Out, 7.0);

  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 7.0);
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 7.0);
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 12.0);
}

TEST(RippleTrim, IsNotStoppedByTheClipBesideIt) {
  // The whole difference from an ordinary trim: that clip is what moves.
  Project trimmed = three_clip_project();
  trimmed = set_clip_edge(std::move(trimmed), "a", ClipEdge::Out, 7.0);
  EXPECT_DOUBLE_EQ(clip_end(video(trimmed).clips[0]), 5.0) << "an ordinary trim stops at b";
}

TEST(RippleTrim, TrimmingAHeadLeavesTheClipWhereItIs) {
  // And pulls what follows along. Without that a ripple on the in-edge would
  // leave a hole in front of the very clip that was trimmed.
  Project p = three_clip_project();
  p = ripple_trim_edge(std::move(p), "b", ClipEdge::In, 7.0);

  ASSERT_EQ(video(p).clips.size(), 3u);
  EXPECT_DOUBLE_EQ(video(p).clips[0].start, 0.0);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 5.0) << "a is untouched";
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 5.0) << "b still starts where it did";
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[1]), 8.0) << "and is two seconds shorter";
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 8.0);
}

TEST(RippleTrim, EveryTrackFollows) {
  // Or a ripple on the picture would slide the sound out of sync with it.
  Project p = three_clip_project();
  Track audio;
  audio.id = "a1";
  audio.kind = TrackKind::Audio;
  audio.clips = {clip_of("s1", 0.0, 0.0, 5.0), clip_of("s2", 5.0, 5.0, 10.0)};
  p.tracks.push_back(audio);

  p = ripple_trim_edge(std::move(p), "a", ClipEdge::Out, 3.0);
  EXPECT_DOUBLE_EQ(p.tracks[1].clips[1].start, 3.0);
}

TEST(RippleTrim, StillStopsWhenTheSourceRunsOut) {
  Project p = three_clip_project();
  // c is [10,15) drawn from 10 to 15 of a twenty-second media: five to spare.
  p = ripple_trim_edge(std::move(p), "c", ClipEdge::Out, 40.0);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[2]), 20.0);
}

TEST(RollEdit, MovesTheJoinAndNothingElse) {
  Project p = three_clip_project();
  const double was = timeline_duration(p);

  p = roll_edit(std::move(p), "a", ClipEdge::Out, 7.0);

  ASSERT_EQ(video(p).clips.size(), 3u);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 7.0);
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 7.0);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[1]), 10.0) << "b lost what a gained";
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 10.0) << "c has not moved";
  EXPECT_DOUBLE_EQ(timeline_duration(p), was);
}

TEST(RollEdit, TheJoinCanBeRolledEitherWay) {
  Project p = three_clip_project();
  p = roll_edit(std::move(p), "b", ClipEdge::In, 3.0);

  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 3.0);
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 3.0);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[1]), 10.0);
}

TEST(RollEdit, TheSourceTravelsWithTheJoin) {
  // What a roll is for: the same instant of the sequence shows a later part of
  // one clip and an earlier part of the other.
  Project p = three_clip_project();
  p = roll_edit(std::move(p), "a", ClipEdge::Out, 7.0);

  EXPECT_DOUBLE_EQ(video(p).clips[0].source_out, 7.0);
  EXPECT_DOUBLE_EQ(video(p).clips[1].source_in, 7.0);
}

TEST(RollEdit, AnEdgeThatIsNotAJoinIsLeftAlone) {
  Project p = three_clip_project();
  const Project before = p;
  // Nothing after c, and nothing before a.
  EXPECT_EQ(roll_edit(before, "c", ClipEdge::Out, 12.0), before);
  EXPECT_EQ(roll_edit(before, "a", ClipEdge::In, 1.0), before);
}

TEST(RollEdit, StopsWhereEitherSideRunsOutOfSource) {
  Project p;
  p.media = {media_of("m1", 20.0)};
  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  // b has only a second of source past its out point.
  v.clips = {clip_of("a", 0.0, 0.0, 5.0), clip_of("b", 5.0, 5.0, 10.0)};
  v.clips[1].source_in = 5.0;
  v.clips[1].source_out = 10.0;
  p.tracks = {v};

  // a has fifteen seconds of handle to give, so what stops this is b's head
  // running out — it can only give up five before it is at its own source in.
  p = roll_edit(std::move(p), "a", ClipEdge::Out, 40.0);
  EXPECT_LE(clip_end(p.tracks[0].clips[0]), 10.0 + 1e-9);
  EXPECT_GE(clip_duration(p.tracks[0].clips[1]), kMinClip - 1e-9);
}

// ------------------------------------------------------------ copy / paste --

TEST(CopyClips, TakesTheWholeClipAndTheLaneItWasOn) {
  Project p = three_clip_project();
  p.tracks[0].clips[1].gain = 0.5;

  const std::vector<ClipCopy> copies = copy_clips(p, Ids{"b"});
  ASSERT_EQ(copies.size(), 1u);
  EXPECT_EQ(copies[0].track_id, "v1");
  EXPECT_EQ(copies[0].clip.id, "b");
  EXPECT_DOUBLE_EQ(copies[0].clip.gain, 0.5);
  EXPECT_DOUBLE_EQ(copies[0].clip.source_in, 5.0);
}

TEST(PasteClips, LandsTheEarliestAtThePastePointAndKeepsTheShape) {
  Project p = three_clip_project();
  const std::vector<ClipCopy> copies = copy_clips(p, Ids{"a", "c"});
  ASSERT_EQ(copies.size(), 2u);

  p = paste_clips(std::move(p), copies, 20.0);

  // a at 20 and c ten seconds after it, which is the gap they had.
  ASSERT_EQ(video(p).clips.size(), 5u);
  EXPECT_DOUBLE_EQ(video(p).clips[3].start, 20.0);
  EXPECT_DOUBLE_EQ(video(p).clips[4].start, 30.0);
}

TEST(PasteClips, GivesEveryPastedClipAnIdOfItsOwn) {
  Project p = three_clip_project();
  const std::vector<ClipCopy> copies = copy_clips(p, Ids{"a"});
  p = paste_clips(std::move(p), copies, 20.0);

  ASSERT_EQ(video(p).clips.size(), 4u);
  EXPECT_NE(video(p).clips[3].id, "a");
  EXPECT_EQ(video(p).clips[3].media_id, video(p).clips[0].media_id);
}

TEST(PasteClips, OverwritesWhatItLandsOn) {
  Project p = three_clip_project();
  const std::vector<ClipCopy> copies = copy_clips(p, Ids{"a"});

  // Five seconds of clip pasted over the middle of b, which is [5,10).
  p = paste_clips(std::move(p), copies, 6.0);

  // a untouched, b cut back to [5,6), the paste over [6,11), and c starting a
  // second later than it did.
  ASSERT_EQ(video(p).clips.size(), 4u);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[1]), 6.0);
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 6.0);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[2]), 11.0);
  EXPECT_DOUBLE_EQ(video(p).clips[3].start, 11.0);
  EXPECT_EQ(video(p).clips[3].id, "c");
}

TEST(PasteClips, LinksThePastedPairToEachOtherRatherThanToTheOriginal) {
  Project p = layered_project();
  p = place_media(std::move(p), "m1", 0.0);

  Ids placed;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) placed.push_back(c.id);
  }
  ASSERT_GE(placed.size(), 2u);
  const std::vector<ClipCopy> copies = copy_clips(p, placed);
  const std::string original = copies.front().clip.group_id.value_or("");
  ASSERT_FALSE(original.empty());

  p = paste_clips(std::move(p), copies, 20.0);

  std::vector<std::string> pasted_groups;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) {
      if (c.start >= 20.0 && c.group_id.has_value()) pasted_groups.push_back(*c.group_id);
    }
  }
  ASSERT_GE(pasted_groups.size(), 2u);
  for (const std::string& group : pasted_groups) {
    EXPECT_NE(group, original) << "the copies are still tied to what they came from";
    EXPECT_EQ(group, pasted_groups.front()) << "and no longer tied to each other";
  }
}

TEST(PasteClips, AnEmptyClipboardChangesNothing) {
  const Project before = three_clip_project();
  EXPECT_EQ(paste_clips(before, {}, 5.0), before);
}

TEST(PasteClips, FallsBackToTheFirstLaneOfItsKindWhenTheOriginalHasGone) {
  Project p = three_clip_project();
  const std::vector<ClipCopy> copies = copy_clips(p, Ids{"a"});

  Project other;
  other.media = p.media;
  Track v;
  v.id = "elsewhere";
  v.kind = TrackKind::Video;
  other.tracks = {v};

  other = paste_clips(std::move(other), copies, 3.0);
  ASSERT_EQ(other.tracks[0].clips.size(), 1u);
  EXPECT_DOUBLE_EQ(other.tracks[0].clips[0].start, 3.0);
}

TEST(PasteClips, WithNoLaneOfThatKindNothingHappens) {
  Project p = three_clip_project();
  const std::vector<ClipCopy> copies = copy_clips(p, Ids{"a"});

  Project audio_only;
  Track a;
  a.id = "a1";
  a.kind = TrackKind::Audio;
  audio_only.tracks = {a};

  EXPECT_EQ(paste_clips(audio_only, copies, 0.0), audio_only);
}

TEST(PasteClipsInsert, OpensAGapRatherThanOverwriting) {
  Project p = three_clip_project();
  const std::vector<ClipCopy> copies = copy_clips(p, Ids{"a"});

  p = paste_clips_insert(std::move(p), copies, 5.0);

  // a stays, the paste takes [5,10), and b and c are five seconds later.
  ASSERT_EQ(video(p).clips.size(), 4u);
  EXPECT_DOUBLE_EQ(clip_end(video(p).clips[0]), 5.0);
  EXPECT_DOUBLE_EQ(video(p).clips[1].start, 5.0);
  EXPECT_DOUBLE_EQ(video(p).clips[2].start, 10.0);
  EXPECT_DOUBLE_EQ(video(p).clips[3].start, 15.0);
}

}  // namespace
}  // namespace cutline::core
