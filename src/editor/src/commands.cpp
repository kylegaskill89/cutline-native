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
  for (const core::Track& track : project.tracks) {
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
  for (const core::Track& track : project.tracks) {
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
  for (const core::Track& track : project.tracks) {
    for (const core::Clip& clip : track.clips) ids.push_back(clip.id);
  }
  return ids;
}

}  // namespace

std::string_view to_string(Command command) noexcept {
  switch (command) {
    case Command::Split: return "split";
    case Command::Delete: return "delete";
    case Command::RippleDelete: return "ripple_delete";
    case Command::NudgeLeft: return "nudge_left";
    case Command::NudgeRight: return "nudge_right";
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
      return !session.selection().empty();

    case Command::MarkIn:
      return can_mark(session.project(), session.project().in_point);
    case Command::MarkOut:
      return can_mark(session.project(), session.project().out_point);

    case Command::ClearMarks:
      return core::has_marks(session.project());

    case Command::AddMarker:
      // The same rule as the in and out points: something to mark, or a marker
      // to take away — dropping one where one already sits removes it.
      return core::timeline_duration(session.project()) > 0.0 ||
             !session.project().markers.empty();
    case Command::ClearMarkers:
      return !session.project().markers.empty();
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
  const double frame = core::frame_duration(project.fps);

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

    case Command::MarkIn: {
      if (!can_mark(project, project.in_point)) return false;
      // Already there, so this takes it away.
      const bool here = mark_is_here(project.in_point, session.playhead(), frame);
      return session.apply(core::set_in_point(
          project, here ? std::nullopt : std::optional<double>(session.playhead())));
    }

    case Command::MarkOut: {
      if (!can_mark(project, project.out_point)) return false;
      const bool here = mark_is_here(project.out_point, session.playhead(), frame);
      return session.apply(core::set_out_point(
          project, here ? std::nullopt : std::optional<double>(session.playhead())));
    }

    case Command::ClearMarks:
      return session.apply(core::clear_marks(project));

    case Command::AddMarker: {
      if (core::timeline_duration(project) <= 0.0 && project.markers.empty()) return false;
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
      if (project.markers.empty()) return false;
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
