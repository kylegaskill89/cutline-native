/// Project files, and knowing whether there is anything to save.
///
/// These touch the disk, in a directory of their own that is removed
/// afterwards. The interesting cases are the unhappy ones: a file that is not
/// there, a file that is not a project, and a save that fails partway.

#include "cutline/editor/document.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/query.hpp"
#include "cutline/editor/session.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

[[nodiscard]] Project sample_project() {
  Project project;
  project.fps = 30.0;
  project.canvas_w = 1280;
  project.canvas_h = 720;
  project.media.push_back(
      Media{.id = "m1", .name = "wide.mp4", .duration = 60.0, .has_video = true});

  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {
      Clip{.id = "c1", .media_id = "m1", .source_in = 0.0, .source_out = 5.0, .start = 0.0}};
  project.tracks.push_back(std::move(video));
  return project;
}

/// A directory of its own, removed when the test finishes.
class Scratch : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("cutline-document-test-" + std::to_string(::testing::UnitTest::GetInstance()
                                                          ->random_seed()) +
            "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code error;
    std::filesystem::remove_all(dir_, error);
    std::filesystem::create_directories(dir_, error);
    if (error) GTEST_SKIP() << "could not make a scratch directory";
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  [[nodiscard]] std::filesystem::path file(std::string name) const { return dir_ / name; }

  std::filesystem::path dir_;
};

// ------------------------------------------------------------- round trip --

TEST_F(Scratch, AProjectSurvivesBeingWrittenAndReadBack) {
  const Project before = sample_project();
  const std::filesystem::path path = file("round.cutline");

  ASSERT_TRUE(write_project(path, before).has_value());
  const auto loaded = read_project(path);
  ASSERT_TRUE(loaded.has_value()) << loaded.error();

  EXPECT_EQ(loaded->project, before);
}

TEST_F(Scratch, WritingLeavesNoDebrisBehind) {
  const std::filesystem::path path = file("clean.cutline");
  ASSERT_TRUE(write_project(path, sample_project()).has_value());

  // The staging file is renamed over the target, so it should not still be
  // sitting there confusing whoever opens the folder.
  int count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
    ++count;
    EXPECT_EQ(entry.path().filename(), path.filename());
  }
  EXPECT_EQ(count, 1);
}

TEST_F(Scratch, SavingOverAProjectReplacesIt) {
  const std::filesystem::path path = file("over.cutline");
  ASSERT_TRUE(write_project(path, sample_project()).has_value());

  Project changed = sample_project();
  changed.fps = 24.0;
  ASSERT_TRUE(write_project(path, changed).has_value());

  const auto loaded = read_project(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_DOUBLE_EQ(loaded->project.fps, 24.0);
}

// --------------------------------------------------------------- failures --

TEST_F(Scratch, AFileThatIsNotThereSaysSo) {
  const auto loaded = read_project(file("nothing-here.cutline"));
  ASSERT_FALSE(loaded.has_value());
  EXPECT_FALSE(loaded.error().empty());
}

TEST_F(Scratch, AFileThatIsNotAProjectSaysWhy) {
  const std::filesystem::path path = file("rubbish.cutline");
  {
    std::ofstream out(path);
    out << "this is not JSON at all {{{";
  }

  const auto loaded = read_project(path);
  ASSERT_FALSE(loaded.has_value());
  EXPECT_FALSE(loaded.error().empty());
}

TEST_F(Scratch, WritingSomewhereImpossibleFails) {
  const auto written = write_project(dir_ / "no-such-folder" / "x.cutline", sample_project());
  EXPECT_FALSE(written.has_value());
}

TEST_F(Scratch, AFailedWriteLeavesTheOldFileAlone) {
  // The reason for staging and renaming. Losing an afternoon's work to a
  // half-written file is not a failure anyone recovers from.
  const std::filesystem::path path = file("precious.cutline");
  ASSERT_TRUE(write_project(path, sample_project()).has_value());

  // A directory where the staging file wants to go, so the write cannot
  // succeed but the original is untouched.
  std::error_code error;
  std::filesystem::create_directory(path.string() + ".saving", error);
  ASSERT_FALSE(error);

  Project changed = sample_project();
  changed.fps = 24.0;
  EXPECT_FALSE(write_project(path, changed).has_value());

  const auto loaded = read_project(path);
  ASSERT_TRUE(loaded.has_value()) << "the original was damaged";
  EXPECT_DOUBLE_EQ(loaded->project.fps, 30.0);
}

// ------------------------------------------------------------- extensions --

TEST(Document, ANameWithNoExtensionGetsOne) {
  EXPECT_EQ(with_project_extension("my film").extension(), kProjectExtension);
}

TEST(Document, AnExistingExtensionIsLeftAlone) {
  EXPECT_EQ(with_project_extension("my film.cutline").extension(), kProjectExtension);
  EXPECT_EQ(with_project_extension("notes.txt").extension(), ".txt");
}

TEST(Document, AnEmptyPathStaysEmpty) {
  EXPECT_TRUE(with_project_extension({}).empty());
}

// --------------------------------------------------- the document's state --

TEST(SessionDocument, ANewSessionHasNothingToSave) {
  const Session session(sample_project());
  EXPECT_FALSE(session.modified());
  EXPECT_TRUE(session.path().empty());
  EXPECT_EQ(session.document_title(), "Untitled");
}

TEST(SessionDocument, EditingMarksItModified) {
  Session session(sample_project());
  const std::vector<std::string> ids{"c1"};
  session.apply(core::move_clips(session.project(), ids, 2.0));

  EXPECT_TRUE(session.modified());
  EXPECT_EQ(session.document_title(), "Untitled *");
}

TEST(SessionDocument, SavingClearsIt) {
  Session session(sample_project());
  const std::vector<std::string> ids{"c1"};
  session.apply(core::move_clips(session.project(), ids, 2.0));

  session.mark_saved("D:/films/cut.cutline");
  EXPECT_FALSE(session.modified());
  EXPECT_EQ(session.document_title(), "cut.cutline");
}

TEST(SessionDocument, UndoingBackToWhatIsOnDiskLeavesNothingToSave) {
  // A latched flag could never work this out, which is why it is a comparison
  // against the saved state rather than a boolean that only ever goes one way.
  Session session(sample_project());
  session.mark_saved("D:/films/cut.cutline");

  const std::vector<std::string> ids{"c1"};
  session.apply(core::move_clips(session.project(), ids, 2.0));
  ASSERT_TRUE(session.modified());

  ASSERT_TRUE(session.undo());
  EXPECT_FALSE(session.modified()) << "it still thinks there is something to save";

  ASSERT_TRUE(session.redo());
  EXPECT_TRUE(session.modified());
}

TEST(SessionDocument, OpeningAProjectCountsAsSaved) {
  Session session(sample_project());
  const std::vector<std::string> ids{"c1"};
  session.apply(core::move_clips(session.project(), ids, 2.0));
  ASSERT_TRUE(session.modified());

  session.reset(sample_project(), "D:/films/other.cutline");

  EXPECT_FALSE(session.modified());
  EXPECT_EQ(session.document_title(), "other.cutline");
  EXPECT_FALSE(session.can_undo()) << "the history belonged to the file that was closed";
}

TEST(SessionDocument, ANewUntitledDocumentHasNoPath) {
  Session session(sample_project());
  session.mark_saved("D:/films/cut.cutline");
  session.reset(sample_project());

  EXPECT_TRUE(session.path().empty());
  EXPECT_EQ(session.document_title(), "Untitled");
}

}  // namespace
}  // namespace cutline::editor
