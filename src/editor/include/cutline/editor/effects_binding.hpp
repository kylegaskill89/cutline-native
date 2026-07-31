#pragma once

/// What the Effect Controls panel shows for a clip's effect stack.
///
/// The same shape as `inspector.hpp`, and for the same reason: the core has
/// stack operations and the interface has sliders, and neither should learn
/// about the other. This describes a clip's effects as data, so the panel is a
/// loop over a list rather than a form that has to be remembered whenever an
/// effect is added.
///
/// Only the *description* lives here. Removing, reordering, toggling and
/// setting a parameter are already core operations taking a clip id and an
/// index, and the panel calls those directly. Adding is the exception: "add a
/// blur" means "add a blur with the values a new blur should have", and that
/// needs the catalogue.
///
/// Values are in the units the effect stores, which the registry defines as the
/// units a person reads — percent, degrees, pixels. Unlike the transform, there
/// is no conversion to do.

#include "cutline/core/model.hpp"
#include "cutline/render/effect_catalog.hpp"
#include "cutline/ui/controls.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// One numeric parameter of one effect on one clip.
struct EffectParamRow {
  /// As stored, and as the core operations take it.
  std::string key;
  std::string name;
  ui::ValueRange range;
  double value = 0.0;
  /// What a reset returns to; the value a newly added effect was given.
  double fallback = 0.0;
  std::string suffix;
  /// Worth a checkbox rather than a slider.
  bool toggle = false;
  /// Animated, so `value` is what the keyframes evaluate to rather than the
  /// stored one — which is ignored entirely while a parameter is animated.
  bool animated = false;
  /// Animated, and one of the keyframes is at the time asked about. What the
  /// keyframe marker draws as filled rather than hollow.
  bool keyed_here = false;
  /// The curve the whole parameter animates along. One per parameter rather
  /// than one per keyframe, as the reference had it. Meaningless unless
  /// `animated`.
  core::Interp interp = core::Interp::Linear;

  friend bool operator==(const EffectParamRow&, const EffectParamRow&) = default;
};

struct EffectColorRow {
  std::string key;
  std::string name;
  /// Hex, as stored. The catalogue's default when the effect has none set.
  std::string value;

  friend bool operator==(const EffectColorRow&, const EffectColorRow&) = default;
};

/// One effect in the stack, in stack order.
struct EffectRow {
  /// Position in the clip's stack, which is what every core operation takes.
  /// Not stable across a removal or a move, so it is read again after either.
  std::size_t index = 0;
  std::string type;
  /// The catalogue's name, or the raw type for an effect this build does not
  /// know — a project written by a newer version should still open and still
  /// show that something is there.
  std::string name;
  bool enabled = true;
  /// True when the type is not in the catalogue, so the panel can say so
  /// rather than drawing an effect with no parameters and no explanation.
  bool unknown = false;
  std::vector<EffectParamRow> params;
  std::vector<EffectColorRow> colors;

  friend bool operator==(const EffectRow&, const EffectRow&) = default;
};

/// A clip's visual effect stack, as it stands at clip-local time `local_t`.
///
/// The time is what an animated parameter is read at, so the panel shows what
/// the picture is actually doing at the playhead rather than a stored number
/// the keyframes are overriding.
///
/// Empty when the clip is not there, which is also what an empty selection
/// produces.
[[nodiscard]] std::vector<EffectRow> clip_effects(const core::Project& project,
                                                  std::string_view clip_id,
                                                  double local_t = 0.0);

/// An effect that can be added, for the menu that offers them.
struct EffectChoice {
  std::string type;
  std::string name;
  std::string category;

  friend bool operator==(const EffectChoice&, const EffectChoice&) = default;
};

[[nodiscard]] std::vector<EffectChoice> addable_effects();

/// Adds an effect with the values a new one should have.
///
/// Returns the project unchanged when the type is not in the catalogue or the
/// clip is not there, like everything else that edits, so the session can skip
/// the undo entry.
[[nodiscard]] core::Project add_effect(core::Project project, std::string_view clip_id,
                                       std::string_view type);

// -------------------------------------------------------------- keyframes --

/// Sets a parameter: a keyframe at `local_t` when it is animated, the stored
/// value when it is not.
///
/// One entry point rather than two, because which of them a drag means is not
/// the slider's business — it is a property of the parameter being dragged, and
/// asking every caller to check would eventually get it wrong somewhere.
[[nodiscard]] core::Project set_effect_parameter(core::Project project,
                                                 std::string_view clip_id, std::size_t index,
                                                 std::string_view key, double value,
                                                 double local_t = 0.0);

/// Turns animation on or off — Premiere's stopwatch.
///
/// On, the value the parameter has now becomes its first keyframe at `local_t`,
/// so switching it on changes nothing about the picture. Off, every keyframe is
/// dropped and the value at `local_t` is kept as the static one, so switching
/// it off does not either. Anything else loses work silently.
[[nodiscard]] core::Project set_effect_parameter_animated(core::Project project,
                                                          std::string_view clip_id,
                                                          std::size_t index,
                                                          std::string_view key, bool animated,
                                                          double local_t);

/// Adds a keyframe at `local_t` holding the current value, or removes the one
/// already there. Does nothing when the parameter is not animated: the first
/// keyframe is the stopwatch's job.
[[nodiscard]] core::Project toggle_effect_keyframe(core::Project project,
                                                   std::string_view clip_id, std::size_t index,
                                                   std::string_view key, double local_t);

/// Sets the curve the whole parameter animates along. Does nothing when it is
/// not animated: there are no keyframes to set it on.
[[nodiscard]] core::Project set_effect_parameter_interp(core::Project project,
                                                        std::string_view clip_id,
                                                        std::size_t index, std::string_view key,
                                                        core::Interp mode);

// ------------------------------------------------------------ audio stack --
//
// The same rows, built from the audio registry instead of the video catalogue,
// so a panel showing an audio clip's effects is the same loop over the same
// struct. Two differences, both in what is absent rather than what is here:
//
//   - **No colours.** Nothing in the audio registry takes one.
//   - **No keyframes.** `AudioClipEffect` holds parameters and nothing else, so
//     `animated` and `keyed_here` are always false and a panel must not offer a
//     stopwatch. Clip gain *is* automatable — that is a different property with
//     its own rubber band, not part of this stack.
//
// The registry already exists, in `cutline::audio`: each effect declares its
// parameters once with ranges, steps, defaults and units, exactly as the video
// catalogue does. There was nothing to write here but the join.

[[nodiscard]] std::vector<EffectRow> clip_audio_effects(const core::Project& project,
                                                        std::string_view clip_id);

[[nodiscard]] std::vector<EffectChoice> addable_audio_effects();

/// Adds an audio effect with the values a new one should have — every parameter
/// at its registry default, so a freshly added filter is the neutral one until
/// somebody moves a slider.
[[nodiscard]] core::Project add_audio_effect(core::Project project, std::string_view clip_id,
                                             std::string_view type);

}  // namespace cutline::editor
