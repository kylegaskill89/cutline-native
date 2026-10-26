/// Trim mode: choosing an edit point and trimming it by each of the three kinds
/// of trim.

#include "cutline/editor/trim.hpp"

#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

/// Two clips meeting at 5 s on V1, each from the middle of a minute of media,
/// and a third after a gap.
[[nodiscard]] Project cut() {
  Project p;
  p.sequence().fps = 30.0;
  p.media = {Media{.id = "m", .name = "m.mp4", .duration = 60.0, .has_video = true}};
  Track video{.id = "v1", .kind = TrackKind::Video};
  video.targeted = true;
  video.clips = {
      Clip{.id = "a", .media_id = "m", .source_in = 10.0, .source_out = 15.0, .start = 0.0},
      Clip{.id = "b", .media_id = "m", .source_in = 30.0, .source_out = 35.0, .start = 5.0},
      Clip{.id = "c", .media_id = "m", .source_in = 40.0, .source_out = 42.0, .start = 12.0},
  };
  p.sequence().tracks = {video};
  return p;
}

TEST(TrimMode, TheNearestCutIsARollWhereThereAreClipsBothSides) {
  const auto point = nearest_edit_point(cut(), 4.6);
  ASSERT_TRUE(point.has_value());
  EXPECT_EQ(point->outgoing, "a");
  EXPECT_EQ(point->incoming, "b");
  EXPECT_DOUBLE_EQ(point->at, 5.0);
  EXPECT_EQ(point->type, TrimType::Roll);
}

TEST(TrimMode, AClipMeetingAGapIsTrimmedOnTheSideThereIs) {
  const auto end = nearest_edit_point(cut(), 10.4);
  ASSERT_TRUE(end.has_value());
  EXPECT_EQ(end->outgoing, "b");
  EXPECT_TRUE(end->incoming.empty());
  EXPECT_EQ(end->type, TrimType::TrimOut);
  const auto start = nearest_edit_point(cut(), 11.9);
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(start->incoming, "c");
  EXPECT_EQ(start->type, TrimType::TrimIn);
}

TEST(TrimMode, TogglingGoesRoundTheKindsTheCutHas) {
  EditPoint point = *nearest_edit_point(cut(), 5.0);
  std::vector<TrimType> seen;
  for (int i = 0; i < 5; ++i) {
    point = next_trim_type(point);
    seen.push_back(point.type);
  }
  EXPECT_EQ(seen, (std::vector<TrimType>{TrimType::RippleOut, TrimType::RippleIn,
                                         TrimType::TrimOut, TrimType::TrimIn, TrimType::Roll}));
  // A clip ending at a gap has only its out side.
  const EditPoint end = *nearest_edit_point(cut(), 10.0);
  EXPECT_EQ(next_trim_type(end).type, TrimType::RippleOut);
  EXPECT_EQ(next_trim_type(next_trim_type(end)).type, TrimType::TrimOut);
}

TEST(TrimMode, ARollMovesTheCutAndNothingElse) {
  const Trimmed done = trim_edit_point(cut(), *nearest_edit_point(cut(), 5.0), 0.5);
  EXPECT_DOUBLE_EQ(done.point.at, 5.5);
  EXPECT_DOUBLE_EQ(core::clip_end(*core::find_clip(done.project, "a")), 5.5);
  EXPECT_DOUBLE_EQ(core::find_clip(done.project, "b")->start, 5.5);
  EXPECT_DOUBLE_EQ(core::find_clip(done.project, "c")->start, 12.0);
}

TEST(TrimMode, ARippleOutPushesEverythingAfter) {
  EditPoint point = next_trim_type(*nearest_edit_point(cut(), 5.0));
  ASSERT_EQ(point.type, TrimType::RippleOut);
  const Trimmed done = trim_edit_point(cut(), point, 1.0);
  EXPECT_DOUBLE_EQ(done.point.at, 6.0);
  EXPECT_DOUBLE_EQ(core::find_clip(done.project, "b")->start, 6.0);
  EXPECT_DOUBLE_EQ(core::find_clip(done.project, "c")->start, 13.0);
}

TEST(TrimMode, ARippleInKeepsTheCutWhereItIs) {
  EditPoint point = *nearest_edit_point(cut(), 5.0);
  point.type = TrimType::RippleIn;
  const Trimmed done = trim_edit_point(cut(), point, 1.0);
  EXPECT_DOUBLE_EQ(done.point.at, 5.0);
  EXPECT_FALSE(trim_moves_cut(TrimType::RippleIn));
  // A second shorter, from a second later in its source.
  const Clip* b = core::find_clip(done.project, "b");
  EXPECT_DOUBLE_EQ(b->source_in, 31.0);
  EXPECT_DOUBLE_EQ(core::find_clip(done.project, "c")->start, 11.0);
}

TEST(TrimMode, AnOrdinaryTrimLeavesAGap) {
  EditPoint point = *nearest_edit_point(cut(), 10.0);
  const Trimmed done = trim_edit_point(cut(), point, -1.0);
  EXPECT_DOUBLE_EQ(done.point.at, 9.0);
  EXPECT_DOUBLE_EQ(core::find_clip(done.project, "c")->start, 12.0);
}

TEST(TrimMode, AnEditPointIsFoundAgainOrLostWithItsClips) {
  const EditPoint point = *nearest_edit_point(cut(), 5.0);
  Project moved = cut();
  core::find_clip(moved, "a")->source_out = 14.0;
  EXPECT_DOUBLE_EQ(refresh_edit_point(moved, point)->at, 4.0);
  Project gone = cut();
  gone.sequence().tracks[0].clips.erase(gone.sequence().tracks[0].clips.begin(),
                                        gone.sequence().tracks[0].clips.begin() + 2);
  EXPECT_FALSE(refresh_edit_point(gone, point).has_value());
}

}  // namespace
}  // namespace cutline::editor
