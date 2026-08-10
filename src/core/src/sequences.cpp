#include "cutline/core/sequences.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace cutline::core {
namespace {

/// Tracks for a fresh sequence, named the way `empty_project` names them.
void fill_tracks(Sequence& s, int video_tracks, int audio_tracks) {
  for (int i = 0; i < video_tracks; ++i) {
    s.tracks.push_back(Track{.id = new_id("track"), .kind = TrackKind::Video});
  }
  for (int i = 0; i < audio_tracks; ++i) {
    s.tracks.push_back(Track{.id = new_id("track"), .kind = TrackKind::Audio});
  }
}

}  // namespace

std::size_t sequence_index(const Project& p, std::string_view id) noexcept {
  const auto found = std::ranges::find(p.sequences, id, &Sequence::id);
  return found == p.sequences.end()
             ? std::string::npos
             : static_cast<std::size_t>(found - p.sequences.begin());
}

const Sequence* find_sequence(const Project& p, std::string_view id) noexcept {
  const std::size_t at = sequence_index(p, id);
  return at == std::string::npos ? nullptr : &p.sequences[at];
}

std::string unused_sequence_name(const Project& p, std::string_view wanted) {
  const auto taken = [&p](std::string_view name) {
    return std::ranges::any_of(p.sequences,
                               [name](const Sequence& s) { return s.name == name; });
  };

  // "Sequence 01" upwards when nothing was asked for, which is what Premiere
  // offers and what the tab strip reads best.
  if (wanted.empty()) {
    for (int n = 1;; ++n) {
      std::string name = std::format("Sequence {:02}", n);
      if (!taken(name)) return name;
    }
  }

  if (!taken(wanted)) return std::string(wanted);
  for (int n = 2;; ++n) {
    std::string name = std::format("{} {}", wanted, n);
    if (!taken(name)) return name;
  }
}

Project add_sequence(Project p, std::string name, int video_tracks, int audio_tracks) {
  Sequence s;
  s.id = new_id("seq");
  s.name = unused_sequence_name(p, name);
  // The shape of the one being cut, because a second sequence in a project is
  // almost always another cut of the same material.
  s.canvas_w = p.sequence().canvas_w;
  s.canvas_h = p.sequence().canvas_h;
  s.fps = p.sequence().fps;
  s.drop_frame = p.sequence().drop_frame;
  fill_tracks(s, video_tracks, audio_tracks);

  p.sequences.push_back(std::move(s));
  return p;
}

Project sequence_from_clip(Project p, std::string_view media_id) {
  const auto found = std::ranges::find(p.media, media_id, &Media::id);
  if (found == p.media.end()) return p;
  const Media& media = *found;

  Sequence s;
  s.id = new_id("seq");
  s.name = unused_sequence_name(p, media.name);

  // Shaped to the footage, the same reasoning `match_sequence_to` uses for the
  // first import: a sequence made *from* a clip has no other sensible shape.
  // A generated source has no size of its own, so it takes the open sequence's.
  s.canvas_w = media.width.value_or(p.sequence().canvas_w);
  s.canvas_h = media.height.value_or(p.sequence().canvas_h);
  s.fps = media.fps.value_or(p.sequence().fps);
  s.drop_frame = p.sequence().drop_frame;

  // One video track and one audio track, and the clip on whichever it belongs
  // to. Two tracks rather than the usual three: this sequence exists to hold
  // one piece of footage, and empty tracks under it are furniture.
  const bool has_video = media.has_video;
  const bool has_audio = media.audio_stream_count > 0;
  fill_tracks(s, has_video ? 1 : 0, has_audio ? 1 : 0);
  if (s.tracks.empty()) fill_tracks(s, 1, 0);

  const double length = media.duration > 0.0 ? media.duration : 5.0;
  for (Track& t : s.tracks) {
    Clip c;
    c.id = new_id("clip");
    c.media_id = media.id;
    c.kind = t.kind;
    c.start = 0.0;
    c.source_in = 0.0;
    c.source_out = length;
    t.clips.push_back(std::move(c));
  }

  // Linked, so trimming the picture trims the sound with it — the same group a
  // placement makes.
  if (s.tracks.size() > 1) {
    const std::string group = new_id("group");
    for (Track& t : s.tracks) t.clips.front().group_id = group;
  }

  p.sequences.push_back(std::move(s));
  return p;
}

Project open_sequence(Project p, std::string_view id) {
  const std::size_t at = sequence_index(p, id);
  if (at == std::string::npos) return p;
  p.open = at;
  return p;
}

Project rename_sequence(Project p, std::string_view id, std::string name) {
  const std::size_t at = sequence_index(p, id);
  if (at == std::string::npos) return p;

  // Against the others rather than against all of them, or renaming a sequence
  // to what it already is would number it.
  Project without = p;
  without.sequences.erase(without.sequences.begin() + static_cast<std::ptrdiff_t>(at));
  p.sequences[at].name = unused_sequence_name(without, name);
  return p;
}

Project remove_sequence(Project p, std::string_view id) {
  if (p.sequences.size() <= 1) return p;
  const std::size_t at = sequence_index(p, id);
  if (at == std::string::npos) return p;

  p.sequences.erase(p.sequences.begin() + static_cast<std::ptrdiff_t>(at));
  // Whatever took its place, or the one before it if it was the last. The
  // sequence that was open stays open when something *else* was removed, which
  // means following it as the indices shift.
  if (p.open > at) {
    --p.open;
  } else if (p.open == at) {
    p.open = std::min(at, p.sequences.size() - 1);
  }
  return p;
}

}  // namespace cutline::core
