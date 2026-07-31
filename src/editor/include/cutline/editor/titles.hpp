#pragma once

/// Creating and editing titles.
///
/// A title is a generated media: nothing on disk, a `TextSpec` in the project.
/// That makes it a source an editor creates rather than imports, and the reason
/// this is not part of `import.hpp` — there is no file, no probe, and no pool to
/// deduplicate against. Two titles with the same words are two titles.
///
/// The other generated media — colour mattes and adjustment layers — are in
/// `generators.hpp`, apart from these because text carries a whole
/// specification of its own and they carry almost nothing.
///
/// Editing is by whole spec rather than field by field. A caller reads the spec
/// it has, changes what it means to change, and sets it back; a setter per
/// property would be eleven functions that each have to be remembered when a
/// property is added.

#include "cutline/core/model.hpp"

#include <string>
#include <string_view>

namespace cutline::editor {

/// How long a new title lasts, in seconds.
///
/// A title has no inherent duration, so something has to choose. Five seconds
/// is long enough to read and short enough to trim rather than having to.
inline constexpr double kDefaultTitleLength = 5.0;

/// Adds a title to the project's media pool.
///
/// The id is reported through `id` when one is wanted, since a caller that has
/// just made a title almost always wants to place or select it.
[[nodiscard]] core::Project add_title(core::Project project, core::TextSpec spec,
                                      std::string* id = nullptr);

/// Adds a title and puts it on the timeline at `at`, on the topmost video track
/// unless one is named — which is where a title belongs: over the picture.
[[nodiscard]] core::Project add_title_at(core::Project project, core::TextSpec spec, double at,
                                         std::string_view video_track_id = {},
                                         std::string* clip_id = nullptr);

/// The text spec of a media, or null when that media is not a title.
[[nodiscard]] const core::TextSpec* title_spec(const core::Project& project,
                                               std::string_view media_id) noexcept;

/// The title a clip shows, or null when the clip is not a title's.
[[nodiscard]] const core::TextSpec* clip_title_spec(const core::Project& project,
                                                    std::string_view clip_id) noexcept;

/// Replaces a title's text and styling.
///
/// Returns the project unchanged when the media is not a title or the spec is
/// the one it already has, so the session can skip the undo entry.
[[nodiscard]] core::Project set_title_spec(core::Project project, std::string_view media_id,
                                           core::TextSpec spec);

/// What a new title says before anyone has typed anything.
[[nodiscard]] core::TextSpec default_title_spec();

}  // namespace cutline::editor
