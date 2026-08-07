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

/// How many edits are kept when nobody has said otherwise.
///
/// A snapshot is a whole project, which is kilobytes for an ordinary cut and
/// megabytes for a long one with a great many keyframes — so this is a memory
/// budget as much as a convenience, and it is the reason the limit is worth
/// exposing at all rather than being chosen here once and for everybody.
inline constexpr std::size_t kDefaultUndoDepth = 100;

class History {
 public:
  /// Older snapshots are discarded once the limit is reached.
  explicit History(std::size_t limit = kDefaultUndoDepth) : limit_(limit) {}

  /// Changes how many edits are kept, discarding the oldest immediately rather
  /// than waiting for the next edit to notice.
  ///
  /// Doing it now is the honest reading of the control: somebody who has just
  /// lowered this to free memory has said what they want, and a history that
  /// stayed large until the next edit would look like the setting had been
  /// ignored. The redo stack is left alone — it is bounded by the undo stack it
  /// came from, and throwing away a future somebody is stepping through is not
  /// what "keep fewer past edits" asks for.
  void set_limit(std::size_t limit);

  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

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
