/// Recovery copies.
///
/// Two halves, tested separately for the reason they are separate: *when* a
/// copy is due is arithmetic on a clock and is asserted without touching a
/// disk, and *where* it goes and what is offered back is filesystem behaviour
/// with no timing in it.

#include "cutline/editor/autosave.hpp"

#include "cutline/editor/document.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace cutline::editor {
namespace {

using namespace std::chrono_literals;

const std::chrono::steady_clock::time_point kNow = std::chrono::steady_clock::now();

// ------------------------------------------------------------ when it is due --

TEST(AutosaveDue, ADocumentWithNothingUnsavedIsNeverDue) {
  const AutosaveState fresh;
  EXPECT_FALSE(autosave_due(fresh, false, 7, kNow));

  const AutosaveState old{.written_at = kNow - 1h, .written_revision = 3, .ever_written = true};
  EXPECT_FALSE(autosave_due(old, false, 7, kNow));
}

// The window most likely to be lost is the one right after somebody starts
// working, so the first change is written immediately rather than a minute on.
TEST(AutosaveDue, TheFirstChangeIsWrittenStraightAway) {
  const AutosaveState fresh;
  EXPECT_TRUE(autosave_due(fresh, true, 1, kNow));
}

TEST(AutosaveDue, ADocumentUntouchedSinceTheLastCopyIsNotWrittenAgain) {
  const AutosaveState state{
      .written_at = kNow - 1h, .written_revision = 12, .ever_written = true};
  // An hour later, and still nothing new to say.
  EXPECT_FALSE(autosave_due(state, true, 12, kNow));
}

TEST(AutosaveDue, AChangedDocumentWaitsForTheInterval) {
  const AutosaveState state{
      .written_at = kNow - 10s, .written_revision = 12, .ever_written = true};
  EXPECT_FALSE(autosave_due(state, true, 13, kNow, 60s));
  EXPECT_TRUE(autosave_due(state, true, 13, kNow, 5s));
}

TEST(AutosaveDue, TheIntervalIsMeasuredFromTheLastCopy) {
  const AutosaveState state{
      .written_at = kNow - 61s, .written_revision = 12, .ever_written = true};
  EXPECT_TRUE(autosave_due(state, true, 13, kNow, 60s));
}

// ------------------------------------------------------------------ where --

TEST(AutosavePath, TwoProjectsOfTheSameNameDoNotShareACopy) {
  const auto one = autosave_path_for("d:/films/first/cut.cutline");
  const auto two = autosave_path_for("d:/films/second/cut.cutline");

  EXPECT_NE(one, two);
  // Readable, though: the name is in there as well as the digest.
  EXPECT_NE(one.filename().string().find("cut"), std::string::npos);
}

TEST(AutosavePath, TheSameProjectAlwaysGetsTheSameCopy) {
  EXPECT_EQ(autosave_path_for("d:/films/cut.cutline"),
            autosave_path_for("d:/films/cut.cutline"));
}

TEST(AutosavePath, ADocumentNeverSavedStillHasSomewhereToGo) {
  const auto path = autosave_path_for({});
  EXPECT_FALSE(path.empty());
  EXPECT_EQ(path.extension().string(), std::string(kProjectExtension));
}

TEST(AutosavePath, AwkwardNamesDoNotEscapeTheDirectory) {
  const auto path = autosave_path_for("d:/films/../../a b/we:rd*name.cutline");
  EXPECT_EQ(path.parent_path(), autosave_dir());
  for (const char c : path.filename().string()) {
    EXPECT_NE(c, '/');
    EXPECT_NE(c, '\\');
    EXPECT_NE(c, ':');
  }
}

// ------------------------------------------------------- writing and finding --

/// A document path nothing else will use, cleaned up afterwards along with
/// whatever recovery copy it produced.
class Scratch {
 public:
  Scratch() {
    dir_ = std::filesystem::temp_directory_path() /
           ("cutline-autosave-" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
  }
  ~Scratch() {
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
    discard_autosave(document());
  }

  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  [[nodiscard]] std::filesystem::path document() const {
    return dir_ / ("film" + std::string(kProjectExtension));
  }

 private:
  std::filesystem::path dir_;
};

[[nodiscard]] core::Project a_project() {
  core::Project p;
  p.canvas_w = 1280;
  p.canvas_h = 720;
  return p;
}

TEST(Autosave, ACopyCanBeWrittenAndReadBack) {
  const Scratch scratch;
  ASSERT_TRUE(write_autosave(scratch.document(), a_project()).has_value());

  const auto found = find_recovery(scratch.document());
  ASSERT_TRUE(found.has_value());

  const auto loaded = read_project(found->path);
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project.canvas_w, 1280);
}

TEST(Autosave, WithNoCopyThereIsNothingToOffer) {
  const Scratch scratch;
  EXPECT_FALSE(find_recovery(scratch.document()).has_value());
}

TEST(Autosave, DiscardingLeavesNothingToOffer) {
  const Scratch scratch;
  ASSERT_TRUE(write_autosave(scratch.document(), a_project()).has_value());
  ASSERT_TRUE(find_recovery(scratch.document()).has_value());

  discard_autosave(scratch.document());
  EXPECT_FALSE(find_recovery(scratch.document()).has_value());
}

// A document saved since the copy was made is ahead of it, so offering the
// copy would be offering to go backwards.
TEST(Autosave, ACopyOlderThanTheSavedFileIsNotOffered) {
  const Scratch scratch;
  ASSERT_TRUE(write_autosave(scratch.document(), a_project()).has_value());

  // The filesystem's timestamps are coarse on some systems, so the saved file
  // is given a moment to land after the copy rather than in the same tick.
  std::this_thread::sleep_for(20ms);
  ASSERT_TRUE(write_project(scratch.document(), a_project()).has_value());

  EXPECT_FALSE(find_recovery(scratch.document()).has_value());
}

TEST(Autosave, ACopyNewerThanTheSavedFileIsOffered) {
  const Scratch scratch;
  ASSERT_TRUE(write_project(scratch.document(), a_project()).has_value());

  std::this_thread::sleep_for(20ms);
  core::Project changed = a_project();
  changed.canvas_w = 3840;
  ASSERT_TRUE(write_autosave(scratch.document(), changed).has_value());

  const auto found = find_recovery(scratch.document());
  ASSERT_TRUE(found.has_value());
  const auto loaded = read_project(found->path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->project.canvas_w, 3840);
}

}  // namespace
}  // namespace cutline::editor
