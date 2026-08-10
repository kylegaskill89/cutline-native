/// Recovery copies.
///
/// Two halves, tested separately for the reason they are separate: *when* a
/// copy is due is arithmetic on a clock and is asserted without touching a
/// disk, and *where* it goes and what is offered back is filesystem behaviour
/// with no timing in it.

#include "cutline/editor/autosave.hpp"

#include "cutline/editor/document.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

/// A moment to name copies after, so the tests are about the naming rather than
/// about what the clock happened to say while they ran.
constexpr std::chrono::system_clock::time_point kWhen =
    std::chrono::sys_days{std::chrono::year{2026} / std::chrono::August / 6} +
    std::chrono::hours{14} + std::chrono::minutes{3} + std::chrono::seconds{9};

TEST(AutosavePath, TwoProjectsOfTheSameNameDoNotShareACopy) {
  const auto one = autosave_path_for("d:/films/first/cut.cutline", kWhen);
  const auto two = autosave_path_for("d:/films/second/cut.cutline", kWhen);

  EXPECT_NE(one, two);
  // Readable, though: the name is in there as well as the digest.
  EXPECT_NE(one.filename().string().find("cut"), std::string::npos);
}

TEST(AutosavePath, TheSameProjectAtTheSameMomentGetsTheSameCopy) {
  EXPECT_EQ(autosave_path_for("d:/films/cut.cutline", kWhen),
            autosave_path_for("d:/films/cut.cutline", kWhen));
}

// The stamp is what makes one document's copies different files rather than one
// file overwritten, which is the whole of keeping more than one.
TEST(AutosavePath, TheSameProjectAtAnotherMomentGetsAnotherCopy) {
  EXPECT_NE(autosave_path_for("d:/films/cut.cutline", kWhen),
            autosave_path_for("d:/films/cut.cutline", kWhen + 1min));
}

// Fixed width and most-significant-first, so ordering the copies never needs a
// parse — which is what `autosave_copies` relies on to sort them.
TEST(AutosavePath, ALaterCopySortsAfterAnEarlierOneAsText) {
  const auto earlier = autosave_path_for("d:/films/cut.cutline", kWhen);
  const auto later = autosave_path_for("d:/films/cut.cutline", kWhen + 1h);
  EXPECT_LT(earlier.stem().string(), later.stem().string());
}

// A copy written before this application kept more than one has no stamp, and
// has to read as the oldest rather than as the newest.
TEST(AutosavePath, AStampedCopySortsAfterAnUnstampedOne) {
  const auto stamped = autosave_path_for("d:/films/cut.cutline", kWhen);
  EXPECT_LT(autosave_prefix("d:/films/cut.cutline"), stamped.stem().string());
}

TEST(AutosavePath, ADocumentNeverSavedStillHasSomewhereToGo) {
  const auto path = autosave_path_for({}, kWhen);
  EXPECT_FALSE(path.empty());
  EXPECT_EQ(path.extension().string(), std::string(kProjectExtension));
}

TEST(AutosavePath, AwkwardNamesDoNotEscapeTheDirectory) {
  const auto path = autosave_path_for("d:/films/../../a b/we:rd*name.cutline", kWhen);
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
  p.sequence().canvas_w = 1280;
  p.sequence().canvas_h = 720;
  return p;
}

TEST(Autosave, ACopyCanBeWrittenAndReadBack) {
  const Scratch scratch;
  ASSERT_TRUE(write_autosave(scratch.document(), a_project()).has_value());

  const auto found = find_recovery(scratch.document());
  ASSERT_TRUE(found.has_value());

  const auto loaded = read_project(found->path);
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project.sequence().canvas_w, 1280);
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
  changed.sequence().canvas_w = 3840;
  ASSERT_TRUE(write_autosave(scratch.document(), changed).has_value());

  const auto found = find_recovery(scratch.document());
  ASSERT_TRUE(found.has_value());
  const auto loaded = read_project(found->path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->project.sequence().canvas_w, 3840);
}

// ------------------------------------------------------------- how many kept --

TEST(AutosaveVersions, EveryCopyIsKeptUpToTheLimit) {
  const Scratch scratch;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(
        write_autosave(scratch.document(), a_project(), 5, kWhen + std::chrono::minutes{i})
            .has_value());
  }
  EXPECT_EQ(autosave_copies(scratch.document()).size(), 3u);
}

TEST(AutosaveVersions, PastTheLimitTheOldestGoes) {
  const Scratch scratch;
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(
        write_autosave(scratch.document(), a_project(), 2, kWhen + std::chrono::minutes{i})
            .has_value());
  }

  const std::vector<Recovery> copies = autosave_copies(scratch.document());
  ASSERT_EQ(copies.size(), 2u);
  // The two newest, newest first.
  EXPECT_EQ(copies[0].path, autosave_path_for(scratch.document(), kWhen + 4min));
  EXPECT_EQ(copies[1].path, autosave_path_for(scratch.document(), kWhen + 3min));
}

// What the whole feature is for: the newest copy is written from the state the
// application is in, so the one before it is what somebody actually wants back.
TEST(AutosaveVersions, TheCopyBeforeTheNewestIsStillThere) {
  const Scratch scratch;
  core::Project good = a_project();
  good.sequence().canvas_w = 1280;
  ASSERT_TRUE(write_autosave(scratch.document(), good, 3, kWhen).has_value());

  core::Project bad = a_project();
  bad.sequence().canvas_w = 640;
  ASSERT_TRUE(write_autosave(scratch.document(), bad, 3, kWhen + 1min).has_value());

  const std::vector<Recovery> copies = autosave_copies(scratch.document());
  ASSERT_EQ(copies.size(), 2u);

  const auto newest = read_project(copies[0].path);
  ASSERT_TRUE(newest.has_value()) << newest.error();
  EXPECT_EQ(newest->project.sequence().canvas_w, 640);

  const auto before = read_project(copies[1].path);
  ASSERT_TRUE(before.has_value()) << before.error();
  EXPECT_EQ(before->project.sequence().canvas_w, 1280);
}

TEST(AutosaveVersions, TheNewestIsTheOneOffered) {
  const Scratch scratch;
  core::Project older = a_project();
  older.sequence().canvas_w = 1280;
  ASSERT_TRUE(write_autosave(scratch.document(), older, 5, kWhen).has_value());

  core::Project newer = a_project();
  newer.sequence().canvas_w = 3840;
  ASSERT_TRUE(write_autosave(scratch.document(), newer, 5, kWhen + 1min).has_value());

  const auto found = find_recovery(scratch.document());
  ASSERT_TRUE(found.has_value());
  const auto loaded = read_project(found->path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->project.sequence().canvas_w, 3840);
}

TEST(AutosaveVersions, DiscardingTakesEveryCopy) {
  const Scratch scratch;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(
        write_autosave(scratch.document(), a_project(), 5, kWhen + std::chrono::minutes{i})
            .has_value());
  }
  ASSERT_EQ(autosave_copies(scratch.document()).size(), 3u);

  discard_autosave(scratch.document());
  EXPECT_TRUE(autosave_copies(scratch.document()).empty());
}

// Asking for none would mean writing a copy and immediately deleting it, which
// is the one answer that makes the feature do nothing at all.
TEST(AutosaveVersions, AtLeastOneIsAlwaysKept) {
  const Scratch scratch;
  ASSERT_TRUE(write_autosave(scratch.document(), a_project(), 0, kWhen).has_value());
  EXPECT_EQ(autosave_copies(scratch.document()).size(), 1u);
}

// A never-saved document's prefix is "untitled", which is also the start of
// every copy of a *saved* project called "untitled" — the pair most likely to
// both exist, and the one a looser match would confuse.
TEST(AutosaveVersions, ADocumentDoesNotClaimAnotherDocumentsCopies) {
  const std::filesystem::path saved =
      std::filesystem::temp_directory_path() / ("untitled" + std::string(kProjectExtension));

  ASSERT_TRUE(write_autosave(saved, a_project(), 5, kWhen).has_value());
  const std::filesystem::path written = autosave_path_for(saved, kWhen);

  // Asserted as "this copy is not in that list" rather than as "that list is
  // empty". These tests share the real recovery directory with the application,
  // which may perfectly well have left a copy of its own untitled document
  // there — and it is not this test's to delete.
  EXPECT_EQ(std::ranges::count(autosave_copies({}), written, &Recovery::path), 0)
      << "a never-saved document must not claim a saved one's copies";
  EXPECT_EQ(std::ranges::count(autosave_copies(saved), written, &Recovery::path), 1);

  discard_autosave(saved);
}

// A copy written before this application kept more than one has no stamp in its
// name. It is still somebody's unsaved work, and there is one sitting in the
// real recovery directory on any machine that ran an earlier build.
TEST(AutosaveVersions, ACopyFromBeforeTheStampsIsStillFound) {
  const Scratch scratch;
  std::error_code ignored;
  std::filesystem::create_directories(autosave_dir(), ignored);

  const std::filesystem::path legacy =
      autosave_dir() / (autosave_prefix(scratch.document()) + std::string(kProjectExtension));
  ASSERT_TRUE(write_project(legacy, a_project()).has_value());

  const std::vector<Recovery> copies = autosave_copies(scratch.document());
  ASSERT_EQ(copies.size(), 1u);
  EXPECT_EQ(copies[0].path, legacy);
}

// And it is the oldest thing there can be, since anything with a stamp was
// written after the upgrade that introduced them.
TEST(AutosaveVersions, AStampedCopyIsNewerThanAnUnstampedOne) {
  const Scratch scratch;
  std::error_code ignored;
  std::filesystem::create_directories(autosave_dir(), ignored);

  const std::filesystem::path legacy =
      autosave_dir() / (autosave_prefix(scratch.document()) + std::string(kProjectExtension));
  ASSERT_TRUE(write_project(legacy, a_project()).has_value());
  ASSERT_TRUE(write_autosave(scratch.document(), a_project(), 5, kWhen).has_value());

  const std::vector<Recovery> copies = autosave_copies(scratch.document());
  ASSERT_EQ(copies.size(), 2u);
  EXPECT_EQ(copies[0].path, autosave_path_for(scratch.document(), kWhen));
  EXPECT_EQ(copies[1].path, legacy);
}

}  // namespace
}  // namespace cutline::editor
