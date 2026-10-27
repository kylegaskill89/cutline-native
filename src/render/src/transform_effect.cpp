#include "cutline/render/transform_effect.hpp"

#include "cutline/core/effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>

namespace cutline::render {
namespace {

constexpr double kRadians = std::numbers::pi / 180.0;

/// One moment's transform, in the order the shader reads it.
[[nodiscard]] std::array<float, kSampleSize> sample_at(const core::ClipEffect& effect, double t) {
  const auto at = [&](std::string_view key, double fallback) {
    const bool stored = effect.params.contains(std::string(key)) ||
                        core::is_effect_param_animated(effect, key);
    const double value = stored ? core::effect_param_at(effect, key, t) : fallback;
    return std::isfinite(value) ? value : fallback;
  };
  const double height = at("scale_height", 100.0) / 100.0;
  // Uniform Scale makes Scale Height the scale both ways, as in Premiere.
  const double width = at("uniform", 1.0) >= 0.5 ? height : at("scale_width", 100.0) / 100.0;
  return {static_cast<float>(at("anchor_x", 0.5)),
          static_cast<float>(at("anchor_y", 0.5)),
          static_cast<float>(at("position_x", 0.5)),
          static_cast<float>(at("position_y", 0.5)),
          static_cast<float>(width),
          static_cast<float>(height),
          // Short of a right angle, where the shear would be infinite.
          static_cast<float>(std::clamp(at("skew", 0.0), -89.0, 89.0) * kRadians),
          static_cast<float>(at("skew_axis", 0.0) * kRadians),
          static_cast<float>(at("rotation", 0.0) * kRadians)};
}

[[nodiscard]] bool is_identity(const std::array<float, kSampleSize>& s) {
  constexpr float e = 1e-6f;
  return std::abs(s[kSampleAnchor] - s[kSamplePosition]) < e &&
         std::abs(s[kSampleAnchor + 1] - s[kSamplePosition + 1]) < e &&
         std::abs(s[kSampleScale] - 1.0f) < e && std::abs(s[kSampleScale + 1] - 1.0f) < e &&
         std::abs(s[kSampleSkew]) < e && std::abs(s[kSampleRotation]) < e;
}

}  // namespace

void plan_transform(const core::ClipEffect& effect, double local_t, double frame_duration,
                    std::vector<EffectPass>& passes) {
  const auto param = [&](std::string_view key, double fallback) {
    const bool stored = effect.params.contains(std::string(key)) ||
                        core::is_effect_param_animated(effect, key);
    return stored ? core::effect_param_at(effect, key, local_t) : fallback;
  };
  const double opacity = std::clamp(param("opacity", 100.0) / 100.0, 0.0, 1.0);
  const bool bicubic = param("sampling", 0.0) >= 0.5;

  // A composition's shutter angle is After Effects' setting; a Premiere
  // sequence has none, so using it means no motion blur, as it does there.
  const double angle = param("composition_shutter", 1.0) >= 0.5
                           ? 0.0
                           : std::clamp(param("shutter_angle", 0.0), 0.0, 360.0);
  const double open = angle / 360.0 * std::max(0.0, frame_duration);

  std::vector<std::array<float, kSampleSize>> samples;
  if (open > 0.0) {
    // Centred on the frame's moment, which is AE's default phase.
    for (int i = 0; i < kMaxShutterSamples; ++i) {
      const double t = local_t + open * ((i + 0.5) / kMaxShutterSamples - 0.5);
      samples.push_back(sample_at(effect, t));
    }
    // A shutter on something that is not moving is one picture.
    if (std::ranges::all_of(samples, [&](const auto& s) { return s == samples.front(); })) {
      samples.resize(1);
    }
  } else {
    samples.push_back(sample_at(effect, local_t));
  }

  if (samples.size() == 1 && is_identity(samples.front()) && opacity >= 1.0) return;

  EffectPass pass;
  pass.kind = EffectPassKind::Transform;
  std::vector<float>& data = pass.data;
  data.assign(kTransformHeader, 0.0f);
  data[kTransformSamples] = static_cast<float>(samples.size());
  data[kTransformBicubic] = bicubic ? 1.0f : 0.0f;
  data[kTransformOpacity] = static_cast<float>(opacity);
  for (const auto& sample : samples) data.insert(data.end(), sample.begin(), sample.end());
  passes.push_back(std::move(pass));
}

}  // namespace cutline::render
