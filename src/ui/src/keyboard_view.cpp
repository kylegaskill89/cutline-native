#include "cutline/ui/keyboard_view.hpp"

#include "cutline/ui/painter.hpp"

#include <algorithm>

namespace cutline::ui {
namespace {

using Cap = KeyboardView::Cap;
using Modifier = KeyboardView::Modifier;

/// A row of ordinary keys, one width each, from `x`.
void letters(std::vector<Cap>& caps, std::string_view keys, double x, double y) {
  for (const char c : keys) {
    caps.push_back(Cap{.key = static_cast<Key>(c), .legend = std::string(1, c), .x = x, .y = y});
    x += 1.0;
  }
}

/// A standard Windows keyboard: fifteen key widths of main block, half a
/// width of gap, and the three columns of the navigation keys.
[[nodiscard]] std::vector<Cap> keyboard_layout() {
  std::vector<Cap> caps;
  const auto key = [&caps](Key k, std::string legend, double x, double y, double width = 1.0,
                           Modifier modifier = Modifier::None) {
    caps.push_back(Cap{.key = k, .legend = std::move(legend), .x = x, .y = y, .width = width,
                       .modifier = modifier});
  };

  // The function row, half a row above the rest.
  key(Key::Escape, "Esc", 0.0, 0.0);
  for (int i = 0; i < 12; ++i) {
    const double group = static_cast<double>(i / 4);
    key(static_cast<Key>(static_cast<int>(Key::F1) + i), "F" + std::to_string(i + 1),
        2.0 + static_cast<double>(i) + group * 0.5, 0.0);
  }

  const double r1 = 1.5;
  key(Key::Backtick, "`", 0.0, r1);
  letters(caps, "1234567890", 1.0, r1);
  key(Key::Minus, "-", 11.0, r1);
  key(Key::Equal, "=", 12.0, r1);
  key(Key::Backspace, "Backspace", 13.0, r1, 2.0);

  const double r2 = 2.5;
  key(Key::Tab, "Tab", 0.0, r2, 1.5);
  letters(caps, "QWERTYUIOP", 1.5, r2);
  key(Key::LeftBracket, "[", 11.5, r2);
  key(Key::RightBracket, "]", 12.5, r2);
  key(Key::Backslash, "\\", 13.5, r2, 1.5);

  const double r3 = 3.5;
  key(Key::None, "Caps", 0.0, r3, 1.75);
  letters(caps, "ASDFGHJKL", 1.75, r3);
  key(Key::Semicolon, ";", 10.75, r3);
  key(Key::None, "'", 11.75, r3);
  key(Key::Enter, "Enter", 12.75, r3, 2.25);

  const double r4 = 4.5;
  key(Key::None, "Shift", 0.0, r4, 2.25, Modifier::Shift);
  letters(caps, "ZXCVBNM", 2.25, r4);
  key(Key::Comma, ",", 9.25, r4);
  key(Key::Period, ".", 10.25, r4);
  key(Key::Slash, "/", 11.25, r4);
  key(Key::None, "Shift", 12.25, r4, 2.75, Modifier::Shift);

  const double r5 = 5.5;
  key(Key::None, "Ctrl", 0.0, r5, 1.25, Modifier::Control);
  key(Key::None, "Win", 1.25, r5, 1.25);
  key(Key::None, "Alt", 2.5, r5, 1.25, Modifier::Alt);
  key(Key::Space, "Space", 3.75, r5, 6.25);
  key(Key::None, "Alt", 10.0, r5, 1.25, Modifier::Alt);
  key(Key::None, "Win", 11.25, r5, 1.25);
  key(Key::None, "Menu", 12.5, r5, 1.25);
  key(Key::None, "Ctrl", 13.75, r5, 1.25, Modifier::Control);

  // The navigation keys.
  const double nav = 15.5;
  key(Key::Insert, "Ins", nav, r1);
  key(Key::Home, "Home", nav + 1.0, r1);
  key(Key::PageUp, "PgUp", nav + 2.0, r1);
  key(Key::Delete, "Del", nav, r2);
  key(Key::End, "End", nav + 1.0, r2);
  key(Key::PageDown, "PgDn", nav + 2.0, r2);
  key(Key::Up, "Up", nav + 1.0, r4);
  key(Key::Left, "Left", nav, r5);
  key(Key::Down, "Down", nav + 1.0, r5);
  key(Key::Right, "Right", nav + 2.0, r5);
  return caps;
}

[[nodiscard]] bool held(const Modifiers& modifiers, Modifier modifier) {
  switch (modifier) {
    case Modifier::Control: return modifiers.control;
    case Modifier::Shift: return modifiers.shift;
    case Modifier::Alt: return modifiers.alt;
    case Modifier::None: return false;
  }
  return false;
}

}  // namespace

KeyboardView::KeyboardView() : caps_(keyboard_layout()) {}

double KeyboardView::unit() const {
  return std::max(1.0, std::min(bounds().width / kKeyboardWidth, bounds().height / kKeyboardDepth));
}

Rect KeyboardView::cap_rect(const Cap& cap) const {
  const double u = unit();
  // Centred across whatever width is spare, and a sliver of gap between keys
  // so each reads as a key.
  const double left = bounds().x + (bounds().width - u * kKeyboardWidth) / 2.0;
  const double gap = std::max(1.0, u * 0.08);
  return Rect{left + cap.x * u + gap / 2.0, bounds().y + cap.y * u + gap / 2.0,
              cap.width * u - gap, u - gap};
}

const Cap* KeyboardView::cap_at(double x, double y) const {
  for (const Cap& cap : caps_) {
    if (cap_rect(cap).contains(x, y)) return &cap;
  }
  return nullptr;
}

LayoutItem KeyboardView::sizing(Axis axis, const LayoutContext& context) const {
  // A key wide enough for its legend and a word of the command under it.
  const double u = context.metrics().control_height * 1.5;
  if (axis == Axis::Vertical) return LayoutItem::fixed(u * kKeyboardDepth);
  return LayoutItem::flexible(1.0, u * kKeyboardWidth);
}

void KeyboardView::paint_content(Painter& painter, const Theme& theme) const {
  const Metrics& metrics = theme.metrics;
  // Two tints: the accent for what works everywhere, and a green for what a
  // panel is offered, as Premiere tells its two kinds of command apart.
  Color everywhere = theme.accent;
  everywhere.a = 0.45f;
  Color in_panel = mix(theme.accent, Color{0.18f, 0.68f, 0.35f, 1.0f}, 0.75f);
  in_panel.a = 0.45f;

  for (const Cap& cap : caps_) {
    const Rect area = cap_rect(cap);
    const bool modifier_held = held(modifiers_, cap.modifier);
    const bool chosen = cap.key != Key::None && cap.key == selected_;
    State state = State::Normal;
    if (cap.key == Key::None && cap.modifier == Modifier::None) state = State::Disabled;
    if (modifier_held || chosen) state = State::Pressed;
    const SurfaceStyle& style = theme.style(Part::Button, state);
    paint_surface(painter, area, style);

    const auto found = cap.key == Key::None ? bound_.end() : bound_.find(cap.key);
    if (found != bound_.end()) {
      painter.fill(area.inset(1.0), std::max(0.0, style.corner_radius - 1.0),
                   Fill::solid(found->second.panel ? in_panel : everywhere));
    }

    // The legend at the top left, as it is printed on a real key, and the
    // command along the bottom.
    const double pad = std::max(2.0, area.width * 0.06);
    const Rect top{area.x + pad, area.y + pad * 0.5, area.width - 2.0 * pad, area.height * 0.45};
    painter.text(text_run(top, cap.legend, style, metrics.small_font_size, TextAlign::Left,
                          chosen || modifier_held));
    if (found != bound_.end()) {
      const Rect bottom{area.x + pad, area.y + area.height * 0.48, area.width - 2.0 * pad,
                        area.height * 0.48};
      painter.push_clip(bottom, 0.0);
      painter.text(text_run(bottom, found->second.label, style, metrics.tiny_font_size,
                            TextAlign::Left, false));
      painter.pop_clip();
    }
  }
}

std::string KeyboardView::tooltip_at(double x, double y) const {
  const Cap* cap = cap_at(x, y);
  if (cap == nullptr || cap->key == Key::None) return {};
  const auto found = bound_.find(cap->key);
  return found == bound_.end() ? cap->legend + ": nothing" : cap->legend + ": " + found->second.label;
}

bool KeyboardView::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  const Cap* cap = cap_at(event.x, event.y);
  if (cap == nullptr) return false;
  if (cap->modifier != Modifier::None) {
    // Held down or let go on the drawing, which shows that layer.
    switch (cap->modifier) {
      case Modifier::Control: modifiers_.control = !modifiers_.control; break;
      case Modifier::Shift: modifiers_.shift = !modifiers_.shift; break;
      case Modifier::Alt: modifiers_.alt = !modifiers_.alt; break;
      case Modifier::None: break;
    }
    if (on_modifiers_) on_modifiers_(modifiers_);
    return true;
  }
  if (cap->key == Key::None) return true;
  selected_ = cap->key;
  if (on_choose_) on_choose_(cap->key);
  return true;
}

}  // namespace cutline::ui
