#pragma once

/// Input, as plain data.
///
/// Nothing here knows about windows or Win32. Events are values, which is what
/// lets the whole of routing — hover, capture, focus, bubbling — be driven from
/// a test with no message loop at all. The platform layer's only job is to
/// translate `WM_*` into these.

#include <cstdint>
#include <string_view>

namespace cutline::ui {

enum class MouseButton { Left, Middle, Right, Back, Forward };

/// Modifier keys held while an event happened.
struct Modifiers {
  bool shift = false;
  bool control = false;
  bool alt = false;

  [[nodiscard]] bool none() const noexcept { return !shift && !control && !alt; }

  friend bool operator==(const Modifiers&, const Modifiers&) = default;
};

struct MouseEvent {
  /// In the same coordinates as widget bounds: pixels, y down, origin at the
  /// top left of the root.
  double x = 0.0;
  double y = 0.0;
  MouseButton button = MouseButton::Left;
  Modifiers modifiers;
  /// 1 for a single click, 2 for a double, and so on. Only meaningful on press.
  int click_count = 1;

  friend bool operator==(const MouseEvent&, const MouseEvent&) = default;
};

struct WheelEvent {
  double x = 0.0;
  double y = 0.0;
  /// Notches, one per detent. Positive is down and right, matching the
  /// direction content moves rather than the direction the wheel turns.
  double delta_x = 0.0;
  double delta_y = 0.0;
  Modifiers modifiers;

  friend bool operator==(const WheelEvent&, const WheelEvent&) = default;
};

/// Physical keys, valued as ASCII where a key has an obvious character so that
/// a binding reads as the key it is. Everything without one lives above 255.
///
/// This is deliberately about keys rather than text: `Key::Z` with control held
/// is undo whatever the keyboard layout calls that key, and typing goes through
/// `WidgetHost::text` instead.
enum class Key : std::uint16_t {
  None = 0,

  Space = ' ',
  Comma = ',',
  Minus = '-',
  Period = '.',
  Slash = '/',

  Digit0 = '0',
  Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,

  Semicolon = ';',
  Equal = '=',

  A = 'A',
  B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

  LeftBracket = '[',
  Backslash = '\\',
  RightBracket = ']',
  Backtick = '`',

  Escape = 256,
  Tab,
  Enter,
  Backspace,
  Delete,
  Insert,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

[[nodiscard]] std::string_view to_string(Key key) noexcept;

struct KeyEvent {
  Key key = Key::None;
  Modifiers modifiers;
  /// Set when the key is repeating because it is being held. Shortcuts that
  /// should fire once — toggling playback — check this; ones that should
  /// repeat, like nudging a clip, do not.
  bool repeat = false;

  friend bool operator==(const KeyEvent&, const KeyEvent&) = default;
};

}  // namespace cutline::ui
