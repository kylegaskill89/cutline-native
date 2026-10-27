#pragma once

/// What tracking a mask writes: a keyframe on every tracked frame, as
/// Premiere's tracker leaves one on every frame of its Mask Path.
///
/// Ours is the mask's position, and its rotation and size or path as far as
/// the tracking method follows them, because those are the numbers our mask
/// keeps. What tracking leaves is what any keyframes leave: a drag on the
/// picture afterwards changes the one at the playhead, and undo takes the
/// whole run back at once.

#include "cutline/core/mask_path.hpp"
#include "cutline/core/model.hpp"

#include <span>
#include <string_view>

namespace cutline::editor {

/// Keys `frames` onto the mask of effect `index`, as far as its tracking
/// method goes.
[[nodiscard]] core::Project apply_tracked_masks(core::Project project, std::string_view clip_id,
                                                std::size_t index,
                                                std::span<const core::TrackedMask> frames);

/// Sets the mask's tracking method, which is not a number and not keyframed.
[[nodiscard]] core::Project set_mask_tracking(core::Project project, std::string_view clip_id,
                                              std::size_t index, core::MaskTracking tracking);

/// Mask Path's stopwatch and keyframe button, at clip-local `t`.
[[nodiscard]] core::Project set_mask_path_animated(core::Project project,
                                                   std::string_view clip_id, std::size_t index,
                                                   bool animated, double t);
[[nodiscard]] core::Project toggle_mask_path_key(core::Project project, std::string_view clip_id,
                                                 std::size_t index, double t);

}  // namespace cutline::editor
