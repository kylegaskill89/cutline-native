#pragma once

/// The editing commands, named and separated from whatever triggers them.
///
/// A keystroke, a menu item and a toolbar button should all be the same edit,
/// and the way to guarantee that is for none of them to contain one. They name
/// a command; this runs it.
///
/// It also gives menus something to ask: `can_run` is what greys an item out,
/// and having it next to the command rather than in the menu is what stops the
/// two disagreeing about when an edit is possible.

#include "cutline/editor/session.hpp"

#include <string>
#include <string_view>

namespace cutline::editor {

enum class Command {
  // -- editing
  /// Razor: splits the selection at the playhead, or everything under it when
  /// nothing is selected.
  Split,
  /// Lift: removes the selection and leaves the gap.
  Delete,
  /// Extract: removes the selection and closes the gap.
  RippleDelete,
  /// Moves the selection one frame earlier or later.
  NudgeLeft,
  NudgeRight,

  /// Trims the edit point *before* the playhead up to it, closing the gap:
  /// the clip under the playhead loses its head and the sequence comes with
  /// it. Premiere's Q.
  TrimPreviousToPlayhead,
  /// And the edit point *after*: the clip under the playhead loses its tail
  /// and everything past it comes back to meet it. Premiere's W.
  TrimNextToPlayhead,

  // -- the clipboard
  /// Takes a copy of the selection, whole clips and all.
  Copy,
  /// Copies, then lifts: the clips go and the gap stays, which is what Cut
  /// means everywhere and what keeps everything else where it was.
  Cut,
  /// Puts the clipboard down at the playhead, **overwriting** whatever is
  /// there — the same thing dropping a clip on top of another does.
  Paste,
  /// Opens a gap the length of the clipboard and puts it in there instead.
  PasteInsert,

  // -- three-point editing
  /// Puts the source at the playhead, rippling everything after it along to
  /// make room. Premiere's comma.
  Insert,
  /// Puts the source at the playhead over whatever is there. Premiere's full
  /// stop, and the one that leaves the sequence exactly as long as it was
  /// unless the source runs off the end of it.
  Overwrite,
  /// Four-point editing: the source fills the marked span exactly, retimed to
  /// whatever rate that takes. Offered only when all four marks are set and
  /// the two spans disagree — with three marks the fourth is derived and
  /// there is nothing to fit, and with four that agree it is an overwrite.
  FitToFill,

  // -- the marked span
  /// Marks the in point at the playhead — or clears it, when it is already
  /// there. The same key undoing itself is how a mark is removed without a
  /// third control that exists only to take one away.
  MarkIn,
  MarkOut,
  ClearMarks,

  // -- markers
  /// Drops a marker at the playhead, or takes away the one already there. The
  /// same toggle as the in and out points, for the same reason: one key, and no
  /// second control that exists only to undo it.
  AddMarker,
  ClearMarkers,
  /// Moves the playhead to the marker after or before it.
  NextMarker,
  PreviousMarker,

  // -- linking
  /// Ties the selected clips together so they move, trim and cut as one. What
  /// a video clip and the audio that came in with it usually want to be.
  LinkClips,
  /// Unties them. Separate commands rather than one that toggles, because a
  /// mixed selection — some linked, some not — has no honest answer to "which
  /// way is this toggling", and guessing it would silently unlink what somebody
  /// meant to gather up.
  UnlinkClips,

  // -- tracks
  /// A new empty track. Video goes on top, since that is the layer new overlay
  /// footage wants; audio goes at the bottom, where the next lane belongs.
  AddVideoTrack,
  AddAudioTrack,
  /// Removes the track the selection is on, and everything on it. Offered only
  /// when something is selected, because otherwise there is no answer to which
  /// track is meant — and deleting a track is not a thing to guess at.
  RemoveTrack,

  // -- selection
  SelectAll,
  SelectNone,

  // -- the playhead
  GoToStart,
  GoToEnd,
  PreviousFrame,
  NextFrame,

  // -- history
  Undo,
  Redo,
};

[[nodiscard]] std::string_view to_string(Command command) noexcept;

/// The track a keyboard edit lands on, or empty when nothing is targeted.
///
/// The first targeted *video* track, because that is what carries a picture and
/// what the audio lanes are matched against; a project with only audio targeted
/// answers with the first of those, which is how a source with no picture is
/// aimed.
///
/// Public because placing a source is not only reached through a `Command`.
/// Anything that puts media on the timeline has to aim at the same track a
/// keystroke would, and two answers to "which track" is how an edit lands
/// somewhere nobody was looking.
[[nodiscard]] std::string edit_target(const core::Project& project);

/// Whether the command would do anything right now. What greys a menu item.
[[nodiscard]] bool can_run(const Session& session, Command command);

/// Runs it, and reports whether anything changed.
///
/// A command that cannot apply is not an error — it simply does nothing, which
/// is the same contract the model's operations keep and what lets a keystroke
/// be pressed hopefully rather than checked first.
bool run(Session& session, Command command);

}  // namespace cutline::editor
