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

}  // namespace cutline::core
