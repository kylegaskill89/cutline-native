#pragma once

/// Premiere's trim mode: an edit point selected on the timeline, trimmed a
/// frame or five at a time from the keyboard or the Program Monitor's buttons,
/// by one of its three kinds of trim.
///
/// Pure: the trims themselves are the core's ripple, roll and edge trims, and
/// this keeps track of which cut is selected and where it has got to.

#include "cutline/core/model.hpp"

#include <optional>
#include <string>

namespace cutline::editor {

/// Which side of a cut a trim works on, and how.
enum class TrimType {
  /// Both sides: the cut moves and nothing else does. Premiere's red brackets
  /// either side.
  Roll,
  /// The outgoing clip's end, with everything after following. Yellow.
  RippleOut,
  /// The incoming clip's start, the same way.
  RippleIn,
  /// The outgoing clip's end alone, opening or closing a gap. Red.
  TrimOut,
  /// The incoming clip's start alone.
  TrimIn,
};

/// A selected edit point: a cut on one track, between the clip ending there and
/// the clip starting there. Either may be missing where a gap meets a clip,
/// though a roll needs both.
struct EditPoint {
  std::string track_id;
  std::string outgoing;
  std::string incoming;
  /// Where the cut is, in sequence seconds.
  double at = 0.0;
  TrimType type = TrimType::Roll;

  friend bool operator==(const EditPoint&, const EditPoint&) = default;
};

/// Premiere's Trim Edit: the edit point nearest `t` on the first targeted video
/// track, or the first targeted audio track when no video one is, or failing
/// both the topmost track with a cut. A roll where there are clips both sides,
/// and otherwise a trim of the side there is.
[[nodiscard]] std::optional<EditPoint> nearest_edit_point(const core::Project& project,
                                                          double t);

/// Premiere's Toggle Trim Type: roll, ripple out, ripple in, trim out, trim in,
/// and round again, passing over any the cut has no clip for.
[[nodiscard]] EditPoint next_trim_type(const EditPoint& point);

/// Whether a trim of this type moves the cut on the timeline. A ripple of the
/// incoming clip keeps the cut where it is and changes what comes after it.
[[nodiscard]] bool trim_moves_cut(TrimType type) noexcept;

/// The project with the edit point trimmed by `seconds`, positive later, and
/// the edit point where it now is. Clamped by the core as any trim is, so
/// asking for more than the media can give gives what it can.
struct Trimmed {
  core::Project project;
  EditPoint point;
};
[[nodiscard]] Trimmed trim_edit_point(core::Project project, const EditPoint& point,
                                      double seconds);

/// The edit point found again in `project`, after an undo or anything else
/// that moved its clips: none when its clips are gone.
[[nodiscard]] std::optional<EditPoint> refresh_edit_point(const core::Project& project,
                                                          const EditPoint& point);

}  // namespace cutline::editor
