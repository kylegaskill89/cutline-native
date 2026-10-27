#pragma once

/// Ultra Key: Premiere's keyer, as passes.
///
/// Four sections in Premiere's order. Matte Generation pulls a matte from how
/// much of the key colour each pixel holds; Spill Suppression takes the key's
/// colour back out of what stays; Color Correction grades the foreground; and
/// Matte Cleanup chokes, softens and steepens the matte. Generation, spill and
/// correction look at one pixel at a time and are one pass, carrying its
/// settings as data the way Lumetri does. Choke and Soften look at neighbours
/// and are passes of their own, and the last pass applies Contrast and Mid
/// Point and chooses what Output shows.
///
/// The data layout below is the shader's as well, and stated once here.

#include "cutline/core/model.hpp"
#include "cutline/render/effect_passes.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace cutline::render {

/// What Output shows, in the order Premiere's menu lists it.
enum class UltraKeyOutput { Composite = 0, AlphaChannel = 1, ColorChannel = 2 };

/// Premiere's Setting menu. The first three are presets for Matte Generation;
/// Custom is what moving any of its controls makes it.
enum class UltraKeySetting { Default = 0, Relaxed = 1, Aggressive = 2, Custom = 3 };

/// Matte Generation's controls, by the keys they are stored under, in the
/// order `ultra_key_preset` gives their values.
inline constexpr std::array<std::string_view, 5> kUltraKeyMatteKeys{
    "transparency", "highlight", "shadow", "tolerance", "pedestal"};

/// What a Setting puts in Matte Generation's controls, 0 to 100 each. Custom
/// has none and leaves them as they are.
[[nodiscard]] std::array<double, 5> ultra_key_preset(UltraKeySetting setting) noexcept;

/// The key pass's data.
enum UltraKeySlot : std::size_t {
  kUltraKeyColour = 0,       ///< the key colour, coded
  kUltraTransparency = 3,    ///< how much of the screen counts as wholly clear
  kUltraHighlight = 4,       ///< 0 to 1
  kUltraShadow = 5,          ///< 0 to 1
  kUltraTolerance = 6,       ///< how far from the key's hue still counts
  kUltraPedestal = 7,        ///< alpha below this is taken to nought
  kUltraDesaturate = 8,      ///< 0 to 1
  kUltraRange = 9,           ///< how far from the key's hue spill is looked for
  kUltraSpill = 10,          ///< how much of it is taken out, 0 to 2
  kUltraLuma = 11,           ///< how much of the light it took is put back, 0 to 2
  kUltraSaturation = 12,     ///< a multiplier
  kUltraHue = 13,            ///< in radians
  kUltraLuminance = 14,      ///< a multiplier
  kUltraKeySettings = 16,
};

/// Ultra Key's passes for one effect, appended to `passes`.
void plan_ultra_key(const core::ClipEffect& effect, std::vector<EffectPass>& passes);

}  // namespace cutline::render
