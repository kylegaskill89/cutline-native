#include "cutline/editor/commands.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/time.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace cutline::editor {
namespace {

/// Every clip the playhead is inside, which is what a razor cuts when nothing
/// is selected. Strictly inside: a cut exactly on an edge would produce a
/// zero-length piece, which is not a cut.
[[nodiscard]] std::vector<std::string> clips_under(const core::Project& project, double time) {
  std::vector<std::string> ids;
  for (const core::Track& track : project.sequence().tracks) {
    for (const core::Clip& clip : track.clips) {
      if (time > clip.start && time < core::clip_end(clip)) ids.push_back(clip.id);
    }
  }
  return ids;
}

/// What a razor should act on: the selection when there is one, everything
/// under the playhead when there is not.
[[nodiscard]] std::vector<std::string> razor_targets(const Session& session) {
  std::vector<std::string> ids = session.selected_group();
  if (!ids.empty()) {
    // Only the ones the playhead actually crosses. Cutting a selected clip
    // the playhead is nowhere near would be a surprise.
    const double at = session.playhead();
    std::erase_if(ids, [&](const std::string& id) {
      const core::Clip* clip = core::find_clip(session.project(), id);
      return clip == nullptr || at <= clip->start || at >= core::clip_end(*clip);
    });
    return ids;
  }
  return clips_under(session.project(), session.playhead());
}

/// The one clip a trim-to-playhead should act on, or nothing.
///
/// One clip rather than a list, and that is a deliberate limit. `ripple_trim_edge`
/// closes the sequence up by however much *this* clip lost, on every track; a
/// second clip on another lane crossing the same playhead has its own available
/// source and so its own answer, and applying both in turn would ripple twice
/// for one gesture. One edit point at a time is the honest version.
///
/// Which one: the first targeted track the playhead crosses, and any track it
/// crosses when nothing is targeted. Targeting is how the sequence is told which
/// lane an edit is aimed at, and this is exactly such an edit.
[[nodiscard]] std::string trim_target(const Session& session) {
  const core::Project& project = session.project();
  const double at = session.playhead();
  const bool any_targeted =
      std::ranges::any_of(project.sequence().tracks, [](const core::Track& t) { return t.targeted; });

  for (const core::Track& track : project.sequence().tracks) {
    if (any_targeted && !track.targeted) continue;
    for (const core::Clip& clip : track.clips) {
      if (at > clip.start && at < core::clip_end(clip)) return clip.id;
    }
  }
  return {};
}

/// Whether marking is worth offering: something to mark, or a mark to remove.
///
/// The second half matters. Pressing the key where the mark already is removes
/// it, so a sequence emptied out from under a mark must keep offering the key
/// that takes it away.
[[nodiscard]] bool can_mark(const core::Project& project, const std::optional<double>& mark) {
  return core::timeline_duration(project) > 0.0 || mark.has_value();
}

/// Whether the mark is already where the playhead is, to within half a frame.
/// Both came from frame-snapped times, so they agree to within rounding and
/// nothing else.
[[nodiscard]] bool mark_is_here(const std::optional<double>& mark, double playhead,
                                double frame) {
  return mark.has_value() && std::abs(*mark - playhead) < frame * 0.5;
}

/// The track a clip is on, or null.
[[nodiscard]] const core::Track* track_of(const core::Project& project,
                                          std::string_view clip_id) {
  for (const core::Track& track : project.sequence().tracks) {
    if (std::ranges::any_of(track.clips, [&](const core::Clip& c) { return c.id == clip_id; })) {
      return &track;
    }
  }
  return nullptr;
}

/// The track the selection is on, or null when nothing is selected — or when
/// the selection spans more than one, which has no single answer.
[[nodiscard]] const core::Track* selected_track(const Session& session) {
  const std::span<const std::string> selection = session.selection();
  if (selection.empty()) return nullptr;

  const core::Track* found = nullptr;
  for (const std::string& id : selection) {
    const core::Track* track = track_of(session.project(), id);
    if (track == nullptr) continue;
    if (found != nullptr && found != track) return nullptr;
    found = track;
  }
  return found;
}


/// Whether there is something to place and somewhere to put it.
[[nodiscard]] bool can_place(const Session& session) {
  if (session.source_media().empty()) return false;
  const std::vector<core::Media>& pool = session.project().media;
  if (std::ranges::find(pool, session.source_media(), &core::Media::id) == pool.end()) {
    return false;
  }
  // A sequence with nothing targeted has no answer to *where*, and guessing is
  // how an edit lands on a track nobody was looking at.
  return !edit_target(session.project()).empty();
}

/// Whether any of the selected clips belongs to a linked group. What decides
/// there is something to unlink.
[[nodiscard]] bool any_linked(const Session& session) {
  return std::ranges::any_of(session.selection(), [&](const std::string& id) {
    const core::Clip* clip = core::find_clip(session.project(), id);
    return clip != nullptr && clip->group_id.has_value();
  });
}

[[nodiscard]] std::vector<std::string> every_clip(const core::Project& project) {
  std::vector<std::string> ids;
  for (const core::Track& track : project.sequence().tracks) {
    for (const core::Clip& clip : track.clips) ids.push_back(clip.id);
  }
  return ids;
}

}  // namespace

// Reached from the application as well, which is why it is not file-local: the
// Fit Clip dialogue has to aim at the same track a keyboard edit would.
/// The track a keyboard edit lands on: the one targeted, or nothing.
///
/// The first targeted video track, because that is what carries a picture and
/// what the audio lanes are matched against. A project with only audio tracks
/// targeted answers with the first of those instead, which is how a source with
/// no picture is aimed.
[[nodiscard]] std::string edit_target(const core::Project& project) {
  for (const core::Track& track : project.sequence().tracks) {
    if (track.targeted && track.kind == core::TrackKind::Video) return track.id;
  }
  for (const core::Track& track : project.sequence().tracks) {
    if (track.targeted) return track.id;
  }
  return {};
}

std::string_view to_string(Command command) noexcept {
  switch (command) {
    case Command::Split: return "split";
    case Command::Delete: return "delete";
    case Command::RippleDelete: return "ripple_delete";
    case Command::NudgeLeft: return "nudge_left";
    case Command::NudgeRight: return "nudge_right";
    case Command::TrimPreviousToPlayhead: return "trim_previous_to_playhead";
    case Command::TrimNextToPlayhead: return "trim_next_to_playhead";
    case Command::Insert: return "insert";
    case Command::Overwrite: return "overwrite";
    case Command::FitToFill: return "fit_to_fill";
    case Command::Copy: return "copy";
    case Command::Cut: return "cut";
    case Command::Paste: return "paste";
    case Command::PasteInsert: return "paste_insert";
    case Command::MarkIn: return "mark_in";
    case Command::MarkOut: return "mark_out";
    case Command::ClearMarks: return "clear_marks";
    case Command::AddMarker: return "add_marker";
    case Command::ClearMarkers: return "clear_markers";
    case Command::NextMarker: return "next_marker";
    case Command::PreviousMarker: return "previous_marker";
    case Command::SelectAll: return "select_all";
    case Command::SelectNone: return "select_none";
    case Command::GoToStart: return "go_to_start";
    case Command::GoToEnd: return "go_to_end";
    case Command::PreviousFrame: return "previous_frame";
    case Command::NextFrame: return "next_frame";
    case Command::LinkClips: return "link_clips";
    case Command::UnlinkClips: return "unlink_clips";
    case Command::AddVideoTrack: return "add_video_track";
    case Command::AddAudioTrack: return "add_audio_track";
    case Command::RemoveTrack: return "remove_track";
    case Command::Undo: return "undo";
    case Command::Redo: return "redo";
  }
  return "unknown";
}

bool can_run(const Session& session, Command command) {
  switch (command) {
    case Command::Split:
      return !razor_targets(session).empty();

    case Command::Delete:
    case Command::RippleDelete:
    case Command::NudgeLeft:
    case Command::NudgeRight:
    case Command::SelectNone:
    case Command::Copy:
    case Command::Cut:
      return !session.selection().empty();

    case Command::TrimPreviousToPlayhead:
    case Command::TrimNextToPlayhead:
      return !trim_target(session).empty();

    case Command::Paste:
    case Command::PasteInsert:
      return !session.clipboard().empty();

    case Command::MarkIn:
      return can_mark(session.project(), session.project().sequence().in_point);
    case Command::MarkOut:
      return can_mark(session.project(), session.project().sequence().out_point);

    case Command::Insert:
    case Command::Overwrite:
      return can_place(session);

    // Only when there is a conflict to resolve. With three marks the fourth is
    // derived and an overwrite already lands it exactly; offering to "fit" a
    // source that already fits would be a command that retimes by 100%.
    case Command::FitToFill:
      return can_place(session) &&
             core::edit_points(session.project(), session.source_media(), session.playhead())
                 .over_determined;

    case Command::ClearMarks:
      return core::has_marks(session.project());

    case Command::AddMarker:
      // The same rule as the in and out points: something to mark, or a marker
      // to take away — dropping one where one already sits removes it.
      return core::timeline_duration(session.project()) > 0.0 ||
             !session.project().sequence().markers.empty();
    case Command::ClearMarkers:
      return !session.project().sequence().markers.empty();
    case Command::NextMarker:
      return core::next_marker(session.project(), session.playhead()) != nullptr;
    case Command::PreviousMarker:
      return core::previous_marker(session.project(), session.playhead()) != nullptr;

    case Command::SelectAll:
      return !every_clip(session.project()).empty();

    case Command::GoToStart:
    case Command::PreviousFrame:
      return session.playhead() > 0.0;

    case Command::GoToEnd:
    case Command::NextFrame:
      // There is always somewhere further to go; a timeline does not end at
      // its last clip, and the playhead is what says where the next one goes.
      return true;

    // Two clips at least, or there is nothing to tie together.
    case Command::LinkClips:
      return session.selection().size() >= 2;
    case Command::UnlinkClips:
      return any_linked(session);

    // Always: a sequence can always take another lane.
    case Command::AddVideoTrack:
    case Command::AddAudioTrack:
      return true;
    case Command::RemoveTrack:
      return selected_track(session) != nullptr;

    case Command::Undo:
      return session.can_undo();
    case Command::Redo:
      return session.can_redo();
  }
  return false;
}

bool run(Session& session, Command command) {
  const core::Project& project = session.project();
  const double frame = core::frame_duration(project.sequence().fps);

  switch (command) {
    case Command::Split: {
      const std::vector<std::string> targets = razor_targets(session);
      if (targets.empty()) return false;
      return session.apply(core::split_at(project, session.playhead(), targets));
    }

    case Command::Delete:
      return session.apply(core::remove_clips(project, session.selected_group()));

    case Command::RippleDelete:
      return session.apply(core::ripple_delete(project, session.selected_group()));

    case Command::NudgeLeft:
      return session.apply(core::move_clips(project, session.selected_group(), -frame));
    case Command::NudgeRight:
      return session.apply(core::move_clips(project, session.selected_group(), frame));

    case Command::TrimPreviousToPlayhead:
    case Command::TrimNextToPlayhead: {
      const std::string target = trim_target(session);
      if (target.empty()) return false;
      // The head for the previous edit point and the tail for the next, which
      // is the same statement read from either side: what goes is the material
      // between the playhead and the cut, and the sequence closes over it.
      const core::ClipEdge edge = command == Command::TrimPreviousToPlayhead
                                      ? core::ClipEdge::In
                                      : core::ClipEdge::Out;
      return session.apply(
          core::ripple_trim_edge(project, target, edge, session.playhead()));
    }

    case Command::Copy: {
      // The whole linked group, like every other edit: copying a shot has to
      // bring the sound that came in with it, or a paste is silent.
      std::vector<core::ClipCopy> copied =
          core::copy_clips(project, session.selected_group());
      if (copied.empty()) return false;
      session.set_clipboard(std::move(copied));
      return true;
    }

    case Command::Cut: {
      const std::vector<std::string> targets = session.selected_group();
      std::vector<core::ClipCopy> copied = core::copy_clips(project, targets);
      if (copied.empty()) return false;
      session.set_clipboard(std::move(copied));
      // Lifted rather than extracted, which is what Cut means: the gap stays
      // and nothing downstream moves. Extracting is Ripple Delete and has its
      // own key.
      return session.apply(core::remove_clips(project, targets));
    }

    case Command::Paste:
    case Command::PasteInsert: {
      // What was put down becomes the selection, which is what every editor
      // does and what makes a paste followed by a nudge or a drag act on the
      // copies rather than on whatever was highlighted before.
      std::vector<std::string> pasted;
      const bool changed = session.apply(
          command == Command::Paste
              ? core::paste_clips(project, session.clipboard(), session.playhead(), &pasted)
              : core::paste_clips_insert(project, session.clipboard(), session.playhead(),
                                         &pasted));
      if (changed) session.select(std::move(pasted));
      return changed;
    }

    case Command::MarkIn: {
      if (!can_mark(project, project.sequence().in_point)) return false;
      // Already there, so this takes it away.
      const bool here = mark_is_here(project.sequence().in_point, session.playhead(), frame);
      return session.apply(core::set_in_point(
          project, here ? std::nullopt : std::optional<double>(session.playhead())));
    }

    case Command::MarkOut: {
      if (!can_mark(project, project.sequence().out_point)) return false;
      const bool here = mark_is_here(project.sequence().out_point, session.playhead(), frame);
      return session.apply(core::set_out_point(
          project, here ? std::nullopt : std::optional<double>(session.playhead())));
    }

    // `match_sequence_to` first, and inside the same `apply`, so the sequence
    // taking the footage's shape and the footage landing on it are one edit and
    // one undo entry. It does nothing unless the sequence is still empty.
    // Both go through `edit_points`, which is what makes a sequence mark mean
    // anything: with neither mark set it answers "the playhead", so the common
    // case is exactly what it was, and with them set the edit lands where it
    // was told to rather than where the playhead happens to be parked.
    case Command::Insert: {
      if (!can_place(session)) return false;
      core::Project ready = core::match_sequence_to(project, session.source_media());
      const std::string target = edit_target(ready);
      const auto points = core::edit_points(ready, session.source_media(), session.playhead());
      return session.apply(core::insert_media_at(std::move(ready), session.source_media(),
                                                 points.at, target, points.source));
    }

    case Command::Overwrite: {
      if (!can_place(session)) return false;
      core::Project ready = core::match_sequence_to(project, session.source_media());
      const std::string target = edit_target(ready);
      const auto points = core::edit_points(ready, session.source_media(), session.playhead());
      return session.apply(core::overwrite_media_at(std::move(ready), session.source_media(),
                                                    points.at, target, points.source));
    }

    case Command::FitToFill: {
      if (!can_run(session, Command::FitToFill)) return false;
      core::Project ready = core::match_sequence_to(project, session.source_media());
      const std::string target = edit_target(ready);
      const auto points = core::edit_points(ready, session.source_media(), session.playhead());
      return session.apply(core::fit_media_to(std::move(ready), session.source_media(), points.at,
                                              points.destination, target, points.source));
    }

    case Command::ClearMarks:
      return session.apply(core::clear_marks(project));

    case Command::AddMarker: {
      if (core::timeline_duration(project) <= 0.0 && project.sequence().markers.empty()) return false;
      // Within half a frame, like the in and out points: both times came from
      // frame-snapped values, so they agree to within rounding and nothing else.
      if (const core::Marker* here =
              core::marker_near(project, session.playhead(), frame * 0.5);
          here != nullptr) {
        return session.apply(core::remove_marker(project, here->id));
      }
      return session.apply(core::add_marker(project, session.playhead()));
    }

    case Command::ClearMarkers:
      if (project.sequence().markers.empty()) return false;
      return session.apply(core::clear_markers(project));

    case Command::NextMarker: {
      const core::Marker* next = core::next_marker(project, session.playhead());
      if (next == nullptr) return false;
      session.set_playhead(next->time);
      return true;
    }

    case Command::PreviousMarker: {
      const core::Marker* previous = core::previous_marker(project, session.playhead());
      if (previous == nullptr) return false;
      session.set_playhead(previous->time);
      return true;
    }

    case Command::SelectAll: {
      std::vector<std::string> all = every_clip(project);
      if (all.empty()) return false;
      session.select(std::move(all));
      return true;
    }

    case Command::SelectNone: {
      if (session.selection().empty()) return false;
      session.clear_selection();
      return true;
    }

    case Command::GoToStart: {
      if (session.playhead() <= 0.0) return false;
      session.set_playhead(0.0);
      return true;
    }

    case Command::GoToEnd: {
      const double end = core::timeline_duration(project);
      if (session.playhead() == end) return false;
      session.set_playhead(end);
      return true;
    }

    case Command::PreviousFrame: {
      if (session.playhead() <= 0.0) return false;
      session.set_playhead(session.playhead() - frame);
      return true;
    }
    case Command::NextFrame:
      session.set_playhead(session.playhead() + frame);
      return true;

    case Command::LinkClips: {
      if (!can_run(session, command)) return false;
      const std::vector<std::string> ids(session.selection().begin(),
                                         session.selection().end());
      return session.apply(core::link_clips(session.project(), ids));
    }

    case Command::UnlinkClips: {
      if (!can_run(session, command)) return false;
      // The whole group, not only what is selected. Unlinking one clip out of
      // three would leave the other two tied to each other, which is not what
      // anybody means by taking a link off.
      const std::vector<std::string> ids = session.selected_group();
      return session.apply(core::unlink_clips(session.project(), ids));
    }

    case Command::AddVideoTrack:
      return session.apply(core::add_video_track(session.project()));
    case Command::AddAudioTrack:
      return session.apply(core::add_audio_track(session.project()));

    case Command::RemoveTrack: {
      const core::Track* track = selected_track(session);
      if (track == nullptr) return false;
      // The id by value: `remove_track` is handed the project this points into,
      // and the vector it walks is rewritten underneath it.
      const std::string id = track->id;
      return session.apply(core::remove_track(session.project(), id));
    }

    case Command::Undo:
      return session.undo();
    case Command::Redo:
      return session.redo();
  }
  return false;
}

}  // namespace cutline::editor
