/// A sequence inside a sequence.
///
/// The interesting behaviour is what a nest *keeps*: the clips keep their
/// lanes and their offsets from each other, the pool entry keeps agreeing with
/// the sequence as that is edited, and no sequence ever ends up inside itself.

#include "cutline/core/nesting.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/sequences.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

/// Two shots on V1 with a title over the second on V2, and sound under both.
[[nodiscard]] Project a_cut() {
  Project p = empty_project();
  p.sequences.front().id = "seq";
  p.sequences.front().canvas_w = 1920;
  p.sequences.front().canvas_h = 1080;
  p.sequences.front().fps = 30.0;
  p.media = {Media{.id = "m1", .name = "wide.mp4", .duration = 60.0, .has_video = true,
                   .audio_stream_count = 1}};

  Track upper{.id = "v2", .kind = TrackKind::Video};
  upper.clips = {Clip{.id = "over", .media_id = "m1", .source_in = 0.0, .source_out = 2.0,
                      .start = 3.0}};

  Track lower{.id = "v1", .kind = TrackKind::Video};
  lower.clips = {
      Clip{.id = "a", .media_id = "m1", .source_in = 0.0, .source_out = 4.0, .start = 1.0},
      Clip{.id = "b", .media_id = "m1", .source_in = 4.0, .source_out = 8.0, .start = 5.0}};

  Track sound{.id = "a1", .kind = TrackKind::Audio};
  sound.clips = {Clip{.id = "s", .media_id = "m1", .kind = TrackKind::Audio, .source_in = 0.0,
                      .source_out = 4.0, .start = 1.0}};

  p.sequences.front().tracks = {std::move(upper), std::move(lower), std::move(sound)};
  return p;
}

[[nodiscard]] const Media* nest_media(const Project& p) {
  for (const Media& m : p.media) {
    if (is_nested_sequence(m)) return &m;
  }
  return nullptr;
}

}  // namespace

TEST(Nesting, PutsTheClipsInASequenceAndLeavesOneClipBehind) {
  const std::vector<std::string> chosen{"a", "b"};
  const Project after = nest_clips(a_cut(), chosen);

  EXPECT_EQ(after.sequences.size(), 2u);
  EXPECT_EQ(find_clip(after, "a"), nullptr) << "the originals are still out here";
  EXPECT_EQ(find_clip(after, "b"), nullptr);

  const Media* media = nest_media(after);
  ASSERT_NE(media, nullptr);
  const Sequence* inner = find_sequence(after, media->sequence_id);
  ASSERT_NE(inner, nullptr);

  std::size_t held = 0;
  for (const Track& track : inner->tracks) held += track.clips.size();
  EXPECT_EQ(held, 2u);
}

TEST(Nesting, TheNestCoversWhatWasNested) {
  // From the start of the earliest to the end of the latest. A gap between two
  // nested clips is part of what was nested, and closing it would move footage
  // nobody asked to move.
  const std::vector<std::string> chosen{"a", "b"};
  const Project after = nest_clips(a_cut(), chosen);

  const Clip* nest = nullptr;
  for (const Track& track : after.sequence().tracks) {
    for (const Clip& clip : track.clips) {
      const Media* media = nullptr;
      for (const Media& m : after.media) {
        if (m.id == clip.media_id) media = &m;
      }
      if (media != nullptr && is_nested_sequence(*media)) nest = &clip;
    }
  }
  ASSERT_NE(nest, nullptr);
  EXPECT_DOUBLE_EQ(nest->start, 1.0);
  EXPECT_DOUBLE_EQ(clip_end(*nest), 9.0);
}

TEST(Nesting, WhatWasAboveStaysAbove) {
  // Two video lanes go in and two come out, in the same order — otherwise a
  // nest of an overlay and its background composites them the wrong way round.
  const std::vector<std::string> chosen{"a", "over"};
  const Project after = nest_clips(a_cut(), chosen);

  const Media* media = nest_media(after);
  ASSERT_NE(media, nullptr);
  const Sequence* inner = find_sequence(after, media->sequence_id);
  ASSERT_NE(inner, nullptr);

  std::vector<const Track*> video;
  for (const Track& track : inner->tracks) {
    if (track.kind == TrackKind::Video) video.push_back(&track);
  }
  ASSERT_GE(video.size(), 2u);
  ASSERT_EQ(video[0]->clips.size(), 1u) << "the topmost lane";
  EXPECT_EQ(video[0]->clips[0].id, "over");
  ASSERT_EQ(video[1]->clips.size(), 1u);
  EXPECT_EQ(video[1]->clips[0].id, "a");
}

TEST(Nesting, TheClipsKeepTheirOffsetsFromEachOther) {
  // Rebased on the earliest, so the nest begins at zero and everything inside
  // it keeps the spacing it had.
  const std::vector<std::string> chosen{"a", "b"};
  const Project after = nest_clips(a_cut(), chosen);

  const Sequence* inner = find_sequence(after, nest_media(after)->sequence_id);
  ASSERT_NE(inner, nullptr);

  double first = -1.0;
  double second = -1.0;
  for (const Track& track : inner->tracks) {
    for (const Clip& clip : track.clips) {
      if (clip.id == "a") first = clip.start;
      if (clip.id == "b") second = clip.start;
    }
  }
  EXPECT_DOUBLE_EQ(first, 0.0);
  EXPECT_DOUBLE_EQ(second, 4.0) << "they were four seconds apart before";
}

TEST(Nesting, TheNestTakesTheCanvasItIsCompositedInto) {
  const std::vector<std::string> chosen{"a"};
  const Project after = nest_clips(a_cut(), chosen);
  const Sequence* inner = find_sequence(after, nest_media(after)->sequence_id);

  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->canvas_w, 1920);
  EXPECT_EQ(inner->canvas_h, 1080);
  EXPECT_DOUBLE_EQ(inner->fps, 30.0);
}

TEST(Nesting, NestingNothingChangesNothing) {
  const Project before = a_cut();
  EXPECT_EQ(nest_clips(before, {}), before);

  const std::vector<std::string> ghost{"nobody"};
  EXPECT_EQ(nest_clips(before, ghost), before);
}

// ------------------------------------------------------------- the pool --

TEST(Nesting, ThePoolEntryReportsTheSequenceInIt) {
  const std::vector<std::string> chosen{"a", "b"};
  const Project after = nest_clips(a_cut(), chosen);

  const Media* media = nest_media(after);
  ASSERT_NE(media, nullptr);
  EXPECT_TRUE(media->path.empty()) << "a nest has no file behind it";
  EXPECT_TRUE(media->has_video);
  EXPECT_DOUBLE_EQ(media->duration, 8.0) << "the length of what was nested";
  EXPECT_EQ(media->width.value_or(0), 1920);
}

TEST(Nesting, ThePoolEntryFollowsAnEditInsideTheNest) {
  // The whole reason `sync_nested_media` exists. Trimming inside a nest makes
  // it shorter, and a pool entry still claiming the old length would let a clip
  // of it be trimmed to footage that is not there.
  const std::vector<std::string> chosen{"a", "b"};
  Project after = nest_clips(a_cut(), chosen);
  const std::string inner_id = nest_media(after)->sequence_id;

  const std::size_t inner = sequence_index(after, inner_id);
  ASSERT_NE(inner, std::string::npos);
  for (Track& track : after.sequences[inner].tracks) {
    std::erase_if(track.clips, [](const Clip& clip) { return clip.id == "b"; });
  }

  after = sync_nested_media(std::move(after));
  EXPECT_DOUBLE_EQ(nest_media(after)->duration, 4.0);
}

TEST(Nesting, RenamingTheSequenceRenamesWhatIsInThePool) {
  const std::vector<std::string> chosen{"a"};
  Project after = nest_clips(a_cut(), chosen);
  const std::string inner_id = nest_media(after)->sequence_id;

  after = rename_sequence(std::move(after), inner_id, "The Grade");
  after = sync_nested_media(std::move(after));
  EXPECT_EQ(nest_media(after)->name, "The Grade");
}

// --------------------------------------------------------------- cycles --

TEST(Nesting, ASequenceIsInsideItself) {
  const Project p = a_cut();
  EXPECT_TRUE(sequence_contains(p, "seq", "seq"));
}

TEST(Nesting, NestingIsFoundAtAnyDepth) {
  const std::vector<std::string> chosen{"a"};
  Project after = nest_clips(a_cut(), chosen);
  const std::string one = nest_media(after)->sequence_id;

  // And again, around the nest itself: two deep.
  std::vector<std::string> outer;
  for (const Track& track : after.sequence().tracks) {
    for (const Clip& clip : track.clips) {
      for (const Media& m : after.media) {
        if (m.id == clip.media_id && is_nested_sequence(m)) outer.push_back(clip.id);
      }
    }
  }
  ASSERT_FALSE(outer.empty());
  after = nest_clips(std::move(after), outer, "Outer");

  const Sequence& open = after.sequence();
  EXPECT_TRUE(sequence_contains(after, open.id, one))
      << "the sequence two levels down was not found";
}

TEST(Nesting, ASequenceCannotBePutInsideItself) {
  // Directly is the easy case. This is the one that matters: the clip being
  // nested already holds the sequence being cut in, so nesting it would close
  // the loop — and nothing that renders it could terminate.
  Project after = nest_clips(a_cut(), std::vector<std::string>{"a"});
  const std::string inner_id = nest_media(after)->sequence_id;

  // Put a clip of the *outer* sequence inside the nest by hand, which is what
  // a file written by something less careful would look like.
  Media outer_entry;
  outer_entry.id = "outer-media";
  outer_entry.sequence_id = "seq";
  after.media.push_back(outer_entry);

  const std::size_t inner = sequence_index(after, inner_id);
  after.sequences[inner].tracks[0].clips.push_back(
      Clip{.id = "loop", .media_id = "outer-media", .source_in = 0.0, .source_out = 1.0});

  EXPECT_TRUE(sequence_contains(after, inner_id, "seq"));

  // And now nesting that nest would put "seq" inside itself. Refused.
  std::vector<std::string> holding;
  for (const Track& track : after.sequence().tracks) {
    for (const Clip& clip : track.clips) {
      for (const Media& m : after.media) {
        if (m.id == clip.media_id && is_nested_sequence(m)) holding.push_back(clip.id);
      }
    }
  }
  ASSERT_FALSE(holding.empty());
  EXPECT_EQ(nest_clips(after, holding), after) << "it closed the loop";
}

TEST(Nesting, AProjectThatAlreadyHoldsACycleIsWalkedOnce) {
  // Written by hand or by an older build. `sequence_contains` must come back
  // rather than going round for ever.
  Project p = a_cut();
  p = add_sequence(std::move(p), "Other");
  p.sequences[1].id = "other";

  Media into_other{.id = "mo"};
  into_other.sequence_id = "other";
  Media into_seq{.id = "ms"};
  into_seq.sequence_id = "seq";
  p.media.push_back(into_other);
  p.media.push_back(into_seq);

  p.sequences[0].tracks[0].clips.push_back(
      Clip{.id = "x", .media_id = "mo", .source_in = 0.0, .source_out = 1.0});
  p.sequences[1].tracks[0].clips.push_back(
      Clip{.id = "y", .media_id = "ms", .source_in = 0.0, .source_out = 1.0});

  EXPECT_TRUE(sequence_contains(p, "seq", "other"));
  EXPECT_TRUE(sequence_contains(p, "other", "seq"));
}

// ----------------------------------------------------------- and the file --

TEST(Nesting, ANestSurvivesBeingSavedAndOpened) {
  const std::vector<std::string> chosen{"a", "b"};
  const Project before = nest_clips(a_cut(), chosen);
  const auto read = from_json(to_json(before));

  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_EQ(read->project, before);
  ASSERT_NE(nest_media(read->project), nullptr);
  EXPECT_FALSE(nest_media(read->project)->sequence_id.empty());
}

TEST(Nesting, AProjectWithNoNestWritesNoSequenceField) {
  EXPECT_EQ(to_json(a_cut()).find("sequence_id"), std::string::npos);
}

}  // namespace cutline::core
