#pragma once

/// What the pointer should look like where it is.
///
/// The interface had exactly one cursor — the arrow the window class was
/// registered with — from the first widget until now, which meant nothing on
/// screen ever announced what a press would do. A clip's trim edge looked
/// identical to its middle, a splitter looked like the gap between two panels,
/// a field looked like a label, and the tool you were holding was visible only
/// in a toolbar a long way from where you were working.
///
/// An enum here rather than a Win32 handle for the usual reason: `cutline::ui`
/// draws and hit-tests without knowing what platform it is on, and a widget
/// naming `IDC_SIZEWE` would drag windows.h into every test. What each of these
/// *is* is decided by whoever owns the window.

#include <string_view>

namespace cutline::ui {

enum class Cursor {
  /// The default, and the answer that means "I have nothing to say about this
  /// point" — a widget answering `Arrow` lets the question pass to its parent,
  /// so a label sitting on a timeline does not blank out the timeline's cursor.
  Arrow,
  /// Over text that can be typed into.
  Text,
  /// A vertical edge that can be dragged left and right: a clip's in or out
  /// point, a splitter between two columns.
  ResizeWE,
  /// A horizontal one — the line under a track header, a splitter between two
  /// rows, a level that is dragged up and down.
  ResizeNS,
  /// Something that can be picked up and put somewhere else.
  Move,

  // The tools. Each is drawn from the same art as its button in the palette, so
  // the thing under the pointer is the thing that was pressed.
  Razor,
  RateStretch,
  Slip,
  Slide,
  Ripple,
  Roll,
};

[[nodiscard]] std::string_view to_string(Cursor cursor) noexcept;

}  // namespace cutline::ui
