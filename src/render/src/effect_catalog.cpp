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

// ---------------------------------------------------------------- grading --
//
// Everything from here down arrived after effects became passes, and between
// them they are the argument for having done it: each is one branch in one
// shader and one entry here. None needed a permanent field in the root
// constants, which is what the old flat struct charged for every effect.

constexpr std::array kExposure{
    Param{.key = "stops", .name = "Stops", .minimum = -4.0, .maximum = 4.0, .fallback = 0.5,
          .suffix = ""}};

constexpr std::array kGamma{
    Param{.key = "amount", .name = "Amount", .minimum = 10.0, .maximum = 400.0,
          .fallback = 120.0, .suffix = "%"}};

constexpr std::array kLevels{
    Param{.key = "black", .name = "Black Point", .minimum = 0.0, .maximum = 95.0,
          .fallback = 5.0, .suffix = "%"},
    Param{.key = "white", .name = "White Point", .minimum = 5.0, .maximum = 100.0,
          .fallback = 95.0, .suffix = "%"},
    Param{.key = "gamma", .name = "Gamma", .minimum = 10.0, .maximum = 400.0,
          .fallback = 100.0, .suffix = "%"}};

constexpr std::array kBalance{
    Param{.key = "red", .name = "Red", .minimum = 0.0, .maximum = 200.0, .fallback = 110.0,
          .suffix = "%"},
    Param{.key = "green", .name = "Green", .minimum = 0.0, .maximum = 200.0, .fallback = 100.0,
          .suffix = "%"},
    Param{.key = "blue", .name = "Blue", .minimum = 0.0, .maximum = 200.0, .fallback = 100.0,
          .suffix = "%"}};

constexpr std::array kTint{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 100.0,
          .fallback = 100.0, .suffix = "%"}};

// A warm shadow and a cool highlight, which is the split-tone everybody reaches
// for first and is visibly a tint rather than visibly nothing.
constexpr std::array kTintColors{
    EffectColorSpec{.key = "shadow", .name = "Shadows", .fallback = "#1a1030"},
    EffectColorSpec{.key = "highlight", .name = "Highlights", .fallback = "#ffe8c0"}};

constexpr std::array kSharpen{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 300.0,
          .fallback = 60.0, .suffix = "%"},
    Param{.key = "radius", .name = "Radius", .minimum = 0.5, .maximum = 10.0, .fallback = 1.0,
          .suffix = "px"}};

constexpr std::array kDirectionalBlur{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 50.0, .fallback = 8.0,
          .suffix = "px"},
    Param{.key = "angle", .name = "Angle", .minimum = -180.0, .maximum = 180.0,
          .fallback = 0.0, .suffix = "°"}};

constexpr std::array kPosterize{
    Param{.key = "levels", .name = "Levels", .minimum = 2.0, .maximum = 32.0, .fallback = 6.0,
          .suffix = ""}};

constexpr std::array kThreshold{
    Param{.key = "level", .name = "Level", .minimum = 0.0, .maximum = 100.0, .fallback = 50.0,
          .suffix = "%"}};

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
    EffectSpec{.type = "exposure", .name = "Exposure", .category = EffectCategory::Color,
               .params = kExposure},
    EffectSpec{.type = "gamma", .name = "Gamma", .category = EffectCategory::Color,
               .params = kGamma},
    EffectSpec{.type = "levels", .name = "Levels", .category = EffectCategory::Color,
               .params = kLevels},
    EffectSpec{.type = "balance", .name = "Colour Balance",
               .category = EffectCategory::Color, .params = kBalance},
    EffectSpec{.type = "tint", .name = "Tint", .category = EffectCategory::Color,
               .params = kTint, .colors = kTintColors},
    EffectSpec{.type = "blur", .name = "Gaussian Blur",
               .category = EffectCategory::BlurAndSharpen, .params = kBlur},
    EffectSpec{.type = "directionalblur", .name = "Directional Blur",
               .category = EffectCategory::BlurAndSharpen, .params = kDirectionalBlur},
    EffectSpec{.type = "sharpen", .name = "Sharpen",
               .category = EffectCategory::BlurAndSharpen, .params = kSharpen},
    EffectSpec{.type = "chromakey", .name = "Chroma Key", .category = EffectCategory::Keying,
               .params = kChromaKey, .colors = kChromaKeyColors},
    EffectSpec{.type = "crop", .name = "Crop", .category = EffectCategory::Transform,
               .params = kCrop},
    EffectSpec{.type = "flip", .name = "Flip", .category = EffectCategory::Transform,
               .params = kFlip},
    EffectSpec{.type = "vignette", .name = "Vignette", .category = EffectCategory::Stylize,
               .params = kVignette},
    EffectSpec{.type = "posterize", .name = "Posterize", .category = EffectCategory::Stylize,
               .params = kPosterize},
    EffectSpec{.type = "threshold", .name = "Threshold", .category = EffectCategory::Stylize,
               .params = kThreshold},
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
