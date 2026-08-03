#include "cutline/core/edit.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cutline::core {
namespace {

constexpr double kInfinity = std::numeric_limits<double>::infinity();

/// How close two times have to be to count as the same instant — as the edge a
/// ripple pushes from, or the join a roll edits. A millisecond, which is the
/// same tolerance the slide already uses for whether two clips abut: a cut the
/// interface drew as a join and the model treated as a gap would be an edit
/// that silently did nothing.
constexpr double kTouchEps = 1e-3;

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

namespace {

/// Which lanes a placement will use, creating audio lanes when there are too
/// few — so asking twice gives the same answer.
///
/// Extracted because the overwrite has to carve exactly the lanes the placement
/// is about to fill. It used to clear lanes 0..N and let the placement choose
/// its own, which was the same bug in a second place the moment the two
/// disagreed about where the sound goes.
struct PlacementLanes {
  std::optional<std::size_t> video;
  std::vector<std::size_t> audio;
};

[[nodiscard]] PlacementLanes reserve_lanes(Project& p, const Media& media,
                                           std::string_view track_id) {
  PlacementLanes lanes;

  // A named track that turns out to be an audio one is a target for the sound
  // rather than for the picture — that is what targeting an audio lane means,
  // and the only way an audio-only source can be aimed anywhere but A1.
  std::optional<std::size_t> audio_target;
  if (!track_id.empty()) {
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
      if (p.tracks[i].id == track_id && p.tracks[i].kind == TrackKind::Audio) {
        audio_target = i;
        break;
      }
    }
  }

  const std::vector<std::size_t> video_tracks = track_indices_of_kind(p, TrackKind::Video);
  if (media.has_video && !video_tracks.empty()) {
    std::size_t target = video_tracks.front();
    if (!track_id.empty()) {
      for (const std::size_t i : video_tracks) {
        if (p.tracks[i].id == track_id) {
          target = i;
          break;
        }
      }
    }
    lanes.video = target;
  }

  std::vector<std::size_t> audio_tracks = track_indices_of_kind(p, TrackKind::Audio);

  // Which lane the video landed on, counted from the bottom, because that is
  // how video lanes are numbered: V1 is the base layer and the topmost track
  // has the highest number, while A1 is the *first* audio lane. Pairing them is
  // what makes V1's sound land on A1.
  std::size_t first_lane = 0;
  if (audio_target.has_value()) {
    const auto found = std::ranges::find(audio_tracks, *audio_target);
    if (found != audio_tracks.end()) {
      first_lane = static_cast<std::size_t>(std::distance(audio_tracks.begin(), found));
    }
  } else if (lanes.video.has_value()) {
    const auto found = std::ranges::find(video_tracks, *lanes.video);
    if (found != video_tracks.end()) {
      first_lane = static_cast<std::size_t>(std::distance(found, video_tracks.end())) - 1;
    }
  }

  for (int stream = 0; stream < media.audio_stream_count; ++stream) {
    const std::size_t lane = first_lane + static_cast<std::size_t>(stream);
    while (audio_tracks.size() <= lane) {
      Track fresh;
      fresh.id = new_id("track");
      fresh.kind = TrackKind::Audio;
      p.tracks.push_back(std::move(fresh));
      audio_tracks.push_back(p.tracks.size() - 1);
    }
    lanes.audio.push_back(audio_tracks[lane]);
  }
  return lanes;
}

}  // namespace

Project place_media(Project p, std::string_view media_id, double start,
                    std::string_view track_id, std::optional<PlacementRange> range) {
  const auto media_it = std::ranges::find(p.media, media_id, &Media::id);
  if (media_it == p.media.end()) return p;
  const Media media = *media_it;  // copied: p.media may not reallocate, but tracks change

  const std::string group_id = new_id("grp");

  // Still-like media have no inherent time, so they ignore any source range.
  const bool honour_range = range.has_value() && !is_still_like(media);
  const double source_in = honour_range ? std::max(0.0, range->in) : 0.0;
  const double source_out = honour_range ? std::min(media.duration, range->out) : media.duration;

  const PlacementLanes lanes = reserve_lanes(p, media, track_id);

  if (lanes.video.has_value()) {
    Clip clip;
    clip.id = new_id("clip");
    clip.media_id = media.id;
    clip.kind = TrackKind::Video;
    clip.source_in = source_in;
    clip.source_out = source_out;
    clip.start = start;
    clip.group_id = group_id;
    p.tracks[*lanes.video].clips.push_back(std::move(clip));
    sort_track(p.tracks[*lanes.video]);
  }

  // Each audio stream on the lane that matches the video's, and the ones below
  // it for the streams after the first. Streams take a lane each — piling a
  // stereo pair's two onto one was the original bug — but a second placement
  // over the first lands on the same lanes rather than running off to find a
  // free one, because that is what the video does with its own track.
  for (std::size_t stream = 0; stream < lanes.audio.size(); ++stream) {
    Clip clip;
    clip.id = new_id("clip");
    clip.media_id = media.id;
    clip.kind = TrackKind::Audio;
    clip.audio_stream = static_cast<int>(stream);
    clip.source_in = source_in;
    clip.source_out = source_out;
    clip.start = start;
    clip.group_id = group_id;
    p.tracks[lanes.audio[stream]].clips.push_back(std::move(clip));
    sort_track(p.tracks[lanes.audio[stream]]);
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

namespace {

/// How far an edge may move, in timeline seconds, before something stops it.
///
/// Intersected across every member of the linked group, so a video clip and its
/// audio are trimmed by the same amount or not at all.
///
/// `neighbours` is what separates a trim from a ripple trim. An ordinary trim
/// stops where the clip beside it begins; a ripple pushes that clip along
/// instead, so the only limits left are the source available and the shortest a
/// clip is allowed to be.
struct EdgeRoom {
  double lo = -kInfinity;
  double hi = kInfinity;

  [[nodiscard]] bool empty() const noexcept { return lo > hi; }
};

[[nodiscard]] EdgeRoom edge_room(const Project& p, const std::unordered_set<std::string>& members,
                                 ClipEdge edge, bool neighbours) {
  EdgeRoom room;

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
        double member_lo = std::max(-c.start, neighbours && previous != nullptr
                                                  ? clip_end(*previous) - c.start
                                                  : -kInfinity);
        member_lo = std::max(member_lo, c.reverse ? -(media_duration - c.source_out) / speed
                                                  : -c.source_in / speed);
        room.lo = std::max(room.lo, member_lo);
        room.hi = std::min(room.hi, duration - kMinClip);
      } else {
        double member_hi =
            neighbours && next != nullptr ? next->start - clip_end(c) : kInfinity;
        member_hi = std::min(member_hi, c.reverse ? c.source_in / speed
                                                  : (media_duration - c.source_out) / speed);
        room.lo = std::max(room.lo, kMinClip - duration);
        room.hi = std::min(room.hi, member_hi);
      }
    }
  }
  return room;
}

/// Moves an edge of every member of a group by `delta` timeline seconds,
/// taking the source with it. The caller has already decided the delta is
/// allowed.
void move_edge(Project& p, const std::unordered_set<std::string>& members, ClipEdge edge,
               double delta) {
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
}

}  // namespace

Project set_clip_edge(Project p, std::string_view clip_id, ClipEdge edge, double timeline_time) {
  const Clip* target = find_clip(p, clip_id);
  if (target == nullptr) return p;

  const double desired =
      edge == ClipEdge::In ? timeline_time - target->start : timeline_time - clip_end(*target);
  const std::unordered_set<std::string> members = id_set(group_members(p, clip_id));

  const EdgeRoom room = edge_room(p, members, edge, true);
  if (room.empty()) return p;  // over-constrained: no room to trim

  move_edge(p, members, edge, std::clamp(desired, room.lo, room.hi));
  return p;
}

Project ripple_trim_edge(Project p, std::string_view clip_id, ClipEdge edge,
                         double timeline_time) {
  const Clip* target = find_clip(p, clip_id);
  if (target == nullptr) return p;

  const double edge_was = edge == ClipEdge::In ? target->start : clip_end(*target);
  const double desired = timeline_time - edge_was;
  const std::unordered_set<std::string> members = id_set(group_members(p, clip_id));

  // Without the neighbours: they are what moves, so they cannot also be what
  // stops it.
  const EdgeRoom room = edge_room(p, members, edge, false);
  if (room.empty()) return p;

  const double delta = std::clamp(desired, room.lo, room.hi);
  if (delta == 0.0) return p;
  move_edge(p, members, edge, delta);

  // Everything downstream follows, on every track, so nothing that was in sync
  // stops being. Compared against where the edge *was*, since the clips that
  // move are the ones that were after it before the trim.
  //
  // The in-edge case includes the group itself: trimming a head and then
  // pulling everything from there along means the clip keeps its place and the
  // gap never opens. Without that a ripple on the head would leave a hole in
  // front of the very clip that was trimmed.
  const double shift = edge == ClipEdge::In ? -delta : delta;
  for (Track& track : p.tracks) {
    for (Clip& c : track.clips) {
      const bool member = members.contains(c.id);
      if (edge == ClipEdge::In) {
        if (member || c.start >= edge_was - kTouchEps) c.start += shift;
      } else {
        if (!member && c.start >= edge_was - kTouchEps) c.start += shift;
      }
    }
    sort_track(track);
  }
  return p;
}

Project roll_edit(Project p, std::string_view clip_id, ClipEdge edge, double timeline_time) {
  const Clip* target = find_clip(p, clip_id);
  if (target == nullptr) return p;

  // The clip on the other side of the cut, which must actually meet this one:
  // a roll is an edit to a *join*, and there is nothing to roll at an edge with
  // a gap or the end of the track beyond it.
  const double at = edge == ClipEdge::In ? target->start : clip_end(*target);
  const Clip* other = nullptr;
  for (const Track& track : p.tracks) {
    for (const Clip& c : track.clips) {
      if (c.id == target->id) continue;
      const double meets = edge == ClipEdge::In ? clip_end(c) : c.start;
      if (std::abs(meets - at) <= kTouchEps &&
          std::ranges::any_of(track.clips, [&](const Clip& t) { return t.id == target->id; })) {
        other = &c;
      }
    }
  }
  if (other == nullptr) return p;

  const std::unordered_set<std::string> mine = id_set(group_members(p, clip_id));
  const std::unordered_set<std::string> theirs = id_set(group_members(p, other->id));
  const ClipEdge far = edge == ClipEdge::In ? ClipEdge::Out : ClipEdge::In;

  // Both sides, without neighbours — each one's neighbour is the other, and the
  // whole point is that they move together.
  const EdgeRoom near_room = edge_room(p, mine, edge, false);
  const EdgeRoom far_room = edge_room(p, theirs, far, false);
  const EdgeRoom both{.lo = std::max(near_room.lo, far_room.lo),
                      .hi = std::min(near_room.hi, far_room.hi)};
  if (both.empty()) return p;

  const double delta = std::clamp(timeline_time - at, both.lo, both.hi);
  if (delta == 0.0) return p;

  // The shrinking side first, so the join is briefly open rather than briefly
  // overlapping — the growing one then has somewhere to go.
  move_edge(p, mine, edge, delta);
  move_edge(p, theirs, far, delta);
  return p;
}

namespace {

/// Which video lane, counting only video tracks, holds this clip. -1 when it is
/// not on one.
[[nodiscard]] long long video_lane_of(const Project& p, std::string_view clip_id) noexcept {
  long long lane = 0;
  for (const Track& t : p.tracks) {
    if (t.kind != TrackKind::Video) continue;
    if (std::ranges::any_of(t.clips, [&](const Clip& c) { return c.id == clip_id; })) return lane;
    ++lane;
  }
  return -1;
}

}  // namespace

Project move_clips_layered(Project p, std::span<const std::string> clip_ids, double delta_time,
                           int delta_track_index, TrackKind kind) {
  const Project before = p;
  p = move_clips(std::move(p), clip_ids, delta_time, delta_track_index, kind);
  if (kind != TrackKind::Video || delta_track_index == 0) return p;

  // Only a real change of compositing layer triggers the audio reflow.
  const bool changed_layer = std::ranges::any_of(clip_ids, [&](const std::string& id) {
    const Clip* c = find_clip(p, id);
    return c != nullptr && c->kind == TrackKind::Video &&
           video_lane_of(before, id) != video_lane_of(p, id);
  });
  if (!changed_layer) return p;

  const std::unordered_set<std::string> ids = id_set(clip_ids);
  std::vector<std::string> moved_audio;
  std::optional<std::string> group_id;
  for (const Track& t : p.tracks) {
    if (t.kind != TrackKind::Audio) continue;
    for (const Clip& c : t.clips) {
      if (!ids.contains(c.id)) continue;
      if (moved_audio.empty()) group_id = c.group_id;
      moved_audio.push_back(c.id);
    }
  }
  if (moved_audio.empty()) return p;

  const std::unordered_set<std::string> moved_ids(moved_audio.begin(), moved_audio.end());

  // A lane is available to this group when nothing on it belongs to anyone else.
  const auto dedicated_to_group = [&](const Track& t) {
    return std::ranges::all_of(t.clips, [&](const Clip& c) {
      return moved_ids.contains(c.id) || (group_id.has_value() && c.group_id == group_id);
    });
  };

  std::vector<std::size_t> audio_tracks = track_indices_of_kind(p, TrackKind::Audio);
  std::unordered_set<std::string> claimed;

  for (const std::string& clip_id : moved_audio) {
    std::optional<std::size_t> current;
    for (const std::size_t i : audio_tracks) {
      if (std::ranges::any_of(p.tracks[i].clips,
                              [&](const Clip& c) { return c.id == clip_id; })) {
        current = i;
        break;
      }
    }

    std::optional<std::size_t> destination;
    if (current.has_value() && !claimed.contains(p.tracks[*current].id) &&
        dedicated_to_group(p.tracks[*current])) {
      destination = current;  // already on a lane of its own
    } else {
      for (const std::size_t i : audio_tracks) {
        if (!claimed.contains(p.tracks[i].id) && dedicated_to_group(p.tracks[i])) {
          destination = i;
          break;
        }
      }
    }

    if (!destination.has_value()) {
      Track lane;
      lane.id = new_id("track");
      lane.kind = TrackKind::Audio;
      p.tracks.push_back(std::move(lane));
      destination = p.tracks.size() - 1;
      audio_tracks.push_back(*destination);
    }

    if (current.has_value() && *current != *destination) {
      if (std::optional<Clip> taken = extract_clip(p.tracks[*current], clip_id)) {
        p.tracks[*destination].clips.push_back(*std::move(taken));
      }
    }
    claimed.insert(p.tracks[*destination].id);
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
                        std::string_view track_id, std::optional<PlacementRange> range) {
  const auto media = std::ranges::find(p.media, media_id, &Media::id);
  if (media == p.media.end()) return p;

  p = ripple_insert(std::move(p), at_time, placed_length(*media, range));
  return place_media(std::move(p), media_id, at_time, track_id, range);
}

Project overwrite_media_at(Project p, std::string_view media_id, double at_time,
                           std::string_view track_id, std::optional<PlacementRange> range) {
  const auto media_it = std::ranges::find(p.media, media_id, &Media::id);
  if (media_it == p.media.end()) return p;
  const Media media = *media_it;

  const double end = at_time + placed_length(media, range);

  // Exactly the lanes the placement is about to fill, worked out by the same
  // function it uses. This used to clear lanes zero upwards and let the
  // placement choose its own, which carved holes in one place and put the sound
  // in another the moment the two disagreed.
  const PlacementLanes lanes = reserve_lanes(p, media, track_id);
  if (lanes.video.has_value()) clear_range(p.tracks[*lanes.video], at_time, end);
  for (const std::size_t lane : lanes.audio) clear_range(p.tracks[lane], at_time, end);

  // By id rather than index: `clear_range` sorts, and nothing here adds tracks
  // after the reservation, but naming the track is what makes that irrelevant.
  const std::string resolved =
      lanes.video.has_value() ? p.tracks[*lanes.video].id : std::string(track_id);
  return place_media(std::move(p), media_id, at_time, resolved, range);
}

// ------------------------------------------------------------ copy / paste --

std::vector<ClipCopy> copy_clips(const Project& p, std::span<const std::string> clip_ids) {
  const std::unordered_set<std::string> ids = id_set(clip_ids);

  std::vector<ClipCopy> copies;
  for (const Track& t : p.tracks) {
    for (const Clip& c : t.clips) {
      if (ids.contains(c.id)) copies.push_back(ClipCopy{.clip = c, .track_id = t.id});
    }
  }
  // Tracks are walked in order and each track is kept sorted, so this is
  // already in track-then-start order for everything except across tracks. The
  // paste only needs the earliest start, which a scan finds either way.
  return copies;
}

namespace {

/// Where a copy would land, or nothing when this project has no lane for it.
///
/// The lane it came from when that still exists, and the first of its kind
/// otherwise — which is what happens when a clip is copied out of one project
/// and pasted into another, or after the track it lived on has been removed.
[[nodiscard]] std::optional<std::size_t> paste_track(const Project& p, const ClipCopy& copy) {
  for (std::size_t i = 0; i < p.tracks.size(); ++i) {
    if (p.tracks[i].id == copy.track_id && p.tracks[i].kind == copy.clip.kind) return i;
  }
  const std::vector<std::size_t> same = track_indices_of_kind(p, copy.clip.kind);
  if (same.empty()) return std::nullopt;
  return same.front();
}

}  // namespace

Project paste_clips(Project p, std::span<const ClipCopy> clips, double at_time,
                    std::vector<std::string>* pasted) {
  if (clips.empty()) return p;

  double origin = kInfinity;
  for (const ClipCopy& copy : clips) origin = std::min(origin, copy.clip.start);

  // Resolved first, and the whole paste abandoned if nothing can land. A paste
  // that silently drops half of what was copied is worse than one that does
  // nothing: the second is obvious.
  std::vector<std::pair<std::size_t, const ClipCopy*>> landing;
  for (const ClipCopy& copy : clips) {
    if (const std::optional<std::size_t> track = paste_track(p, copy); track.has_value()) {
      landing.emplace_back(*track, &copy);
    }
  }
  if (landing.empty()) return p;

  // Every span carved before anything is placed. Carving as we go would let the
  // second clip of a paste cut a hole in the first one.
  for (const auto& [track, copy] : landing) {
    const double start = at_time + (copy->clip.start - origin);
    clear_range(p.tracks[track], start, start + clip_duration(copy->clip));
  }

  // Groups are remapped rather than kept: a pasted pair stays linked to each
  // other and not to the pair it was copied from, which would otherwise drag
  // the original along on every move.
  std::unordered_map<std::string, std::string> groups;
  for (const auto& [track, copy] : landing) {
    Clip clip = copy->clip;
    clip.id = new_id("clip");
    clip.start = at_time + (copy->clip.start - origin);
    if (clip.group_id.has_value()) {
      auto [entry, fresh] = groups.try_emplace(*clip.group_id);
      if (fresh) entry->second = new_id("grp");
      clip.group_id = entry->second;
    }
    if (pasted != nullptr) pasted->push_back(clip.id);
    p.tracks[track].clips.push_back(std::move(clip));
  }
  for (const auto& [track, copy] : landing) sort_track(p.tracks[track]);

  return p;
}

Project paste_clips_insert(Project p, std::span<const ClipCopy> clips, double at_time,
                           std::vector<std::string>* pasted) {
  if (clips.empty()) return p;

  double origin = kInfinity;
  double finish = -kInfinity;
  for (const ClipCopy& copy : clips) {
    origin = std::min(origin, copy.clip.start);
    finish = std::max(finish, clip_end(copy.clip));
  }

  p = ripple_insert(std::move(p), at_time, finish - origin);
  return paste_clips(std::move(p), clips, at_time, pasted);
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
