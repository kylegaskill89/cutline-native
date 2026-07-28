#include "cutline/render/effects.hpp"

#include "cutline/core/effects.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace cutline::render {
namespace {

/// Looks a parameter up, falling back to the registry default when it is
/// absent. Missing is not zero: an absent contrast means 100%, not black.
[[nodiscard]] double param(const core::ClipEffect& effect, const std::string& key,
                           double fallback) noexcept {
  const auto found = effect.params.find(key);
  if (found == effect.params.end()) return fallback;
  if (!std::isfinite(found->second)) return fallback;
  return found->second;
}

[[nodiscard]] bool toggled(const core::ClipEffect& effect, const std::string& key,
                           double fallback) noexcept {
  return param(effect, key, fallback) >= 0.5;
}

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

bool EffectParams::is_neutral() const noexcept {
  return brightness == 0.0f && contrast == 1.0f && saturation == 1.0f && hue_degrees == 0.0f &&
         !invert && !flip_x && !flip_y && vignette == 0.0f && crop_left == 0.0f &&
         crop_top == 0.0f && crop_right == 0.0f && crop_bottom == 0.0f && !chroma_key;
}

EffectParams resolve_effect_params(const core::Clip& clip, double local_t) {
  EffectParams out;

  for (const core::ClipEffect& effect : core::resolved_effects(clip, local_t)) {
    if (!effect.enabled) continue;

    if (effect.type == "brightness") {
      // eq=brightness=amount/100
      out.brightness += static_cast<float>(param(effect, "amount", 0.0) / 100.0);

    } else if (effect.type == "contrast") {
      // eq=contrast=amount/100
      out.contrast *= static_cast<float>(param(effect, "amount", 100.0) / 100.0);

    } else if (effect.type == "saturation") {
      // eq=saturation=amount/100
      out.saturation *= static_cast<float>(param(effect, "amount", 100.0) / 100.0);

    } else if (effect.type == "hue") {
      // hue=h=angle
      out.hue_degrees += static_cast<float>(param(effect, "angle", 0.0));

    } else if (effect.type == "grayscale") {
      // hue=s=1-amount/100, which is a saturation multiplier like the others.
      // At 100% this is zero, and any later saturation cannot bring colour
      // back — which is exactly how chaining the fragments behaved.
      out.saturation *= static_cast<float>(1.0 - param(effect, "amount", 100.0) / 100.0);

    } else if (effect.type == "invert") {
      // Two negates cancel, so this toggles rather than sets.
      if (toggled(effect, "on", 1.0)) out.invert = !out.invert;

    } else if (effect.type == "flip") {
      if (toggled(effect, "horizontal", 0.0)) out.flip_x = !out.flip_x;
      if (toggled(effect, "vertical", 0.0)) out.flip_y = !out.flip_y;

    } else if (effect.type == "blur") {
      out.blur_sigma += static_cast<float>(std::max(0.0, param(effect, "amount", 0.0)));

    } else if (effect.type == "vignette") {
      const double amount = param(effect, "amount", 0.0);
      if (amount > 0.0) {
        // vignette=a=(amount/100)*(pi/2), capped at a quarter turn.
        constexpr double quarter_turn = std::numbers::pi / 2.0;
        out.vignette += static_cast<float>(std::min(quarter_turn, (amount / 100.0) * quarter_turn));
      }

    } else if (effect.type == "crop") {
      out.crop_left += static_cast<float>(param(effect, "left", 0.0) / 100.0);
      out.crop_top += static_cast<float>(param(effect, "top", 0.0) / 100.0);
      out.crop_right += static_cast<float>(param(effect, "right", 0.0) / 100.0);
      out.crop_bottom += static_cast<float>(param(effect, "bottom", 0.0) / 100.0);

    } else if (effect.type == "chromakey") {
      out.chroma_key = true;
      out.chroma_similarity = static_cast<float>(param(effect, "similarity", 30.0) / 100.0);
      out.chroma_blend = static_cast<float>(param(effect, "blend", 10.0) / 100.0);

      const auto color = effect.colors.find("color");
      out.chroma_color = color == effect.colors.end()
                             ? kDefaultKeyColor
                             : parse_hex_color(color->second, kDefaultKeyColor);
    }
    // An unknown type is ignored rather than rejected: a project written by a
    // newer version should still open, minus the effect it cannot draw.
  }

  // Clamped after accumulating, so stacked crops cannot invert the rectangle.
  // The reference clamped the kept region to 1%; the same floor applies here.
  const float max_crop = 0.99f;
  if (out.crop_left + out.crop_right > max_crop) {
    const float scale = max_crop / (out.crop_left + out.crop_right);
    out.crop_left *= scale;
    out.crop_right *= scale;
  }
  if (out.crop_top + out.crop_bottom > max_crop) {
    const float scale = max_crop / (out.crop_top + out.crop_bottom);
    out.crop_top *= scale;
    out.crop_bottom *= scale;
  }

  out.saturation = std::max(0.0f, out.saturation);
  out.contrast = std::max(0.0f, out.contrast);
  return out;
}

}  // namespace cutline::render
