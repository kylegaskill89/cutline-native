#include "cutline/core/multicam.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/id.hpp"
#include "cutline/core/interpret.hpp"
#include "cutline/core/nesting.hpp"
#include "cutline/core/pool.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/sequences.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <unordered_set>

namespace cutline::core {
namespace {

const Media* find_media(const Project& p, std::string_view id) noexcept {
  const auto found = std::ranges::find(p.media, id, &Media::id);
  return found == p.media.end() ? nullptr : &*found;
}

/// Where a track sits among the tracks of its kind, counted the way Premiere
/// numbers them: video from the bottom, audio from the top.
std::optional<std::size_t> ordinal(const Sequence& s, std::size_t track) {
  if (track >= s.tracks.size()) return std::nullopt;
  const TrackKind kind = s.tracks[track].kind;
  std::vector<std::size_t> of_kind;
  for (std::size_t i = 0; i < s.tracks.size(); ++i) {
    if (s.tracks[i].kind == kind) of_kind.push_back(i);
  }
  if (kind == TrackKind::Video) std::ranges::reverse(of_kind);
  const auto at = std::ranges::find(of_kind, track);
  return static_cast<std::size_t>(std::distance(of_kind.begin(), at));
}

/// Whether a sound clip linked to a switched picture changes camera with it.
bool sound_follows(const Sequence& source, bool audio_follows) {
  return audio_follows || source.multicam_audio == MulticamAudio::Switch;
}

/// The clips a switch on `clip_id` changes: the clip, and its linked clips of
/// the same source when the sound follows.
std::vector<std::string> switched_with(const Project& p, std::string_view clip_id,
                                       bool audio_follows) {
  const Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr) return {};
  const Sequence* source = multicam_source(p, *clip);
  if (source == nullptr) return {};
  std::vector<std::string> ids{std::string(clip_id)};
  if (!sound_follows(*source, audio_follows)) return ids;
  for (const std::string& member : group_members(p, clip_id)) {
    const Clip* other = find_clip(p, member);
    if (member == clip_id || other == nullptr || other->media_id != clip->media_id) continue;
    if (other->kind != TrackKind::Audio) continue;
    ids.push_back(member);
  }
  return ids;
}

}  // namespace

const Sequence* multicam_source(const Project& p, const Clip& clip) noexcept {
  const Sequence* inner = nested_sequence(p, clip);
  return inner != nullptr && inner->multicam ? inner : nullptr;
}

bool shows_camera(const Project& p, const Clip& clip) noexcept {
  return !clip.multicam_off && multicam_source(p, clip) != nullptr;
}

std::vector<std::size_t> camera_tracks(const Sequence& s) {
  std::vector<std::size_t> tracks;
  for (std::size_t i = s.tracks.size(); i-- > 0;) {
    if (s.tracks[i].kind == TrackKind::Video) tracks.push_back(i);
  }
  return tracks;
}

std::vector<std::string> camera_names(const Sequence& s) {
  std::vector<std::string> names;
  for (const std::size_t track : camera_tracks(s)) {
    names.push_back(s.tracks[track].label.empty() ? std::format("Camera {}", names.size() + 1)
                                                  : s.tracks[track].label);
  }
  return names;
}

bool camera_uses_track(const Sequence& s, std::size_t track, int camera) {
  const std::optional<std::size_t> nth = ordinal(s, track);
  if (!nth.has_value()) return false;
  if (s.tracks[track].kind == TrackKind::Video) {
    return *nth == static_cast<std::size_t>(std::max(camera, 0));
  }
  if (s.multicam_audio == MulticamAudio::AllCameras) return true;
  // Past the last camera's track is sound recorded on its own.
  if (*nth >= camera_tracks(s).size()) return true;
  return *nth == static_cast<std::size_t>(std::max(camera, 0));
}

std::string camera_tag(const Project& p, const Clip& clip) {
  if (!shows_camera(p, clip)) return {};
  return std::format("[MC{}]", clip.camera + 1);
}

int camera_grid_columns(int cameras) noexcept {
  int columns = 1;
  while (columns * columns < cameras) ++columns;
  return columns;
}

Project camera_grid(const Project& p, std::string_view media_id, int width) {
  Project grid = p;
  const Media* media = find_media(p, media_id);
  const Sequence* source = media == nullptr ? nullptr : find_sequence(p, media->sequence_id);
  if (source == nullptr || !source->multicam) return grid;

  Sequence cells;
  cells.id = "multicam-grid";
  cells.name = source->name;
  cells.fps = source->fps;
  cells.colour = source->colour;
  cells.canvas_w = std::max(16, std::min(width, source->canvas_w));
  cells.canvas_h = std::max(16, static_cast<int>(std::lround(
                                    static_cast<double>(source->canvas_h) * cells.canvas_w /
                                    std::max(1, source->canvas_w))));
  const int cameras = static_cast<int>(camera_tracks(*source).size());
  const int columns = camera_grid_columns(cameras);
  const double length = sequence_duration(*source);
  for (int k = 0; k < cameras; ++k) {
    Clip cell;
    cell.id = std::format("camera{}", k + 1);
    cell.media_id = std::string(media_id);
    cell.source_in = 0.0;
    cell.source_out = length;
    cell.camera = k;
    cell.transform.x = (k % columns + 0.5) / columns;
    cell.transform.y = (k / columns + 0.5) / columns;
    cell.transform.scale_x = 1.0 / columns;
    cell.transform.scale_y = 1.0 / columns;
    Track track;
    track.id = cell.id;
    track.kind = TrackKind::Video;
    track.clips = {std::move(cell)};
    cells.tracks.push_back(std::move(track));
  }
  grid.sequences.push_back(std::move(cells));
  grid.open = grid.sequences.size() - 1;
  return grid;
}

// ------------------------------------------------------------------ making --

std::expected<std::vector<double>, std::string> multicam_starts(
    const Project& p, std::span<const std::string> media_ids, MulticamSync sync,
    bool ignore_hours) {
  if (sync == MulticamSync::Audio) {
    return std::unexpected(std::string("Synchronising by sound needs the sound decoded"));
  }
  // Each source's synchronise point, as a time within it.
  std::vector<double> points;
  for (const std::string& id : media_ids) {
    const Media* media = find_media(p, id);
    if (media == nullptr) return std::unexpected(std::format("{} is not in the project", id));
    switch (sync) {
      case MulticamSync::InPoints: points.push_back(media->in_point.value_or(0.0)); break;
      case MulticamSync::OutPoints:
        points.push_back(media->out_point.value_or(media->duration));
        break;
      case MulticamSync::Timecode: {
        if (!media->timecode.has_value()) {
          return std::unexpected(std::format("{} records no timecode", media->name));
        }
        // The point is the start of the file, so the clock's earlier reading
        // comes first: a negative point puts it later.
        double clock = *media->timecode;
        if (ignore_hours) clock = std::fmod(clock, 3600.0);
        points.push_back(-clock);
        break;
      }
      case MulticamSync::Audio: break;
    }
  }
  if (points.empty()) return std::vector<double>{};
  const double latest = std::ranges::max(points);
  std::vector<double> starts;
  for (const double point : points) starts.push_back(latest - point);
  return starts;
}

Project create_multicam_sequence(Project p, std::span<const std::string> media_ids,
                                 std::span<const double> starts, const MulticamOptions& options,
                                 std::string* made_media) {
  struct Source {
    const Media* media = nullptr;
    double start = 0.0;
  };
  std::vector<Source> cameras;
  std::vector<Source> sound_only;
  for (std::size_t i = 0; i < media_ids.size(); ++i) {
    const Media* media = find_media(p, media_ids[i]);
    if (media == nullptr || is_still_like(*media)) continue;
    const Source source{.media = media, .start = i < starts.size() ? starts[i] : 0.0};
    if (media->has_video) cameras.push_back(source);
    else if (media->audio_stream_count > 0) sound_only.push_back(source);
  }
  if (cameras.empty()) return p;

  // Pointers into the pool are about to be invalidated by adding to it, so
  // everything needed from the sources is copied out first.
  struct Taken {
    std::string id;
    std::string name;
    double length = 0.0;
    bool sound = false;
    double start = 0.0;
  };
  const auto take = [](const Source& s) {
    return Taken{.id = s.media->id,
                 .name = s.media->name,
                 .length = s.media->duration,
                 .sound = s.media->audio_stream_count > 0,
                 .start = s.start};
  };
  std::vector<Taken> shot;
  std::vector<Taken> heard;
  for (const Source& s : cameras) shot.push_back(take(s));
  for (const Source& s : sound_only) heard.push_back(take(s));

  // Shaped like the first camera, as Premiere's Automatic preset is.
  const Media& first = *cameras.front().media;
  Sequence s;
  s.id = new_id("seq");
  s.name = unused_sequence_name(
      p, options.name.empty() ? first.name + " Multicam" : options.name);
  s.canvas_w = first.width.value_or(p.sequence().canvas_w);
  s.canvas_h = first.height.value_or(p.sequence().canvas_h);
  const std::optional<double> rate = playback_fps(first);
  s.fps = rate.value_or(p.sequence().fps);
  s.drop_frame = p.sequence().drop_frame;
  s.colour = p.sequence().colour;
  s.multicam = true;
  s.multicam_audio = options.audio;

  const auto camera_name = [&](std::size_t k) {
    return options.naming == CameraNaming::ClipNames ? shot[k].name
                                                     : std::format("Camera {}", k + 1);
  };
  const auto clip_of = [](const Taken& t, TrackKind kind) {
    Clip c;
    c.id = new_id("clip");
    c.media_id = t.id;
    c.kind = kind;
    c.source_in = 0.0;
    c.source_out = t.length;
    c.start = t.start;
    return c;
  };

  // Stored top first, so the last camera's track goes first and Camera 1 is
  // the bottom one.
  for (std::size_t k = shot.size(); k-- > 0;) {
    Track track;
    track.id = new_id("track");
    track.kind = TrackKind::Video;
    track.label = camera_name(k);
    track.clips.push_back(clip_of(shot[k], TrackKind::Video));
    s.tracks.push_back(std::move(track));
  }
  // One audio track per camera, in camera order, even for a camera with no
  // sound, so that A1 is always the first camera's.
  for (std::size_t k = 0; k < shot.size(); ++k) {
    Track track;
    track.id = new_id("track");
    track.kind = TrackKind::Audio;
    track.label = camera_name(k);
    if (shot[k].sound) {
      Clip sound = clip_of(shot[k], TrackKind::Audio);
      // Linked to its picture, so the camera's two halves move together.
      const std::string group = new_id("group");
      sound.group_id = group;
      s.tracks[shot.size() - 1 - k].clips.front().group_id = group;
      track.clips.push_back(std::move(sound));
    }
    s.tracks.push_back(std::move(track));
  }
  for (const Taken& t : heard) {
    Track track;
    track.id = new_id("track");
    track.kind = TrackKind::Audio;
    track.label = t.name;
    track.clips.push_back(clip_of(t, TrackKind::Audio));
    s.tracks.push_back(std::move(track));
  }

  const std::string inner_id = s.id;
  const std::string bin = first.bin;
  p.sequences.push_back(std::move(s));

  // Its pool entry, beside the first camera's source as Premiere puts it.
  Media entry;
  entry.id = new_id("media");
  entry.sequence_id = inner_id;
  entry.bin = bin;
  if (made_media != nullptr) *made_media = entry.id;
  p.media.push_back(std::move(entry));
  p = sync_nested_media(std::move(p));

  if (options.move_to_processed) {
    std::string processed;
    for (const Bin& b : p.bins) {
      if (b.name == "Processed Clips" && b.parent.empty()) processed = b.id;
    }
    if (processed.empty()) {
      p = create_bin(std::move(p), "Processed Clips");
      processed = p.bins.back().id;
    }
    for (const Taken& t : shot) p = file_media(std::move(p), t.id, processed);
    for (const Taken& t : heard) p = file_media(std::move(p), t.id, processed);
  }
  return p;
}

// --------------------------------------------------------------- switching --

Project switch_camera(Project p, std::string_view clip_id, int camera, bool audio_follows) {
  const Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr) return p;
  const Sequence* source = multicam_source(p, *clip);
  if (source == nullptr) return p;
  const int count = static_cast<int>(camera_tracks(*source).size());
  if (camera < 0 || camera >= count) return p;
  for (const std::string& id : switched_with(p, clip_id, audio_follows)) {
    if (Clip* each = find_clip(p, id); each != nullptr) each->camera = camera;
  }
  return p;
}

Project record_camera_switches(Project p, std::string_view track_id,
                               std::span<const CameraSwitch> switches, bool audio_follows) {
  std::vector<CameraSwitch> sorted(switches.begin(), switches.end());
  std::ranges::stable_sort(sorted, {}, &CameraSwitch::time);

  // A cut lands on a frame, as every cut here does.
  const double fps = p.sequence().fps > 0.0 ? p.sequence().fps : 30.0;
  constexpr double kSame = 1e-6;

  for (const CameraSwitch& each : sorted) {
    const double time = std::round(each.time * fps) / fps;
    const auto track = std::ranges::find(p.sequence().tracks, track_id, &Track::id);
    if (track == p.sequence().tracks.end()) return p;
    const Clip* under = nullptr;
    for (const Clip& clip : track->clips) {
      if (clip.start <= time + kSame && time < clip_end(clip) - kSame) under = &clip;
    }
    if (under == nullptr || !shows_camera(p, *under)) continue;
    if (under->camera == each.camera) continue;
    const Sequence& source = *multicam_source(p, *under);
    if (each.camera < 0 || each.camera >= static_cast<int>(camera_tracks(source).size())) continue;

    std::string target = under->id;
    if (time > under->start + kSame) {
      // Cut it and whatever is linked to it, and switch the piece after.
      const std::vector<std::string> cut = group_members(p, under->id);
      p = split_at(std::move(p), time, cut);
      const auto again = std::ranges::find(p.sequence().tracks, track_id, &Track::id);
      const auto after = std::ranges::find_if(again->clips, [&](const Clip& clip) {
        return std::abs(clip.start - time) <= kSame;
      });
      if (after == again->clips.end()) continue;
      target = after->id;
    }
    p = switch_camera(std::move(p), target, each.camera, audio_follows);
  }
  return p;
}

Project set_multicam_enabled(Project p, std::span<const std::string> clip_ids, bool enabled) {
  for (const std::string& id : clip_ids) {
    Clip* clip = find_clip(p, id);
    if (clip == nullptr || multicam_source(p, *clip) == nullptr) continue;
    clip->multicam_off = !enabled;
  }
  return p;
}

Project flatten_multicam(Project p, std::span<const std::string> clip_ids) {
  const std::unordered_set<std::string> wanted(clip_ids.begin(), clip_ids.end());

  // What replaces each multicam, one lane per track of the source sequence it
  // was showing, gathered before anything is removed so lookups stay valid.
  struct Replacement {
    std::string clip_id;
    std::vector<std::vector<Clip>> lanes;
  };
  std::vector<Replacement> replacements;
  // A camera's picture and sound were linked inside the source sequence; once
  // flattened they are linked again, under a fresh group for each multicam
  // pair they came out of.
  std::map<std::string, std::string> regroup;

  for (const Track& track : p.sequence().tracks) {
    for (const Clip& clip : track.clips) {
      if (!wanted.contains(clip.id) || !shows_camera(p, clip)) continue;
      if (clip.reverse || is_time_remapped(clip) || clip.hold.has_value()) continue;
      const Sequence& inner = *multicam_source(p, clip);
      const double speed = clip_speed(clip);
      const std::string pair = clip.group_id.value_or(clip.id);
      Replacement made{.clip_id = clip.id, .lanes = {}};
      for (std::size_t t = 0; t < inner.tracks.size(); ++t) {
        if (inner.tracks[t].kind != clip.kind) continue;
        if (!camera_uses_track(inner, t, clip.camera)) continue;
        std::vector<Clip> lane;
        for (const Clip& piece : inner.tracks[t].clips) {
          // The part of it the multicam shows, in the source sequence's time.
          const double from = std::max(piece.start, clip.source_in);
          const double to = std::min(clip_end(piece), clip.source_out);
          if (!(to > from)) continue;
          Clip flat = piece;
          flat.id = new_id("clip");
          const SourceRange range = clip_sub_source(piece, from, to);
          flat.source_in = range.source_in;
          flat.source_out = range.source_out;
          flat.speed = clip_speed(piece) * speed;
          flat.start = clip.start + (from - clip.source_in) / speed;
          // A fade belongs to an edge, and these edges are new.
          if (from > piece.start) flat.fade_in = 0.0;
          if (to < clip_end(piece)) {
            flat.fade_out = 0.0;
            flat.transition_out.reset();
          }
          for (const ClipEffect& effect : clip.effects) flat.effects.push_back(effect);
          if (flat.label_color.empty()) flat.label_color = clip.label_color;
          flat.group_id.reset();
          if (piece.group_id.has_value()) {
            auto [at, fresh] = regroup.try_emplace(pair + "|" + *piece.group_id, std::string{});
            if (fresh) at->second = new_id("group");
            flat.group_id = at->second;
          }
          lane.push_back(std::move(flat));
        }
        if (!lane.empty()) made.lanes.push_back(std::move(lane));
      }
      replacements.push_back(std::move(made));
    }
  }

  Sequence& outer = p.sequence();
  const auto free_over = [](const Track& track, const std::vector<Clip>& lane) {
    for (const Clip& placed : track.clips) {
      for (const Clip& clip : lane) {
        if (placed.start < clip_end(clip) - 1e-9 && clip.start < clip_end(placed) - 1e-9) {
          return false;
        }
      }
    }
    return true;
  };
  for (Replacement& made : replacements) {
    std::size_t home = outer.tracks.size();
    for (std::size_t t = 0; t < outer.tracks.size(); ++t) {
      auto& clips = outer.tracks[t].clips;
      const auto at = std::ranges::find(clips, made.clip_id, &Clip::id);
      if (at == clips.end()) continue;
      clips.erase(at);
      home = t;
    }
    if (home == outer.tracks.size()) continue;
    const TrackKind kind = outer.tracks[home].kind;
    // The first lane where the multicam was. Any more, which only sound has,
    // on the next track of the kind with room, or a new one at the end.
    for (std::size_t lane = 0; lane < made.lanes.size(); ++lane) {
      std::size_t onto = outer.tracks.size();
      for (std::size_t t = home; t < outer.tracks.size(); ++t) {
        if (outer.tracks[t].kind != kind) continue;
        if (lane > 0 && t == home) continue;
        if (free_over(outer.tracks[t], made.lanes[lane])) {
          onto = t;
          break;
        }
      }
      if (onto == outer.tracks.size()) {
        Track fresh;
        fresh.id = new_id("track");
        fresh.kind = kind;
        // Video is stored top first, so a new one goes in front of the rest.
        if (kind == TrackKind::Video) {
          outer.tracks.insert(outer.tracks.begin(), std::move(fresh));
          onto = 0;
          ++home;
        } else {
          outer.tracks.push_back(std::move(fresh));
        }
      }
      for (Clip& clip : made.lanes[lane]) outer.tracks[onto].clips.push_back(std::move(clip));
      std::ranges::stable_sort(outer.tracks[onto].clips, {}, &Clip::start);
    }
  }
  return p;
}

}  // namespace cutline::core
