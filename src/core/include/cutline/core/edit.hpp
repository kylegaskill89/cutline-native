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
///
/// A cut is not an edge of the original clip, so nothing that belongs to an
/// edge may appear at it: the right piece's keyframes are rebased onto its new
/// origin, the fade-in and fade-out stay on the halves that own them, and the
/// out-transition follows the out-edge to the right piece. The reference
/// implementation copied all three verbatim, which shifted the animation of any
/// split clip and invented both a fade and a transition at every cut.
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

/// Splits anything spanning `at_time`, then opens a gap of `amount` seconds by
/// shifting everything from there onward to the right. Insert editing.
[[nodiscard]] Project ripple_insert(Project p, double at_time, double amount);

/// Insert-edit: ripples the sequence open and places the media in the gap.
[[nodiscard]] Project insert_media_at(Project p, std::string_view media_id, double at_time,
                                      std::string_view video_track_id = {},
                                      std::optional<PlacementRange> range = std::nullopt);

/// Overwrite-edit: carves out whatever occupies the span, then places the media
/// over it. Clips partly covered are trimmed; clips fully covered are dropped.
[[nodiscard]] Project overwrite_media_at(Project p, std::string_view media_id, double at_time,
                                         std::string_view video_track_id = {},
                                         std::optional<PlacementRange> range = std::nullopt);

/// Trims a clip's in or out edge to a new timeline time, moving the whole linked
/// group together so video and its audio stay in sync.
///
/// The drag is clamped to the tightest limit across every group member: its
/// available source, the minimum clip length, the neighbouring clips, and the
/// start of the timeline.
[[nodiscard]] Project set_clip_edge(Project p, std::string_view clip_id, ClipEdge edge,
                                    double timeline_time);

/// Rate stretch: dragging an edge changes the clip's *speed* rather than
/// trimming its source. The source in and out stay fixed and the speed becomes
/// `source_span / new_length`. Applied across the linked group.
///
/// Dragging the in-edge keeps the tail anchored, so the clip grows leftward.
[[nodiscard]] Project rate_stretch_edge(Project p, std::string_view clip_id, ClipEdge edge,
                                        double new_time);

/// Slip: shifts which part of the source a clip shows by `delta_source` source
/// seconds, without moving the clip or changing its length. Applied across the
/// linked group and clamped to every member's media bounds. Still-like media
/// have no source to slip and are left alone.
[[nodiscard]] Project slip_clip(Project p, std::string_view clip_id, double delta_source);

/// Slide: moves a clip in time, growing the abutting previous clip and
/// shrinking the next so the surrounding clips keep their positions and the
/// sequence length is unchanged. The slid clip's own source is untouched.
[[nodiscard]] Project slide_clip(Project p, std::string_view clip_id, double delta_time);

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
