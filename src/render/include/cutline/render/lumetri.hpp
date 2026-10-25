#pragma once

/// Lumetri Color: Premiere's colour grade, as passes.
///
/// One effect with six sections, applied in Premiere's order: Basic Correction,
/// Creative, Curves, Color Wheels, HSL Secondary and Vignette. Everything that
/// works on one pixel at a time is one pass, carrying its settings, curve
/// tables and LUTs as data, because a grade has far more to say than the eight
/// values an ordinary pass holds. The parts that look at neighbours (Creative's
/// sharpen, the secondary's softened key, the vignette's place in the frame)
/// are passes of their own, so each runs where Premiere runs it.
///
/// Every section grades in the same space. In a Rec.709 sequence that is the
/// sequence's own coded values, which is what Premiere's controls are tuned
/// for. In an HDR one the light is first scaled so graphics white is 1.0 and
/// coded with the same curve, so a slider means the same thing in either, and
/// anything brighter than white carries on above 1 on a gentle log scale where
/// HDR Specular reaches it.
///
/// The data layout below is the shader's as well, and stated once here.

#include "cutline/core/model.hpp"
#include "cutline/render/effect_passes.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::render {

/// A colour lookup table, as a .cube file holds one.
struct Lut {
  std::string title;
  /// Points along each axis.
  int size = 0;
  /// A cube of `size`³ entries, or a curve per channel of `size` entries.
  bool three_d = true;
  std::array<float, 3> domain_min{0.0f, 0.0f, 0.0f};
  std::array<float, 3> domain_max{1.0f, 1.0f, 1.0f};
  /// Red, green and blue for each entry, red changing fastest, as the file
  /// lists them.
  std::vector<float> rgb;
};

/// Reads the text of a .cube file: Adobe's and Resolve's format, 1D or 3D.
[[nodiscard]] std::expected<Lut, std::string> parse_cube(std::string_view text);

/// Reads a .cube file, once: kept by path, and read again only when the file
/// changes on disk.
[[nodiscard]] std::expected<std::shared_ptr<const Lut>, std::string> load_lut(
    const std::filesystem::path& path);

/// How many samples a curve is reduced to for the shader.
inline constexpr std::size_t kCurveSamples = 256;

/// What a curve with no points is.
enum class CurveRest {
  /// Out equals in: the RGB curves and Sat vs Sat.
  Diagonal,
  /// A flat line at one half, meaning no change: the hue curves and Luma vs Sat.
  Middle,
};

/// A curve as `kCurveSamples` evenly spaced values from 0 to 1, drawn through
/// its points smoothly without overshooting between them. A periodic curve
/// wraps, as a hue curve does: what leaves the right comes back on the left.
[[nodiscard]] std::vector<float> curve_table(std::span<const core::CurvePoint> points,
                                             CurveRest rest, bool periodic);

/// The curves Lumetri has, by the key they are stored under, in the order
/// their tables are laid out.
inline constexpr std::array<std::string_view, 9> kLumetriCurveKeys{
    "rgb_master", "rgb_red", "rgb_green", "rgb_blue", "hue_sat",
    "hue_hue",    "hue_luma", "luma_sat", "sat_sat"};

// ----------------------------------------------------------------- layout --
//
// The grade pass. Its values: [0] which sections run, as the bits below, then
// where in its data the input LUT, the look LUT and the curve tables start,
// each -1 when there is none.

inline constexpr int kLumetriBasic = 1;
inline constexpr int kLumetriCreative = 2;
inline constexpr int kLumetriCurves = 4;
inline constexpr int kLumetriWheels = 8;

/// The grade's settings, at the start of its data.
enum LumetriSlot : std::size_t {
  kWhiteBalance = 0,   ///< three gains, in linear light
  kExposure = 3,       ///< a multiplier, in linear light
  kContrast = 4,       ///< the exponent of the S-curve about the middle
  kHighlights = 5,     ///< -1 to 1
  kShadows = 6,
  kWhites = 7,
  kBlacks = 8,
  kSaturation = 9,     ///< a multiplier
  kSpecular = 10,      ///< what lies above white is stretched by
  kLookIntensity = 11, ///< how much of the look, 0 to 2
  kFadedFilm = 12,     ///< 0 to 1
  kVibrance = 13,      ///< -1 to 1
  kCreativeSaturation = 14,
  kShadowTint = 15,    ///< an offset per channel
  kHighlightTint = 18,
  kTintBalance = 21,   ///< -1 to 1
  kLift = 22,          ///< the shadows wheel, an offset per channel
  kGamma = 25,         ///< the midtones wheel, an exponent change per channel
  kGain = 28,          ///< the highlights wheel, a gain change per channel
  kLumetriSettings = 32,
};

/// A LUT in the data: its size, whether it is a cube, its domain, and then
/// its entries.
inline constexpr std::size_t kLutHeader = 8;

/// The secondary's data.
enum SecondarySlot : std::size_t {
  kKeyHue = 0,         ///< centre, half width and softness, in turns
  kKeySaturation = 3,  ///< low, high and softness, 0 to 1
  kKeyLuma = 6,
  kDenoise = 9,        ///< in the layer's pixels
  kKeyBlur = 10,
  kKeyInvert = 11,
  kShowMask = 12,
  kSecondaryOffset = 13,   ///< the wheel, an offset per channel
  kSecondaryBalance = 16,  ///< temperature and tint, three gains
  kSecondaryContrast = 19,
  kSecondarySaturation = 20,
  kSecondarySharpen = 21,
  kSecondarySettings = 24,
};

/// A colour wheel's offset as a change per channel that leaves luma alone:
/// `x` and `y` from -1 to 1 across the wheel, laid out as a vectorscope is, so
/// the wheel and the scope agree about where each hue is.
[[nodiscard]] std::array<float, 3> wheel_offset(double x, double y) noexcept;

/// Lumetri's passes for one effect, appended to `passes`. A section left
/// neutral costs nothing, and an effect left wholly neutral adds no pass.
void plan_lumetri(const core::ClipEffect& effect, std::vector<EffectPass>& passes);

}  // namespace cutline::render
