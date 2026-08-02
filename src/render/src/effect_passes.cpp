#include "cutline/render/effect_passes.hpp"

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

/// The most an edge pair may cut, leaving one percent of the frame.
///
/// Per pass rather than across the stack, which is the difference between this
/// and the flat resolver: two crops now cut what is left of the picture in turn,
/// so the second one takes 60% of the 40% the first left rather than the two
/// being added and scaled back to fit.
constexpr float kMaxCrop = 0.99f;

}  // namespace

// ------------------------------------------------------------------ makers --

EffectPass color_pass(float brightness, float contrast, float saturation,
                      float hue_radians) noexcept {
  EffectPass pass{.kind = EffectPassKind::Color};
  pass.values[0] = brightness;
  pass.values[1] = contrast;
  pass.values[2] = saturation;
  pass.values[3] = hue_radians;
  return pass;
}

EffectPass invert_pass() noexcept { return EffectPass{.kind = EffectPassKind::Invert}; }

EffectPass vignette_pass(float angle) noexcept {
  EffectPass pass{.kind = EffectPassKind::Vignette};
  pass.values[0] = angle;
  return pass;
}

EffectPass crop_pass(float left, float top, float right, float bottom) noexcept {
  // Clamped here so a single crop can never invert its own rectangle. Stacking
  // two is fine and needs no clamp: each takes a share of what the last left.
  const auto fit = [](float& near, float& far) {
    near = std::max(0.0f, near);
    far = std::max(0.0f, far);
    if (const float total = near + far; total > kMaxCrop) {
      near *= kMaxCrop / total;
      far *= kMaxCrop / total;
    }
  };
  fit(left, right);
  fit(top, bottom);

  EffectPass pass{.kind = EffectPassKind::Crop};
  pass.values[0] = left;
  pass.values[1] = top;
  pass.values[2] = right;
  pass.values[3] = bottom;
  return pass;
}

EffectPass chroma_key_pass(EffectColor color, float similarity, float blend) noexcept {
  EffectPass pass{.kind = EffectPassKind::ChromaKey};
  pass.values[0] = color.r;
  pass.values[1] = color.g;
  pass.values[2] = color.b;
  pass.values[3] = similarity;
  pass.values[4] = blend;
  return pass;
}

EffectPass flip_pass(bool horizontal, bool vertical) noexcept {
  EffectPass pass{.kind = EffectPassKind::Flip};
  // As the multiplier the shader mirrors uv with, so the branch is arithmetic
  // rather than a comparison.
  pass.values[0] = horizontal ? -1.0f : 1.0f;
  pass.values[1] = vertical ? -1.0f : 1.0f;
  return pass;
}

EffectPass blur_pass(float sigma) noexcept {
  EffectPass pass{.kind = EffectPassKind::Blur};
  pass.values[0] = std::max(0.0f, sigma);
  return pass;
}

PassMask pass_mask(const core::Mask& mask) noexcept {
  if (!mask.active()) return PassMask{};

  const double radians = mask.rotation * std::numbers::pi / 180.0;
  return PassMask{
      .shape = static_cast<float>(static_cast<int>(mask.shape)),
      .x = static_cast<float>(mask.x),
      .y = static_cast<float>(mask.y),
      .width = static_cast<float>(std::max(0.0, mask.width)),
      .height = static_cast<float>(std::max(0.0, mask.height)),
      .cos_rotation = static_cast<float>(std::cos(radians)),
      .sin_rotation = static_cast<float>(std::sin(radians)),
      .feather = static_cast<float>(std::max(0.0, mask.feather)),
      .opacity = static_cast<float>(std::clamp(mask.opacity, 0.0, 1.0)),
      .inverted = mask.inverted ? 1.0f : 0.0f,
  };
}

// --------------------------------------------------------------- accessors --

float pass_brightness(const EffectPass& pass) noexcept { return pass.values[0]; }
float pass_contrast(const EffectPass& pass) noexcept { return pass.values[1]; }
float pass_saturation(const EffectPass& pass) noexcept { return pass.values[2]; }
float pass_hue_radians(const EffectPass& pass) noexcept { return pass.values[3]; }
float pass_vignette(const EffectPass& pass) noexcept { return pass.values[0]; }

std::array<float, 4> pass_crop(const EffectPass& pass) noexcept {
  return {pass.values[0], pass.values[1], pass.values[2], pass.values[3]};
}

EffectColor pass_key_color(const EffectPass& pass) noexcept {
  return {pass.values[0], pass.values[1], pass.values[2]};
}

float pass_similarity(const EffectPass& pass) noexcept { return pass.values[3]; }
float pass_blend(const EffectPass& pass) noexcept { return pass.values[4]; }
bool pass_flips_x(const EffectPass& pass) noexcept { return pass.values[0] < 0.0f; }
bool pass_flips_y(const EffectPass& pass) noexcept { return pass.values[1] < 0.0f; }
float pass_sigma(const EffectPass& pass) noexcept { return pass.values[0]; }

// ------------------------------------------------------------------- plan --

std::vector<EffectPass> plan_effect_passes(const core::Clip& clip, double local_t) {
  std::vector<EffectPass> passes;

  for (const core::ClipEffect& effect : core::resolved_effects(clip, local_t)) {
    if (!effect.enabled) continue;
    const std::size_t before = passes.size();

    // Every branch is one effect becoming one pass. The conversions are the
    // same ones the flat resolver makes — the FFmpeg fragments in the spec are
    // still the authoritative definition of what each effect does — and what
    // has changed is only that they no longer accumulate into shared fields.
    if (effect.type == "brightness") {
      const auto amount = static_cast<float>(param(effect, "amount", 0.0) / 100.0);
      if (amount != 0.0f) passes.push_back(color_pass(amount, 1.0f, 1.0f, 0.0f));

    } else if (effect.type == "contrast") {
      const auto amount = static_cast<float>(param(effect, "amount", 100.0) / 100.0);
      if (amount != 1.0f) passes.push_back(color_pass(0.0f, std::max(0.0f, amount), 1.0f, 0.0f));

    } else if (effect.type == "saturation") {
      const auto amount = static_cast<float>(param(effect, "amount", 100.0) / 100.0);
      if (amount != 1.0f) passes.push_back(color_pass(0.0f, 1.0f, std::max(0.0f, amount), 0.0f));

    } else if (effect.type == "grayscale") {
      // hue=s=1-amount/100, which is a saturation multiplier like the others.
      const auto amount = static_cast<float>(1.0 - param(effect, "amount", 100.0) / 100.0);
      if (amount != 1.0f) passes.push_back(color_pass(0.0f, 1.0f, std::max(0.0f, amount), 0.0f));

    } else if (effect.type == "hue") {
      const double degrees = param(effect, "angle", 0.0);
      if (degrees != 0.0) {
        passes.push_back(color_pass(0.0f, 1.0f, 1.0f,
                                    static_cast<float>(degrees * std::numbers::pi / 180.0)));
      }

    } else if (effect.type == "invert") {
      // Two of them still cancel, but now because they are two passes that each
      // negate rather than because a flag was toggled twice.
      if (toggled(effect, "on", 1.0)) passes.push_back(invert_pass());

    } else if (effect.type == "flip") {
      const bool horizontal = toggled(effect, "horizontal", 0.0);
      const bool vertical = toggled(effect, "vertical", 0.0);
      if (horizontal || vertical) passes.push_back(flip_pass(horizontal, vertical));

    } else if (effect.type == "blur") {
      const auto sigma = static_cast<float>(std::max(0.0, param(effect, "amount", 0.0)));
      if (sigma > 0.0f) passes.push_back(blur_pass(sigma));

    } else if (effect.type == "vignette") {
      const double amount = param(effect, "amount", 0.0);
      if (amount > 0.0) {
        // vignette=a=(amount/100)*(pi/2), capped at a quarter turn.
        constexpr double quarter_turn = std::numbers::pi / 2.0;
        passes.push_back(vignette_pass(
            static_cast<float>(std::min(quarter_turn, (amount / 100.0) * quarter_turn))));
      }

    } else if (effect.type == "crop") {
      const auto edge = [&effect](const char* key) {
        return static_cast<float>(param(effect, key, 0.0) / 100.0);
      };
      const EffectPass pass =
          crop_pass(edge("left"), edge("top"), edge("right"), edge("bottom"));
      if (pass_crop(pass) != std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}) {
        passes.push_back(pass);
      }

    } else if (effect.type == "chromakey") {
      const auto color = effect.colors.find("color");
      passes.push_back(chroma_key_pass(
          color == effect.colors.end() ? kDefaultKeyColor
                                       : parse_hex_color(color->second, kDefaultKeyColor),
          static_cast<float>(param(effect, "similarity", 30.0) / 100.0),
          static_cast<float>(param(effect, "blend", 10.0) / 100.0)));
    }
    // An unknown type is ignored rather than rejected: a project written by a
    // newer version should still open, minus the effect it cannot draw.

    // Whatever this effect became — a pass, two, or none — carries its mask.
    // A blur is two draws and both of them are masked, which is what keeps the
    // two axes agreeing about where the blur is.
    const PassMask mask = pass_mask(effect.mask);
    for (std::size_t at = before; at < passes.size(); ++at) passes[at].mask = mask;
  }

  return passes;
}

}  // namespace cutline::render
