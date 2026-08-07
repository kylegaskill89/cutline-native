#pragma once

/// Keeping a recovery copy of the document being edited.
///
/// This is not "save the file for you". It never touches what the user saved:
/// it writes a *separate* copy alongside, and offers it back when the previous
/// session did not end cleanly. An autosave that wrote over the real file would
/// turn a crash into a silently overwritten afternoon, and there would be
/// nothing to go back to.
///
/// The copies live under the user's application data rather than beside the
/// project, for two reasons. A document that has never been saved has no
/// directory to live beside, and it is the one that has most to lose. And a
/// recovery file dropped next to somebody's project is a file they have to
/// tidy up, which is a poor thank-you for a feature they never asked for.
///
/// The timing decision is separated from the writing so it can be tested by
/// passing a clock rather than by sleeping.

#include "cutline/core/model.hpp"
#include "cutline/core/serialize.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cutline::editor {

/// How often a recovery copy is written while the document is being changed.
///
/// A minute. A project file is text measured in kilobytes, so the cost is
/// nothing, and a minute is about as much work as anyone will forgive losing.
inline constexpr std::chrono::seconds kAutosaveInterval{60};

/// How many copies of one document are kept.
///
/// Five rather than one, and the reason is the case this feature exists for:
/// the newest copy is written *from the state the application is in*, so a
/// document that has been quietly corrupted for the last few minutes has had
/// that corruption faithfully copied over the only thing there was to go back
/// to. Keeping a handful means the copy from before it went wrong is still
/// there. Five is about five minutes at the default interval, which is as far
/// back as anybody notices in time to want it.
inline constexpr int kAutosaveVersions = 5;

/// What has been written so far, so `autosave_due` can decide.
struct AutosaveState {
  /// When the last copy was written. Steady rather than wall time: this is a
  /// question about elapsed time, and the wall clock can move backwards.
  std::chrono::steady_clock::time_point written_at{};
  /// The session revision that copy was made from, so a document nobody has
  /// touched since is not written again every minute for nothing.
  std::uint64_t written_revision = 0;
  bool ever_written = false;
};

/// Whether a copy is due.
///
/// Never for an unmodified document — there is nothing to recover that is not
/// already on disk — and never twice for the same revision.
[[nodiscard]] bool autosave_due(const AutosaveState& state, bool modified,
                                std::uint64_t revision,
                                std::chrono::steady_clock::time_point now,
                                std::chrono::seconds interval = kAutosaveInterval) noexcept;

/// The directory recovery copies live in.
[[nodiscard]] std::filesystem::path autosave_dir();

/// What every copy of `document` has at the front of its name. An empty path is
/// the document that has never been saved, which gets a prefix of its own.
///
/// It carries both the document's own name and a digest of its full path: the
/// name alone would make two projects called "cut" in two folders share a
/// recovery file, and the digest alone would give a directory full of numbers
/// nobody could read.
[[nodiscard]] std::string autosave_prefix(const std::filesystem::path& document);

/// Where the copy of `document` written at `when` goes.
///
/// The time is in the name rather than left to the filesystem's own timestamp,
/// for two reasons. A copy is identified by the moment it describes, and a
/// file's modification time is a property of the file rather than of the work
/// in it — a backup tool that rewrites one has changed which copy looks newest.
/// And a fixed-width stamp sorts chronologically as text, so ordering the
/// copies needs neither the filesystem nor a parse.
[[nodiscard]] std::filesystem::path autosave_path_for(const std::filesystem::path& document,
                                                      std::chrono::system_clock::time_point when);

/// Writes a recovery copy, creating the directory if it is not there, and then
/// discards all but the newest `versions` of them.
///
/// Pruning happens *after* the write rather than before it, so a failure to
/// write leaves what was already there rather than making room for a copy that
/// never arrived.
[[nodiscard]] std::expected<void, std::string> write_autosave(
    const std::filesystem::path& document, const core::Project& project,
    int versions = kAutosaveVersions,
    std::chrono::system_clock::time_point when = std::chrono::system_clock::now());

/// Removes every recovery copy of the document. Called on a successful save and
/// on a clean exit: what is left behind is exactly what was never recovered
/// from.
void discard_autosave(const std::filesystem::path& document);

/// A recovery copy worth offering.
struct Recovery {
  std::filesystem::path path;
  /// When it was written, for telling the user how much they stand to get back.
  std::filesystem::file_time_type written_at{};
};

/// Every copy of `document`, newest first.
///
/// Ordered by the stamp in the name rather than by the filesystem's timestamps,
/// which are coarse enough on some volumes that two copies a minute apart can
/// report the same second.
///
/// A copy written before this application kept more than one has no stamp at
/// all. It sorts last, which is right: it is the older shape, and anything with
/// a stamp was written after the upgrade that introduced them.
[[nodiscard]] std::vector<Recovery> autosave_copies(const std::filesystem::path& document);

/// The newest recovery copy for `document`, if there is one worth offering.
///
/// Nothing when there is no copy, and nothing when the document on disk is
/// newer than the copy — that means the work was saved after the copy was
/// made, so the copy is behind and offering it would be offering to go
/// backwards. A document that has never been saved has nothing to compare
/// against, so any copy of it counts.
[[nodiscard]] std::optional<Recovery> find_recovery(const std::filesystem::path& document);

}  // namespace cutline::editor
