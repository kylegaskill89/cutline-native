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

constexpr std::array<std::pair<Part, std::string_view>, 26> kPartNames{{
    {Part::Window, "window"},
    {Part::TitleBar, "title_bar"},
    {Part::Panel, "panel"},
    {Part::PanelHeader, "panel_header"},
    {Part::TabBar, "tab_bar"},
    {Part::Tab, "tab"},
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
    {Part::Check, "check"},
    {Part::Radio, "radio"},
    {Part::Progress, "progress"},
    {Part::Caption, "caption"},
    {Part::CaptionClose, "caption_close"},
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

Color mix(const Color& a, const Color& b, float t) noexcept {
  return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
               a.a + (b.a - a.a) * t};
}

namespace {

/// One fill `t` of the way to another, as far as their shapes allow.
[[nodiscard]] Fill mix_fill(const Fill& a, const Fill& b, float t) {
  const auto between = [t](double x, double y) { return x + (y - x) * t; };
  Fill out = t < 0.5f ? a : b;
  if (a.kind == b.kind) {
    out.color = mix(a.color, b.color, t);
    out.blur_radius = between(a.blur_radius, b.blur_radius);
    if (a.stops.size() == b.stops.size()) {
      for (std::size_t i = 0; i < out.stops.size(); ++i) {
        out.stops[i].at = static_cast<float>(between(a.stops[i].at, b.stops[i].at));
        out.stops[i].color = mix(a.stops[i].color, b.stops[i].color, t);
      }
    }
  } else if (a.kind == FillKind::Solid && b.kind == FillKind::Gradient) {
    // A flat rest state glowing into a ramp: the ramp, starting from the flat
    // colour at every stop.
    out = b;
    for (GradientStop& stop : out.stops) stop.color = mix(a.color, stop.color, t);
  } else if (a.kind == FillKind::Gradient && b.kind == FillKind::Solid) {
    out = a;
    for (GradientStop& stop : out.stops) stop.color = mix(stop.color, b.color, t);
  }
  return out;
}

}  // namespace

SurfaceStyle mix(const SurfaceStyle& a, const SurfaceStyle& b, float t) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  const auto between = [t](double x, double y) { return x + (y - x) * t; };

  SurfaceStyle out = t < 0.5f ? a : b;
  out.border = mix(a.border, b.border, t);
  out.border_width = between(a.border_width, b.border_width);
  out.corner_radius = between(a.corner_radius, b.corner_radius);
  out.text = mix(a.text, b.text, t);
  out.text_glow = between(a.text_glow, b.text_glow);

  out.fill = mix_fill(a.fill, b.fill, t);

  // A band arriving from nothing grows out of the body it is drawn on rather
  // than appearing in full halfway through the fade.
  if (a.rim && b.rim) {
    out.rim = Rim{.width = between(a.rim->width, b.rim->width),
                  .fill = mix_fill(a.rim->fill, b.rim->fill, t)};
  } else if (!a.rim && b.rim) {
    out.rim = Rim{.width = b.rim->width, .fill = mix_fill(a.fill, b.rim->fill, t)};
  } else if (a.rim && !b.rim) {
    out.rim = Rim{.width = a.rim->width, .fill = mix_fill(a.rim->fill, b.fill, t)};
  }

  // The same for a cap: a line arriving fades in out of the border it covers.
  if (a.cap && b.cap) {
    out.cap = Rim{.width = between(a.cap->width, b.cap->width),
                  .fill = mix_fill(a.cap->fill, b.cap->fill, t)};
  } else if (!a.cap && b.cap) {
    out.cap = Rim{.width = b.cap->width, .fill = mix_fill(Fill::solid(a.border), b.cap->fill, t)};
  } else if (a.cap && !b.cap) {
    out.cap = Rim{.width = a.cap->width, .fill = mix_fill(a.cap->fill, Fill::solid(b.border), t)};
  }

  if (a.bevel && b.bevel) {
    out.bevel->light = mix(a.bevel->light, b.bevel->light, t);
    out.bevel->dark = mix(a.bevel->dark, b.bevel->dark, t);
  }
  if (a.shadow && b.shadow) {
    out.shadow->color = mix(a.shadow->color, b.shadow->color, t);
    out.shadow->blur = between(a.shadow->blur, b.shadow->blur);
  } else if (!a.shadow && b.shadow) {
    // A glow arriving from nothing fades in rather than appearing halfway.
    out.shadow = b.shadow;
    out.shadow->color.a *= t;
  } else if (a.shadow && !b.shadow) {
    out.shadow = a.shadow;
    out.shadow->color.a *= 1.0f - t;
  }
  return out;
}

}  // namespace cutline::ui
