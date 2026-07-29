#include "cutline/render/effect_catalog.hpp"

#include <algorithm>
#include <array>

namespace cutline::render {
namespace {

using Param = EffectParamSpec;

constexpr std::array kBrightness{
    Param{.key = "amount", .name = "Amount", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "%"}};

constexpr std::array kContrast{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 300.0,
          .fallback = 100.0, .suffix = "%"}};

constexpr std::array kSaturation{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 300.0,
          .fallback = 100.0, .suffix = "%"}};

constexpr std::array kHue{
    Param{.key = "angle", .name = "Angle", .minimum = -180.0, .maximum = 180.0,
          .fallback = 0.0, .suffix = "°"}};

constexpr std::array kGrayscale{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 100.0,
          .fallback = 100.0, .suffix = "%"}};

constexpr std::array kInvert{
    Param{.key = "on", .name = "On", .minimum = 0.0, .maximum = 1.0, .fallback = 1.0,
          .suffix = "", .toggle = true}};

// Horizontal on by default: a flip that flips nothing looks like a flip that
// did not work. A *missing* horizontal still reads as off, which is what keeps
// a stored `{vertical: 1}` from also flipping the other way.
constexpr std::array kFlip{
    Param{.key = "horizontal", .name = "Horizontal", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true},
    Param{.key = "vertical", .name = "Vertical", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .toggle = true}};

constexpr std::array kBlur{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 50.0, .fallback = 0.0,
          .suffix = "px"}};

// Forty percent rather than the resolver's zero: an effect you add should do
// something. A vignette at zero is an effect in the stack with nothing to show
// for it, which reads as a broken button rather than as a neutral start.
constexpr std::array kVignette{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 100.0,
          .fallback = 40.0, .suffix = "%"}};

// Capped at 45% a side, so two opposite crops cannot meet in the middle. The
// resolver clamps the sum as well; this keeps a single slider from getting
// anywhere near it.
constexpr std::array kCrop{
    Param{.key = "left", .name = "Left", .minimum = 0.0, .maximum = 45.0, .fallback = 0.0,
          .suffix = "%"},
    Param{.key = "top", .name = "Top", .minimum = 0.0, .maximum = 45.0, .fallback = 0.0,
          .suffix = "%"},
    Param{.key = "right", .name = "Right", .minimum = 0.0, .maximum = 45.0, .fallback = 0.0,
          .suffix = "%"},
    Param{.key = "bottom", .name = "Bottom", .minimum = 0.0, .maximum = 45.0, .fallback = 0.0,
          .suffix = "%"}};

constexpr std::array kChromaKey{
    Param{.key = "similarity", .name = "Similarity", .minimum = 1.0, .maximum = 100.0,
          .fallback = 30.0, .suffix = "%"},
    Param{.key = "blend", .name = "Blend", .minimum = 0.0, .maximum = 100.0, .fallback = 10.0,
          .suffix = "%"}};

constexpr std::array kChromaKeyColors{
    EffectColorSpec{.key = "color", .name = "Key Colour", .fallback = "#00d000"}};

constexpr std::array kCatalog{
    EffectSpec{.type = "brightness", .name = "Brightness", .category = EffectCategory::Color,
               .params = kBrightness},
    EffectSpec{.type = "contrast", .name = "Contrast", .category = EffectCategory::Color,
               .params = kContrast},
    EffectSpec{.type = "saturation", .name = "Saturation", .category = EffectCategory::Color,
               .params = kSaturation},
    EffectSpec{.type = "hue", .name = "Hue", .category = EffectCategory::Color, .params = kHue},
    EffectSpec{.type = "grayscale", .name = "Black & White", .category = EffectCategory::Color,
               .params = kGrayscale},
    EffectSpec{.type = "invert", .name = "Invert", .category = EffectCategory::Color,
               .params = kInvert},
    EffectSpec{.type = "blur", .name = "Gaussian Blur",
               .category = EffectCategory::BlurAndSharpen, .params = kBlur},
    EffectSpec{.type = "chromakey", .name = "Chroma Key", .category = EffectCategory::Keying,
               .params = kChromaKey, .colors = kChromaKeyColors},
    EffectSpec{.type = "crop", .name = "Crop", .category = EffectCategory::Transform,
               .params = kCrop},
    EffectSpec{.type = "flip", .name = "Flip", .category = EffectCategory::Transform,
               .params = kFlip},
    EffectSpec{.type = "vignette", .name = "Vignette", .category = EffectCategory::Stylize,
               .params = kVignette},
};

}  // namespace

std::string_view to_string(EffectCategory category) noexcept {
  switch (category) {
    case EffectCategory::Color: return "Colour";
    case EffectCategory::BlurAndSharpen: return "Blur & Sharpen";
    case EffectCategory::Keying: return "Keying";
    case EffectCategory::Transform: return "Transform";
    case EffectCategory::Stylize: return "Stylise";
  }
  return "Other";
}

std::span<const EffectSpec> effect_catalog() noexcept { return kCatalog; }

const EffectSpec* find_effect_spec(std::string_view type) noexcept {
  const auto found = std::ranges::find(kCatalog, type, &EffectSpec::type);
  return found == kCatalog.end() ? nullptr : &*found;
}

}  // namespace cutline::render
