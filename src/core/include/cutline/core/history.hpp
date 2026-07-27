#pragma once

/// Undo/redo as a stack of whole-project snapshots.
///
/// Snapshotting the entire project rather than recording inverse operations is
/// what makes undo trivially correct: every editing operation already produces
/// a new project, so there is nothing to invert and no operation can be missed.
/// Projects are plain value types, so a snapshot is a copy.

#include "cutline/core/model.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace cutline::core {

class History {
 public:
  /// Older snapshots are discarded once the limit is reached.
  explicit History(std::size_t limit = 100) : limit_(limit) {}

  /// Records the state *before* an edit. Doing so invalidates any redo, since
  /// the future being redone no longer follows from the present.
  void push(Project snapshot);

  [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }

  /// Steps back, handing `current` to the redo stack. Returns nothing when
  /// there is no history to step into.
  [[nodiscard]] std::optional<Project> undo(Project current);

  /// Steps forward again, handing `current` back to the undo stack.
  [[nodiscard]] std::optional<Project> redo(Project current);

  void clear() noexcept;

  [[nodiscard]] std::size_t undo_depth() const noexcept { return undo_.size(); }
  [[nodiscard]] std::size_t redo_depth() const noexcept { return redo_.size(); }

 private:
  std::vector<Project> undo_;
  std::vector<Project> redo_;
  std::size_t limit_;
};

}  // namespace cutline::core
