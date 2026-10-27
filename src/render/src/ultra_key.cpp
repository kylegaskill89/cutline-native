#include "cutline/render/ultra_key.hpp"

#include "cutline/render/effects.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace cutline::render {
namespace {

[[nodiscard]] double param(const core::ClipEffect& effect, std::string_view key,
                           double fallback) {
  const auto found = effect.params.find(std::string(key));
  if (found == effect.params.end() || !std::isfinite(found->second)) return fallback;
  return found->second;
}

}  // namespace

std::array<double, 5> ultra_key_preset(UltraKeySetting setting) noexcept {
  // Default is Premiere's documented one. Adobe does not publish what Relaxed
  // and Aggressive set, so they are the same controls moved the way each name
  // says: Relaxed keeps more of the edge and lets less count as screen,
  // Aggressive clears more of an uneven screen and its noise.
  switch (setting) {
    case UltraKeySetting::Relaxed: return {35.0, 10.0, 60.0, 35.0, 5.0};
    case UltraKeySetting::Aggressive: return {60.0, 10.0, 40.0, 70.0, 25.0};
    default: return {45.0, 10.0, 50.0, 50.0, 10.0};
  }
}

void plan_ultra_key(const core::ClipEffect& effect, std::vector<EffectPass>& passes) {
  const auto value = [&effect](std::string_view key, double fallback) {
    return param(effect, key, fallback);
  };
  const auto colour = effect.colors.find("color");
  const EffectColor key = colour == effect.colors.end()
                              ? kDefaultKeyColor
                              : parse_hex_color(colour->second, kDefaultKeyColor);

  EffectPass pass;
  pass.kind = EffectPassKind::UltraKey;
  std::vector<float>& data = pass.data;
  data.assign(kUltraKeySettings, 0.0f);
  data[kUltraKeyColour] = key.r;
  data[kUltraKeyColour + 1] = key.g;
  data[kUltraKeyColour + 2] = key.b;
  const auto unit = [&value](std::string_view name, double fallback) {
    return static_cast<float>(std::clamp(value(name, fallback), 0.0, 100.0) / 100.0);
  };
  data[kUltraTransparency] = unit("transparency", 45.0);
  data[kUltraHighlight] = unit("highlight", 10.0);
  data[kUltraShadow] = unit("shadow", 50.0);
  data[kUltraTolerance] = unit("tolerance", 50.0);
  data[kUltraPedestal] = unit("pedestal", 10.0);
  data[kUltraDesaturate] = unit("desaturate", 25.0);
  data[kUltraRange] = unit("range", 50.0);
  data[kUltraSpill] = 2.0f * unit("spill", 50.0);
  data[kUltraLuma] = 2.0f * unit("luma", 50.0);
  data[kUltraSaturation] =
      static_cast<float>(std::clamp(value("saturation", 100.0), 0.0, 200.0) / 100.0);
  data[kUltraHue] = static_cast<float>(value("hue", 0.0) * std::numbers::pi / 180.0);
  data[kUltraLuminance] =
      static_cast<float>(std::clamp(value("luminance", 100.0), 0.0, 200.0) / 100.0);
  passes.push_back(std::move(pass));

  // Choke is an erode, 100 being Premiere's nine-by-nine kernel: four pixels
  // each way.
  if (const double choke = std::clamp(value("choke", 0.0), 0.0, 100.0); choke > 0.0) {
    EffectPass shrink;
    shrink.kind = EffectPassKind::MatteChoke;
    shrink.values[0] = static_cast<float>(choke / 100.0 * 4.0);
    passes.push_back(shrink);
  }
  // Soften blurs the matte alone, up to ten pixels across at 100.
  if (const double soften = std::clamp(value("soften", 0.0), 0.0, 100.0); soften > 0.0) {
    EffectPass soft;
    soft.kind = EffectPassKind::MatteSoften;
    soft.values[0] = static_cast<float>(soften / 100.0 * 10.0);
    passes.push_back(soft);
  }

  const auto contrast = static_cast<float>(std::clamp(value("contrast", 0.0), 0.0, 100.0));
  const auto output = static_cast<int>(std::lround(value("output", 0.0)));
  if (contrast > 0.0f || output != 0) {
    EffectPass finish;
    finish.kind = EffectPassKind::UltraKeyOutput;
    // Contrast steepens the matte about Mid Point, up to sixteen times.
    finish.values[0] = 1.0f + contrast / 100.0f * 15.0f;
    finish.values[1] = unit("mid_point", 50.0);
    finish.values[2] = static_cast<float>(std::clamp(output, 0, 2));
    passes.push_back(finish);
  }
}

}  // namespace cutline::render
