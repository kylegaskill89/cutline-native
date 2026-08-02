#include "cutline/render/effects.hpp"

#include <algorithm>

namespace cutline::render {
namespace {

[[nodiscard]] int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

EffectColor parse_hex_color(std::string_view text, EffectColor fallback) noexcept {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);

  const auto component = [](int value) { return static_cast<float>(value) / 255.0f; };

  if (text.size() == 6) {
    int values[6];
    for (std::size_t i = 0; i < 6; ++i) {
      values[i] = hex_digit(text[i]);
      if (values[i] < 0) return fallback;
    }
    return {component(values[0] * 16 + values[1]), component(values[2] * 16 + values[3]),
            component(values[4] * 16 + values[5])};
  }

  if (text.size() == 3) {
    // #abc means #aabbcc, the usual shorthand.
    int values[3];
    for (std::size_t i = 0; i < 3; ++i) {
      values[i] = hex_digit(text[i]);
      if (values[i] < 0) return fallback;
    }
    return {component(values[0] * 17), component(values[1] * 17), component(values[2] * 17)};
  }

  return fallback;
}

}  // namespace cutline::render
