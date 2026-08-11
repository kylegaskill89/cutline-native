#include "cutline/core/nesting.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/sequences.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <vector>

namespace cutline::core {
namespace {

/// The media a clip shows, or null.
[[nodiscard]] const Media* media_of(const Project& p, const Clip& clip) noexcept {
  const auto found = std::ranges::find(p.media, clip.media_id, &Media::id);
  return found == p.media.end() ? nullptr : &*found;
}

}  // namespace

const Sequence* nested_sequence(const Project& p, const Clip& clip) noexcept {
  const Media* media = media_of(p, clip);
  if (media == nullptr || !is_nested_sequence(*media)) return nullptr;
  return find_sequence(p, media->sequence_id);
}

double sequence_duration(const Sequence& s) noexcept {
  double end = 0.0;
  for (const Track& track : s.tracks) {
    for (const Clip& clip : track.clips) end = std::max(end, clip_end(clip));
  }
  return end;
}

bool sequence_contains(const Project& p, std::string_view outer, std::string_view inner) {
  if (outer.empty() || inner.empty()) return false;
  if (outer == inner) return true;

  // Breadth first over the sequences reachable from `outer`, with a visited set
  // so a project that already holds a cycle — one written by an older build, or
  // by hand — is walked once rather than for ever.
  std::vector<std::string> pending{std::string(outer)};
  std::unordered_set<std::string> seen{std::string(outer)};

  while (!pending.empty()) {
    const std::string at = std::move(pending.back());
    pending.pop_back();

    const Sequence* sequence = find_sequence(p, at);
    if (sequence == nullptr) continue;

    for (const Track& track : sequence->tracks) {
      for (const Clip& clip : track.clips) {
        const Media* media = media_of(p, clip);
        if (media == nullptr || !is_nested_sequence(*media)) continue;
        if (media->sequence_id == inner) return true;
        if (seen.insert(media->sequence_id).second) pending.push_back(media->sequence_id);
      }
    }
  }
  return false;
}

Project sync_nested_media(Project p) {
  for (Media& media : p.media) {
    if (!is_nested_sequence(media)) continue;
    const Sequence* sequence = find_sequence(p, media.sequence_id);
    // A pool entry for a sequence that has been removed. Left as it is rather
    // than deleted: something may still be cut with it, and a clip whose media
    // vanished is a hole in the timeline where a stale name is only a stale
    // name.
    if (sequence == nullptr) continue;

    media.name = sequence->name;
    media.duration = sequence_duration(*sequence);
    media.width = sequence->canvas_w;
    media.height = sequence->canvas_h;
    media.fps = sequence->fps;
    media.has_video = true;
    // One stereo pair. A nest is a mix, however many lanes went into it, and
    // the mixer hands back what the sequence sums to rather than its parts.
    media.audio_stream_count = 1;
  }
  return p;
}

Project nest_clips(Project p, std::span<const std::string> clip_ids, std::string name,
                   std::string* made_id) {
  if (clip_ids.empty()) return p;

  // Gathered with the track each came from, since the new sequence has to keep
  // what was above what.
  struct Taken {
    Clip clip;
    std::size_t track = 0;
  };
  std::vector<Taken> taken;
  double earliest = std::numeric_limits<double>::infinity();
  double latest = 0.0;

  const std::unordered_set<std::string> wanted(clip_ids.begin(), clip_ids.end());
  for (std::size_t t = 0; t < p.sequence().tracks.size(); ++t) {
    for (const Clip& clip : p.sequence().tracks[t].clips) {
      if (!wanted.contains(clip.id)) continue;
      earliest = std::min(earliest, clip.start);
      latest = std::max(latest, clip_end(clip));
      taken.push_back(Taken{.clip = clip, .track = t});
    }
  }
  if (taken.empty() || !(latest > earliest)) return p;

  // A sequence cannot be put inside itself, at any depth. The one being cut in
  // is about to hold the new nest, so anything already nested among these clips
  // must not lead back to it.
  for (const Taken& entry : taken) {
    const Media* media = media_of(p, entry.clip);
    if (media == nullptr || !is_nested_sequence(*media)) continue;
    if (sequence_contains(p, media->sequence_id, p.sequence().id)) return p;
  }

  // The canvas and rate of the sequence being cut into, because that is what
  // the nest is composited into. `add_sequence` takes them from the open one,
  // which is the same thing said once.
  const std::size_t was_open = p.open;
  p = add_sequence(std::move(p), name.empty() ? std::string("Nested Sequence") : name);
  const std::string inner_id = p.sequences.back().id;
  p.open = was_open;

  // Which video and audio lanes the taken clips were on, mapped onto the new
  // sequence's, so what was above stays above. The lanes are made as needed:
  // `add_sequence` builds one video and two audio, and a nest of four video
  // layers needs four.
  Sequence& inner = p.sequences.back();
  const auto lane_for = [&](TrackKind kind, std::size_t nth) -> Track& {
    std::vector<Track*> of_kind;
    for (Track& track : inner.tracks) {
      if (track.kind == kind) of_kind.push_back(&track);
    }
    while (of_kind.size() <= nth) {
      Track fresh;
      fresh.id = new_id("track");
      fresh.kind = kind;
      inner.tracks.push_back(std::move(fresh));
      of_kind.clear();
      for (Track& track : inner.tracks) {
        if (track.kind == kind) of_kind.push_back(&track);
      }
    }
    return *of_kind[nth];
  };

  // Counted per kind and in the order the outer sequence stores them, which is
  // top first for video — so the topmost video track taken becomes the topmost
  // in the nest.
  std::vector<std::size_t> video_tracks;
  std::vector<std::size_t> audio_tracks;
  for (std::size_t t = 0; t < p.sequences[was_open].tracks.size(); ++t) {
    if (p.sequences[was_open].tracks[t].kind == TrackKind::Video) video_tracks.push_back(t);
    else audio_tracks.push_back(t);
  }

  for (Taken& entry : taken) {
    const std::vector<std::size_t>& order =
        entry.clip.kind == TrackKind::Video ? video_tracks : audio_tracks;
    const auto at = std::ranges::find(order, entry.track);
    const auto nth = at == order.end()
                         ? std::size_t{0}
                         : static_cast<std::size_t>(std::distance(order.begin(), at));

    // Offsets from the earliest, so the nest begins at zero and a gap between
    // two nested clips is kept — it is part of what was nested, and closing it
    // would move footage nobody asked to move.
    entry.clip.start -= earliest;
    lane_for(entry.clip.kind, nth).clips.push_back(entry.clip);
  }
  for (Track& track : inner.tracks) {
    std::ranges::stable_sort(track.clips, {}, &Clip::start);
  }

  // The pool entry, which is what makes the nest an ordinary clip of an
  // ordinary source.
  Media entry;
  entry.id = new_id("media");
  entry.sequence_id = inner_id;
  p.media.push_back(std::move(entry));
  p = sync_nested_media(std::move(p));

  // Out with the originals, in with the nest, on the track and at the time the
  // earliest of them was.
  const std::size_t landing = taken.front().track;
  for (Track& track : p.sequences[was_open].tracks) {
    std::erase_if(track.clips, [&](const Clip& clip) { return wanted.contains(clip.id); });
  }

  Clip nest;
  nest.id = new_id("clip");
  nest.media_id = p.media.back().id;
  nest.kind = TrackKind::Video;
  nest.source_in = 0.0;
  nest.source_out = latest - earliest;
  nest.start = earliest;
  if (made_id != nullptr) *made_id = nest.id;

  // Onto a video lane, since a nest is a picture. The earliest clip's own track
  // when that is a video one, and the topmost otherwise — a nest of nothing but
  // sound still has to be seen somewhere.
  std::size_t onto = landing;
  if (p.sequences[was_open].tracks[onto].kind != TrackKind::Video) {
    onto = video_tracks.empty() ? onto : video_tracks.front();
  }
  p.sequences[was_open].tracks[onto].clips.push_back(std::move(nest));
  std::ranges::stable_sort(p.sequences[was_open].tracks[onto].clips, {}, &Clip::start);
  return p;
}

}  // namespace cutline::core
