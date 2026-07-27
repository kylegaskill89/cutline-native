#pragma once

#include <span>
#include <vector>

namespace cutline::core {

/// An edit lands on an existing keyframe when it is within this of its time.
inline constexpr double kKeyframeMatchEps = 1e-4;

/// A delete removes any keyframe within this of the requested time. The looser
/// tolerance is inherited from the reference, where clicking to delete needs
/// more slack than dragging to place.
inline constexpr double kKeyframeRemoveEps = 1e-3;

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

// ------------------------------------------------------------ list editing --

/// Inserts a keyframe at `t`, or updates the one already there, keeping the
/// list sorted by time. An updated keyframe keeps its own interpolation; a new
/// one inherits the list's mode, so a property animated with "ease" stays eased
/// as points are added.
void upsert_keyframe(std::vector<Keyframe>& kfs, double t, double v);

/// Removes any keyframe within `kKeyframeRemoveEps` of `t`.
void remove_keyframe_near(std::vector<Keyframe>& kfs, double t);

/// The list's interpolation mode, taken from its first keyframe.
[[nodiscard]] Interp keyframe_list_interp(std::span<const Keyframe> kfs) noexcept;

/// Applies one interpolation mode across the whole list.
void set_keyframe_list_interp(std::vector<Keyframe>& kfs, Interp mode) noexcept;

}  // namespace cutline::core
