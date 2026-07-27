#pragma once

/// Editing operations over the model.
///
/// Every operation takes a Project by value and returns the new one, which is
/// how "takes a project, returns a new project, never mutates" is spelled in
/// C++ — callers who no longer need the old project can `std::move` into the
/// call. Operations that cannot apply return the project unchanged rather than
/// signalling an error; the reference behaves the same way, and the UI relies
/// on it (a no-op edit simply does not push an undo entry).
///
/// Undo/redo is a stack of whole-project snapshots, held by the caller.

#include "cutline/core/model.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cutline::core {

// --------------------------------------------------------------- placement --

[[nodiscard]] Project add_media(Project p, Media media);

/// A trimmed source sub-range, in source seconds, for placing part of a media.
struct PlacementRange {
  double in = 0.0;
  double out = 0.0;
};

/// Places a media on the timeline at `start`: one video clip when the media has
/// video, plus one audio clip per source audio stream, all sharing a fresh group
/// so they stay linked.
///
/// Each audio stream lands on a *distinct* audio lane that is free across the
/// clip's span, and fresh lanes are created at the bottom when none is free.
/// Piling several streams — or several placements — onto one lane region was a
/// real bug; the distinct-and-free rule is the fix.
///
/// An empty `video_track_id` targets the topmost video track.
[[nodiscard]] Project place_media(Project p, std::string_view media_id, double start,
                                  std::string_view video_track_id = {},
                                  std::optional<PlacementRange> range = std::nullopt);

/// Timeline length a media occupies when placed with an optional source range.
/// Still-like media ignore the range, having no inherent time.
[[nodiscard]] double placed_length(const Media& media,
                                   std::optional<PlacementRange> range = std::nullopt) noexcept;

// ----------------------------------------------------------------- cutting --

/// Razor cut: splits every listed clip that strictly spans `time` into two
/// abutting clips. To cut linked clips together, expand the selection with
/// `group_members` first.
///
/// The right-hand pieces share one fresh group, so the segment after the cut
/// stays internally linked but is independent of the segment before it. Pieces
/// of an already-unlinked clip stay unlinked.
[[nodiscard]] Project split_at(Project p, double time, std::span<const std::string> clip_ids);

// --------------------------------------------------------------- arranging --

/// Moves clips by `delta_time` seconds and `delta_track_index` tracks within
/// their own kind. The whole set is clamped so no clip starts before zero.
///
/// `restrict_kind` limits vertical movement to one kind, so dragging a video
/// clip upward does not also drag its linked audio between audio lanes.
[[nodiscard]] Project move_clips(Project p, std::span<const std::string> clip_ids,
                                 double delta_time, int delta_track_index = 0,
                                 std::optional<TrackKind> restrict_kind = std::nullopt);

enum class ClipEdge { In, Out };

/// Trims a clip's in or out edge to a new timeline time, moving the whole linked
/// group together so video and its audio stay in sync.
///
/// The drag is clamped to the tightest limit across every group member: its
/// available source, the minimum clip length, the neighbouring clips, and the
/// start of the timeline.
[[nodiscard]] Project set_clip_edge(Project p, std::string_view clip_id, ClipEdge edge,
                                    double timeline_time);

// ---------------------------------------------------------------- deleting --

/// Removes the given clips, leaving a gap.
[[nodiscard]] Project remove_clips(Project p, std::span<const std::string> clip_ids);

/// Removes the given clips together with their linked groups and closes the
/// gap, pulling every later clip on *all* tracks left by the removed span so the
/// sequence stays in sync.
[[nodiscard]] Project ripple_delete(Project p, std::span<const std::string> clip_ids);

// ----------------------------------------------------------------- linking --

/// Links clips into one group so they move and cut together. Fewer than two
/// clips is a no-op.
[[nodiscard]] Project link_clips(Project p, std::span<const std::string> clip_ids);

/// Removes the given clips from whatever group they belong to.
[[nodiscard]] Project unlink_clips(Project p, std::span<const std::string> clip_ids);

/// Unlinks every member of the group containing `clip_id`.
[[nodiscard]] Project unlink_group(Project p, std::string_view clip_id);

}  // namespace cutline::core
