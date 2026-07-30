#pragma once

/// Turning a project into something the timeline can draw, and turning what
/// the timeline reports back into an edit.
///
/// Both halves already exist and neither knows about the other: `cutline::ui`
/// draws plain structs and `cutline::core` performs operations on a project.
/// This is the whole of the join, kept in one file so there is exactly one
/// place where a clip becomes a rectangle.

#include "cutline/core/model.hpp"
#include "cutline/ui/timeline.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cutline::editor {

/// What the timeline should draw for this project.
///
/// Track order follows the project's, which is video first and topmost first —
/// the same order the timeline stacks rows in, so V2 sits above V1 the way it
/// does everywhere else.
[[nodiscard]] ui::TimelineModel timeline_model(const core::Project& project,
                                               std::span<const std::string> selection = {});

/// The name a track shows when it has not been given one.
///
/// Numbered from the bottom for video and from the top for audio, which is
/// what every editor does: V1 is the base layer and A1 is the first audio
/// lane, and they meet in the middle.
[[nodiscard]] std::string default_track_label(const core::Project& project,
                                              std::size_t index);

/// Applies a gesture the timeline reported.
///
/// One function for all of them. The alternative is the caller switching on the
/// mode to decide which operation to call, which is the same switch one layer
/// further away from the operations it chooses between.
///
/// This is where the interface's units become the model's. A slip is dragged in
/// timeline seconds and stored in source seconds, and the two differ by the
/// clip's speed; a rate stretch and a trim are the same gesture and different
/// operations. Neither the timeline nor the core should have to know that.
///
/// Returns the project unchanged when the edit cannot apply — clamped against
/// a neighbour, or past the end of the source — which is what every core
/// operation does and what lets the caller skip the undo entry.
[[nodiscard]] core::Project apply_timeline_edit(core::Project project,
                                                std::string_view clip_id,
                                                const ui::TimelineEdit& edit);

/// Flips one of a track's header switches.
///
/// Takes the control rather than a value, and reads the current one out of the
/// project. The interface therefore never holds the truth about a switch and
/// cannot toggle from a stale copy of it — which is exactly what would happen
/// the first time two things changed a track between frames.
[[nodiscard]] core::Project toggle_track_switch(core::Project project,
                                                std::string_view track_id,
                                                ui::TrackControl control);

/// The clip a block refers to, or nothing when the model has moved on.
[[nodiscard]] std::optional<std::string> block_clip_id(const ui::TimelineModel& model,
                                                       ui::BlockRef ref);

}  // namespace cutline::editor
