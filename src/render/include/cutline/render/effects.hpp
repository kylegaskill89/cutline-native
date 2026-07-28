#pragma once

/// Flattening a clip's effect stack into the handful of numbers a shader needs.
///
/// The model stores what the user chose — a list of effects keyed by type, with
/// parameters that may be animated. The shader wants a fixed set of scalars. In
/// between sits this: resolve the keyframes, look up each effect by its
/// registry key, convert from the units the UI uses into the units the maths
/// uses, and fold the result into one struct.
///
/// The FFmpeg fragments recorded in the spec are the *authoritative definition*
/// of each effect's behaviour, so the conversions here reproduce their argument
/// scaling exactly. They are no longer a code path — nothing shells out to
/// FFmpeg — but they remain the specification the shader implements.
///
/// Stacking two of the same effect multiplies or sums as the maths implies,
/// rather than the last one winning: the stack is an apply order, and the
/// reference chained the fragments the same way.

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

/// A clip's whole visual effect stack, resolved and flattened.
///
/// The neutral value of every field is "does nothing", so a default-constructed
/// instance is a clip with no effects.
struct EffectParams {
  /// `eq=brightness`, an offset applied to luma in -1..1.
  float brightness = 0.0f;
  /// `eq=contrast`, a multiplier about the midpoint. 1 is neutral.
  float contrast = 1.0f;
  /// `eq=saturation` multiplied by `hue=s` from Black & White. 1 is neutral.
  float saturation = 1.0f;
  /// `hue=h`, in degrees.
  float hue_degrees = 0.0f;

  /// `negate`. Toggled, so an even number of inverts cancels out.
  bool invert = false;

  /// `hflip` / `vflip`.
  bool flip_x = false;
  bool flip_y = false;

  /// `vignette=a`, the angle in radians. Zero is no vignette.
  float vignette = 0.0f;

  /// Fractions cut from each edge, 0..1. The frame keeps its size and the cut
  /// area becomes transparent.
  float crop_left = 0.0f;
  float crop_top = 0.0f;
  float crop_right = 0.0f;
  float crop_bottom = 0.0f;

  /// `chromakey`. Similarity and blend are fractions, not percentages.
  bool chroma_key = false;
  float chroma_similarity = 0.3f;
  float chroma_blend = 0.1f;
  EffectColor chroma_color = kDefaultKeyColor;

  /// `gblur=sigma`, in pixels. Not yet implemented by the compositor — a
  /// separable blur needs its own passes — so this is carried but unused.
  float blur_sigma = 0.0f;

  /// True when nothing in the stack does anything, letting the compositor skip
  /// the effect path entirely.
  [[nodiscard]] bool is_neutral() const noexcept;

  friend bool operator==(const EffectParams&, const EffectParams&) = default;
};

/// Resolves a clip's effect stack at clip-local `local_t`.
[[nodiscard]] EffectParams resolve_effect_params(const core::Clip& clip, double local_t);

}  // namespace cutline::render
