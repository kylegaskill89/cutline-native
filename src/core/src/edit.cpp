#include "cutline/core/edit.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cutline::core {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

/// Clips within a track are kept sorted by start time.
void sort_track(Track& track) {
  std::ranges::stable_sort(track.clips, {}, &Clip::start);
}

void sort_all_tracks(Project& p) {
  for (Track& t : p.tracks) sort_track(t);
}

[[nodiscard]] std::unordered_set<std::string> id_set(std::span<const std::string> ids) {
  return {ids.begin(), ids.end()};
}

/// Indices into `p.tracks` of every track of the given kind, in storage order.
[[nodiscard]] std::vector<std::size_t> track_indices_of_kind(const Project& p, TrackKind kind) {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < p.tracks.size(); ++i) {
    if (p.tracks[i].kind == kind) out.push_back(i);
  }
  return out;
}

/// The media's usable source length for trimming. Still-like media have no
/// source limit, so their edges extend freely.
[[nodiscard]] double trim_limit_duration(const Project& p, std::string_view media_id) noexcept {
  const auto it = std::ranges::find(p.media, media_id, &Media::id);
  if (it == p.media.end()) return kInfinity;
  return is_still_like(*it) ? kInfinity : it->duration;
}

/// Shifts keyframe times so they stay anchored to the same timeline moment
/// after a clip's start moves.
///
/// Keyframe `t` is measured from the clip's start, so a clip whose origin moves
/// must move its animation with it. Out-of-range keyframes are kept rather than
/// dropped: evaluation clamps outside the keyframe range, so keeping them is
/// what preserves the value at the new edge.
///
/// The reference implementation did not do this. Splitting an animated clip
/// there left the right-hand piece's keyframes at their original offsets, so
/// its animation jumped by the length of the left-hand piece.
void rebase_keyframes(std::vector<Keyframe>& kfs, double shift) noexcept {
  for (Keyframe& k : kfs) k.t -= shift;
}

/// Rebases every keyframe list a clip owns.
void rebase_all_keyframes(Clip& c, double shift) noexcept {
  for (std::vector<Keyframe>& kfs : c.keyframes) rebase_keyframes(kfs, shift);
  rebase_keyframes(c.gain_keyframes, shift);
  for (ClipEffect& effect : c.effects) {
    for (auto& [key, kfs] : effect.keyframes) rebase_keyframes(kfs, shift);
  }
}

/// Moves a clip's head to `new_start` after its leading part was cut away.
/// The animation travels with the new origin, and the fade-in goes: it was
/// authored against a head that no longer exists.
void recut_head(Clip& c, double new_start) noexcept {
  rebase_all_keyframes(c, new_start - c.start);
  c.start = new_start;
  c.fade_in = 0.0;
}

/// Drops what belonged to a tail that was cut away. The out-transition goes
/// with it, since it described a hand-off to a clip this one no longer meets.
void recut_tail(Clip& c) noexcept {
  c.fade_out = 0.0;
  c.transition_out.reset();
}

/// Removes the clip with `clip_id` from `track` and returns it, or nullopt.
[[nodiscard]] std::optional<Clip> extract_clip(Track& track, std::string_view clip_id) {
  const auto it = std::ranges::find(track.clips, clip_id, &Clip::id);
  if (it == track.clips.end()) return std::nullopt;
  Clip taken = std::move(*it);
  track.clips.erase(it);
  return taken;
}

}  // namespace

// --------------------------------------------------------------- placement --

Project add_media(Project p, Media media) {
  p.media.push_back(std::move(media));
  return p;
}

double placed_length(const Media& media, std::optional<PlacementRange> range) noexcept {
  if (range.has_value() && !is_still_like(media)) {
    return std::max(0.0, std::min(media.duration, range->out) - std::max(0.0, range->in));
  }
  return media.duration;
}

Project place_media(Project p, std::string_view media_id, double start,
                    std::string_view video_track_id, std::optional<PlacementRange> range) {
  const auto media_it = std::ranges::find(p.media, media_id, &Media::id);
  if (media_it == p.media.end()) return p;
  const Media media = *media_it;  // copied: p.media may not reallocate, but tracks change

  const std::string group_id = new_id("grp");

  // Still-like media have no inherent time, so they ignore any source range.
  const bool honour_range = range.has_value() && !is_still_like(media);
  const double source_in = honour_range ? std::max(0.0, range->in) : 0.0;
  const double source_out = honour_range ? std::min(media.duration, range->out) : media.duration;

  const std::vector<std::size_t> video_tracks = track_indices_of_kind(p, TrackKind::Video);
  if (media.has_video && !video_tracks.empty()) {
    std::size_t target = video_tracks.front();
    if (!video_track_id.empty()) {
      for (const std::size_t i : video_tracks) {
        if (p.tracks[i].id == video_track_id) {
          target = i;
          break;
        }
      }
    }
    Clip clip;
    clip.id = new_id("clip");
    clip.media_id = media.id;
    clip.kind = TrackKind::Video;
    clip.source_in = source_in;
    clip.source_out = source_out;
    clip.start = start;
    clip.group_id = group_id;
    p.tracks[target].clips.push_back(std::move(clip));
    sort_track(p.tracks[target]);
  }

  // Each audio stream goes to a distinct lane that is free across the clip's
  // span, creating fresh lanes at the bottom when none is free.
  const double audio_end = start + (source_out - source_in);
  std::vector<std::size_t> audio_tracks = track_indices_of_kind(p, TrackKind::Audio);
  std::unordered_set<std::string> used_lanes;

  const auto lane_free = [&](const Track& t) {
    return std::ranges::none_of(t.clips, [&](const Clip& c) {
      return start < clip_end(c) && c.start < audio_end;
    });
  };

  for (int stream = 0; stream < media.audio_stream_count; ++stream) {
    std::optional<std::size_t> target;
    for (const std::size_t i : audio_tracks) {
      if (!used_lanes.contains(p.tracks[i].id) && lane_free(p.tracks[i])) {
        target = i;
        break;
      }
    }
    if (!target.has_value()) {
      Track lane;
      lane.id = new_id("track");
      lane.kind = TrackKind::Audio;
      p.tracks.push_back(std::move(lane));
      target = p.tracks.size() - 1;
      audio_tracks.push_back(*target);
    }
    used_lanes.insert(p.tracks[*target].id);

    Clip clip;
    clip.id = new_id("clip");
    clip.media_id = media.id;
    clip.kind = TrackKind::Audio;
    clip.audio_stream = stream;
    clip.source_in = source_in;
    clip.source_out = source_out;
    clip.start = start;
    clip.group_id = group_id;
    p.tracks[*target].clips.push_back(std::move(clip));
    sort_track(p.tracks[*target]);
  }

  return p;
}

// ----------------------------------------------------------------- cutting --

Project split_at(Project p, double time, std::span<const std::string> clip_ids) {
  const std::unordered_set<std::string> ids = id_set(clip_ids);
  const std::string right_group_id = new_id("grp");

  for (Track& track : p.tracks) {
    std::vector<Clip> added;
    const std::size_t original_count = track.clips.size();
    for (std::size_t i = 0; i < original_count; ++i) {
      Clip& c = track.clips[i];
      if (!ids.contains(c.id)) continue;
      if (time <= c.start || time >= clip_end(c)) continue;  // does not span the cut

      // Both halves are derived from the clip before it shrinks.
      const SourceRange left = clip_sub_source(c, c.start, time);
      const SourceRange right = clip_sub_source(c, time, clip_end(c));

      // DIVERGENCE from the reference, which inherited the clip wholesale and
      // so mangled three things at every cut. See `rebase_keyframes`.
      Clip right_clip = c;
      right_clip.id = new_id("clip");
      right_clip.source_in = right.source_in;
      right_clip.source_out = right.source_out;
      if (c.group_id.has_value()) right_clip.group_id = right_group_id;

      // recut_head sets the new start itself, and needs the old one to work out
      // how far the animation has to move.
      recut_head(right_clip, time);
      recut_tail(c);

      c.source_in = left.source_in;
      c.source_out = left.source_out;
      added.push_back(std::move(right_clip));
    }
    if (!added.empty()) {
      track.clips.insert(track.clips.end(), std::make_move_iterator(added.begin()),
                         std::make_move_iterator(added.end()));
      sort_track(track);
    }
  }
  return p;
}

// --------------------------------------------------------------- arranging --

Project move_clips(Project p, std::span<const std::string> clip_ids, double delta_time,
                   int delta_track_index, std::optional<TrackKind> restrict_kind) {
  const std::unordered_set<std::string> ids = id_set(clip_ids);

  // The whole set shifts together, clamped so nothing starts before zero.
  double min_start = kInfinity;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) {
      if (ids.contains(c.id)) min_start = std::min(min_start, c.start);
    }
  }
  if (!std::isfinite(min_start)) return p;
  const double clamped_delta = std::max(delta_time, -min_start);

  /// A pending track change, resolved against the original layout before any
  /// clip actually moves.
  struct Relocation {
    std::size_t from_track = 0;
    std::size_t to_track = 0;
    std::string clip_id;
  };
  std::vector<Relocation> relocations;

  const std::vector<std::size_t> by_kind[2] = {
      track_indices_of_kind(p, TrackKind::Video),
      track_indices_of_kind(p, TrackKind::Audio),
  };

  for (std::size_t ti = 0; ti < p.tracks.size(); ++ti) {
    for (Clip& c : p.tracks[ti].clips) {
      if (!ids.contains(c.id)) continue;
      c.start += clamped_delta;

      if (delta_track_index == 0) continue;
      if (restrict_kind.has_value() && c.kind != *restrict_kind) continue;

      const std::vector<std::size_t>& lanes =
          by_kind[c.kind == TrackKind::Video ? 0 : 1];
      const auto pos = std::ranges::find(lanes, ti);
      if (pos == lanes.end()) continue;

      const auto current = static_cast<long long>(std::distance(lanes.begin(), pos));
      const long long destination = current + delta_track_index;
      if (destination < 0 || destination >= static_cast<long long>(lanes.size())) continue;
      if (destination == current) continue;

      relocations.push_back({.from_track = ti,
                             .to_track = lanes[static_cast<std::size_t>(destination)],
                             .clip_id = c.id});
    }
  }

  for (const Relocation& r : relocations) {
    if (std::optional<Clip> taken = extract_clip(p.tracks[r.from_track], r.clip_id)) {
      p.tracks[r.to_track].clips.push_back(*std::move(taken));
    }
  }

  sort_all_tracks(p);
  return p;
}

Project set_clip_edge(Project p, std::string_view clip_id, ClipEdge edge, double timeline_time) {
  const Clip* target = find_clip(p, clip_id);
  if (target == nullptr) return p;

  const double desired =
      edge == ClipEdge::In ? timeline_time - target->start : timeline_time - clip_end(*target);

  const std::vector<std::string> member_ids = group_members(p, clip_id);
  const std::unordered_set<std::string> members = id_set(member_ids);

  // Intersect the allowable delta across every group member.
  double lo = -kInfinity;
  double hi = kInfinity;

  for (const Track& track : p.tracks) {
    // Neighbours are determined in timeline order, which is how the track is
    // stored, but sorting here keeps this correct even mid-edit.
    std::vector<const Clip*> sorted;
    sorted.reserve(track.clips.size());
    for (const Clip& c : track.clips) sorted.push_back(&c);
    std::ranges::stable_sort(sorted, {}, [](const Clip* c) { return c->start; });

    for (std::size_t i = 0; i < sorted.size(); ++i) {
      const Clip& c = *sorted[i];
      if (!members.contains(c.id)) continue;

      const double media_duration = trim_limit_duration(p, c.media_id);
      const double speed = clip_speed(c);
      const double duration = clip_duration(c);
      const Clip* previous = i > 0 ? sorted[i - 1] : nullptr;
      const Clip* next = i + 1 < sorted.size() ? sorted[i + 1] : nullptr;

      if (edge == ClipEdge::In) {
        // Pulling the head earlier is bounded by the timeline start, the
        // previous clip, and the source available on that side. Reverse swaps
        // which source edge the head is anchored to.
        double member_lo = std::max(-c.start, previous != nullptr
                                                  ? clip_end(*previous) - c.start
                                                  : -kInfinity);
        member_lo = std::max(member_lo, c.reverse ? -(media_duration - c.source_out) / speed
                                                  : -c.source_in / speed);
        lo = std::max(lo, member_lo);
        hi = std::min(hi, duration - kMinClip);
      } else {
        double member_hi = next != nullptr ? next->start - clip_end(c) : kInfinity;
        member_hi = std::min(member_hi, c.reverse ? c.source_in / speed
                                                  : (media_duration - c.source_out) / speed);
        lo = std::max(lo, kMinClip - duration);
        hi = std::min(hi, member_hi);
      }
    }
  }

  if (lo > hi) return p;  // over-constrained: no room to trim
  const double delta = std::clamp(desired, lo, hi);

  for (Track& track : p.tracks) {
    for (Clip& c : track.clips) {
      if (!members.contains(c.id)) continue;
      const double speed = clip_speed(c);
      if (edge == ClipEdge::In) {
        c.start += delta;
        if (c.reverse) {
          c.source_out -= delta * speed;
        } else {
          c.source_in += delta * speed;
        }
      } else {
        if (c.reverse) {
          c.source_in -= delta * speed;
        } else {
          c.source_out += delta * speed;
        }
      }
    }
  }

  sort_all_tracks(p);
  return p;
}

Project ripple_insert(Project p, double at_time, double amount) {
  std::vector<std::string> spanning;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) {
      if (at_time > c.start && at_time < clip_end(c)) spanning.push_back(c.id);
    }
  }
  p = split_at(std::move(p), at_time, spanning);

  for (Track& t : p.tracks) {
    for (Clip& c : t.clips) {
      if (c.start >= at_time - 1e-6) c.start += amount;
    }
    sort_track(t);
  }
  return p;
}

namespace {

/// Carves [start, end) out of one track, trimming or dropping whatever it
/// covers. A clip spanning the whole region is left as two pieces.
void clear_range(Track& track, double start, double end) {
  std::vector<Clip> kept;
  kept.reserve(track.clips.size());

  for (Clip& c : track.clips) {
    const double clip_start = c.start;
    const double end_of_clip = clip_end(c);

    if (end_of_clip <= start || clip_start >= end) {
      kept.push_back(std::move(c));  // untouched
    } else if (clip_start >= start && end_of_clip <= end) {
      // Fully covered: drop it.
    } else if (clip_start < start && end_of_clip > end) {
      // Spans the region: keep a piece on each side.
      const SourceRange left = clip_sub_source(c, clip_start, start);
      const SourceRange right = clip_sub_source(c, end, end_of_clip);

      Clip right_clip = c;
      right_clip.id = new_id("clip");
      right_clip.source_in = right.source_in;
      right_clip.source_out = right.source_out;
      recut_head(right_clip, end);

      c.source_in = left.source_in;
      c.source_out = left.source_out;
      recut_tail(c);

      kept.push_back(std::move(c));
      kept.push_back(std::move(right_clip));
    } else if (clip_start < start) {
      const SourceRange left = clip_sub_source(c, clip_start, start);
      c.source_in = left.source_in;
      c.source_out = left.source_out;
      recut_tail(c);
      kept.push_back(std::move(c));
    } else {
      const SourceRange right = clip_sub_source(c, end, end_of_clip);
      c.source_in = right.source_in;
      c.source_out = right.source_out;
      recut_head(c, end);
      kept.push_back(std::move(c));
    }
  }

  track.clips = std::move(kept);
  sort_track(track);
}

}  // namespace

Project insert_media_at(Project p, std::string_view media_id, double at_time,
                        std::string_view video_track_id, std::optional<PlacementRange> range) {
  const auto media = std::ranges::find(p.media, media_id, &Media::id);
  if (media == p.media.end()) return p;

  p = ripple_insert(std::move(p), at_time, placed_length(*media, range));
  return place_media(std::move(p), media_id, at_time, video_track_id, range);
}

Project overwrite_media_at(Project p, std::string_view media_id, double at_time,
                           std::string_view video_track_id, std::optional<PlacementRange> range) {
  const auto media_it = std::ranges::find(p.media, media_id, &Media::id);
  if (media_it == p.media.end()) return p;
  const Media media = *media_it;

  const double end = at_time + placed_length(media, range);
  const std::vector<std::size_t> video_tracks = track_indices_of_kind(p, TrackKind::Video);
  const std::vector<std::size_t> audio_tracks = track_indices_of_kind(p, TrackKind::Audio);

  std::string target_video_id;
  if (!video_tracks.empty()) {
    std::size_t target = video_tracks.front();
    if (!video_track_id.empty()) {
      for (const std::size_t i : video_tracks) {
        if (p.tracks[i].id == video_track_id) {
          target = i;
          break;
        }
      }
    }
    target_video_id = p.tracks[target].id;
    if (media.has_video) clear_range(p.tracks[target], at_time, end);
  }

  for (int stream = 0; stream < media.audio_stream_count; ++stream) {
    const auto lane = static_cast<std::size_t>(stream);
    if (lane < audio_tracks.size()) clear_range(p.tracks[audio_tracks[lane]], at_time, end);
  }

  return place_media(std::move(p), media_id, at_time, target_video_id, range);
}

Project rate_stretch_edge(Project p, std::string_view clip_id, ClipEdge edge, double new_time) {
  const Clip* target = find_clip(p, clip_id);
  if (target == nullptr) return p;

  const double new_duration = std::max(
      kMinClip, edge == ClipEdge::Out ? new_time - target->start : clip_end(*target) - new_time);

  const std::unordered_set<std::string> members = id_set(group_members(p, clip_id));
  for (Track& t : p.tracks) {
    for (Clip& c : t.clips) {
      if (!members.contains(c.id)) continue;
      const double old_end = clip_end(c);
      c.speed = std::clamp(source_span(c) / new_duration, kMinSpeed, kMaxSpeed);
      // Dragging the in-edge keeps the tail where it was.
      if (edge == ClipEdge::In) c.start = old_end - clip_duration(c);
    }
    sort_track(t);
  }
  return p;
}

Project slip_clip(Project p, std::string_view clip_id, double delta_source) {
  const std::unordered_set<std::string> members = id_set(group_members(p, clip_id));

  // Intersect the allowable shift across every member that has real source.
  double lo = -kInfinity;
  double hi = kInfinity;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) {
      if (!members.contains(c.id)) continue;
      const auto m = std::ranges::find(p.media, c.media_id, &Media::id);
      if (m == p.media.end() || is_still_like(*m)) continue;
      lo = std::max(lo, -c.source_in);                 // source_in stays >= 0
      hi = std::min(hi, m->duration - c.source_out);   // source_out stays within the media
    }
  }
  if (!std::isfinite(lo)) lo = 0.0;
  if (!std::isfinite(hi)) hi = 0.0;
  const double delta = std::clamp(delta_source, lo, hi);

  for (Track& t : p.tracks) {
    for (Clip& c : t.clips) {
      if (!members.contains(c.id)) continue;
      const auto m = std::ranges::find(p.media, c.media_id, &Media::id);
      if (m == p.media.end() || is_still_like(*m)) continue;
      c.source_in += delta;
      c.source_out += delta;
    }
  }
  return p;
}

Project slide_clip(Project p, std::string_view clip_id, double delta_time) {
  Track* track = track_of_clip(p, clip_id);
  if (track == nullptr) return p;

  std::vector<Clip*> sorted;
  sorted.reserve(track->clips.size());
  for (Clip& c : track->clips) sorted.push_back(&c);
  std::ranges::stable_sort(sorted, {}, [](const Clip* c) { return c->start; });

  const auto it = std::ranges::find(sorted, clip_id, [](const Clip* c) -> const std::string& {
    return c->id;
  });
  if (it == sorted.end()) return p;

  const auto index = static_cast<std::size_t>(std::distance(sorted.begin(), it));
  Clip& c = **it;
  Clip* previous = index > 0 ? sorted[index - 1] : nullptr;
  Clip* next = index + 1 < sorted.size() ? sorted[index + 1] : nullptr;

  constexpr double kAbutEps = 1e-3;
  const bool has_previous =
      previous != nullptr && std::abs(clip_end(*previous) - c.start) < kAbutEps;
  const bool has_next = next != nullptr && std::abs(clip_end(c) - next->start) < kAbutEps;
  if (!has_previous && !has_next) return p;  // nothing to slide against

  double lo = -c.start;  // the clip cannot slide before zero
  double hi = kInfinity;
  if (has_previous) {
    const auto m = std::ranges::find(p.media, previous->media_id, &Media::id);
    const double media_duration =
        m != p.media.end() && !is_still_like(*m) ? m->duration : kInfinity;
    hi = std::min(hi, (media_duration - previous->source_out) / clip_speed(*previous));
    lo = std::max(lo, kMinClip - clip_duration(*previous));
  }
  if (has_next) {
    lo = std::max(lo, -next->source_in / clip_speed(*next));
    hi = std::min(hi, clip_duration(*next) - kMinClip);
  }
  if (lo > hi) return p;

  const double delta = std::clamp(delta_time, lo, hi);
  if (std::abs(delta) < 1e-6) return p;

  c.start += delta;
  if (has_previous) previous->source_out += delta * clip_speed(*previous);
  if (has_next) {
    next->start += delta;
    next->source_in += delta * clip_speed(*next);
  }
  sort_track(*track);
  return p;
}

// ---------------------------------------------------------------- deleting --

Project remove_clips(Project p, std::span<const std::string> clip_ids) {
  const std::unordered_set<std::string> ids = id_set(clip_ids);
  for (Track& t : p.tracks) {
    std::erase_if(t.clips, [&](const Clip& c) { return ids.contains(c.id); });
  }
  return p;
}

Project ripple_delete(Project p, std::span<const std::string> clip_ids) {
  std::unordered_set<std::string> ids;
  for (const std::string& id : clip_ids) {
    for (std::string& member : group_members(p, id)) ids.insert(std::move(member));
  }

  double min_start = kInfinity;
  double max_end = -kInfinity;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) {
      if (!ids.contains(c.id)) continue;
      min_start = std::min(min_start, c.start);
      max_end = std::max(max_end, clip_end(c));
    }
  }
  if (!std::isfinite(min_start)) return p;

  const double ripple = max_end - min_start;
  for (Track& t : p.tracks) {
    std::erase_if(t.clips, [&](const Clip& c) { return ids.contains(c.id); });
    for (Clip& c : t.clips) {
      if (c.start >= max_end - 1e-6) c.start -= ripple;
    }
    sort_track(t);
  }
  return p;
}

// ----------------------------------------------------------------- linking --

Project link_clips(Project p, std::span<const std::string> clip_ids) {
  if (clip_ids.size() < 2) return p;
  const std::unordered_set<std::string> ids = id_set(clip_ids);
  const std::string group_id = new_id("grp");
  for (Track& t : p.tracks) {
    for (Clip& c : t.clips) {
      if (ids.contains(c.id)) c.group_id = group_id;
    }
  }
  return p;
}

Project unlink_clips(Project p, std::span<const std::string> clip_ids) {
  const std::unordered_set<std::string> ids = id_set(clip_ids);
  for (Track& t : p.tracks) {
    for (Clip& c : t.clips) {
      if (ids.contains(c.id)) c.group_id.reset();
    }
  }
  return p;
}

Project unlink_group(Project p, std::string_view clip_id) {
  const Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr || !clip->group_id.has_value()) return p;

  const std::string group_id = *clip->group_id;
  for (Track& t : p.tracks) {
    for (Clip& c : t.clips) {
      if (c.group_id == group_id) c.group_id.reset();
    }
  }
  return p;
}

}  // namespace cutline::core
