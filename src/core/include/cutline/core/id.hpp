#pragma once

#include <string>
#include <string_view>

namespace cutline::core {

/// Generates a unique id of the form "<prefix>_<n>".
///
/// This is the one piece of the core that is not a pure function. Threading an
/// id source through every editing operation would cost more in API noise than
/// it buys, and the reference implementation made the same trade. Tests that
/// assert on generated ids call `reset_ids` first.
[[nodiscard]] std::string new_id(std::string_view prefix = "id");

/// Resets the id counter. Intended for tests.
void reset_ids() noexcept;

/// Raises the counter past an id that already exists, so nothing minted later
/// in this run can collide with it.
///
/// The counter starts at zero every time the application does, which is fine
/// for a project built in one sitting and wrong the moment one is *opened*: the
/// file's `clip_4` is already taken, and the next split would hand out that
/// name again. Two clips with one id is worse than it sounds — every operation
/// finds clips by id, so an edit aimed at one silently lands on both.
///
/// Ignores anything not shaped like a generated id, since an id that came from
/// somewhere else cannot be minted again by accident.
void note_id(std::string_view id) noexcept;

}  // namespace cutline::core
