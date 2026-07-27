#pragma once

/// Keyframe editing: transform and opacity animation, and volume automation.
/// Effect-parameter keyframes live in `effects.hpp`, next to the effect stack
/// they belong to.
///
/// Keyframe times are clip-local: seconds from the clip's start, not source
/// seconds. A clip whose start moves must move its keyframes with it.

#include "cutline/core/model.hpp"

#include <string_view>

namespace cutline::core {

// ------------------------------------------------- transform and opacity --

/// Sets, or replaces, a keyframe for a property at clip-local `local_t`.
[[nodiscard]] Project set_keyframe(Project p, std::string_view clip_id, AnimProp prop,
                                   double local_t, double v);

/// The interpolation mode a property's keyframes are using.
[[nodiscard]] Interp keyframe_interp_of(const Clip& c, AnimProp prop) noexcept;

/// Applies one interpolation mode across a property's keyframes.
[[nodiscard]] Project set_keyframe_interp(Project p, std::string_view clip_id, AnimProp prop,
                                          Interp mode);

/// Removes the keyframe near clip-local `local_t`.
[[nodiscard]] Project remove_keyframe_at(Project p, std::string_view clip_id, AnimProp prop,
                                         double local_t);

/// Removes every keyframe for a property, turning its animation off.
[[nodiscard]] Project clear_keyframes(Project p, std::string_view clip_id, AnimProp prop);

// ------------------------------------------------------- volume automation --

/// Sets, or replaces, a gain keyframe at clip-local `local_t`. The value is
/// clamped to the allowed gain range.
///
/// DIVERGENCE: the reference rebuilt the keyframe on every edit, discarding its
/// interpolation mode, which made eased volume automation unreachable even
/// though evaluation supported it. Here gain keyframes behave like every other
/// keyframe list.
[[nodiscard]] Project set_gain_keyframe(Project p, std::string_view clip_id, double local_t,
                                        double v);

[[nodiscard]] Interp gain_keyframe_interp_of(const Clip& c) noexcept;

[[nodiscard]] Project set_gain_keyframe_interp(Project p, std::string_view clip_id, Interp mode);

[[nodiscard]] Project remove_gain_keyframe_at(Project p, std::string_view clip_id, double local_t);

/// Moves a gain keyframe from `from_t` to `to_t`, used while dragging a point
/// on the volume rubber-band.
[[nodiscard]] Project move_gain_keyframe(Project p, std::string_view clip_id, double from_t,
                                         double to_t, double v);

/// Clears all volume automation, returning the clip to constant gain.
[[nodiscard]] Project clear_gain_keyframes(Project p, std::string_view clip_id);

}  // namespace cutline::core
