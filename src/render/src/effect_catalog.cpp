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

// Ultra Key, with Premiere's sections, names, ranges and defaults. Output and
// Setting are menus. Setting's presets write Matte Generation's controls, and
// moving one of those makes it Custom, which is the editor's business.
constexpr std::array<std::string_view, 3> kUltraKeyOutputs{"Composite", "Alpha Channel",
                                                           "Color Channel"};
constexpr std::array<std::string_view, 4> kUltraKeySettings{"Default", "Relaxed", "Aggressive",
                                                            "Custom"};

constexpr std::array kUltraKey{
    Param{.key = "output", .name = "Output", .minimum = 0.0, .maximum = 2.0, .fallback = 0.0,
          .suffix = "", .choices = kUltraKeyOutputs},
    Param{.key = "setting", .name = "Setting", .minimum = 0.0, .maximum = 3.0, .fallback = 0.0,
          .suffix = "", .choices = kUltraKeySettings},
    Param{.key = "transparency", .name = "Transparency", .minimum = 0.0, .maximum = 100.0,
          .fallback = 45.0, .suffix = "", .group = "Matte Generation"},
    Param{.key = "highlight", .name = "Highlight", .minimum = 0.0, .maximum = 100.0,
          .fallback = 10.0, .suffix = "", .group = "Matte Generation"},
    Param{.key = "shadow", .name = "Shadow", .minimum = 0.0, .maximum = 100.0,
          .fallback = 50.0, .suffix = "", .group = "Matte Generation"},
    Param{.key = "tolerance", .name = "Tolerance", .minimum = 0.0, .maximum = 100.0,
          .fallback = 50.0, .suffix = "", .group = "Matte Generation"},
    Param{.key = "pedestal", .name = "Pedestal", .minimum = 0.0, .maximum = 100.0,
          .fallback = 10.0, .suffix = "", .group = "Matte Generation"},
    Param{.key = "choke", .name = "Choke", .minimum = 0.0, .maximum = 100.0, .fallback = 0.0,
          .suffix = "", .group = "Matte Cleanup"},
    Param{.key = "soften", .name = "Soften", .minimum = 0.0, .maximum = 100.0, .fallback = 0.0,
          .suffix = "", .group = "Matte Cleanup"},
    Param{.key = "contrast", .name = "Contrast", .minimum = 0.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Matte Cleanup"},
    Param{.key = "mid_point", .name = "Mid Point", .minimum = 0.0, .maximum = 100.0,
          .fallback = 50.0, .suffix = "", .group = "Matte Cleanup"},
    Param{.key = "desaturate", .name = "Desaturate", .minimum = 0.0, .maximum = 100.0,
          .fallback = 25.0, .suffix = "", .group = "Spill Suppression"},
    Param{.key = "range", .name = "Range", .minimum = 0.0, .maximum = 100.0, .fallback = 50.0,
          .suffix = "", .group = "Spill Suppression"},
    Param{.key = "spill", .name = "Spill", .minimum = 0.0, .maximum = 100.0, .fallback = 50.0,
          .suffix = "", .group = "Spill Suppression"},
    Param{.key = "luma", .name = "Luma", .minimum = 0.0, .maximum = 100.0, .fallback = 50.0,
          .suffix = "", .group = "Spill Suppression"},
    Param{.key = "saturation", .name = "Saturation", .minimum = 0.0, .maximum = 200.0,
          .fallback = 100.0, .suffix = "", .group = "Color Correction"},
    Param{.key = "hue", .name = "Hue", .minimum = -180.0, .maximum = 180.0, .fallback = 0.0,
          .suffix = "°", .group = "Color Correction"},
    Param{.key = "luminance", .name = "Luminance", .minimum = 0.0, .maximum = 200.0,
          .fallback = 100.0, .suffix = "", .group = "Color Correction"}};

constexpr std::array kUltraKeyColors{
    EffectColorSpec{.key = "color", .name = "Key Color", .fallback = "#00d000"}};

// Premiere's Transform effect, in its order. Anchor Point and Position are
// fractions of the layer's frame and shown in its pixels; the centre is where
// both start, so adding it changes nothing.
constexpr std::array<std::string_view, 2> kTransformSampling{"Bilinear", "Bicubic"};

constexpr std::array kTransform{
    Param{.key = "anchor_x", .name = "Anchor Point", .minimum = -2.0, .maximum = 3.0,
          .fallback = 0.5, .suffix = "", .unit = EffectParamUnit::LayerWidth,
          .pairs_next = true},
    Param{.key = "anchor_y", .name = "Anchor Point Y", .minimum = -2.0, .maximum = 3.0,
          .fallback = 0.5, .suffix = "", .unit = EffectParamUnit::LayerHeight},
    Param{.key = "position_x", .name = "Position", .minimum = -2.0, .maximum = 3.0,
          .fallback = 0.5, .suffix = "", .unit = EffectParamUnit::LayerWidth,
          .pairs_next = true},
    Param{.key = "position_y", .name = "Position Y", .minimum = -2.0, .maximum = 3.0,
          .fallback = 0.5, .suffix = "", .unit = EffectParamUnit::LayerHeight},
    Param{.key = "uniform", .name = "Uniform Scale", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true},
    Param{.key = "scale_height", .name = "Scale Height", .minimum = 0.0, .maximum = 600.0,
          .fallback = 100.0, .suffix = ""},
    Param{.key = "scale_width", .name = "Scale Width", .minimum = 0.0, .maximum = 600.0,
          .fallback = 100.0, .suffix = ""},
    Param{.key = "skew", .name = "Skew", .minimum = -89.0, .maximum = 89.0, .fallback = 0.0,
          .suffix = "°"},
    Param{.key = "skew_axis", .name = "Skew Axis", .minimum = -180.0, .maximum = 180.0,
          .fallback = 0.0, .suffix = "°"},
    Param{.key = "rotation", .name = "Rotation", .minimum = -360.0, .maximum = 360.0,
          .fallback = 0.0, .suffix = "°"},
    Param{.key = "opacity", .name = "Opacity", .minimum = 0.0, .maximum = 100.0,
          .fallback = 100.0, .suffix = ""},
    Param{.key = "composition_shutter", .name = "Use Composition's Shutter Angle",
          .minimum = 0.0, .maximum = 1.0, .fallback = 1.0, .suffix = "", .toggle = true},
    Param{.key = "shutter_angle", .name = "Shutter Angle", .minimum = 0.0, .maximum = 360.0,
          .fallback = 0.0, .suffix = ""},
    Param{.key = "sampling", .name = "Sampling", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .choices = kTransformSampling}};

// Warp Stabilizer, with Premiere's sections, names, ranges and defaults.
// Auto-scale's own reading is worked out from the analysis rather than set,
// so it is shown beside the Analyze button rather than here.
constexpr std::array<std::string_view, 2> kStabilizeResults{"Smooth Motion", "No Motion"};
constexpr std::array<std::string_view, 4> kStabilizeMethods{
    "Position", "Position, Scale, Rotation", "Perspective", "Subspace Warp"};
constexpr std::array<std::string_view, 4> kStabilizeFraming{
    "Stabilize Only", "Stabilize, Crop", "Stabilize, Crop, Auto-scale",
    "Stabilize, Synthesize Edges"};
constexpr std::array<std::string_view, 2> kRollingShutter{"Automatic Reduction",
                                                          "Enhanced Reduction"};

constexpr std::array kWarpStabilizer{
    Param{.key = "result", .name = "Result", .minimum = 0.0, .maximum = 1.0, .fallback = 0.0,
          .suffix = "", .group = "Stabilization", .choices = kStabilizeResults},
    Param{.key = "smoothness", .name = "Smoothness", .minimum = 0.0, .maximum = 1000.0,
          .fallback = 50.0, .suffix = "%", .group = "Stabilization"},
    Param{.key = "method", .name = "Method", .minimum = 0.0, .maximum = 3.0, .fallback = 3.0,
          .suffix = "", .group = "Stabilization", .choices = kStabilizeMethods},
    Param{.key = "preserve_scale", .name = "Preserve Scale", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .toggle = true, .group = "Stabilization"},
    Param{.key = "framing", .name = "Framing", .minimum = 0.0, .maximum = 3.0, .fallback = 2.0,
          .suffix = "", .group = "Borders", .choices = kStabilizeFraming},
    Param{.key = "maximum_scale", .name = "Maximum Scale", .minimum = 100.0, .maximum = 1000.0,
          .fallback = 150.0, .suffix = "%", .group = "Borders"},
    Param{.key = "safe_margin", .name = "Action-Safe Margin", .minimum = 0.0, .maximum = 45.0,
          .fallback = 0.0, .suffix = "%", .group = "Borders"},
    Param{.key = "additional_scale", .name = "Additional Scale", .minimum = 1.0,
          .maximum = 1000.0, .fallback = 100.0, .suffix = "%", .group = "Borders"},
    Param{.key = "detailed", .name = "Detailed Analysis", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .toggle = true, .group = "Advanced"},
    Param{.key = "rolling_shutter", .name = "Rolling Shutter Ripple", .minimum = 0.0,
          .maximum = 1.0, .fallback = 0.0, .suffix = "", .group = "Advanced",
          .choices = kRollingShutter},
    Param{.key = "crop_smooth", .name = "Crop Less <-> Smooth More", .minimum = 0.0,
          .maximum = 100.0, .fallback = 50.0, .suffix = "%", .group = "Advanced"},
    Param{.key = "synthesis_range", .name = "Synthesis Input Range (seconds)", .minimum = 0.0,
          .maximum = 10.0, .fallback = 3.0, .suffix = "", .group = "Advanced"},
    Param{.key = "synthesis_feather", .name = "Synthesis Edge Feather", .minimum = 0.0,
          .maximum = 100.0, .fallback = 60.0, .suffix = "", .group = "Advanced"},
    Param{.key = "synthesis_left", .name = "Synthesis Edge Cropping Left", .minimum = 0.0,
          .maximum = 100.0, .fallback = 0.0, .suffix = "", .group = "Advanced"},
    Param{.key = "synthesis_top", .name = "Synthesis Edge Cropping Top", .minimum = 0.0,
          .maximum = 100.0, .fallback = 0.0, .suffix = "", .group = "Advanced"},
    Param{.key = "synthesis_right", .name = "Synthesis Edge Cropping Right", .minimum = 0.0,
          .maximum = 100.0, .fallback = 0.0, .suffix = "", .group = "Advanced"},
    Param{.key = "synthesis_bottom", .name = "Synthesis Edge Cropping Bottom", .minimum = 0.0,
          .maximum = 100.0, .fallback = 0.0, .suffix = "", .group = "Advanced"},
    Param{.key = "hide_banner", .name = "Hide Warning Banner", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .toggle = true, .group = "Advanced"}};

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

constexpr std::array kRadialBlur{
    // Nothing at first, like every other effect here: adding one to a stack has
    // to leave the picture alone until somebody asks it not to.
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 100.0, .fallback = 0.0,
          .suffix = "%"},
    Param{.key = "x", .name = "Centre X", .minimum = -50.0, .maximum = 150.0, .fallback = 50.0,
          .suffix = "%"},
    Param{.key = "y", .name = "Centre Y", .minimum = -50.0, .maximum = 150.0, .fallback = 50.0,
          .suffix = "%"},
    Param{.key = "spin", .name = "Spin", .minimum = 0.0, .maximum = 1.0, .fallback = 0.0,
          .suffix = "", .toggle = true}};

constexpr std::array kDistort{
    Param{.key = "amount", .name = "Distortion", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "%"},
    Param{.key = "scale", .name = "Scale", .minimum = 25.0, .maximum = 200.0,
          .fallback = 100.0, .suffix = "%"}};

constexpr std::array kNoise{
    Param{.key = "amount", .name = "Amount", .minimum = 0.0, .maximum = 100.0, .fallback = 0.0,
          .suffix = "%"},
    Param{.key = "monochrome", .name = "Monochrome", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true}};

// Premiere's defaults: half opaque, down and to the right, ten pixels away
// and ten soft.
// Lumetri Color, with Premiere's sections, names, ranges and defaults. Every
// section starts switched on and every control neutral, so adding it changes
// nothing until something is moved, as in Premiere. Its curves and LUTs are
// not numbers and are kept beside these, in `ClipEffect::curves` and `files`.
constexpr std::array kLumetri{
    Param{.key = "basic", .name = "Basic Correction", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true, .group = "Basic Correction"},
    Param{.key = "temperature", .name = "Temperature", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "tint", .name = "Tint", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "exposure", .name = "Exposure", .minimum = -5.0, .maximum = 5.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "contrast", .name = "Contrast", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "highlights", .name = "Highlights", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "shadows", .name = "Shadows", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "whites", .name = "Whites", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "blacks", .name = "Blacks", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "hdr_specular", .name = "HDR Specular", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Basic Correction"},
    Param{.key = "saturation", .name = "Saturation", .minimum = 0.0, .maximum = 200.0,
          .fallback = 100.0, .suffix = "%", .group = "Basic Correction"},
    Param{.key = "creative", .name = "Creative", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true, .group = "Creative"},
    Param{.key = "intensity", .name = "Intensity", .minimum = 0.0, .maximum = 200.0,
          .fallback = 100.0, .suffix = "%", .group = "Creative"},
    Param{.key = "faded_film", .name = "Faded Film", .minimum = 0.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Creative"},
    Param{.key = "sharpen", .name = "Sharpen", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Creative"},
    Param{.key = "vibrance", .name = "Vibrance", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Creative"},
    Param{.key = "creative_saturation", .name = "Saturation", .minimum = 0.0, .maximum = 200.0,
          .fallback = 100.0, .suffix = "%", .group = "Creative"},
    Param{.key = "shadow_tint_x", .name = "Shadow Tint X", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Creative"},
    Param{.key = "shadow_tint_y", .name = "Shadow Tint Y", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Creative"},
    Param{.key = "highlight_tint_x", .name = "Highlight Tint X", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Creative"},
    Param{.key = "highlight_tint_y", .name = "Highlight Tint Y", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Creative"},
    Param{.key = "tint_balance", .name = "Tint Balance", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Creative"},
    Param{.key = "curves", .name = "Curves", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true, .group = "Curves"},
    Param{.key = "wheels", .name = "Color Wheels", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true, .group = "Color Wheels"},
    Param{.key = "shadows_x", .name = "Shadows X", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "shadows_y", .name = "Shadows Y", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "shadows_luma", .name = "Shadows Luma", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "midtones_x", .name = "Midtones X", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "midtones_y", .name = "Midtones Y", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "midtones_luma", .name = "Midtones Luma", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "highlights_x", .name = "Highlights X", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "highlights_y", .name = "Highlights Y", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "highlights_luma", .name = "Highlights Luma", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Color Wheels"},
    Param{.key = "secondary", .name = "HSL Secondary", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true, .group = "HSL Secondary"},
    Param{.key = "key_hue_lo", .name = "Hue From", .minimum = 0.0, .maximum = 360.0,
          .fallback = 0.0, .suffix = "°", .group = "HSL Secondary"},
    Param{.key = "key_hue_hi", .name = "Hue To", .minimum = 0.0, .maximum = 360.0,
          .fallback = 360.0, .suffix = "°", .group = "HSL Secondary"},
    Param{.key = "key_hue_soft", .name = "Hue Softness", .minimum = 0.0, .maximum = 90.0,
          .fallback = 0.0, .suffix = "°", .group = "HSL Secondary"},
    Param{.key = "key_sat_lo", .name = "Saturation From", .minimum = 0.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "%", .group = "HSL Secondary"},
    Param{.key = "key_sat_hi", .name = "Saturation To", .minimum = 0.0, .maximum = 100.0,
          .fallback = 100.0, .suffix = "%", .group = "HSL Secondary"},
    Param{.key = "key_sat_soft", .name = "Saturation Softness", .minimum = 0.0, .maximum = 50.0,
          .fallback = 0.0, .suffix = "%", .group = "HSL Secondary"},
    Param{.key = "key_luma_lo", .name = "Luma From", .minimum = 0.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "%", .group = "HSL Secondary"},
    Param{.key = "key_luma_hi", .name = "Luma To", .minimum = 0.0, .maximum = 100.0,
          .fallback = 100.0, .suffix = "%", .group = "HSL Secondary"},
    Param{.key = "key_luma_soft", .name = "Luma Softness", .minimum = 0.0, .maximum = 50.0,
          .fallback = 0.0, .suffix = "%", .group = "HSL Secondary"},
    Param{.key = "denoise", .name = "Denoise", .minimum = 0.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "blur", .name = "Blur", .minimum = 0.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "invert_key", .name = "Invert Mask", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .toggle = true, .group = "HSL Secondary"},
    Param{.key = "show_mask", .name = "Show Mask", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .toggle = true, .group = "HSL Secondary"},
    Param{.key = "sec_x", .name = "Correction X", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "sec_y", .name = "Correction Y", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "sec_luma", .name = "Correction Luma", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "sec_temperature", .name = "Temperature", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "sec_tint", .name = "Tint", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "sec_contrast", .name = "Contrast", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "sec_sharpen", .name = "Sharpen", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "HSL Secondary"},
    Param{.key = "sec_saturation", .name = "Saturation", .minimum = 0.0, .maximum = 200.0,
          .fallback = 100.0, .suffix = "%", .group = "HSL Secondary"},
    Param{.key = "vignette", .name = "Vignette", .minimum = 0.0, .maximum = 1.0,
          .fallback = 1.0, .suffix = "", .toggle = true, .group = "Vignette"},
    Param{.key = "vig_amount", .name = "Amount", .minimum = -5.0, .maximum = 5.0,
          .fallback = 0.0, .suffix = "", .group = "Vignette"},
    Param{.key = "vig_midpoint", .name = "Midpoint", .minimum = 0.0, .maximum = 100.0,
          .fallback = 50.0, .suffix = "", .group = "Vignette"},
    Param{.key = "vig_roundness", .name = "Roundness", .minimum = -100.0, .maximum = 100.0,
          .fallback = 0.0, .suffix = "", .group = "Vignette"},
    Param{.key = "vig_feather", .name = "Feather", .minimum = 0.0, .maximum = 100.0,
          .fallback = 50.0, .suffix = "", .group = "Vignette"}};

constexpr std::array kDropShadow{
    Param{.key = "opacity", .name = "Opacity", .minimum = 0.0, .maximum = 100.0,
          .fallback = 50.0, .suffix = "%"},
    Param{.key = "direction", .name = "Direction", .minimum = 0.0, .maximum = 360.0,
          .fallback = 135.0, .suffix = "°"},
    Param{.key = "distance", .name = "Distance", .minimum = 0.0, .maximum = 200.0,
          .fallback = 10.0, .suffix = "px"},
    Param{.key = "softness", .name = "Softness", .minimum = 0.0, .maximum = 100.0,
          .fallback = 10.0, .suffix = "px"},
    Param{.key = "shadow_only", .name = "Shadow Only", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .toggle = true}};

constexpr std::array kDropShadowColors{
    EffectColorSpec{.key = "color", .name = "Shadow Colour", .fallback = "#000000"}};

// The matte by its track's number counting up from V1, which is how Premiere's
// menu names them. Nought is none, which is where Premiere's starts too: a key
// that picked a track by itself would key against something nobody chose.
constexpr std::array kTrackMatte{
    Param{.key = "track", .name = "Matte Track", .minimum = 0.0, .maximum = 99.0,
          .fallback = 0.0, .suffix = ""},
    Param{.key = "luma", .name = "Composite Using Luma", .minimum = 0.0, .maximum = 1.0,
          .fallback = 0.0, .suffix = "", .toggle = true},
    Param{.key = "reverse", .name = "Reverse", .minimum = 0.0, .maximum = 1.0, .fallback = 0.0,
          .suffix = "", .toggle = true}};

constexpr std::array kPosterize{
    Param{.key = "levels", .name = "Levels", .minimum = 2.0, .maximum = 32.0, .fallback = 6.0,
          .suffix = ""}};

constexpr std::array kThreshold{
    Param{.key = "level", .name = "Level", .minimum = 0.0, .maximum = 100.0, .fallback = 50.0,
          .suffix = "%"}};

constexpr std::array kCatalog{
    // First among the colour effects: in Premiere it is the colour effect, and
    // the rest are older single-purpose tools.
    EffectSpec{.type = "lumetri", .name = "Lumetri Color", .category = EffectCategory::Color,
               .params = kLumetri},
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
    EffectSpec{.type = "radialblur", .name = "Radial Blur",
               .category = EffectCategory::BlurAndSharpen, .params = kRadialBlur},
    EffectSpec{.type = "ultrakey", .name = "Ultra Key", .category = EffectCategory::Keying,
               .params = kUltraKey, .colors = kUltraKeyColors},
    EffectSpec{.type = "chromakey", .name = "Chroma Key", .category = EffectCategory::Keying,
               .params = kChromaKey, .colors = kChromaKeyColors},
    EffectSpec{.type = "trackmatte", .name = "Track Matte Key", .category = EffectCategory::Keying,
               .params = kTrackMatte},
    EffectSpec{.type = "transform", .name = "Transform",
               .category = EffectCategory::Transform, .params = kTransform},
    EffectSpec{.type = "warpstabilizer", .name = "Warp Stabilizer",
               .category = EffectCategory::Transform, .params = kWarpStabilizer},
    EffectSpec{.type = "crop", .name = "Crop", .category = EffectCategory::Transform,
               .params = kCrop},
    EffectSpec{.type = "flip", .name = "Flip", .category = EffectCategory::Transform,
               .params = kFlip},
    EffectSpec{.type = "distort", .name = "Lens Distortion",
               .category = EffectCategory::Transform, .params = kDistort},
    EffectSpec{.type = "vignette", .name = "Vignette", .category = EffectCategory::Stylize,
               .params = kVignette},
    EffectSpec{.type = "noise", .name = "Noise", .category = EffectCategory::Stylize,
               .params = kNoise},
    EffectSpec{.type = "dropshadow", .name = "Drop Shadow", .category = EffectCategory::Stylize,
               .params = kDropShadow, .colors = kDropShadowColors},
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
