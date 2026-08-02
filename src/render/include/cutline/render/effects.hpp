#pragma once

/// Colours, as an effect stores them.
///
/// This was the whole flattening of an effect stack until effects became
/// passes. What is left is the part that had nothing to do with flattening: a
/// hex string in a project file, read into the numbers a shader wants. See
/// `effect_passes.hpp` for the stack itself.

#include "cutline/core/model.hpp"

namespace cutline::render {

/// A colour with components in 0..1, sRGB-encoded as written in the project.
struct EffectColor {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;

  friend bool operator==(const EffectColor&, const EffectColor&) = default;
};

/// Parses `#rgb` or `#rrggbb`, with or without the leading hash. Anything
/// unparseable gives `fallback`, because a malformed colour in a project file
/// should not make a clip disappear.
[[nodiscard]] EffectColor parse_hex_color(std::string_view text,
                                          EffectColor fallback = {}) noexcept;

/// The chroma keyer's default colour, `#00d000`. Written as the exact eighth-bit
/// value rather than a rounded decimal so it matches what parsing the hex gives.
inline constexpr EffectColor kDefaultKeyColor{0.0f, 208.0f / 255.0f, 0.0f};

}  // namespace cutline::render
