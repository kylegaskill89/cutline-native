#pragma once

/// What an edit is called, for the History panel.
///
/// Premiere lists every step by name — "Razor", "Move", "Add Effect" — and the
/// list is only useful if every step has one. Most edits here are applied as a
/// new project with nothing said about what happened, from well over a hundred
/// places, so the name is worked out by comparing the project before the edit
/// with the one after it. A command that knows its own name (Ripple Delete,
/// Add Edit) passes it instead; see `Session::name_next_edit`.
///
/// The comparison looks at what an editor would call the change, in order of
/// how much it says: the pool before the sequences, clips coming and going
/// before clips changing, and a clip's timing before its look. The first
/// difference found names the whole edit, since an edit is one gesture even
/// when it touches several things — a ripple delete moves every clip after the
/// one it removed, and is still a delete.

#include "cutline/core/model.hpp"

#include <string>

namespace cutline::editor {

/// A short name for the edit that turned `before` into `after`, in the words of
/// Premiere's History panel. "Edit" when nothing more particular is found.
[[nodiscard]] std::string describe_edit(const core::Project& before, const core::Project& after);

}  // namespace cutline::editor
