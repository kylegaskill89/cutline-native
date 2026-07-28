#pragma once

/// The editing commands, named and separated from whatever triggers them.
///
/// A keystroke, a menu item and a toolbar button should all be the same edit,
/// and the way to guarantee that is for none of them to contain one. They name
/// a command; this runs it.
///
/// It also gives menus something to ask: `can_run` is what greys an item out,
/// and having it next to the command rather than in the menu is what stops the
/// two disagreeing about when an edit is possible.

#include "cutline/editor/session.hpp"

#include <string_view>

namespace cutline::editor {

enum class Command {
  // -- editing
  /// Razor: splits the selection at the playhead, or everything under it when
  /// nothing is selected.
  Split,
  /// Lift: removes the selection and leaves the gap.
  Delete,
  /// Extract: removes the selection and closes the gap.
  RippleDelete,
  /// Moves the selection one frame earlier or later.
  NudgeLeft,
  NudgeRight,

  // -- selection
  SelectAll,
  SelectNone,

  // -- the playhead
  GoToStart,
  GoToEnd,
  PreviousFrame,
  NextFrame,

  // -- history
  Undo,
  Redo,
};

[[nodiscard]] std::string_view to_string(Command command) noexcept;

/// Whether the command would do anything right now. What greys a menu item.
[[nodiscard]] bool can_run(const Session& session, Command command);

/// Runs it, and reports whether anything changed.
///
/// A command that cannot apply is not an error — it simply does nothing, which
/// is the same contract the model's operations keep and what lets a keystroke
/// be pressed hopefully rather than checked first.
bool run(Session& session, Command command);

}  // namespace cutline::editor
