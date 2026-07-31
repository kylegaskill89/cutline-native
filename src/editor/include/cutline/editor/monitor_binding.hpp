#pragma once

/// Between a clip's transform and the box the monitor draws handles on.
///
/// Two coordinate systems that look alike and are not. The model stores a
/// *scale*, relative to the media's aspect-fit size on the canvas — scale 1
/// means "as large as it goes without distortion", which is what keeps a
/// project's transforms independent of the resolution it is exported at. The
/// overlay works in canvas fractions, because a handle is a place on the frame
/// and knows nothing about what media is under it.
///
/// The conversion between them is one multiplication in each direction, and it
/// lives here because this is the only layer that knows both: `core` has no
/// idea what a widget is, and `ui` has never heard of a clip.

#include "cutline/core/model.hpp"
#include "cutline/ui/monitor.hpp"

#include <optional>
#include <string_view>

namespace cutline::editor {

/// The box the handles should be drawn on for `clip_id` at timeline time `t`,
/// with any keyframed transform evaluated.
///
/// Nothing when there is no such clip, when it is an audio clip, or when it
/// contributes no picture — an overlay on something invisible would be handles
/// for moving nothing.
[[nodiscard]] std::optional<ui::MonitorBox> monitor_box(const core::Project& project,
                                                        std::string_view clip_id, double t);

/// Applies a box the handles produced back to the clip.
///
/// Goes through the same parameter setter the inspector's rows use, so a
/// transform that is animated takes keyframes at the playhead and one that is
/// not takes a stored value. A drag on the monitor and a drag on a slider are
/// the same edit, and having them disagree about that would be a good way to
/// lose somebody's animation.
///
/// Returns the project unchanged when it cannot apply, like every other edit.
[[nodiscard]] core::Project apply_monitor_box(core::Project project, std::string_view clip_id,
                                              const ui::MonitorBox& box, double t);

}  // namespace cutline::editor
