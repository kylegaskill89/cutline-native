/// Captions: a track of clips with text and no media, and SubRip both ways.
///
/// What matters is what Premiere's caption editing depends on: a caption made
/// at the playhead stops short of the next, splitting keeps the words in
/// both halves, merging joins them in time order, the topmost visible track
/// is the one shown, and a SubRip file real enough to have come from
/// somewhere else reads, and writes back as SubRip.

#include "cutline/core/captions.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

[[nodiscard]] Project with_captions(std::string* track_id) {
  Project p = empty_project();
  p = add_caption_track(std::move(p), track_id);
  p = add_caption(std::move(p), *track_id, 1.0, "Hello");
  p = add_caption(std::move(p), *track_id, 6.0, "World");
  return p;
}

[[nodiscard]] const Track& captions_of(const Project& p) { return p.sequence().tracks.front(); }

}  // namespace

TEST(Captions, ACaptionTrackGoesAboveEverything) {
  std::string id;
  const Project p = add_caption_track(empty_project(), &id);
  ASSERT_FALSE(p.sequence().tracks.empty());
  EXPECT_EQ(p.sequence().tracks.front().id, id);
  EXPECT_TRUE(is_caption_track(p.sequence().tracks.front()));
}

TEST(Captions, ACaptionLastsThreeSecondsOrUpToTheNext) {
  std::string id;
  Project p = with_captions(&id);
  ASSERT_EQ(captions_of(p).clips.size(), 2u);
  EXPECT_DOUBLE_EQ(clip_duration(captions_of(p).clips[0]), 3.0);
  // From 5, the next starts at 6, so it gets one second.
  p = add_caption(std::move(p), id, 5.0, "Between");
  ASSERT_EQ(captions_of(p).clips.size(), 3u);
  EXPECT_DOUBLE_EQ(captions_of(p).clips[1].start, 5.0);
  EXPECT_DOUBLE_EQ(clip_duration(captions_of(p).clips[1]), 1.0);
  // Not inside one already there.
  const Project before = p;
  EXPECT_EQ(add_caption(p, id, 2.0, "Inside"), before);
}

TEST(Captions, TheirWordsCanBeChanged) {
  std::string id;
  Project p = with_captions(&id);
  const std::string first = captions_of(p).clips[0].id;
  p = set_caption_text(std::move(p), first, "Good morning\nEveryone");
  EXPECT_EQ(captions_of(p).clips[0].caption, "Good morning\nEveryone");
}

TEST(Captions, SplittingKeepsTheWordsInBothHalves) {
  std::string id;
  Project p = with_captions(&id);
  p = split_caption(std::move(p), captions_of(p).clips[0].id);
  ASSERT_EQ(captions_of(p).clips.size(), 3u);
  EXPECT_DOUBLE_EQ(clip_end(captions_of(p).clips[0]), 2.5);
  EXPECT_DOUBLE_EQ(captions_of(p).clips[1].start, 2.5);
  EXPECT_EQ(captions_of(p).clips[1].caption, "Hello");
}

TEST(Captions, MergingJoinsThemInTimeOrderAcrossTheGap) {
  std::string id;
  Project p = with_captions(&id);
  const std::vector<std::string> both{captions_of(p).clips[1].id, captions_of(p).clips[0].id};
  p = merge_captions(std::move(p), both);
  ASSERT_EQ(captions_of(p).clips.size(), 1u);
  EXPECT_EQ(captions_of(p).clips[0].caption, "Hello World");
  EXPECT_DOUBLE_EQ(captions_of(p).clips[0].start, 1.0);
  EXPECT_DOUBLE_EQ(clip_end(captions_of(p).clips[0]), 9.0);
}

TEST(Captions, TheTopmostVisibleTrackIsTheOneShown) {
  std::string lower;
  Project p = with_captions(&lower);
  std::string upper;
  p = add_caption_track(std::move(p), &upper);
  p = add_caption(std::move(p), upper, 1.5, "Bonjour");
  EXPECT_EQ(caption_at(p.sequence(), 2.0)->caption, "Bonjour");
  p.sequence().tracks.front().hidden = true;
  EXPECT_EQ(caption_at(p.sequence(), 2.0)->caption, "Hello");
  EXPECT_EQ(caption_at(p.sequence(), 4.5), nullptr) << "between captions there is none";
}

TEST(Captions, ACaptionIsDrawnAsATextLayerStandingOnItsFoot) {
  CaptionStyle style;
  style.y = 0.85;
  const Graphic g = caption_graphic(style, "Two\nlines", 1920, 1080);
  ASSERT_EQ(g.layers.size(), 1u);
  const GraphicLayer& layer = g.layers[0];
  EXPECT_EQ(layer.text, "Two\nlines");
  EXPECT_DOUBLE_EQ(layer.y, 0.85);
  EXPECT_DOUBLE_EQ(layer.anchor_y, 1.0);
  EXPECT_DOUBLE_EQ(*layer.box_width, 0.8 * 1920);
  EXPECT_TRUE(layer.style.background.has_value());
}

TEST(Captions, TheyAndTheirStyleSurviveSaving) {
  std::string id;
  Project p = with_captions(&id);
  CaptionStyle style;
  style.look.fill = "#ffff00";
  style.y = 0.1;
  p = set_caption_style(std::move(p), id, style);
  const auto back = from_json(to_json(p));
  ASSERT_TRUE(back.has_value()) << back.error();
  EXPECT_EQ(back->project, p);
}

// ------------------------------------------------------------------- SRT --

TEST(Srt, AFileFromElsewhereReads) {
  // A byte-order mark, Windows line endings, a full stop for the comma, a
  // tag, two lines, and a missing blank line before the third cue.
  const std::string file =
      "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hello</i> there\r\n\r\n"
      "2\r\n00:00:03.25 --> 00:00:05,000\r\nTwo\r\nlines\r\n"
      "3\r\n00:01:00,000 --> 00:01:01,000 X1:10 X2:20\r\nLast\r\n";
  const auto cues = parse_srt(file);
  ASSERT_TRUE(cues.has_value()) << cues.error();
  ASSERT_EQ(cues->size(), 3u);
  EXPECT_EQ((*cues)[0], (CaptionCue{.start = 1.0, .end = 2.5, .text = "Hello there"}));
  EXPECT_DOUBLE_EQ((*cues)[1].start, 3.25);
  EXPECT_EQ((*cues)[1].text, "Two\nlines");
  EXPECT_DOUBLE_EQ((*cues)[2].start, 60.0);
  EXPECT_EQ((*cues)[2].text, "Last");
}

TEST(Srt, SomethingWithNoCuesIsRefused) {
  EXPECT_FALSE(parse_srt("not captions at all").has_value());
}

TEST(Srt, ATrackWritesAndReadsBack) {
  std::string id;
  Project p = with_captions(&id);
  p = set_caption_text(std::move(p), captions_of(p).clips[1].id, "Two\nlines");
  const std::string written = write_srt(captions_of(p));
  EXPECT_TRUE(written.starts_with("1\r\n00:00:01,000 --> 00:00:04,000\r\nHello\r\n\r\n2\r\n"))
      << written;
  const auto back = parse_srt(written);
  ASSERT_TRUE(back.has_value());
  ASSERT_EQ(back->size(), 2u);
  EXPECT_EQ((*back)[1], (CaptionCue{.start = 6.0, .end = 9.0, .text = "Two\nlines"}));
}

TEST(Srt, StylingIsWrittenAsSubRipsOwnTags) {
  std::string id;
  Project p = with_captions(&id);
  CaptionStyle style;
  style.look.italic = true;
  style.look.fill = "#ffff00";
  p = set_caption_style(std::move(p), id, style);
  const std::string written = write_srt(captions_of(p), true);
  EXPECT_NE(written.find("<font color=\"#ffff00\"><i>Hello</i></font>"), std::string::npos);
  EXPECT_EQ(write_srt(captions_of(p)).find("<i>"), std::string::npos);
}

TEST(Srt, ASidecarHoldsWhatTheExportedSpanShowsTimedFromItsStart) {
  std::string id;
  const Project p = with_captions(&id);
  // Exporting 2 to 7: "Hello" (1 to 4) is cut to 0 to 2, "World" (6 to 9) to
  // 4 to 5.
  const auto srt = sidecar_srt(p.sequence(), 2.0, 7.0);
  ASSERT_TRUE(srt.has_value());
  const auto cues = parse_srt(*srt);
  ASSERT_TRUE(cues.has_value());
  ASSERT_EQ(cues->size(), 2u);
  EXPECT_EQ((*cues)[0], (CaptionCue{.start = 0.0, .end = 2.0, .text = "Hello"}));
  EXPECT_EQ((*cues)[1], (CaptionCue{.start = 4.0, .end = 5.0, .text = "World"}));
  EXPECT_FALSE(sidecar_srt(empty_project().sequence(), 0.0, 10.0).has_value());
}

TEST(Srt, ImportingMakesATrackFromTheStartPoint) {
  const std::vector<CaptionCue> cues{{.start = 0.0, .end = 2.0, .text = "A"},
                                     {.start = 1.5, .end = 3.0, .text = "B"}};
  std::string id;
  const Project p = import_captions(empty_project(), cues, 10.0, &id);
  const Track& track = p.sequence().tracks.front();
  EXPECT_EQ(track.id, id);
  ASSERT_EQ(track.clips.size(), 2u);
  EXPECT_DOUBLE_EQ(track.clips[0].start, 10.0);
  EXPECT_DOUBLE_EQ(clip_end(track.clips[0]), 11.5) << "cut back to where the next begins";
  EXPECT_EQ(track.clips[1].caption, "B");
}

}  // namespace cutline::core
