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
      right_clip.start = time;
      if (c.group_id.has_value()) right_clip.group_id = right_group_id;

      // Keyframe times are measured from the clip's start, and the right
      // piece's start just moved to the cut, so its animation moves with it.
      const double shift = time - c.start;
      for (std::vector<Keyframe>& kfs : right_clip.keyframes) rebase_keyframes(kfs, shift);
      rebase_keyframes(right_clip.gain_keyframes, shift);
      for (ClipEffect& effect : right_clip.effects) {
        for (auto& [key, kfs] : effect.keyframes) rebase_keyframes(kfs, shift);
      }

      // A fade belongs to the edge it was authored against. The cut is not an
      // edge of the original clip, so no fade may appear there.
      right_clip.fade_in = 0.0;
      c.fade_out = 0.0;

      // Likewise the out-transition belongs to the original out-edge, which is
      // now the right piece's.
      c.transition_out.reset();

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
