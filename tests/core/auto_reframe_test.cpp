/// Auto Reframe Sequence: a reframed copy in a new shape, with the effect on
/// every picture that should be reframed and on nothing else, and the
/// original left exactly as it was.

#include "cutline/core/auto_reframe.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace cutline::core {
namespace {

/// A 1920 by 1080 sequence: two linked shots of footage, a title over them,
/// and the shots' sound.
[[nodiscard]] Project cut() {
  Project p = empty_project(2, 1);
  p.sequence().name = "Cut";
  Media footage;
  footage.id = "footage";
  footage.path = "D:/footage.mp4";
  footage.duration = 20.0;
  footage.has_video = true;
  footage.audio_stream_count = 1;
  footage.width = 1920;
  footage.height = 1080;
  Media title;
  title.id = "title";
  title.is_graphic = true;
  title.duration = 5.0;
  p.media = {footage, title};
  Track& v2 = p.sequence().tracks[0];
  Track& v1 = p.sequence().tracks[1];
  Track& a1 = p.sequence().tracks[2];
  v2.clips.push_back(Clip{.id = "words", .media_id = "title", .kind = TrackKind::Video,
                          .source_out = 5.0});
  v1.clips.push_back(Clip{.id = "wide", .media_id = "footage", .kind = TrackKind::Video,
                          .source_out = 4.0, .group_id = "g1"});
  v1.clips.push_back(Clip{.id = "close", .media_id = "footage", .kind = TrackKind::Video,
                          .source_in = 10.0, .source_out = 13.0, .start = 4.0});
  a1.clips.push_back(Clip{.id = "sound", .media_id = "footage", .kind = TrackKind::Audio,
                          .source_out = 4.0, .group_id = "g1"});
  return p;
}

[[nodiscard]] bool reframed(const Clip& clip) {
  return std::ranges::any_of(clip.effects,
                             [](const ClipEffect& e) { return e.type == "autoreframe"; });
}

TEST(AutoReframe, NewShapesKeepTheShorterSideOfTheSequence) {
  Sequence wide;
  EXPECT_EQ(reframed_size(wide, 9, 16), (FrameSize{1080, 1920}));
  EXPECT_EQ(reframed_size(wide, 1, 1), (FrameSize{1080, 1080}));
  EXPECT_EQ(reframed_size(wide, 4, 5), (FrameSize{1080, 1350}));
  EXPECT_EQ(reframed_size(wide, 16, 9), (FrameSize{1920, 1080}));
  Sequence tall;
  tall.canvas_w = 1080;
  tall.canvas_h = 1920;
  EXPECT_EQ(reframed_size(tall, 16, 9), (FrameSize{1920, 1080}));
}

TEST(AutoReframe, MakesAReframedCopyAndLeavesTheOriginalAsItWas) {
  const Project before = cut();
  std::string id;
  const Project after = auto_reframe_sequence(
      before, {.size = {1080, 1920}, .motion = 2}, &id);
  ASSERT_EQ(after.sequences.size(), 2u);
  EXPECT_EQ(after.sequences.front(), before.sequences.front());
  EXPECT_EQ(after.sequence().id, id);
  EXPECT_EQ(after.sequence().name, "Cut (1080x1920)");
  EXPECT_EQ(after.sequence().canvas_w, 1080);
  EXPECT_EQ(after.sequence().canvas_h, 1920);

  const Track& v2 = after.sequence().tracks[0];
  const Track& v1 = after.sequence().tracks[1];
  const Track& a1 = after.sequence().tracks[2];
  EXPECT_FALSE(reframed(v2.clips[0])) << "a title is laid out for its frame";
  ASSERT_EQ(v1.clips.size(), 2u);
  for (const Clip& clip : v1.clips) {
    ASSERT_TRUE(reframed(clip));
    EXPECT_DOUBLE_EQ(clip.effects.back().params.at("motion_preset"), 2.0);
    EXPECT_NE(clip.id, "wide");
    EXPECT_NE(clip.id, "close");
  }
  EXPECT_FALSE(reframed(a1.clips[0]));
  // The copies are linked to each other, not to the originals.
  EXPECT_EQ(v1.clips[0].group_id, a1.clips[0].group_id);
  EXPECT_NE(v1.clips[0].group_id, std::optional<std::string>("g1"));
}

TEST(AutoReframe, NestedReframesOneNestThatKeepsTheShapeItWasCutIn) {
  const Project after = auto_reframe_sequence(cut(), {.size = {1080, 1080}, .nest = true});
  std::vector<const Clip*> pictures;
  for (const Track& track : after.sequence().tracks) {
    for (const Clip& clip : track.clips) {
      if (reframed(clip)) pictures.push_back(&clip);
    }
  }
  ASSERT_EQ(pictures.size(), 1u);
  const auto media = std::ranges::find(after.media, pictures.front()->media_id, &Media::id);
  ASSERT_NE(media, after.media.end());
  ASSERT_FALSE(media->sequence_id.empty());
  EXPECT_EQ(media->width, std::optional<int>(1920));
  EXPECT_EQ(media->height, std::optional<int>(1080));
  EXPECT_EQ(after.sequence().canvas_w, 1080);
}

}  // namespace
}  // namespace cutline::core
