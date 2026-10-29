#pragma once

/// Premiere's subclips: a span of a source, filed in the project as an entry
/// of its own.
///
/// Made from the marks in the Source Monitor, from a clip in the timeline, or
/// by Scene Edit Detection, one per shot. See `Subclip` for what one is and
/// why it is a copy of its source rather than a reference to it.

#include "cutline/core/model.hpp"

#include <string>
#include <string_view>

namespace cutline::core {

/// Whether a subclip can be made of this entry: footage with time in it.
/// Stills, graphics, mattes, adjustment layers and sequences have no span of
/// their own to take part of.
[[nodiscard]] bool can_make_subclip(const Media& media) noexcept;

/// The name Premiere's Make Subclip dialog offers.
[[nodiscard]] std::string subclip_name(const Media& source);

/// Adds a subclip of `media_id` spanning `start` to `end` in source time,
/// named `name` and filed in `bin`, and gives its id through `made_id`.
///
/// A subclip of a subclip is a subclip of the same source, since the span is
/// in that source's time either way. The source's clip markers inside the
/// span come with it. Without Restrict Trims the span is where the subclip's
/// own In and Out start, which is how Premiere places a soft subclip. Nothing
/// changes for a span shorter than a frame or an entry that cannot have one.
[[nodiscard]] Project make_subclip(Project p, std::string_view media_id, std::string name,
                                   double start, double end, bool restrict_trims,
                                   std::string_view bin = {}, std::string* made_id = nullptr);

/// Premiere's Edit Subclip: a new span or restriction. Clips already cut from
/// it keep the frames they have.
[[nodiscard]] Project edit_subclip(Project p, std::string_view media_id, double start,
                                   double end, bool restrict_trims);

/// Edit Subclip's Convert to Master Clip: the entry stops being a subclip and
/// reaches the whole of its source.
[[nodiscard]] Project convert_to_master_clip(Project p, std::string_view media_id);

}  // namespace cutline::core
