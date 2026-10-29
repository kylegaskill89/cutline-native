#include "cutline/editor/transitions.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/segments.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace cutline::editor {
namespace {

/// The clip and the one abutting its out-edge, or nulls when there is no join.
///
/// Abutting is the model's own test — a transition between clips with a gap
/// between them is one the renderer will ignore, and offering it here would be
/// a control that does nothing.
struct Join {
  const core::Clip* out = nullptr;
  const core::Clip* in = nullptr;

  [[nodiscard]] bool valid() const noexcept { return out != nullptr && in != nullptr; }
};

[[nodiscard]] Join join_at(const core::Project& project, std::string_view clip_id) {
  const core::Track* track = core::track_of_clip(project, clip_id);
  if (track == nullptr) return {};

  // By time rather than by storage order: a track's clips are kept sorted, but
  // depending on that here would break quietly the first time they were not.
  std::vector<const core::Clip*> sorted;
  sorted.reserve(track->clips.size());
  for (const core::Clip& c : track->clips) sorted.push_back(&c);
  std::ranges::stable_sort(sorted, {}, [](const core::Clip* c) { return c->start; });

  const auto found = std::ranges::find(sorted, clip_id,
                                       [](const core::Clip* c) -> const std::string& {
                                         return c->id;
                                       });
  if (found == sorted.end()) return {};

  Join join{.out = *found};
  const auto next = std::next(found);
  if (next == sorted.end()) return join;
  if (std::abs((*next)->start - core::clip_end(*join.out)) > core::kTransitionEps) return join;

  join.in = *next;
  return join;
}

/// How much overlap the two clips can lend between them, in timeline seconds.
[[nodiscard]] double borrowable(const core::Project& project, const Join& join) {
  const double tail = core::source_handles(project, *join.out).tail;
  const double head = core::source_handles(project, *join.in).head;
  // Half the transition comes off each side, so the longest useful *half* is
  // the larger handle: past that, the smaller side simply stops contributing.
  return std::max(tail, head);
}

}  // namespace

std::string_view transition_name(core::TransitionKind kind) noexcept {
  switch (kind) {
    case core::TransitionKind::Dissolve: return "Cross Dissolve";
    case core::TransitionKind::DipBlack: return "Dip to Black";
    case core::TransitionKind::MorphCut: return "Morph Cut";
    case core::TransitionKind::Push: return "Push";
    case core::TransitionKind::Slide: return "Slide";
    case core::TransitionKind::ConstantPower: return "Constant Power";
    case core::TransitionKind::ConstantGain: return "Constant Gain";
    case core::TransitionKind::ExponentialFade: return "Exponential Fade";
  }
  return "Cross Dissolve";
}

std::span<const core::TransitionKind> transition_kinds() noexcept {
  static constexpr std::array kKinds{
      core::TransitionKind::Dissolve,      core::TransitionKind::DipBlack,
      core::TransitionKind::MorphCut,      core::TransitionKind::Push,
      core::TransitionKind::Slide,         core::TransitionKind::ConstantPower,
      core::TransitionKind::ConstantGain,  core::TransitionKind::ExponentialFade};
  return kKinds;
}

std::span<const core::TransitionKind> transition_kinds_for(core::TrackKind kind) noexcept {
  // Premiere's order in each folder, with its default first.
  const std::span<const core::TransitionKind> all = transition_kinds();
  return kind == core::TrackKind::Audio ? all.subspan(5) : all.first(5);
}

core::TransitionKind default_transition_kind(core::TrackKind kind) noexcept {
  return kind == core::TrackKind::Audio ? core::TransitionKind::ConstantPower
                                        : core::TransitionKind::Dissolve;
}

std::string_view transition_id(core::TransitionKind kind) noexcept {
  // The same words the document format writes, so an id that ends up in a file
  // or a preset means the same thing on both sides.
  switch (kind) {
    case core::TransitionKind::Dissolve: return "dissolve";
    case core::TransitionKind::DipBlack: return "dip-black";
    case core::TransitionKind::MorphCut: return "morph-cut";
    case core::TransitionKind::Push: return "push";
    case core::TransitionKind::Slide: return "slide";
    case core::TransitionKind::ConstantPower: return "constant-power";
    case core::TransitionKind::ConstantGain: return "constant-gain";
    case core::TransitionKind::ExponentialFade: return "exponential-fade";
  }
  return "dissolve";
}

std::optional<core::TransitionKind> transition_from_id(std::string_view id) noexcept {
  for (const core::TransitionKind kind : transition_kinds()) {
    if (transition_id(kind) == id) return kind;
  }
  return std::nullopt;
}

double longest_transition(const core::Project& project, std::string_view clip_id,
                          core::TransitionKind kind) {
  const Join join = join_at(project, clip_id);
  if (!join.valid()) return 0.0;
  // The picture's kinds on pictures and the sound's on sound.
  if (core::is_audio_transition(kind) != (join.out->kind == core::TrackKind::Audio)) return 0.0;

  // Neither half may swallow the clip it sits in, whichever kind this is.
  const double by_length =
      2.0 * std::min(core::clip_duration(*join.out), core::clip_duration(*join.in));

  // Dip to black is sequential rather than overlapping, so it borrows nothing
  // and the clips' own lengths are the only limit. It is the transition that
  // always works, and the reason a join with no handles is not a dead end.
  // Morph Cut holds whichever edge frame runs out, as Premiere does, so it
  // needs no handles either.
  if (kind == core::TransitionKind::DipBlack || kind == core::TransitionKind::MorphCut) {
    return by_length;
  }

  // A crossfade has no frame to hold where one side runs out, so both clips
  // must lend the whole half. A picture can hold its last frame, so there the
  // larger handle is the limit.
  if (core::is_audio_transition(kind)) {
    const double tail = core::source_handles(project, *join.out).tail;
    const double head = core::source_handles(project, *join.in).head;
    return std::min(by_length, 2.0 * std::min(tail, head));
  }

  const double by_handles = 2.0 * borrowable(project, join);
  return std::min(by_length, by_handles);
}

TransitionRow clip_transition(const core::Project& project, std::string_view clip_id) {
  const Join join = join_at(project, clip_id);
  if (join.out == nullptr) return {};

  TransitionRow row;
  row.joins = join.valid();
  if (join.out->transition_out.has_value()) {
    row.present = true;
    row.kind = join.out->transition_out->kind;
    row.duration = join.out->transition_out->duration;
  }
  if (!row.joins) return row;

  row.longest = longest_transition(project, clip_id, row.kind);
  row.handles_exhausted =
      longest_transition(project, clip_id, core::TransitionKind::Dissolve) <=
      core::kTransitionEps;
  return row;
}

core::Project set_transition(core::Project project, std::string_view clip_id,
                             std::optional<core::TransitionKind> kind, double duration) {
  if (!kind.has_value()) return core::set_clip_transition(std::move(project), clip_id, {});

  const double longest = longest_transition(project, clip_id, *kind);
  // A kind this join cannot manage at all clears rather than storing something
  // the renderer would ignore. The panel should not have offered it, and a
  // stored transition that draws nothing is worse than none.
  if (longest <= core::kTransitionEps) {
    return core::set_clip_transition(std::move(project), clip_id, {});
  }

  const double length = std::clamp(duration, 0.0, longest);
  return core::set_clip_transition(
      std::move(project), clip_id,
      core::Transition{.kind = *kind, .duration = length});
}

double default_transition_length(const core::Project& project, std::string_view clip_id,
                                 core::TransitionKind kind, double preferred) {
  return std::min(preferred, longest_transition(project, clip_id, kind));
}

namespace {

/// The track's default transition on a clip's out-edge, unless it has one.
[[nodiscard]] core::Project with_default(core::Project project, const core::Clip& clip,
                                         const TransitionLengths& lengths) {
  if (clip.transition_out.has_value()) return project;
  const core::TransitionKind kind = default_transition_kind(clip.kind);
  const double length =
      default_transition_length(project, clip.id, kind, lengths.for_kind(kind));
  if (length <= core::kTransitionEps) return project;
  const std::string id = clip.id;
  return set_transition(std::move(project), id, kind, length);
}

}  // namespace

core::Project apply_transition_at(core::Project project, double playhead, core::TrackKind kind,
                                  const TransitionLengths& lengths) {
  std::vector<std::string> outgoing;
  for (const core::Track& track : project.sequence().tracks) {
    if (track.kind != kind || !track.targeted) continue;
    // The cut nearest the playhead, among those with a clip on either side.
    const core::Clip* nearest = nullptr;
    double distance = 0.0;
    for (const core::Clip& clip : track.clips) {
      if (!join_at(project, clip.id).valid()) continue;
      const double away = std::abs(core::clip_end(clip) - playhead);
      if (nearest == nullptr || away < distance) {
        nearest = &clip;
        distance = away;
      }
    }
    if (nearest != nullptr) outgoing.push_back(nearest->id);
  }
  for (const std::string& id : outgoing) {
    if (const core::Clip* clip = core::find_clip(project, id); clip != nullptr) {
      const core::Clip copy = *clip;
      project = with_default(std::move(project), copy, lengths);
    }
  }
  return project;
}

core::Project apply_default_transitions(core::Project project,
                                        std::span<const std::string> clip_ids,
                                        const TransitionLengths& lengths) {
  for (const std::string& id : clip_ids) {
    const core::Clip* clip = core::find_clip(project, id);
    if (clip == nullptr) continue;
    const core::Clip copy = *clip;
    // Its out-edge, and the out-edge of whatever ends where it begins.
    project = with_default(std::move(project), copy, lengths);
    if (const core::Track* track = core::track_of_clip(project, id); track != nullptr) {
      for (const core::Clip& other : track->clips) {
        if (other.id == id) continue;
        if (std::abs(core::clip_end(other) - copy.start) > core::kTransitionEps) continue;
        const core::Clip before = other;
        project = with_default(std::move(project), before, lengths);
        break;
      }
    }
  }
  return project;
}

}  // namespace cutline::editor
