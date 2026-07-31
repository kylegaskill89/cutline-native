#pragma once

/// What the interface needs to know about a transition, and how to set one.
///
/// The model already renders all four kinds — the segment resolver borrows
/// handles either side of the cut and hands the compositor alpha ramps or slide
/// windows. None of that was reachable, and this is the join.
///
/// The one thing worth a layer of its own is **whether a transition would do
/// anything**. A dissolve, push or slide overlaps the two clips, and overlapping
/// means borrowing unused source from each side: a clip trimmed to the last
/// frame of its footage has none to lend, and a cross-dissolve there resolves to
/// nothing at all. Offering a control that silently does nothing is worse than
/// not offering it, so the panel asks first.
///
/// Dip to black is the exception and the useful fallback: it fades out and then
/// in, sequentially, with no overlap and therefore no handles needed. It always
/// works.

#include "cutline/core/model.hpp"

#include <optional>
#include <string_view>

namespace cutline::editor {

/// The name a person reads. Premiere's, because these are Premiere's
/// transitions and calling a cross dissolve something else helps nobody.
[[nodiscard]] std::string_view transition_name(core::TransitionKind kind) noexcept;

/// A clip's out-edge transition, as the panel shows it.
struct TransitionRow {
  /// Another clip abuts this one's out-edge, so a transition means something.
  /// False at the end of a track, where the model would keep a transition and
  /// the renderer would ignore it.
  bool joins = false;

  bool present = false;
  core::TransitionKind kind = core::TransitionKind::Dissolve;
  double duration = 0.0;

  /// The longest transition of `kind` this join can actually manage, which is
  /// what bounds the slider. Zero means the kind would do nothing here.
  double longest = 0.0;

  /// True when the overlapping kinds have no handles to borrow, so only dip to
  /// black is left. What the panel says out loud rather than leaving somebody
  /// to drag a slider that changes nothing.
  bool handles_exhausted = false;

  friend bool operator==(const TransitionRow&, const TransitionRow&) = default;
};

[[nodiscard]] TransitionRow clip_transition(const core::Project& project,
                                            std::string_view clip_id);

/// The longest transition of a kind this join supports.
///
/// Bounded by two different things. Every kind is bounded by the clips
/// themselves — half the transition sits either side of the cut, and neither
/// half may swallow its clip. The overlapping kinds are bounded again by the
/// handles, since half of one is only useful as far as there is source to
/// borrow. Zero means "not possible here".
[[nodiscard]] double longest_transition(const core::Project& project,
                                        std::string_view clip_id,
                                        core::TransitionKind kind);

/// Sets or clears the transition, clamped to what the join supports.
///
/// Clamped rather than refused: dragging a slider past what the footage allows
/// should stop at the longest that works, the way a trim stops at the end of
/// its source, instead of snapping back to nothing.
[[nodiscard]] core::Project set_transition(core::Project project, std::string_view clip_id,
                                           std::optional<core::TransitionKind> kind,
                                           double duration);

/// How long a transition should be when one is first added: a second, or as
/// much of one as the join can manage.
[[nodiscard]] double default_transition_length(const core::Project& project,
                                               std::string_view clip_id,
                                               core::TransitionKind kind);

}  // namespace cutline::editor
