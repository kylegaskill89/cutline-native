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
    case Command::SelectAll: return "select_all";
    case Command::SelectNone: return "select_none";
    case Command::GoToStart: return "go_to_start";
    case Command::GoToEnd: return "go_to_end";
    case Command::PreviousFrame: return "previous_frame";
    case Command::NextFrame: return "next_frame";
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

    case Command::Undo:
      return session.undo();
    case Command::Redo:
      return session.redo();
  }
  return false;
}

}  // namespace cutline::editor
