#include "cutline/core/query.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace cutline::core {

bool is_generated_media(const Media& m) noexcept {
  return m.is_text || m.is_color || m.is_adjustment;
}

bool is_still_like(const Media& m) noexcept {
  return m.is_image || m.is_text || m.is_color || m.is_adjustment;
}

double clip_speed(const Clip& c) noexcept { return c.speed > 0.0 ? c.speed : 1.0; }

double source_span(const Clip& c) noexcept { return c.source_out - c.source_in; }

double clip_duration(const Clip& c) noexcept { return source_span(c) / clip_speed(c); }

double clip_end(const Clip& c) noexcept { return c.start + clip_duration(c); }

Handles source_handles(const Clip& c, double media_duration) noexcept {
  const double speed = clip_speed(c);
  const double before_in = c.source_in / speed;
  const double after_out = (media_duration - c.source_out) / speed;
  // Reverse swaps which physical handle feeds the head versus the tail edge.
  if (c.reverse) return {.head = after_out, .tail = before_in};
  return {.head = before_in, .tail = after_out};
}

Handles source_handles(const Project& p, const Clip& c) noexcept {
  const auto media = std::ranges::find(p.media, c.media_id, &Media::id);
  if (media == p.media.end()) return {};
  // A still has no source to run out of, so it can lend whatever is asked for.
  // Reporting zero would refuse a dissolve onto a title.
  if (is_still_like(*media)) {
    constexpr double unbounded = std::numeric_limits<double>::infinity();
    return Handles{.head = unbounded, .tail = unbounded};
  }
  return source_handles(c, media->duration);
}

double source_time_at(const Clip& c, double t) noexcept {
  const double local = (t - c.start) * clip_speed(c);
  return c.reverse ? c.source_out - local : c.source_in + local;
}

SourceRange clip_sub_source(const Clip& c, double a, double b) noexcept {
  const double speed = clip_speed(c);
  const double local_a = (a - c.start) * speed;
  const double local_b = (b - c.start) * speed;
  if (c.reverse) {
    return {.source_in = c.source_out - local_b, .source_out = c.source_out - local_a};
  }
  return {.source_in = c.source_in + local_a, .source_out = c.source_in + local_b};
}

double timeline_duration(const Project& p) noexcept {
  double max = 0.0;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) max = std::max(max, clip_end(c));
  }
  return max;
}

bool is_animated(const Clip& c, AnimProp prop) noexcept {
  return !c.keyframes[anim_prop_index(prop)].empty();
}

Transform animated_transform(const Clip& c, double local_t) noexcept {
  const Transform& base = c.transform;
  const auto value = [&](AnimProp prop, double fallback) {
    const std::vector<Keyframe>& kfs = c.keyframes[anim_prop_index(prop)];
    return kfs.empty() ? fallback : eval_keyframes(kfs, local_t);
  };
  return {
      .x = value(AnimProp::X, base.x),
      .y = value(AnimProp::Y, base.y),
      .scale_x = value(AnimProp::ScaleX, base.scale_x),
      .scale_y = value(AnimProp::ScaleY, base.scale_y),
      .rotation = value(AnimProp::Rotation, base.rotation),
      .anchor_x = value(AnimProp::AnchorX, base.anchor_x),
      .anchor_y = value(AnimProp::AnchorY, base.anchor_y),
  };
}

double animated_opacity(const Clip& c, double local_t) noexcept {
  if (!is_animated(c, AnimProp::Opacity)) return c.opacity;
  const double v = eval_keyframes(c.keyframes[anim_prop_index(AnimProp::Opacity)], local_t);
  return std::clamp(v, 0.0, 1.0);
}

double animated_value(const Clip& c, AnimProp prop, double local_t) noexcept {
  if (prop == AnimProp::Opacity) return animated_opacity(c, local_t);
  const Transform tr = animated_transform(c, local_t);
  switch (prop) {
    case AnimProp::X:
      return tr.x;
    case AnimProp::Y:
      return tr.y;
    case AnimProp::ScaleX:
      return tr.scale_x;
    case AnimProp::ScaleY:
      return tr.scale_y;
    case AnimProp::Rotation:
      return tr.rotation;
    case AnimProp::AnchorX:
      return tr.anchor_x;
    case AnimProp::AnchorY:
      return tr.anchor_y;
    case AnimProp::Opacity:
      break;
  }
  return tr.x;  // unreachable; Opacity is handled above
}

bool is_gain_animated(const Clip& c) noexcept { return !c.gain_keyframes.empty(); }

double gain_at(const Clip& c, double local_t) noexcept {
  if (!is_gain_animated(c)) return c.gain;
  return std::clamp(eval_keyframes(c.gain_keyframes, local_t), 0.0, kMaxGain);
}

bool is_track_audible(const Project& p, const Track& track) noexcept {
  if (track.muted) return false;
  const bool any_solo = std::ranges::any_of(p.tracks, [](const Track& t) {
    return t.kind == TrackKind::Audio && t.solo;
  });
  return !any_solo || track.solo;
}

const Clip* find_clip(const Project& p, std::string_view clip_id) noexcept {
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) {
      if (c.id == clip_id) return &c;
    }
  }
  return nullptr;
}

Clip* find_clip(Project& p, std::string_view clip_id) noexcept {
  return const_cast<Clip*>(find_clip(std::as_const(p), clip_id));
}

const Track* track_of_clip(const Project& p, std::string_view clip_id) noexcept {
  for (const Track& t : p.tracks) {
    if (std::ranges::any_of(t.clips, [&](const Clip& c) { return c.id == clip_id; })) {
      return &t;
    }
  }
  return nullptr;
}

Track* track_of_clip(Project& p, std::string_view clip_id) noexcept {
  return const_cast<Track*>(track_of_clip(std::as_const(p), clip_id));
}

std::vector<std::string> group_members(const Project& p, std::string_view clip_id) {
  const Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr) return {};
  if (!clip->group_id.has_value()) return {clip->id};

  std::vector<std::string> ids;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) {
      if (c.group_id == clip->group_id) ids.push_back(c.id);
    }
  }
  return ids;
}

const Clip* clip_at_time(const Track& track, double t) noexcept {
  for (const Clip& c : track.clips) {
    if (t >= c.start && t < clip_end(c)) return &c;
  }
  return nullptr;
}

}  // namespace cutline::core
