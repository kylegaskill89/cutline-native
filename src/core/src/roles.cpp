#include "cutline/core/roles.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace cutline::core {
namespace {

/// A point on the curve being built, in clip-local seconds.
struct Point {
  double t = 0.0;
  double v = 1.0;
};

/// The curve's value at `t`, reading it as the piecewise-linear ramp it is.
/// Outside the ends it holds, which is what makes clamping a curve to a clip's
/// bounds a matter of evaluating it at 0 and at the end.
[[nodiscard]] double value_at(const std::vector<Point>& points, double t) noexcept {
  if (points.empty()) return 1.0;
  if (t <= points.front().t) return points.front().v;
  if (t >= points.back().t) return points.back().v;
  for (std::size_t i = 1; i < points.size(); ++i) {
    if (t > points[i].t) continue;
    const Point& a = points[i - 1];
    const Point& b = points[i];
    const double span = b.t - a.t;
    if (!(span > 0.0)) return b.v;
    return a.v + (b.v - a.v) * ((t - a.t) / span);
  }
  return points.back().v;
}

}  // namespace

std::string_view role_name(AudioRole role) noexcept {
  switch (role) {
    case AudioRole::Dialogue: return "Dialogue";
    case AudioRole::Music: return "Music";
    case AudioRole::Effects: return "SFX";
    case AudioRole::Ambience: return "Ambience";
    case AudioRole::None: break;
  }
  return "None";
}

Project set_clip_role(Project p, std::string_view clip_id, AudioRole role) {
  Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr || clip->kind != TrackKind::Audio) return p;
  clip->role = role;
  return p;
}

Project set_track_role(Project p, std::string_view track_id, AudioRole role) {
  for (Track& track : p.tracks) {
    if (track.id != track_id) continue;
    if (track.kind != TrackKind::Audio) return p;
    for (Clip& clip : track.clips) clip.role = role;
    return p;
  }
  return p;
}

std::vector<RoleSpan> role_spans(const Project& p, AudioRole role) {
  std::vector<RoleSpan> spans;
  if (role == AudioRole::None) return spans;

  for (const Track& track : p.tracks) {
    if (track.kind != TrackKind::Audio) continue;
    if (!is_track_audible(p, track)) continue;
    for (const Clip& clip : track.clips) {
      if (clip.role != role || clip.disabled) continue;
      const double length = clip_duration(clip);
      if (!(length > 0.0)) continue;
      spans.push_back(RoleSpan{.start = clip.start, .end = clip.start + length});
    }
  }

  std::ranges::sort(spans, [](const RoleSpan& a, const RoleSpan& b) { return a.start < b.start; });

  std::vector<RoleSpan> merged;
  for (const RoleSpan& span : spans) {
    // Touching counts as overlapping: two lines cut hard against each other are
    // one stretch of speech, and treating them as two would put a ramp back up
    // and straight back down in the join.
    if (!merged.empty() && span.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, span.end);
    } else {
      merged.push_back(span);
    }
  }
  return merged;
}

Project duck_clip(Project p, std::string_view clip_id, const DuckSettings& settings) {
  const Clip* found = find_clip(p, clip_id);
  if (found == nullptr || found->kind != TrackKind::Audio) return p;

  const double start = found->start;
  const double length = clip_duration(*found);
  if (!(length > 0.0)) return p;
  const double base = std::clamp(found->gain, 0.0, kMaxGain);
  const double ducked = std::clamp(base * std::pow(10.0, settings.amount_db / 20.0), 0.0, kMaxGain);
  const double fade = std::max(settings.fade, 0.0);

  // Everything the clip has to get out of the way of, shifted to where the
  // ramps belong and merged again at that width: if the music would come back
  // up and go straight down, it should stay down.
  std::vector<RoleSpan> ducks;
  for (const RoleSpan& span : role_spans(p, settings.against)) {
    RoleSpan shifted{.start = span.start + settings.position,
                     .end = std::max(span.end + settings.position, span.start + settings.position)};
    if (shifted.end < start || shifted.start > start + length) continue;
    if (!ducks.empty() && shifted.start <= ducks.back().end + 2.0 * fade) {
      ducks.back().end = std::max(ducks.back().end, shifted.end);
    } else {
      ducks.push_back(shifted);
    }
  }

  Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr) return p;
  if (ducks.empty()) {
    // Nothing to duck under. A flat curve at the clip's own gain says exactly
    // what no curve at all says, and the one that is not there is the one that
    // can still be edited by hand afterwards.
    clip->gain_keyframes.clear();
    return p;
  }

  std::vector<Point> points;
  for (const RoleSpan& duck : ducks) {
    const double down_from = duck.start - start;
    const double down_to = down_from + fade;
    // A duck shorter than its own ramp still has to reach the bottom, or a
    // one-word line would only dip halfway and come back.
    const double up_from = std::max(duck.end - start, down_to);
    const double up_to = up_from + fade;

    if (!points.empty() && points.back().t >= down_from) {
      // The previous duck's ramp back up runs into this one. Drop it: the
      // merge above should have prevented this, and arithmetic at the
      // boundaries can still produce it.
      points.pop_back();
    }
    points.push_back(Point{.t = down_from, .v = base});
    points.push_back(Point{.t = down_to, .v = ducked});
    points.push_back(Point{.t = up_from, .v = ducked});
    points.push_back(Point{.t = up_to, .v = base});
  }

  // Clamped to the clip rather than clipped off at it. A duck that begins
  // before the clip does has to arrive already down, which means evaluating the
  // curve at the boundary rather than dropping the keyframes outside it.
  std::vector<Keyframe> curve;
  curve.push_back(Keyframe{.t = 0.0, .v = value_at(points, 0.0)});
  for (const Point& point : points) {
    if (point.t <= 0.0 || point.t >= length) continue;
    curve.push_back(Keyframe{.t = point.t, .v = point.v});
  }
  curve.push_back(Keyframe{.t = length, .v = value_at(points, length)});

  clip->gain_keyframes = std::move(curve);
  return p;
}

Project duck_role(Project p, AudioRole role, const DuckSettings& settings) {
  if (role == AudioRole::None) return p;

  // Collected first, because ducking a clip rewrites the project and the loop
  // would otherwise be walking a copy of it that is one edit out of date.
  std::vector<std::string> ids;
  for (const Track& track : p.tracks) {
    if (track.kind != TrackKind::Audio) continue;
    for (const Clip& clip : track.clips) {
      if (clip.role == role && !clip.disabled) ids.push_back(clip.id);
    }
  }

  for (const std::string& id : ids) p = duck_clip(std::move(p), id, settings);
  return p;
}

}  // namespace cutline::core
