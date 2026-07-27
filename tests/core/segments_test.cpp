#include "cutline/core/segments.hpp"

#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace cutline::core {
namespace {

constexpr double kMediaDuration = 20.0;

/// Every media in these fixtures is 20 seconds long, so both clips below have
/// real handles to borrow.
double media_duration(std::string_view) { return kMediaDuration; }

/// A: source [5, 10) at timeline 0, so it ends at 5 with a 5 s head handle and
/// a 10 s tail handle.
Clip clip_a() {
  Clip c;
  c.id = "a";
  c.media_id = "m";
  c.source_in = 5.0;
  c.source_out = 10.0;
  c.start = 0.0;
  return c;
}

/// B: source [3, 8) abutting A at timeline 5, so it has a 3 s head handle.
Clip clip_b() {
  Clip c;
  c.id = "b";
  c.media_id = "m";
  c.source_in = 3.0;
  c.source_out = 8.0;
  c.start = 5.0;
  return c;
}

Track track_with(Clip a, Clip b) {
  Track t;
  t.id = "v1";
  t.kind = TrackKind::Video;
  t.clips = {std::move(a), std::move(b)};
  return t;
}

Track abutting_pair(TransitionKind kind, double duration) {
  Clip a = clip_a();
  a.transition_out = Transition{.kind = kind, .duration = duration};
  return track_with(std::move(a), clip_b());
}

// ---------------------------------------------------------- plain segments --

TEST(ResolveSegments, OneSegmentPerClipWithoutTransitions) {
  const Track t = track_with(clip_a(), clip_b());
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  ASSERT_EQ(segs.size(), 2u);
  EXPECT_EQ(segs[0].clip->id, "a");
  EXPECT_DOUBLE_EQ(segs[0].start, 0.0);
  EXPECT_DOUBLE_EQ(segs[0].end, 5.0);
  EXPECT_DOUBLE_EQ(segs[0].source_in, 5.0);
  EXPECT_DOUBLE_EQ(segs[0].source_out, 10.0);
  EXPECT_DOUBLE_EQ(segs[0].x_in, 0.0);
  EXPECT_DOUBLE_EQ(segs[0].x_out, 0.0);
  EXPECT_FALSE(segs[0].to_black);
  EXPECT_FALSE(segs[0].slide_kind.has_value());

  EXPECT_EQ(segs[1].clip->id, "b");
  EXPECT_DOUBLE_EQ(segs[1].start, 5.0);
  EXPECT_DOUBLE_EQ(segs[1].end, 10.0);
}

TEST(ResolveSegments, DisabledClipsAreExcluded) {
  Clip a = clip_a();
  a.disabled = true;
  const Track t = track_with(std::move(a), clip_b());

  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);
  ASSERT_EQ(segs.size(), 1u);
  EXPECT_EQ(segs[0].clip->id, "b");
}

TEST(ResolveSegments, SegmentsAreOrderedByStart) {
  const Track t = track_with(clip_b(), clip_a());  // stored out of order
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  ASSERT_EQ(segs.size(), 2u);
  EXPECT_EQ(segs[0].clip->id, "a");
  EXPECT_EQ(segs[1].clip->id, "b");
}

// ---------------------------------------------------------- dip-to-black --

// Dip-to-black needs no handles: it is two opaque fades meeting at the cut, so
// neither segment's timing moves.
TEST(ResolveSegments, DipToBlackFadesWithoutOverlapping) {
  const Track t = abutting_pair(TransitionKind::DipBlack, 2.0);
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  ASSERT_EQ(segs.size(), 2u);
  EXPECT_DOUBLE_EQ(segs[0].end, 5.0);
  EXPECT_DOUBLE_EQ(segs[0].x_out, 1.0);  // half the duration
  EXPECT_TRUE(segs[0].to_black);

  EXPECT_DOUBLE_EQ(segs[1].start, 5.0);
  EXPECT_DOUBLE_EQ(segs[1].x_in, 1.0);
  EXPECT_TRUE(segs[1].to_black);
}

// ------------------------------------------------------------- dissolve --

TEST(ResolveSegments, DissolveBorrowsHandlesFromBothSides) {
  const Track t = abutting_pair(TransitionKind::Dissolve, 2.0);
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  ASSERT_EQ(segs.size(), 2u);
  // A extends one second past its out point, consuming tail handle.
  EXPECT_DOUBLE_EQ(segs[0].end, 6.0);
  EXPECT_DOUBLE_EQ(segs[0].source_out, 11.0);
  // B starts one second early, consuming head handle.
  EXPECT_DOUBLE_EQ(segs[1].start, 4.0);
  EXPECT_DOUBLE_EQ(segs[1].source_in, 2.0);
  // B cross-fades in across the whole two-second overlap.
  EXPECT_DOUBLE_EQ(segs[1].x_in, 2.0);
  EXPECT_FALSE(segs[1].to_black);
}

// A clip trimmed to the very edge of its media has nothing to borrow, so the
// transition degrades to a hard cut rather than producing a bogus overlap.
TEST(ResolveSegments, DissolveWithoutHandlesIsSkipped) {
  Clip a = clip_a();
  a.source_in = 0.0;
  a.source_out = kMediaDuration;  // no tail
  a.start = 0.0;
  a.transition_out = Transition{.kind = TransitionKind::Dissolve, .duration = 2.0};

  Clip b = clip_b();
  b.source_in = 0.0;  // no head
  b.source_out = 5.0;
  b.start = clip_end(a);

  const Track t = track_with(std::move(a), std::move(b));
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  ASSERT_EQ(segs.size(), 2u);
  EXPECT_DOUBLE_EQ(segs[0].end, kMediaDuration);
  EXPECT_DOUBLE_EQ(segs[1].start, kMediaDuration);
  EXPECT_DOUBLE_EQ(segs[1].x_in, 0.0);
}

TEST(ResolveSegments, HandleBorrowingIsLimitedByWhatExists) {
  Clip a = clip_a();
  a.source_in = 5.0;
  a.source_out = 19.5;  // only 0.5 s of tail left
  a.start = 0.0;
  a.transition_out = Transition{.kind = TransitionKind::Dissolve, .duration = 2.0};

  Clip b = clip_b();
  b.start = clip_end(a);

  const Track t = track_with(std::move(a), std::move(b));
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  ASSERT_EQ(segs.size(), 2u);
  EXPECT_DOUBLE_EQ(segs[0].end, 15.0);          // 14.5 + 0.5 borrowed
  EXPECT_DOUBLE_EQ(segs[0].source_out, 20.0);
  EXPECT_DOUBLE_EQ(segs[1].x_in, 1.5);          // 0.5 from A plus 1.0 from B
}

TEST(ResolveSegments, NonAbuttingClipsDoNotTransition) {
  Clip a = clip_a();
  a.transition_out = Transition{.kind = TransitionKind::Dissolve, .duration = 2.0};
  Clip b = clip_b();
  b.start = 5.5;  // a half-second gap

  const Track t = track_with(std::move(a), std::move(b));
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  ASSERT_EQ(segs.size(), 2u);
  EXPECT_DOUBLE_EQ(segs[0].end, 5.0);
  EXPECT_DOUBLE_EQ(segs[1].start, 5.5);
  EXPECT_DOUBLE_EQ(segs[1].x_in, 0.0);
}

TEST(ResolveSegments, ZeroDurationTransitionIsIgnored) {
  const Track t = abutting_pair(TransitionKind::Dissolve, 0.0);
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);
  EXPECT_DOUBLE_EQ(segs[0].end, 5.0);
  EXPECT_DOUBLE_EQ(segs[1].x_in, 0.0);
}

// Reverse swaps which physical handle feeds each edge, so an extended reversed
// clip walks its source in point backwards rather than its out point forwards.
TEST(ResolveSegments, ReverseAwareHandleExtension) {
  Clip a = clip_a();
  a.reverse = true;
  a.transition_out = Transition{.kind = TransitionKind::Dissolve, .duration = 2.0};

  const Track t = track_with(std::move(a), clip_b());
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  EXPECT_DOUBLE_EQ(segs[0].end, 6.0);
  EXPECT_DOUBLE_EQ(segs[0].source_in, 4.0);   // moved back
  EXPECT_DOUBLE_EQ(segs[0].source_out, 10.0); // unchanged
}

TEST(ResolveSegments, SpeedScalesTheBorrowedSource) {
  Clip a = clip_a();
  a.source_out = 15.0;
  a.speed = 2.0;  // 10 s of source played across 5 s of timeline
  a.transition_out = Transition{.kind = TransitionKind::Dissolve, .duration = 2.0};

  Clip b = clip_b();
  b.start = clip_end(a);

  const Track t = track_with(std::move(a), std::move(b));
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  EXPECT_DOUBLE_EQ(segs[0].end, 6.0);          // 5 s + 1 s borrowed
  EXPECT_DOUBLE_EQ(segs[0].source_out, 17.0);  // 1 timeline second = 2 source
}

// ------------------------------------------------------------ push/slide --

TEST(ResolveSegments, PushSlidesBothClips) {
  const Track t = abutting_pair(TransitionKind::Push, 2.0);
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  const SlideWindow expected{.start = 4.0, .end = 6.0};
  ASSERT_TRUE(segs[0].slide_kind.has_value());
  EXPECT_EQ(*segs[0].slide_kind, TransitionKind::Push);
  EXPECT_EQ(*segs[0].slide_role, SlideRole::Out);
  EXPECT_EQ(*segs[0].slide_win, expected);

  ASSERT_TRUE(segs[1].slide_kind.has_value());
  EXPECT_EQ(*segs[1].slide_role, SlideRole::In);
  EXPECT_EQ(*segs[1].slide_win, expected);

  // Geometric transitions do not also cross-fade.
  EXPECT_DOUBLE_EQ(segs[1].x_in, 0.0);
}

TEST(ResolveSegments, SlideMovesOnlyTheIncomingClip) {
  const Track t = abutting_pair(TransitionKind::Slide, 2.0);
  const std::vector<VideoSeg> segs = resolve_video_segments(t, media_duration);

  EXPECT_FALSE(segs[0].slide_kind.has_value());
  ASSERT_TRUE(segs[1].slide_kind.has_value());
  EXPECT_EQ(*segs[1].slide_kind, TransitionKind::Slide);
  EXPECT_EQ(*segs[1].slide_role, SlideRole::In);
}

// ------------------------------------------------------- slide geometry --

TEST(SlideOffset, IncomingRampsFromOffRightToCentred) {
  VideoSeg seg;
  seg.slide_kind = TransitionKind::Push;
  seg.slide_role = SlideRole::In;
  seg.slide_win = SlideWindow{.start = 4.0, .end = 6.0};

  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 4.0), 1.0);
  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 5.0), 0.5);
  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 6.0), 0.0);
}

TEST(SlideOffset, OutgoingRampsFromCentredToOffLeft) {
  VideoSeg seg;
  seg.slide_kind = TransitionKind::Push;
  seg.slide_role = SlideRole::Out;
  seg.slide_win = SlideWindow{.start = 4.0, .end = 6.0};

  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 4.0), 0.0);
  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 5.0), -0.5);
  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 6.0), -1.0);
}

TEST(SlideOffset, ClampsOutsideTheWindow) {
  VideoSeg seg;
  seg.slide_kind = TransitionKind::Push;
  seg.slide_role = SlideRole::In;
  seg.slide_win = SlideWindow{.start = 4.0, .end = 6.0};

  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 0.0), 1.0);
  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 99.0), 0.0);
}

TEST(SlideOffset, IsZeroWithoutAGeometricTransition) {
  VideoSeg seg;
  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 5.0), 0.0);
}

TEST(SlideOffset, IsZeroForADegenerateWindow) {
  VideoSeg seg;
  seg.slide_kind = TransitionKind::Push;
  seg.slide_role = SlideRole::In;
  seg.slide_win = SlideWindow{.start = 4.0, .end = 4.0};
  EXPECT_DOUBLE_EQ(seg_slide_offset_x(seg, 4.0), 0.0);
}

// ----------------------------------------------------------------- stills --

// Stills and generated media report infinite duration, giving them unlimited
// handles, so a transition always gets the full half it asks for.
TEST(ResolveSegments, InfiniteMediaDurationGivesUnlimitedHandles) {
  Clip a = clip_a();
  a.transition_out = Transition{.kind = TransitionKind::Dissolve, .duration = 2.0};
  const Track t = track_with(std::move(a), clip_b());

  const std::vector<VideoSeg> segs = resolve_video_segments(
      t, [](std::string_view) { return std::numeric_limits<double>::infinity(); });

  EXPECT_DOUBLE_EQ(segs[0].end, 6.0);
  EXPECT_DOUBLE_EQ(segs[1].start, 4.0);
  EXPECT_DOUBLE_EQ(segs[1].x_in, 2.0);
}

}  // namespace
}  // namespace cutline::core
