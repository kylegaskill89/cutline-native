#pragma once

/// A sheet of every control in every state, one per theme.
///
/// What Luna and Aero are matched against the owner's XP and Vista captures
/// with. The application shows each control in one state at a time, wherever
/// it happens to be; matching a theme needs them side by side, in the states
/// Windows drew — at rest, under the pointer, held down, chosen, switched off —
/// at the 96 DPI the captures were taken at, so a pixel here is a pixel there.

#include <string_view>

namespace cutline::tool {

/// Writes `<dir>/<theme>.png` for each built-in theme. Returns the number of
/// sheets that could not be written.
int write_gallery(std::string_view dir);

}  // namespace cutline::tool
