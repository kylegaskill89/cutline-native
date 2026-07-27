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

}  // namespace
}  // namespace cutline::core
