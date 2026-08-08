#pragma once

/// Keyframe editing: transform and opacity animation, and volume automation.
/// Effect-parameter keyframes live in `effects.hpp`, next to the effect stack
/// they belong to.
///
/// Keyframe times are clip-local: seconds from the clip's start, not source
/// seconds. A clip whose start moves must move its keyframes with it.

#include "cutline/core/model.hpp"

#include <span>
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

// ------------------------------------------------------ track automation --

/// Sets, or replaces, a keyframe on a track's fader or panner at **timeline**
/// `time`.
///
/// Timeline rather than track-local, because a track has no local time. Every
/// other function in this header takes clip-local seconds, so the odd one out
/// is named in its signature as well as here.
///
/// What the mixer's Write, Latch and Touch modes call as a pass runs: one
/// keyframe per fader position, laid down at the moment it was passing.
[[nodiscard]] Project set_track_gain_keyframe(Project p, std::string_view track_id, double time,
                                              double v);
[[nodiscard]] Project set_track_pan_keyframe(Project p, std::string_view track_id, double time,
                                             double v);

[[nodiscard]] Project remove_track_gain_keyframe_at(Project p, std::string_view track_id,
                                                    double time);

/// Throws the curve away and leaves the constant. What the Clear on an
/// automated fader does — and it deliberately does *not* touch the mode, since
/// clearing a pass to record another one is the ordinary reason to do it.
[[nodiscard]] Project clear_track_gain_keyframes(Project p, std::string_view track_id);
[[nodiscard]] Project clear_track_pan_keyframes(Project p, std::string_view track_id);

/// Sets what a track's fader does with its automation.
[[nodiscard]] Project set_track_automation(Project p, std::string_view track_id,
                                           AutomationMode mode);

/// Lays a recorded pass over a track's fader curve.
///
/// What Write, Latch and Touch commit when the sequence stops. The pass
/// **replaces** the curve across the span it covers and leaves the rest alone,
/// which is what punching in means: riding the fader through one passage should
/// not disturb what was set either side of it.
///
/// One edit rather than one per keyframe, and that is the whole reason this
/// exists as a function. A pass is a keyframe every displayed frame, so writing
/// them as they arrived would put sixty entries a second in the undo stack and
/// make taking a pass back a matter of holding Ctrl+Z down.
///
/// `pass` is expected in timeline order; an empty one changes nothing, which is
/// what a mode that was armed and never touched produces.
[[nodiscard]] Project write_track_gain_pass(Project p, std::string_view track_id,
                                            std::span<const Keyframe> pass);

/// The same three for the master fader, which has no id to name it by.
[[nodiscard]] Project set_master_automation(Project p, AutomationMode mode);
[[nodiscard]] Project clear_master_gain_keyframes(Project p);
[[nodiscard]] Project write_master_gain_pass(Project p, std::span<const Keyframe> pass);

}  // namespace cutline::core
