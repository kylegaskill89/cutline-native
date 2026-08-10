/// More than one cut in a project.
///
/// The rules worth pinning are the ones a reader would otherwise have to check
/// for: a project always has a sequence, the open one survives its neighbours
/// being removed, and a name never appears twice in the tab strip.

#include "cutline/core/sequences.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::core {
namespace {

[[nodiscard]] Project with_footage() {
  Project p = empty_project();
  p.sequences.front().id = "s1";
  p.sequences.front().name = "First";

  Media m;
  m.id = "m1";
  m.name = "Boiler";
  m.has_video = true;
  m.audio_stream_count = 2;
  m.duration = 8.0;
  m.width = 3840;
  m.height = 2160;
  m.fps = 60.0;
  p.media = {m};
  return p;
}

// ------------------------------------------------------------ the default --

TEST(Sequences, AProjectAlwaysHasOne) {
  const Project p;
  EXPECT_EQ(p.sequences.size(), 1u);
  EXPECT_EQ(p.open, 0u);
  // And `sequence()` holds up even if somebody empties the list by hand, so no
  // reader has to check first.
  Project emptied;
  emptied.sequences.clear();
  EXPECT_TRUE(emptied.sequence().tracks.empty());
  EXPECT_EQ(emptied.sequence().canvas_w, 1920);
}

// ------------------------------------------------------------------ adding --

TEST(Sequences, AddingOneTakesTheShapeOfTheOpenOne) {
  Project p = with_footage();
  p.sequence().canvas_w = 1280;
  p.sequence().canvas_h = 720;
  p.sequence().fps = 24.0;

  p = add_sequence(std::move(p));
  ASSERT_EQ(p.sequences.size(), 2u);
  EXPECT_EQ(p.sequences[1].canvas_w, 1280);
  EXPECT_DOUBLE_EQ(p.sequences[1].fps, 24.0);
  EXPECT_FALSE(p.sequences[1].id.empty());
}

TEST(Sequences, AddingOneDoesNotOpenIt) {
  // Adding and switching are two decisions. A "new sequence from a clip" that
  // opened it would take somebody away from what they were cutting.
  Project p = with_footage();
  p = add_sequence(std::move(p));
  EXPECT_EQ(p.open, 0u);
  EXPECT_EQ(p.sequence().id, "s1");
}

TEST(Sequences, NamesAreNeverRepeated) {
  Project p = with_footage();
  p = add_sequence(std::move(p), "Cut");
  p = add_sequence(std::move(p), "Cut");
  p = add_sequence(std::move(p), "Cut");
  EXPECT_EQ(p.sequences[1].name, "Cut");
  EXPECT_EQ(p.sequences[2].name, "Cut 2");
  EXPECT_EQ(p.sequences[3].name, "Cut 3");
}

TEST(Sequences, AnUnnamedOneIsNumbered) {
  Project p;
  p = add_sequence(std::move(p));
  p = add_sequence(std::move(p));
  EXPECT_EQ(p.sequences[1].name, "Sequence 01");
  EXPECT_EQ(p.sequences[2].name, "Sequence 02");
}

// -------------------------------------------------------------- from a clip --

TEST(Sequences, OneMadeFromAClipIsShapedToTheFootage) {
  Project p = with_footage();
  p = sequence_from_clip(std::move(p), "m1");
  ASSERT_EQ(p.sequences.size(), 2u);

  const Sequence& made = p.sequences[1];
  EXPECT_EQ(made.name, "Boiler");
  EXPECT_EQ(made.canvas_w, 3840);
  EXPECT_EQ(made.canvas_h, 2160);
  EXPECT_DOUBLE_EQ(made.fps, 60.0);
}

TEST(Sequences, OneMadeFromAClipHoldsThatClipOnBothKinds) {
  Project p = sequence_from_clip(with_footage(), "m1");
  const Sequence& made = p.sequences[1];
  ASSERT_EQ(made.tracks.size(), 2u);
  EXPECT_EQ(made.tracks[0].kind, TrackKind::Video);
  EXPECT_EQ(made.tracks[1].kind, TrackKind::Audio);

  ASSERT_EQ(made.tracks[0].clips.size(), 1u);
  ASSERT_EQ(made.tracks[1].clips.size(), 1u);
  EXPECT_DOUBLE_EQ(made.tracks[0].clips[0].source_out, 8.0);

  // Linked, so trimming the picture takes the sound with it.
  ASSERT_TRUE(made.tracks[0].clips[0].group_id.has_value());
  EXPECT_EQ(made.tracks[0].clips[0].group_id, made.tracks[1].clips[0].group_id);
}

TEST(Sequences, AClipWithNoSoundGetsNoAudioTrack) {
  Project p = with_footage();
  p.media.front().audio_stream_count = 0;
  p = sequence_from_clip(std::move(p), "m1");

  const Sequence& made = p.sequences[1];
  ASSERT_EQ(made.tracks.size(), 1u);
  EXPECT_EQ(made.tracks[0].kind, TrackKind::Video);
  EXPECT_FALSE(made.tracks[0].clips[0].group_id.has_value()) << "nothing to link it to";
}

TEST(Sequences, AMediaThatIsNotThereMakesNothing) {
  const Project p = with_footage();
  EXPECT_EQ(sequence_from_clip(p, "nope"), p);
}

// ----------------------------------------------------------------- opening --

TEST(Sequences, OpeningOneSwitchesWhatIsBeingCut) {
  Project p = add_sequence(with_footage(), "Second");
  const std::string second = p.sequences[1].id;

  p = open_sequence(std::move(p), second);
  EXPECT_EQ(p.sequence().name, "Second");
  EXPECT_EQ(p.open, 1u);
}

TEST(Sequences, OpeningOneThatIsNotThereChangesNothing) {
  const Project p = add_sequence(with_footage(), "Second");
  EXPECT_EQ(open_sequence(p, "nope"), p);
}

// ---------------------------------------------------------------- renaming --

TEST(Sequences, RenamingToItsOwnNameDoesNotNumberIt) {
  Project p = add_sequence(with_footage(), "Cut");
  const std::string id = p.sequences[1].id;
  p = rename_sequence(std::move(p), id, "Cut");
  EXPECT_EQ(p.sequences[1].name, "Cut") << "it clashed only with itself";
}

TEST(Sequences, RenamingOntoAnotherNameIsMadeUnique) {
  Project p = add_sequence(with_footage(), "Cut");
  p = add_sequence(std::move(p), "Other");
  p = rename_sequence(std::move(p), p.sequences[2].id, "Cut");
  EXPECT_EQ(p.sequences[2].name, "Cut 2");
}

// ---------------------------------------------------------------- removing --

TEST(Sequences, TheLastOneCannotBeRemoved) {
  const Project p = with_footage();
  EXPECT_EQ(remove_sequence(p, "s1"), p);
}

TEST(Sequences, RemovingTheOpenOneOpensANeighbour) {
  Project p = add_sequence(with_footage(), "Second");
  p = open_sequence(std::move(p), p.sequences[1].id);
  ASSERT_EQ(p.open, 1u);

  p = remove_sequence(std::move(p), p.sequences[1].id);
  ASSERT_EQ(p.sequences.size(), 1u);
  EXPECT_EQ(p.open, 0u);
  EXPECT_EQ(p.sequence().name, "First");
}

TEST(Sequences, RemovingOneBeforeTheOpenOneKeepsItOpen) {
  // The index shifts underneath, and following it is the whole of this.
  Project p = add_sequence(with_footage(), "Second");
  p = add_sequence(std::move(p), "Third");
  p = open_sequence(std::move(p), p.sequences[2].id);
  ASSERT_EQ(p.sequence().name, "Third");

  p = remove_sequence(std::move(p), p.sequences[0].id);
  EXPECT_EQ(p.sequence().name, "Third") << "the open one moved down a place";
}

// ------------------------------------------------------------ on the disc --

TEST(Sequences, ASecondSequenceSurvivesTheRoundTrip) {
  Project p = add_sequence(with_footage(), "Second");
  p = sequence_from_clip(std::move(p), "m1");
  p = open_sequence(std::move(p), p.sequences[1].id);

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project, p);
}

// A project with one sequence is written exactly where a project's own fields
// have always been, so a file written before there could be two reads back as
// the shape it was — and an older build can still open one written now.
TEST(Sequences, OneSequenceIsWrittenTheWayItAlwaysWas) {
  Project p = with_footage();
  p.sequence().canvas_w = 1280;

  const std::string written = to_json(p);
  EXPECT_NE(written.find("\"canvas_w\""), std::string::npos);
  EXPECT_EQ(written.find("\"sequences\""), std::string::npos)
      << "one sequence should add nothing to the file";
}

TEST(Sequences, AFileWrittenBeforeSequencesExistedReadsAsOne) {
  const std::string older = R"({
    "version": 1,
    "project": {
      "canvas_w": 1280, "canvas_h": 720, "fps": 24.0,
      "tracks": [{"id": "v1", "kind": "video", "clips": []}]
    }
  })";

  const auto loaded = from_json(older);
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_EQ(loaded->project.sequences.size(), 1u);
  EXPECT_EQ(loaded->project.open, 0u);
  EXPECT_EQ(loaded->project.sequence().canvas_w, 1280);
  EXPECT_EQ(loaded->project.sequence().tracks.size(), 1u);
}

}  // namespace
}  // namespace cutline::core
