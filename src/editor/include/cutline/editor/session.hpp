#pragma once

/// A project being edited.
///
/// The document, its undo history, what is selected, and where the playhead is.
/// Everything about editing that is not the model itself and not the interface
/// — which is exactly the seam that was missing: `cutline::core` knows nothing
/// about selection, and `cutline::ui` knows nothing about projects.
///
/// Pure, and tested without a window.

#include "cutline/core/edit.hpp"
#include "cutline/core/history.hpp"
#include "cutline/core/model.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

class Session {
 public:
  explicit Session(core::Project project = {});

  [[nodiscard]] const core::Project& project() const noexcept { return project_; }

  /// Applies an edit, recording the state before it for undo.
  ///
  /// Does nothing at all when the result is identical to what is already here.
  /// Every core operation returns the project unchanged when it cannot apply,
  /// so this is how a rejected drag — one clamped against a neighbour, say —
  /// avoids putting an entry in the undo stack that undoes nothing.
  ///
  /// Returns whether anything changed.
  bool apply(core::Project next);

  /// Replaces the project without recording anything. Opening a file, not
  /// editing one, so the history belongs to the document being closed.
  ///
  /// The result counts as saved, because it is exactly what is on disk.
  void reset(core::Project project, std::filesystem::path path = {});

  // ------------------------------------------------------------- document --

  /// Where this project lives. Empty for one that has never been saved.
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  /// Whether anything has changed since it was last saved or opened.
  ///
  /// Compared against a snapshot rather than latched, so undoing back to the
  /// state on disk correctly reports nothing to save. The comparison happens
  /// on edits, which already compare whole projects, so asking is free.
  [[nodiscard]] bool modified() const noexcept { return modified_; }

  /// Records that what is here now is what is on disk.
  void mark_saved(std::filesystem::path path);

  /// Records that what is here is *not* what is on disk, without saying what
  /// is. For a project recovered from an autosave: it belongs to `path()`, the
  /// file there is the older version, and the title bar and the close prompt
  /// both have to know that before anything else has been edited.
  void mark_unsaved();

  /// For the title bar: the file's name, or "Untitled", marked when there is
  /// anything unsaved.
  [[nodiscard]] std::string document_title() const;

  [[nodiscard]] bool can_undo() const noexcept { return history_.can_undo(); }
  [[nodiscard]] bool can_redo() const noexcept { return history_.can_redo(); }
  bool undo();
  bool redo();

  /// Bumped on every change to the project or the selection, so a view can
  /// tell whether it needs rebuilding without comparing whole projects.
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

  // ------------------------------------------------------------ selection --

  [[nodiscard]] std::span<const std::string> selection() const noexcept { return selection_; }
  [[nodiscard]] bool is_selected(std::string_view clip_id) const noexcept;

  /// Ids that no longer exist are dropped, so a selection can never outlive
  /// what it points at — which is what otherwise happens after an undo.
  void select(std::vector<std::string> clip_ids);
  void select_one(std::string clip_id);
  void clear_selection();

  /// The selection expanded to whole linked groups, which is what an edit
  /// should act on: dragging a video clip has to bring its audio.
  [[nodiscard]] std::vector<std::string> selected_group() const;

  // ------------------------------------------------------------ clipboard --

  /// The clips last copied or cut, held as values.
  ///
  /// Here rather than in the application because a paste is an *edit*, and
  /// `run` is where edits live: with the clipboard anywhere else, Copy and
  /// Paste could not be commands, could not be bound like every other key, and
  /// `can_run` could not tell a menu whether Paste is worth offering.
  ///
  /// Not part of the project, and deliberately: saving one would put a copy of
  /// whatever somebody last selected into every file they pressed Copy in, and
  /// it survives opening a different document, which is what makes copying
  /// between two of them work.
  [[nodiscard]] std::span<const core::ClipCopy> clipboard() const noexcept { return clipboard_; }
  void set_clipboard(std::vector<core::ClipCopy> clips) { clipboard_ = std::move(clips); }

  // ------------------------------------------------------------- playhead --

  [[nodiscard]] double playhead() const noexcept { return playhead_; }
  /// Snapped to a frame and never negative.
  void set_playhead(double seconds);

 private:
  /// Drops selected ids that are not in the project any more.
  void prune_selection();
  /// Recomputes whether there is anything to save.
  void refresh_modified();

  core::Project project_;
  /// What was last read from or written to disk, so `modified` can be answered
  /// by comparison rather than by a flag that undo cannot correct.
  core::Project saved_;
  std::filesystem::path path_;
  bool modified_ = false;

  core::History history_;
  std::vector<std::string> selection_;
  std::vector<core::ClipCopy> clipboard_;
  double playhead_ = 0.0;
  std::uint64_t revision_ = 0;
};

}  // namespace cutline::editor
