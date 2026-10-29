/// A remix on a clip: what it plays, and that every ordinary edit still does
/// to a remixed clip what it does to any other, measured along the remix.

#include "cutline/core/remix.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

/// A minute of song on A1 from 2 s, trimmed to its first fifty seconds, and a
/// sound effect further along the same track at 60 s.
[[nodiscard]] Project with_song() {
  Project p = empty_project(1, 1);
  Media song;
  song.id = "song";
  song.path = "D:/song.wav";
  song.name = "song.wav";
  song.duration = 60.0;
  song.audio_stream_count = 1;
  Media effect = song;
  effect.id = "bang";
  p.media = {song, effect};
  Track& a1 = p.sequence().tracks[1];
  a1.clips.push_back(Clip{.id = "music",
                          .media_id = "song",
                          .kind = TrackKind::Audio,
                          .source_in = 0.0,
                          .source_out = 50.0,
                          .start = 2.0});
  a1.clips.push_back(Clip{.id = "bang",
                          .media_id = "bang",
                          .kind = TrackKind::Audio,
                          .source_in = 0.0,
                          .source_out = 1.0,
                          .start = 60.0});
  return p;
}

/// The first ten seconds, then the stretch from 30 to 40: twenty seconds.
[[nodiscard]] Remix twenty_seconds() {
  return Remix{.target = 20.0, .spans = {{0.0, 10.0}, {30.0, 40.0}}};
}

TEST(Remix, MakesTheClipAsLongAsTheRemixAndPlaysItsStretchesInOrder) {
  const Project p = apply_remix(with_song(), "music", twenty_seconds());
  const Clip& clip = *find_clip(p, "music");
  EXPECT_DOUBLE_EQ(clip.start, 2.0);
  EXPECT_DOUBLE_EQ(clip_duration(clip), 20.0);
  EXPECT_DOUBLE_EQ(source_time_at(clip, 2.0 + 5.0), 5.0);
  EXPECT_DOUBLE_EQ(source_time_at(clip, 2.0 + 12.0), 32.0);
  // It remembers the song it was made from.
  EXPECT_DOUBLE_EQ(clip.remix->source_in, 0.0);
  EXPECT_DOUBLE_EQ(clip.remix->source_out, 50.0);
}

TEST(Remix, IsTrimmedAlongTheRemixAndCannotBeTrimmedPastItsEnd) {
  Project p = apply_remix(with_song(), "music", twenty_seconds());
  p = set_clip_edge(std::move(p), "music", ClipEdge::Out, 2.0 + 15.0);
  EXPECT_DOUBLE_EQ(clip_duration(*find_clip(p, "music")), 15.0);
  p = set_clip_edge(std::move(p), "music", ClipEdge::Out, 2.0 + 40.0);
  EXPECT_DOUBLE_EQ(clip_duration(*find_clip(p, "music")), 20.0);
}

TEST(Remix, SplitsIntoTwoHalvesThatEachPlayTheirPartOfIt) {
  Project p = apply_remix(with_song(), "music", twenty_seconds());
  const std::vector<std::string> ids{"music"};
  p = split_at(std::move(p), 2.0 + 8.0, ids);
  const Track& a1 = p.sequence().tracks[1];
  ASSERT_EQ(a1.clips.size(), 3u);
  for (const Clip& c : a1.clips) {
    if (c.id == "bang") continue;
    ASSERT_TRUE(c.remix.has_value());
    if (c.id == "music") {
      EXPECT_DOUBLE_EQ(source_time_at(c, 2.0 + 1.0), 1.0);
    } else {
      EXPECT_DOUBLE_EQ(source_time_at(c, 2.0 + 12.0), 32.0);
    }
  }
}

TEST(Remix, SlipsOnlyAsFarAsTheRemixGoes) {
  Project p = apply_remix(with_song(), "music", twenty_seconds());
  p = set_clip_edge(std::move(p), "music", ClipEdge::Out, 2.0 + 15.0);
  p = slip_clip(std::move(p), "music", 30.0);
  const Clip& clip = *find_clip(p, "music");
  EXPECT_DOUBLE_EQ(clip.source_in, 5.0);
  EXPECT_DOUBLE_EQ(clip.source_out, 20.0);
}

TEST(Remix, IsSavedAndReadBack) {
  const Project p = apply_remix(with_song(), "music",
                                Remix{.target = 21.0,
                                      .segments = 70,
                                      .variations = 20,
                                      .spans = {{0.0, 10.0}, {30.0, 40.0}}});
  auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(find_clip(loaded->project, "music")->remix, find_clip(p, "music")->remix);
}

TEST(Remix, TakenOffGivesTheSongBackCutShortWhereSomethingNowFollowsIt) {
  Project p = apply_remix(with_song(), "music", twenty_seconds());
  // Something put down in the room the remix left.
  p.sequence().tracks[1].clips[1].start = 32.0;
  p = clear_remix(std::move(p), "music");
  const Clip& clip = *find_clip(p, "music");
  EXPECT_FALSE(clip.remix.has_value());
  EXPECT_DOUBLE_EQ(clip.source_in, 0.0);
  EXPECT_DOUBLE_EQ(clip.source_out, 30.0);
}

TEST(Remix, AgainStartsFromTheSongAndNotFromTheLastRemix) {
  Project p = apply_remix(with_song(), "music", twenty_seconds());
  p = apply_remix(std::move(p), "music", Remix{.target = 5.0, .spans = {{0.0, 5.0}}});
  const std::optional<RemixSource> source = remix_source(p, "music");
  ASSERT_TRUE(source.has_value());
  EXPECT_DOUBLE_EQ(source->source_out, 50.0);
  EXPECT_DOUBLE_EQ(clip_duration(*find_clip(p, "music")), 5.0);
}

TEST(Remix, HasRoomUpToTheNextClipOnItsTrack) {
  const Project p = with_song();
  EXPECT_DOUBLE_EQ(remix_room(p, "music"), 58.0);
  EXPECT_TRUE(std::isinf(remix_room(p, "bang")));
}

TEST(Remix, IsRefusedForAPictureANestOrATimeRemappedClip) {
  Project p = with_song();
  EXPECT_FALSE(why_not_remix(p, "music").has_value());

  p.sequence().tracks[0].clips.push_back(
      Clip{.id = "picture", .media_id = "song", .kind = TrackKind::Video, .source_out = 5.0});
  EXPECT_TRUE(why_not_remix(p, "picture").has_value());

  Project nested = with_song();
  nested.media[0].sequence_id = "seq";
  EXPECT_TRUE(why_not_remix(nested, "music").has_value());

  Project remapped = with_song();
  find_clip(remapped, "music")->keyframes[anim_prop_index(AnimProp::Speed)] = {
      Keyframe{.t = 0.0, .v = 1.0}, Keyframe{.t = 1.0, .v = 2.0}};
  EXPECT_TRUE(why_not_remix(remapped, "music").has_value());
  EXPECT_EQ(apply_remix(remapped, "music", twenty_seconds()), remapped);
}

}  // namespace
}  // namespace cutline::core
