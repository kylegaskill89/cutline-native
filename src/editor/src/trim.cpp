#include "cutline/editor/trim.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/segments.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace cutline::editor {
namespace {

/// The track trim mode works on: Premiere's targeted track, picture first.
[[nodiscard]] const core::Track* trim_track(const core::Project& project) {
  const auto& tracks = project.sequence().tracks;
  for (const core::TrackKind kind : {core::TrackKind::Video, core::TrackKind::Audio}) {
    for (const core::Track& track : tracks) {
      if (track.kind == kind && track.targeted && !track.clips.empty()) return &track;
    }
  }
  for (const core::Track& track : tracks) {
    if (!track.clips.empty()) return &track;
  }
  return nullptr;
}

[[nodiscard]] bool has(const EditPoint& point, TrimType type) {
  switch (type) {
    case TrimType::Roll: return !point.outgoing.empty() && !point.incoming.empty();
    case TrimType::RippleOut:
    case TrimType::TrimOut: return !point.outgoing.empty();
    case TrimType::RippleIn:
    case TrimType::TrimIn: return !point.incoming.empty();
  }
  return false;
}

}  // namespace

std::optional<EditPoint> nearest_edit_point(const core::Project& project, double t) {
  const core::Track* track = trim_track(project);
  if (track == nullptr) return std::nullopt;

  // Every cut on the track: each clip's start and end, with whatever clip
  // meets it on the other side.
  std::optional<EditPoint> best;
  double distance = 0.0;
  const auto consider = [&](double at, std::string outgoing, std::string incoming) {
    const double away = std::abs(at - t);
    if (best.has_value() && away >= distance) return;
    best = EditPoint{.track_id = track->id,
                     .outgoing = std::move(outgoing),
                     .incoming = std::move(incoming),
                     .at = at};
    distance = away;
  };
  for (const core::Clip& clip : track->clips) {
    const double end = core::clip_end(clip);
    std::string after;
    std::string before;
    for (const core::Clip& other : track->clips) {
      if (std::abs(other.start - end) <= core::kTransitionEps) after = other.id;
      if (std::abs(core::clip_end(other) - clip.start) <= core::kTransitionEps) before = other.id;
    }
    consider(end, clip.id, after);
    if (before.empty()) consider(clip.start, {}, clip.id);
  }
  if (!best.has_value()) return std::nullopt;
  best->type = has(*best, TrimType::Roll)       ? TrimType::Roll
               : !best->outgoing.empty()        ? TrimType::TrimOut
                                                : TrimType::TrimIn;
  return best;
}

EditPoint next_trim_type(const EditPoint& point) {
  constexpr std::array kOrder{TrimType::Roll, TrimType::RippleOut, TrimType::RippleIn,
                              TrimType::TrimOut, TrimType::TrimIn};
  const auto at = std::ranges::find(kOrder, point.type);
  std::size_t index = static_cast<std::size_t>(at - kOrder.begin());
  EditPoint next = point;
  for (std::size_t step = 1; step <= kOrder.size(); ++step) {
    const TrimType candidate = kOrder[(index + step) % kOrder.size()];
    if (has(point, candidate)) {
      next.type = candidate;
      return next;
    }
  }
  return next;
}

bool trim_moves_cut(TrimType type) noexcept { return type != TrimType::RippleIn; }

Trimmed trim_edit_point(core::Project project, const EditPoint& point, double seconds) {
  const double to = point.at + seconds;
  switch (point.type) {
    case TrimType::Roll:
      project = core::roll_edit(std::move(project), point.outgoing, core::ClipEdge::Out, to);
      break;
    case TrimType::RippleOut:
      project = core::ripple_trim_edge(std::move(project), point.outgoing, core::ClipEdge::Out, to);
      break;
    case TrimType::RippleIn:
      project = core::ripple_trim_edge(std::move(project), point.incoming, core::ClipEdge::In, to);
      break;
    case TrimType::TrimOut:
      project = core::set_clip_edge(std::move(project), point.outgoing, core::ClipEdge::Out, to);
      break;
    case TrimType::TrimIn:
      project = core::set_clip_edge(std::move(project), point.incoming, core::ClipEdge::In, to);
      break;
  }
  EditPoint now = refresh_edit_point(project, point).value_or(point);
  return Trimmed{.project = std::move(project), .point = std::move(now)};
}

std::optional<EditPoint> refresh_edit_point(const core::Project& project,
                                            const EditPoint& point) {
  const core::Clip* outgoing =
      point.outgoing.empty() ? nullptr : core::find_clip(project, point.outgoing);
  const core::Clip* incoming =
      point.incoming.empty() ? nullptr : core::find_clip(project, point.incoming);
  if (outgoing == nullptr && incoming == nullptr) return std::nullopt;
  EditPoint now = point;
  // The side being trimmed says where the cut is; a roll keeps both together.
  const bool by_outgoing =
      outgoing != nullptr && point.type != TrimType::RippleIn && point.type != TrimType::TrimIn;
  if (!by_outgoing && incoming == nullptr) return std::nullopt;
  now.at = by_outgoing ? core::clip_end(*outgoing) : incoming->start;
  return now;
}

}  // namespace cutline::editor
