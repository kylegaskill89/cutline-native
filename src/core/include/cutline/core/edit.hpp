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
#include <vector>

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
/// Each audio stream lands on the lane that **matches** the video's — V1's
/// sound on A1, V2's on A2 — with the streams after the first taking the lanes
/// below it, and fresh lanes made at the bottom when the project has too few.
///
/// Matching rather than searching for a lane that happens to be free, which is
/// what this used to do and which put a clip's picture and its sound on lanes
/// with nothing to do with each other: video is pushed onto its track whatever
/// is already there, so a second placement overlapping the first left two clips
/// on V1 and their sound spread across A1 and A2. Two rules for one placement
/// was one too many, and the video's is the one to keep.
///
/// Streams still take a lane each. Piling a stereo pair's two streams onto one
/// lane was the original bug and that half of the rule stands.
///
/// `track_id` is where to aim it. A video track takes the picture and the sound
/// follows onto the matching lanes; an **audio** track takes the sound, which is
/// the only way a source with no picture can be aimed anywhere but A1. Empty
/// means the topmost video track, which is what a placement with nothing to say
/// about where has always meant.
[[nodiscard]] Project place_media(Project p, std::string_view media_id, double start,
                                  std::string_view track_id = {},
                                  std::optional<PlacementRange> range = std::nullopt);

/// Timeline length a media occupies when placed with an optional source range.
/// Still-like media ignore the range, having no inherent time.
[[nodiscard]] double placed_length(const Media& media,
                                   std::optional<PlacementRange> range = std::nullopt) noexcept;

/// A source's marked span as a placement range, or nothing when neither end is
/// marked — which is what places the whole of it.
///
/// This is the join between the two halves of three-point editing. The marks
/// are kept on the media; the placement operations take a range; until there
/// was something to turn one into the other, every placement used the whole
/// source however it had been marked.
///
/// One end marked is still a range: the unmarked end is the start or the end of
/// the source.
[[nodiscard]] std::optional<PlacementRange> source_range(const Media& media) noexcept;

/// The same for a media named by id, and nothing when the project has no such
/// media — which is the form the edit commands want, having an id in hand.
[[nodiscard]] std::optional<PlacementRange> source_range(const Project& p,
                                                         std::string_view media_id) noexcept;

/// What the marks add up to: where an edit lands, and which part of the source
/// fills it.
///
/// Three-point editing is the rule that any three of the four marks — source
/// in, source out, sequence in, sequence out — determine the fourth. Until this
/// existed, only the source pair was read: insert and overwrite honoured a
/// marked source and then put it at the *playhead*, so marking where an edit
/// should land did nothing, and the mark was left doing the one job it is for
/// by hand.
struct EditPoints {
  /// Where on the timeline the source lands.
  double at = 0.0;
  /// Which part of the source, or nothing for the whole of it.
  std::optional<PlacementRange> source;
  /// Whether the sequence marks fix the *length* as well as the start.
  ///
  /// Only true when all four marks are set and the two spans disagree, which is
  /// four-point editing: the edit is over-determined and something has to give.
  /// Resolving it is a question rather than an arrangement — retime the source
  /// to fill, or trim it — so this reports the conflict and lets the caller
  /// decide rather than picking silently.
  bool over_determined = false;
  /// How long the destination is when the sequence marks fix it. Zero when they
  /// do not.
  double destination = 0.0;
};

/// Resolves the four marks against the playhead.
///
/// The rules, which are Premiere's:
///
/// - **Neither sequence mark.** The playhead is the in point, which is what
///   makes the common case — no marks at all — exactly what it was before.
/// - **Sequence in only.** The edit lands there.
/// - **Sequence out only.** The edit *back-times*: it is placed so that it
///   ends on the mark. "Get me out of this shot at exactly this moment" is a
///   thing people mark, and it is the half of three-point editing that cannot
///   be done by parking the playhead.
/// - **Both sequence marks.** The destination span is fixed. If the source is
///   marked at one end or neither, the missing end is *derived* from the span,
///   which is three-point editing proper. If the source is marked at both ends
///   and the two spans disagree, nothing can satisfy all four and
///   `over_determined` says so.
///
/// A still has no length of its own and so cannot be back-timed against one;
/// it takes the destination span when there is one and its own placement
/// length otherwise.
[[nodiscard]] EditPoints edit_points(const Project& p, std::string_view media_id,
                                     double playhead) noexcept;

/// Overwrites a span with a media retimed to fill it exactly.
///
/// Premiere's Fit to Fill, and the answer to four-point editing that keeps all
/// four marks rather than throwing one away: the source runs from its in to its
/// out, the edit runs from the sequence in to the sequence out, and the speed
/// is whatever makes those the same length. Ten seconds of action across six
/// seconds of sequence is a clip at 167%.
///
/// An overwrite rather than an insert, because the destination span is a span
/// somebody marked: rippling would move the mark's own meaning out from under
/// it.
///
/// Nothing happens when the destination has no length, when the source has
/// none, or when the media is not there. A still is retimed like anything else
/// — `clip_duration` is the source span over the speed whatever the media is,
/// so the arithmetic lands on the right length either way, and a rate on a
/// picture that shows one frame changes nothing else about it.
[[nodiscard]] Project fit_media_to(Project p, std::string_view media_id, double at,
                                   double duration, std::string_view track_id = {},
                                   std::optional<PlacementRange> range = std::nullopt);

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

/// Moves clips like `move_clips`, but when a video clip lands on a *different*
/// video track — setting up an overlay layer — its linked audio is relocated
/// onto lanes dedicated to that group.
///
/// A lane qualifies when everything already on it belongs to this group, so no
/// other layer's audio lives there; fresh lanes are created at the bottom when
/// none qualifies. Without this, stacking video layers silently piles their
/// audio onto the same lanes.
[[nodiscard]] Project move_clips_layered(Project p, std::span<const std::string> clip_ids,
                                         double delta_time, int delta_track_index = 0,
                                         TrackKind kind = TrackKind::Video);

/// Leaves the named clips where they are and moves *copies* of them by the
/// same amounts a move would. Premiere's alt-drag.
///
/// Built out of the move rather than out of the paste, so a duplicate lands
/// exactly where dragging the original there would have — including what it
/// does to the audio lanes when a video clip changes compositing layer. A paste
/// would put the copies back on the track they came from and need moving
/// afterwards, which is a second arrangement pass with its own opinions.
///
/// Fresh ids throughout, and groups are remapped together: a duplicated A/V
/// pair is linked to *itself*, or moving the original would drag the copy along
/// ever after.
///
/// `made` collects the new ids, so the caller can select the copies — which is
/// what makes the nudge or the drag that usually follows act on them.
///
/// Returns the project unchanged when nothing named exists.
[[nodiscard]] Project duplicate_clips(Project p, std::span<const std::string> clip_ids,
                                      double delta_time, int delta_track_index = 0,
                                      TrackKind kind = TrackKind::Video,
                                      std::vector<std::string>* made = nullptr);

/// Splits anything spanning `at_time`, then opens a gap of `amount` seconds by
/// shifting everything from there onward to the right. Insert editing.
[[nodiscard]] Project ripple_insert(Project p, double at_time, double amount);

/// Insert-edit: ripples the sequence open and places the media in the gap.
[[nodiscard]] Project insert_media_at(Project p, std::string_view media_id, double at_time,
                                      std::string_view track_id = {},
                                      std::optional<PlacementRange> range = std::nullopt);

/// Overwrite-edit: carves out whatever occupies the span, then places the media
/// over it. Clips partly covered are trimmed; clips fully covered are dropped.
/// The lanes it carves are exactly the ones the placement will fill.
[[nodiscard]] Project overwrite_media_at(Project p, std::string_view media_id, double at_time,
                                         std::string_view track_id = {},
                                         std::optional<PlacementRange> range = std::nullopt);

/// Trims a clip's in or out edge to a new timeline time, moving the whole linked
/// group together so video and its audio stay in sync.
///
/// The drag is clamped to the tightest limit across every group member: its
/// available source, the minimum clip length, the neighbouring clips, and the
/// start of the timeline.
[[nodiscard]] Project set_clip_edge(Project p, std::string_view clip_id, ClipEdge edge,
                                    double timeline_time);

/// Ripple trim: the same edge move, with everything downstream following.
///
/// The difference from `set_clip_edge` is what stops it. An ordinary trim is
/// bounded by the clip beside it and leaves a gap when it pulls away; a ripple
/// pushes that clip along instead, so the only limits left are the source
/// available and the shortest a clip may be — and the sequence gets shorter or
/// longer by exactly what was trimmed.
///
/// Everything on **every** track moves, so nothing that was in sync stops
/// being. Trimming a head keeps the clip where it is and pulls what follows
/// along; without that a ripple on the in-edge would leave a hole in front of
/// the very clip that was trimmed.
[[nodiscard]] Project ripple_trim_edge(Project p, std::string_view clip_id, ClipEdge edge,
                                       double timeline_time);

/// Rolling edit: moves a *join* — one clip's out-edge and its neighbour's
/// in-edge together — so the cut lands somewhere else and nothing else moves.
///
/// The one edit where the sequence's length is guaranteed unchanged: what one
/// clip gains the other loses. Bounded by the source available on both sides at
/// once, since either running out is what stops the join travelling.
///
/// Does nothing at an edge that is not a join: a gap or the end of the track is
/// not a cut, and there is nothing on the other side of it to roll into.
[[nodiscard]] Project roll_edit(Project p, std::string_view clip_id, ClipEdge edge,
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

// ------------------------------------------------------------ copy / paste --

/// One clip lifted off the timeline, and the lane it came from.
///
/// A copy by value, and the whole clip: its source range, its transform, its
/// keyframes, its effects, its fades. A clipboard holding ids would go stale the
/// moment what it named was trimmed, and undo would leave it pointing at
/// nothing.
///
/// The track is remembered by id rather than by index because a paste may
/// happen after tracks have been added or taken away, and "the second video
/// track" is a different lane by then. When the lane it names has gone, a paste
/// falls back to the first of its kind.
struct ClipCopy {
  Clip clip;
  std::string track_id;
};

/// Copies of the named clips, in track then start order.
///
/// The order matters: a paste puts the earliest of them at the paste point and
/// keeps the rest at their offsets from it, so what is copied keeps its shape.
[[nodiscard]] std::vector<ClipCopy> copy_clips(const Project& p,
                                               std::span<const std::string> clip_ids);

/// Puts copies back on the timeline, the earliest of them starting at
/// `at_time`.
///
/// **Overwrite**, which is what a paste is in every editor: whatever occupies
/// the span on each receiving lane is trimmed or dropped, exactly as dropping a
/// clip on top of another does. `paste_clips_insert` is the other one, which
/// ripples the sequence open instead.
///
/// Every clip gets a fresh id, and groups are remapped together — two linked
/// clips pasted stay linked to *each other* and not to the pair they came from.
/// Otherwise a pasted A/V pair would move whenever its original did.
///
/// Returns the project unchanged when nothing can land: an empty clipboard, or
/// one whose clips are all of a kind this project has no track for.
///
/// `pasted` collects the ids the copies were given, so a caller can select what
/// it has just put down — which is what makes a paste followed by a nudge or a
/// drag work on the new clips rather than on whatever was selected before.
[[nodiscard]] Project paste_clips(Project p, std::span<const ClipCopy> clips, double at_time,
                                  std::vector<std::string>* pasted = nullptr);

/// Paste-insert: opens a gap the length of what is being pasted, on every
/// track, and puts the copies in it. Premiere's Ctrl+Shift+V.
[[nodiscard]] Project paste_clips_insert(Project p, std::span<const ClipCopy> clips,
                                         double at_time,
                                         std::vector<std::string>* pasted = nullptr);

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
