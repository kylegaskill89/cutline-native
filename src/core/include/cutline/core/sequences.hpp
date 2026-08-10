#pragma once

/// Having more than one cut in a project: making them, naming them, switching
/// between them and throwing them away.
///
/// A project always has at least one, so there is no empty state to design for
/// and no reader that has to check. Removing the last one is refused rather
/// than allowed to leave nothing open, which is the only rule here that is not
/// obvious from the name of the function.
///
/// Everything takes a project by value and hands one back, like the rest of
/// core: one project in, one out, so a change is one entry in the undo stack
/// however many fields it touched.

#include "cutline/core/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace cutline::core {

/// Where a sequence sits in the project, or `npos` for one that is not in it.
[[nodiscard]] std::size_t sequence_index(const Project& p, std::string_view id) noexcept;

/// The sequence with this id, or null.
[[nodiscard]] const Sequence* find_sequence(const Project& p, std::string_view id) noexcept;

/// A name nothing else in the project is using, from `wanted`.
///
/// Premiere's rule: the first free "Sequence 01", and a name typed by hand that
/// clashes gets a number put after it rather than being refused. A tab strip
/// with two tabs reading the same thing is a worse answer than one reading
/// something slightly different from what was typed.
[[nodiscard]] std::string unused_sequence_name(const Project& p, std::string_view wanted);

/// Adds an empty sequence, taking the canvas and rate of the open one.
///
/// The new sequence is *not* opened: adding and switching are two decisions,
/// and a "new sequence from a clip" that opened it would take the person away
/// from what they were cutting without being asked. `open_sequence` is one
/// call away for callers that do want it.
[[nodiscard]] Project add_sequence(Project p, std::string name = {}, int video_tracks = 1,
                                   int audio_tracks = 2);

/// Adds a sequence holding one clip of `media_id`, shaped to that media.
///
/// Premiere's "New Sequence From Clip". The sequence takes the footage's own
/// size and rate — which is what `match_sequence_to` does for the first import,
/// and the same reasoning: a sequence made *from* a piece of footage has no
/// other sensible shape to be. Does nothing for a media that is not there.
[[nodiscard]] Project sequence_from_clip(Project p, std::string_view media_id);

/// Opens a sequence. Does nothing if there is no such sequence.
[[nodiscard]] Project open_sequence(Project p, std::string_view id);

/// Renames one. An empty or clashing name is made unique rather than refused.
[[nodiscard]] Project rename_sequence(Project p, std::string_view id, std::string name);

/// Removes one, and opens a neighbour if it was the one open.
///
/// **Refuses to remove the last sequence.** A project with none has nothing to
/// show in the timeline, nothing for the playhead to be in, and no shape for
/// the preview to render at; every reader would need a branch for it, and the
/// branch would be wrong somewhere. Premiere refuses for the same reason.
[[nodiscard]] Project remove_sequence(Project p, std::string_view id);

}  // namespace cutline::core
