#pragma once

/// What the inspector shows for a clip, and what changing it does.
///
/// The same shape as the timeline binding, and for the same reason: the core
/// has property operations and the UI has sliders, and neither should learn
/// about the other. This describes a clip's editable properties as data, so the
/// panel is a loop over a list rather than a hand-built form that has to be
/// remembered whenever a property is added.
///
/// Values here are in **display** units, not model units — percent rather than
/// a fraction, degrees rather than radians. The conversion lives in one place
/// so a slider reading 100% and a clip storing 1.0 cannot drift apart.

#include "cutline/core/model.hpp"
#include "cutline/ui/controls.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// A clip property a control can edit.
enum class ClipParam {
  Opacity,
  X,
  Y,
  ScaleX,
  ScaleY,
  Rotation,
  AnchorX,
  AnchorY,
  Speed,
  Gain,
  Pan,
  FadeIn,
  FadeOut,
};

/// The stable identifier: lower case, underscored, for keys and for tests.
[[nodiscard]] std::string_view to_string(ClipParam param) noexcept;

/// What it is called on screen — "Position X" rather than `x`.
///
/// Its own function because two things now write it: a parameter row and a
/// keyframe lane. Two lists of the same ten strings would eventually disagree,
/// and the one nobody edited would be the one on screen.
[[nodiscard]] std::string_view param_name(ClipParam param) noexcept;

/// One row of the inspector.
struct ParamSpec {
  ClipParam param = ClipParam::Opacity;
  /// What to call it on screen.
  std::string name;
  ui::ValueRange range;
  /// Current value, in display units.
  double value = 0.0;
  /// What a double-click returns to, also in display units.
  double fallback = 0.0;
  /// Appended to the readout: "%", "°", "x", "s".
  std::string suffix;

  /// Whether keyframes are possible at all. Speed and the fades are not
  /// animatable — a fade whose length changed over its own duration is not
  /// something the model can express — so their rows get no stopwatch.
  bool animatable = false;
  /// Animated, so `value` is what the keyframes evaluate to rather than the
  /// stored one.
  bool animated = false;
  /// Animated, with a keyframe at the time asked about.
  bool keyed_here = false;
  /// How the animation gets from one keyframe to the next.
  ///
  /// One mode per property rather than per keyframe. The model stores it on
  /// each breakpoint, and the reference exposed one setting for the whole
  /// property — a panel offering a different curve out of every keyframe is a
  /// control nobody has asked for. Meaningless unless `animated`.
  core::Interp interp = core::Interp::Linear;

  friend bool operator==(const ParamSpec&, const ParamSpec&) = default;
};

/// The three curves, as a person reads them. Premiere's chip is the same three.
[[nodiscard]] std::string_view interp_name(core::Interp mode) noexcept;

/// The blend modes, in the order a control should offer them, and what each is
/// called on screen.
///
/// Premiere's own order and Premiere's own words: Normal first, then the modes
/// that lighten, then the ones that darken, then the odd one out. Alphabetical
/// would put Add above Normal, which is nobody's idea of a default.
[[nodiscard]] std::span<const core::BlendMode> blend_modes() noexcept;
[[nodiscard]] std::string_view blend_name(core::BlendMode mode) noexcept;

/// The next one round, for a chip that cycles rather than a dropdown of three.
[[nodiscard]] core::Interp next_interp(core::Interp mode) noexcept;

/// The parameters worth showing for a clip, in the order they should appear,
/// as they stand at clip-local time `local_t`.
///
/// A video clip gets its transform and opacity; an audio clip gets gain, and
/// none of the geometry that would mean nothing on it. Empty when the clip is
/// not there, which is also what an empty selection produces.
[[nodiscard]] std::vector<ParamSpec> clip_parameters(const core::Project& project,
                                                     std::string_view clip_id,
                                                     double local_t = 0.0);

/// Applies one parameter, taking a value in display units.
///
/// A keyframe at `local_t` when the parameter is animated, the stored value
/// when it is not. Which of those a drag means is a property of the parameter
/// rather than of the control, so it is decided here instead of by each caller.
///
/// Returns the project unchanged when it cannot apply, like everything else
/// that edits, so the session can skip the undo entry.
[[nodiscard]] core::Project set_clip_parameter(core::Project project, std::string_view clip_id,
                                               ClipParam param, double value,
                                               double local_t = 0.0);

/// Turns animation on or off — Premiere's stopwatch.
///
/// On, the value the parameter has now becomes its first keyframe at `local_t`,
/// so switching it on changes nothing about the picture. Off, every keyframe is
/// dropped and the value at `local_t` is kept as the static one, so switching
/// it off does not either.
[[nodiscard]] core::Project set_clip_parameter_animated(core::Project project,
                                                        std::string_view clip_id,
                                                        ClipParam param, bool animated,
                                                        double local_t);

/// Sets the curve the whole property animates along. Does nothing when the
/// parameter is not animated: there are no keyframes to set it on.
[[nodiscard]] core::Project set_clip_parameter_interp(core::Project project,
                                                      std::string_view clip_id, ClipParam param,
                                                      core::Interp mode);

/// Adds a keyframe at `local_t` holding the current value, or removes the one
/// already there. Does nothing when the parameter is not animated: the first
/// keyframe is the stopwatch's job.
[[nodiscard]] core::Project toggle_clip_parameter_keyframe(core::Project project,
                                                           std::string_view clip_id,
                                                           ClipParam param, double local_t);

}  // namespace cutline::editor
