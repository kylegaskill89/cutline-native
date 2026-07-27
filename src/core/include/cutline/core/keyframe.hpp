#pragma once

#include <span>

namespace cutline::core {

/// Interpolation applied *outgoing* from a keyframe, toward the next one.
enum class Interp {
  Linear,
  Hold,
  Ease,
};

/// A single animation breakpoint. `t` is clip-local seconds.
struct Keyframe {
  double t = 0.0;
  double v = 0.0;
  Interp e = Interp::Linear;

  friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

/// Maps a 0..1 progress fraction through a keyframe's outgoing interpolation.
/// Hold collapses to 0 so the "from" value persists until the next keyframe.
[[nodiscard]] double ease_fraction(double f, Interp mode) noexcept;

/// Interpolates a t-sorted keyframe list at `local_t`, honouring each
/// keyframe's outgoing interpolation mode. Clamped to the end values outside
/// the range; an empty list evaluates to 0.
[[nodiscard]] double eval_keyframes(std::span<const Keyframe> kfs, double local_t) noexcept;

}  // namespace cutline::core
