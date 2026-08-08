#pragma once

/// The per-clip effect stacks: visual effects and audio effects.
///
/// Stack order is apply order. An effect's `type` keys into a registry that
/// lives outside the model — the model stores only what the user chose, never
/// how it renders.
///
/// Parameters are keyframeable. Resolving a stack at a moment in time folds
/// every animated parameter down to a plain value, which is what the renderer
/// consumes; it never has to know that animation exists.

#include "cutline/core/model.hpp"

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::core {

// ----------------------------------------------------------------- queries --

/// Whether an effect parameter is animated, meaning it has any keyframes.
[[nodiscard]] bool is_effect_param_animated(const ClipEffect& effect,
                                            std::string_view key) noexcept;

/// One parameter's value at clip-local `local_t` — animated, or the static
/// value, or zero when the parameter is absent.
/// The keys a mask's own numbers answer to.
///
/// A mask is animated through the *same* machinery as any other effect
/// parameter, by giving each of its numbers a reserved parameter name. That is
/// the whole of the feature: the stopwatch, the keyframe navigator, the curve
/// editor, the marks on the clip, copying a stack, saving a preset and the file
/// format all work on effect parameters, and none of them needed to learn what
/// a mask is.
///
/// The *value* still lives on the `Mask`, which stays the one home for it — the
/// monitor drags it, the file writes it, and a parameter map holding a second
/// copy would be two truths about one number. Only the keyframes live under
/// these names.
inline constexpr std::string_view kMaskParamPrefix = "mask.";

/// The mask number a reserved key names, or null when it is not one of them.
///
/// Returns a pointer-to-member so the one mapping from name to field is written
/// down once, and reading and writing cannot disagree about it.
[[nodiscard]] double Mask::* mask_param_field(std::string_view key) noexcept;

/// Every mask key, in the order a panel should show them.
[[nodiscard]] std::span<const std::string_view> mask_param_keys() noexcept;

[[nodiscard]] double effect_param_at(const ClipEffect& effect, std::string_view key,
                                     double local_t) noexcept;

/// The clip's effect stack with every animated parameter resolved to its value
/// at clip-local `local_t`.
[[nodiscard]] std::vector<ClipEffect> resolved_effects(const Clip& c, double local_t);

/// Whether any effect on the clip animates a parameter.
[[nodiscard]] bool clip_has_effect_keyframes(const Clip& c) noexcept;

/// Clip-local times of every effect keyframe on the clip, for drawing markers.
[[nodiscard]] std::vector<double> effect_keyframe_times(const Clip& c);

// ------------------------------------------------------------- stack edits --

[[nodiscard]] Project add_clip_effect(Project p, std::string_view clip_id, std::string type,
                                      std::map<std::string, double> params,
                                      std::map<std::string, std::string> colors = {});

[[nodiscard]] Project remove_clip_effect(Project p, std::string_view clip_id, std::size_t index);

/// Moves an effect within the stack by `direction`, -1 earlier or +1 later.
/// Order affects the render, so this is a real edit rather than presentation.
[[nodiscard]] Project move_clip_effect(Project p, std::string_view clip_id, std::size_t index,
                                       int direction);

/// Appends copies of `effects` to the clip's stack. With `clear_clip_effects`
/// in front of it, this is paste; on its own it is what merging two stacks
/// would be, which nothing offers yet.
[[nodiscard]] Project append_clip_effects(Project p, std::string_view clip_id,
                                          std::span<const ClipEffect> effects);

[[nodiscard]] Project clear_clip_effects(Project p, std::string_view clip_id);

/// Flips an effect's enabled flag; a disabled effect is kept but inert.
[[nodiscard]] Project toggle_clip_effect(Project p, std::string_view clip_id, std::size_t index);

/// Sets a parameter's static value. Animated parameters are edited through
/// `set_effect_keyframe` instead, and ignore this value while animated.
[[nodiscard]] Project set_clip_effect_param(Project p, std::string_view clip_id,
                                            std::size_t index, std::string key, double value);

[[nodiscard]] Project set_clip_effect_color(Project p, std::string_view clip_id,
                                            std::size_t index, std::string key,
                                            std::string value);

/// Sets where one effect applies. `MaskShape::None` takes the mask off again.
///
/// The whole mask at once rather than a field at a time: it is one shape, every
/// number in it means something only alongside the others, and a caller that
/// could move an edge without knowing the centre would be a caller that could
/// leave it inside out.
[[nodiscard]] Project set_effect_mask(Project p, std::string_view clip_id, std::size_t index,
                                      Mask mask);

// -------------------------------------------------- effect param keyframes --

[[nodiscard]] Project set_effect_keyframe(Project p, std::string_view clip_id, std::size_t index,
                                          std::string key, double local_t, double v);

[[nodiscard]] Interp effect_keyframe_interp_of(const ClipEffect& effect,
                                               std::string_view key) noexcept;

[[nodiscard]] Project set_effect_keyframe_interp(Project p, std::string_view clip_id,
                                                 std::size_t index, std::string_view key,
                                                 Interp mode);

[[nodiscard]] Project remove_effect_keyframe_at(Project p, std::string_view clip_id,
                                                std::size_t index, std::string_view key,
                                                double local_t);

[[nodiscard]] Project clear_effect_keyframes(Project p, std::string_view clip_id,
                                             std::size_t index, std::string_view key);

// ------------------------------------------------------------ audio stack --

[[nodiscard]] Project add_audio_effect(Project p, std::string_view clip_id, std::string type,
                                       std::map<std::string, double> params);

[[nodiscard]] Project remove_audio_effect(Project p, std::string_view clip_id, std::size_t index);

[[nodiscard]] Project toggle_audio_effect(Project p, std::string_view clip_id, std::size_t index);

[[nodiscard]] Project move_audio_effect(Project p, std::string_view clip_id, std::size_t index,
                                        int direction);

[[nodiscard]] Project set_audio_effect_param(Project p, std::string_view clip_id,
                                             std::size_t index, std::string key, double value);

// ------------------------------------------------------ the track's stack --
//
// The same five operations on a **track's** audio stack rather than a clip's.
// A clip's stack is what that take needed; a track's is what the whole stem
// needs — one compressor across all the dialogue rather than a different one on
// each line. They are separate functions rather than one taking a flag because
// a track and a clip are found by different means and the id spaces are
// different: passing a clip id to one of these should find nothing, and it
// does.

[[nodiscard]] Project add_track_audio_effect(Project p, std::string_view track_id,
                                             std::string type,
                                             std::map<std::string, double> params);

[[nodiscard]] Project remove_track_audio_effect(Project p, std::string_view track_id,
                                                std::size_t index);

[[nodiscard]] Project toggle_track_audio_effect(Project p, std::string_view track_id,
                                                std::size_t index);

[[nodiscard]] Project move_track_audio_effect(Project p, std::string_view track_id,
                                              std::size_t index, int direction);

[[nodiscard]] Project set_track_audio_effect_param(Project p, std::string_view track_id,
                                                   std::size_t index, std::string key,
                                                   double value);

[[nodiscard]] Project clear_track_audio_effects(Project p, std::string_view track_id);

// -------------------------------------------- audio effect param keyframes --
//
// The same four operations the visual stack has, on the same shape of map. They
// are separate functions rather than one templated pair because the two stacks
// are separate types in the model and a caller always knows which it means.

[[nodiscard]] Project set_audio_effect_keyframe(Project p, std::string_view clip_id,
                                                std::size_t index, std::string key,
                                                double local_t, double v);

[[nodiscard]] Interp audio_effect_keyframe_interp_of(const AudioClipEffect& effect,
                                                     std::string_view key) noexcept;

[[nodiscard]] Project set_audio_effect_keyframe_interp(Project p, std::string_view clip_id,
                                                       std::size_t index, std::string_view key,
                                                       Interp mode);

[[nodiscard]] Project remove_audio_effect_keyframe_at(Project p, std::string_view clip_id,
                                                      std::size_t index, std::string_view key,
                                                      double local_t);

[[nodiscard]] Project clear_audio_effect_keyframes(Project p, std::string_view clip_id,
                                                   std::size_t index, std::string_view key);

/// The audio counterparts of `append_clip_effects` and `clear_clip_effects`.
/// Both stacks are pasted by one gesture, so both need the same pair.
[[nodiscard]] Project append_audio_effects(Project p, std::string_view clip_id,
                                           std::span<const AudioClipEffect> effects);

[[nodiscard]] Project clear_audio_effects(Project p, std::string_view clip_id);

}  // namespace cutline::core
