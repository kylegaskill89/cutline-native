#pragma once

/// Undo/redo as a stack of whole-project snapshots.
///
/// Snapshotting the entire project rather than recording inverse operations is
/// what makes undo trivially correct: every editing operation already produces
/// a new project, so there is nothing to invert and no operation can be missed.
///
/// A snapshot holds its tracks apart from the rest of the project, and a track
/// that is the same as in the snapshot beside it is held once between them. An
/// edit nearly always changes one or two tracks of dozens, and keeping every
/// one of them in every snapshot cost 1.5 GB for a hundred edits of a project
/// with ten thousand clips. Everything outside the tracks -- the pool, bins,
/// each sequence's own settings -- is small, and is copied.

#include "cutline/core/model.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutline::core {

/// How many edits are kept when nobody has said otherwise.
///
/// A snapshot holds only the tracks its edit changed, so on a ten-thousand-clip
/// project a hundred of them came to 76 MB rather than the 1.5 GB of a whole
/// copy each. Still a memory budget as well as a convenience, which is why the
/// limit is exposed rather than chosen here once and for everybody.
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
  ///
  /// Taken by value so a caller that is done with it can move it in: its
  /// tracks are then kept without being copied.
  ///
  /// `name` is what the edit that followed is called — what the History panel
  /// lists and what undoing would take back. It travels with the step between
  /// the two stacks, so a step undone and redone keeps its name.
  void push(Project snapshot, std::string name = {});

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

  /// The names of the edits that undoing would take back, oldest first: the
  /// last is what the next undo undoes.
  [[nodiscard]] std::vector<std::string> undo_names() const;
  /// The names of the edits that redoing would put back, next first.
  [[nodiscard]] std::vector<std::string> redo_names() const;

 private:
  /// A project with every sequence's tracks taken out, and the tracks, shared
  /// with whichever snapshot it was stored beside wherever they are the same.
  struct Snapshot {
    Project rest;
    std::vector<std::vector<std::shared_ptr<const Track>>> tracks;
    /// The edit that took the project from this state to the next one.
    std::string name;
  };

  /// Takes a project apart, sharing each track that `beside` already holds as
  /// it is. Matched by sequence and track id, so a track added or reordered
  /// above another does not stop the rest being found.
  [[nodiscard]] static Snapshot store(Project project, const Snapshot* beside);
  /// Puts one back together.
  [[nodiscard]] static Project restore(const Snapshot& snapshot);

  std::vector<Snapshot> undo_;
  std::vector<Snapshot> redo_;
  std::size_t limit_;
};

}  // namespace cutline::core
