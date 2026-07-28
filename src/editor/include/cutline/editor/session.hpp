#pragma once

/// A project being edited.
///
/// The document, its undo history, what is selected, and where the playhead is.
/// Everything about editing that is not the model itself and not the interface
/// — which is exactly the seam that was missing: `cutline::core` knows nothing
/// about selection, and `cutline::ui` knows nothing about projects.
///
/// Pure, and tested without a window.

#include "cutline/core/history.hpp"
#include "cutline/core/model.hpp"

#include <cstdint>
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
  void reset(core::Project project);

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

  // ------------------------------------------------------------- playhead --

  [[nodiscard]] double playhead() const noexcept { return playhead_; }
  /// Snapped to a frame and never negative.
  void set_playhead(double seconds);

 private:
  /// Drops selected ids that are not in the project any more.
  void prune_selection();

  core::Project project_;
  core::History history_;
  std::vector<std::string> selection_;
  double playhead_ = 0.0;
  std::uint64_t revision_ = 0;
};

}  // namespace cutline::editor
