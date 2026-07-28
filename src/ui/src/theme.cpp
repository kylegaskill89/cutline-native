#include "cutline/ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace cutline::ui {
namespace {

[[nodiscard]] int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

[[nodiscard]] float channel(int value) noexcept { return static_cast<float>(value) / 255.0f; }

constexpr std::array<std::pair<Part, std::string_view>, 19> kPartNames{{
    {Part::Window, "window"},
    {Part::TitleBar, "title_bar"},
    {Part::Panel, "panel"},
    {Part::PanelHeader, "panel_header"},
    {Part::Splitter, "splitter"},
    {Part::Button, "button"},
    {Part::ToolButton, "tool_button"},
    {Part::Input, "input"},
    {Part::Slider, "slider"},
    {Part::SliderThumb, "slider_thumb"},
    {Part::Menu, "menu"},
    {Part::MenuItem, "menu_item"},
    {Part::Tooltip, "tooltip"},
    {Part::Clip, "clip"},
    {Part::TrackHeader, "track_header"},
    {Part::Playhead, "playhead"},
    {Part::Ruler, "ruler"},
    {Part::Scrollbar, "scrollbar"},
    {Part::ScrollThumb, "scroll_thumb"},
}};

constexpr std::array<std::pair<State, std::string_view>, 6> kStateNames{{
    {State::Normal, "normal"},
    {State::Hover, "hover"},
    {State::Pressed, "pressed"},
    {State::Disabled, "disabled"},
    {State::Selected, "selected"},
    {State::Focused, "focused"},
}};

}  // namespace

// ------------------------------------------------------------------ colour --

Color parse_color(std::string_view text, Color fallback) noexcept {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);

  const auto read = [&](std::size_t count) -> std::optional<std::array<int, 8>> {
    std::array<int, 8> digits{};
    for (std::size_t i = 0; i < count; ++i) {
      digits[i] = hex_digit(text[i]);
      if (digits[i] < 0) return std::nullopt;
    }
    return digits;
  };

  if (text.size() == 3) {
    // #abc is #aabbcc, the usual shorthand.
    const auto digits = read(3);
    if (!digits) return fallback;
    return {channel((*digits)[0] * 17), channel((*digits)[1] * 17), channel((*digits)[2] * 17),
            1.0f};
  }
  if (text.size() == 6 || text.size() == 8) {
    const auto digits = read(text.size());
    if (!digits) return fallback;
    const float alpha =
        text.size() == 8 ? channel((*digits)[6] * 16 + (*digits)[7]) : 1.0f;
    return {channel((*digits)[0] * 16 + (*digits)[1]),
            channel((*digits)[2] * 16 + (*digits)[3]),
            channel((*digits)[4] * 16 + (*digits)[5]), alpha};
  }
  return fallback;
}

std::string to_hex(const Color& color) {
  const auto byte = [](float v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  const int r = byte(color.r);
  const int g = byte(color.g);
  const int b = byte(color.b);
  const int a = byte(color.a);
  if (a == 255) return std::format("#{:02x}{:02x}{:02x}", r, g, b);
  return std::format("#{:02x}{:02x}{:02x}{:02x}", r, g, b, a);
}

// -------------------------------------------------------------------- fill --

Fill Fill::solid(Color color) {
  Fill fill;
  fill.kind = FillKind::Solid;
  fill.color = color;
  return fill;
}

Fill Fill::gradient(std::vector<GradientStop> stops, double angle_deg) {
  Fill fill;
  fill.kind = FillKind::Gradient;
  fill.stops = std::move(stops);
  fill.angle_deg = angle_deg;
  // Handy for anything that wants one representative colour without walking the
  // ramp — a thumbnail of the theme, say.
  if (!fill.stops.empty()) fill.color = fill.stops.front().color;
  return fill;
}

Fill Fill::glass(Color tint, double blur_radius) {
  Fill fill;
  fill.kind = FillKind::Glass;
  fill.color = tint;
  fill.blur_radius = blur_radius;
  return fill;
}

// ------------------------------------------------------------------- names --

std::string_view to_string(Part part) noexcept {
  const auto found = std::ranges::find(kPartNames, part, &std::pair<Part, std::string_view>::first);
  return found == kPartNames.end() ? "window" : found->second;
}

std::optional<Part> part_from_string(std::string_view name) noexcept {
  const auto found =
      std::ranges::find(kPartNames, name, &std::pair<Part, std::string_view>::second);
  if (found == kPartNames.end()) return std::nullopt;
  return found->first;
}

std::string_view to_string(State state) noexcept {
  const auto found =
      std::ranges::find(kStateNames, state, &std::pair<State, std::string_view>::first);
  return found == kStateNames.end() ? "normal" : found->second;
}

std::optional<State> state_from_string(std::string_view name) noexcept {
  const auto found =
      std::ranges::find(kStateNames, name, &std::pair<State, std::string_view>::second);
  if (found == kStateNames.end()) return std::nullopt;
  return found->first;
}

// ------------------------------------------------------------------ lookup --

const SurfaceStyle& Theme::style(Part part, State state) const noexcept {
  const auto part_entry = styles.find(part);
  if (part_entry == styles.end()) return fallback;

  const auto exact = part_entry->second.find(state);
  if (exact != part_entry->second.end()) return exact->second;

  // Most parts look the same hovered as at rest, so an absent state is the
  // common case rather than an omission.
  const auto normal = part_entry->second.find(State::Normal);
  if (normal != part_entry->second.end()) return normal->second;

  return fallback;
}

bool Theme::defines(Part part, State state) const noexcept {
  const auto part_entry = styles.find(part);
  return part_entry != styles.end() && part_entry->second.contains(state);
}

void Theme::set(Part part, State state, SurfaceStyle style) {
  styles[part][state] = std::move(style);
}

}  // namespace cutline::ui
