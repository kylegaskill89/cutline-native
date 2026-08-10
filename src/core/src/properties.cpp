#include "cutline/core/properties.hpp"

#include "cutline/core/interpret.hpp"

#include "ripple.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cutline::core {
namespace {

[[nodiscard]] Media* find_media(Project& p, std::string_view media_id) noexcept {
  const auto it = std::ranges::find(p.media, media_id, &Media::id);
  return it == p.media.end() ? nullptr : &*it;
}

[[nodiscard]] Track* find_track(Project& p, std::string_view track_id) noexcept {
  const auto it = std::ranges::find(p.sequence().tracks, track_id, &Track::id);
  return it == p.sequence().tracks.end() ? nullptr : &*it;
}

/// How close two times have to be to count as the same instant. A clip that
/// starts where another ends is touching it, and floating-point arithmetic on
/// frame boundaries does not land on the same double twice.
constexpr double kTouchEps = 1e-3;

/// The clip as it would be with no hold, for asking which frame plays at a
/// time. Without this, moving a hold would keep answering with the old frame.
[[nodiscard]] Clip strip_hold(Clip c) noexcept {
  c.hold.reset();
  return c;
}

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
  constexpr std::string_view ws = " \t\n\r\f\v";
  const auto first = s.find_first_not_of(ws);
  if (first == std::string_view::npos) return {};
  return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

}  // namespace

// -------------------------------------------------------- clip properties --

Project set_clips_enabled(Project p, std::span<const std::string> clip_ids, bool enabled) {
  const std::unordered_set<std::string> ids(clip_ids.begin(), clip_ids.end());
  for (Track& t : p.sequence().tracks) {
    for (Clip& c : t.clips) {
      if (ids.contains(c.id)) c.disabled = !enabled;
    }
  }
  return p;
}

Project set_clips_label(Project p, std::span<const std::string> clip_ids,
                        std::string color) {
  const std::unordered_set<std::string> ids(clip_ids.begin(), clip_ids.end());
  for (Track& t : p.sequence().tracks) {
    for (Clip& c : t.clips) {
      if (ids.contains(c.id)) c.label_color = color;
    }
  }
  return p;
}

Project set_clip_blend(Project p, std::string_view clip_id, BlendMode mode) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->blend = mode;
  return p;
}

Project set_clip_transition(Project p, std::string_view clip_id,
                            std::optional<Transition> transition) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  if (transition.has_value() && transition->duration > 0.0) {
    c->transition_out = *transition;
  } else {
    c->transition_out.reset();
  }
  return p;
}

Project set_clip_gain(Project p, std::string_view clip_id, double gain) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->gain = std::clamp(gain, 0.0, kMaxGain);
  return p;
}

Project set_clip_pan(Project p, std::string_view clip_id, double pan) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->pan = std::clamp(pan, -1.0, 1.0);
  return p;
}

namespace {

[[nodiscard]] Track* find_audio_track(Project& p, std::string_view track_id) {
  const auto found = std::ranges::find(p.sequence().tracks, track_id, &Track::id);
  if (found == p.sequence().tracks.end() || found->kind != TrackKind::Audio) return nullptr;
  return &*found;
}

}  // namespace

Project set_track_gain(Project p, std::string_view track_id, double gain) {
  Track* track = find_audio_track(p, track_id);
  if (track == nullptr) return p;
  track->gain = std::clamp(gain, 0.0, kMaxGain);
  return p;
}

Project set_track_pan(Project p, std::string_view track_id, double pan) {
  Track* track = find_audio_track(p, track_id);
  if (track == nullptr) return p;
  track->pan = std::clamp(pan, -1.0, 1.0);
  return p;
}

Project set_canvas(Project p, int width, int height) {
  p.sequence().canvas_w = std::clamp(width, kMinCanvas, kMaxCanvas);
  p.sequence().canvas_h = std::clamp(height, kMinCanvas, kMaxCanvas);
  return p;
}

Project set_fps(Project p, double fps) {
  // A rate of zero would divide by nothing in every walk of the timeline, and a
  // negative one is reverse wearing the wrong name.
  p.sequence().fps = std::clamp(fps, kMinFps, kMaxFps);
  return p;
}

Project match_sequence_to(Project p, std::string_view media_id) {
  // Anything at all placed, and the question is closed: the sequence has a
  // shape somebody has been working to.
  for (const Track& t : p.sequence().tracks) {
    if (!t.clips.empty()) return p;
  }

  const Media* media = nullptr;
  for (const Media& m : p.media) {
    if (m.id == media_id) media = &m;
  }
  if (media == nullptr) return p;

  // Generated sources take the canvas's shape rather than giving it one, and a
  // still has no rate to give. Both are the same test: only real footage knows
  // what it is.
  if (media->is_text || media->is_color || media->is_adjustment) return p;

  if (media->width.has_value() && media->height.has_value() && *media->width > 0 &&
      *media->height > 0) {
    p = set_canvas(std::move(p), *media->width, *media->height);
  }
  // The rate it is *played* at, not the rate the file claims. Footage conformed
  // to 24 and then used to shape a sequence should give that sequence 24 —
  // matching it to the file's 60 would put every frame of the source between
  // two of the sequence's, which is the one arrangement matching exists to
  // avoid.
  if (const std::optional<double> rate = playback_fps(*media);
      rate.has_value() && *rate > 0.0) {
    p = set_fps(std::move(p), *rate);
  }
  return p;
}

Project set_master_gain(Project p, double gain) {
  p.sequence().master_gain = std::clamp(gain, 0.0, kMaxMasterGain);
  return p;
}

Project set_clip_fade(Project p, std::string_view clip_id, ClipEdge edge, double duration) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;

  const double length = clip_duration(*c);
  const double other = edge == ClipEdge::In ? c->fade_out : c->fade_in;
  const double value = std::max(0.0, std::min(duration, length - other));
  if (edge == ClipEdge::In) {
    c->fade_in = value;
  } else {
    c->fade_out = value;
  }
  return p;
}

Project set_clip_channel_map(Project p, std::string_view clip_id, std::vector<int> map) {
  const std::vector<std::string> member_ids = group_members(p, clip_id);
  const std::unordered_set<std::string> members(member_ids.begin(), member_ids.end());

  for (Track& t : p.sequence().tracks) {
    for (Clip& c : t.clips) {
      // The audio clips of the group only. A picture has no channels, and
      // giving it a map would be a field that means nothing and reads as
      // something somebody set.
      if (!members.contains(c.id) || c.kind != TrackKind::Audio) continue;
      c.channel_map = map;
    }
  }
  return p;
}

Project set_clip_speed(Project p, std::string_view clip_id, double speed,
                       std::optional<bool> reverse) {
  const std::vector<std::string> member_ids = group_members(p, clip_id);
  const std::unordered_set<std::string> members(member_ids.begin(), member_ids.end());
  const double clamped = std::clamp(speed, kMinSpeed, kMaxSpeed);

  for (Track& t : p.sequence().tracks) {
    for (Clip& c : t.clips) {
      if (!members.contains(c.id)) continue;
      c.speed = clamped;
      if (reverse.has_value()) c.reverse = *reverse;
      // Retiming can shorten the clip out from under its fades.
      const double length = clip_duration(c);
      c.fade_in = std::min(c.fade_in, length);
      c.fade_out = std::min(c.fade_out, length - c.fade_in);
    }
    std::ranges::stable_sort(t.clips, {}, &Clip::start);
  }
  return p;
}

Project set_clips_speed(Project p, std::span<const std::string> clip_ids, double speed,
                        std::optional<bool> reverse, bool ripple) {
  // Through the groups, so a picture and its sound retime together however the
  // selection was made — clicking one of a linked pair is the usual way in.
  std::unordered_set<std::string> members;
  for (const std::string& id : clip_ids) {
    for (std::string& member : group_members(p, id)) members.insert(std::move(member));
  }
  if (members.empty()) return p;

  // Where each retime ends and by how much it moves that point. Collected
  // before anything changes, because after the retime the old end is gone.
  //
  // Keyed by the old end and taking the largest delta there: a linked pair is
  // two clips ending at the same time by the same amount, and counting both
  // would shift the sequence twice for one edit.
  std::vector<std::pair<double, double>> shifts;

  for (Track& t : p.sequence().tracks) {
    for (Clip& c : t.clips) {
      if (!members.contains(c.id)) continue;
      const double was_end = clip_end(c);
      const double was = clip_duration(c);

      c.speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
      if (reverse.has_value()) c.reverse = *reverse;
      // Retiming can shorten the clip out from under its fades.
      const double length = clip_duration(c);
      c.fade_in = std::min(c.fade_in, length);
      c.fade_out = std::min(c.fade_out, length - c.fade_in);

      if (ripple && length != was) shifts.emplace_back(was_end, length - was);
    }
  }

  if (ripple) p = ripple_after(std::move(p), std::move(shifts), members);

  for (Track& t : p.sequence().tracks) std::ranges::stable_sort(t.clips, {}, &Clip::start);
  return p;
}

Project set_clips_hold(Project p, std::span<const std::string> clip_ids,
                       std::optional<double> at_timeline_time) {
  const std::unordered_set<std::string> ids(clip_ids.begin(), clip_ids.end());
  for (Track& t : p.sequence().tracks) {
    // Video only. A held frame with its sound still running is the effect, and
    // freezing the audio's "source time" would mean a stuck sample rather than
    // silence — a noise nobody asked for.
    if (t.kind != TrackKind::Video) continue;
    for (Clip& c : t.clips) {
      if (!ids.contains(c.id)) continue;
      if (!at_timeline_time.has_value()) {
        c.hold.reset();
        continue;
      }
      // Clamped into the clip, so asking to hold at a playhead that is not over
      // this member of the group still freezes it on a frame it owns rather
      // than on one outside its own trim.
      const double at = std::clamp(*at_timeline_time, c.start, clip_end(c));
      c.hold = source_time_at(strip_hold(c), at);
    }
  }
  return p;
}

Project set_clip_opacity(Project p, std::string_view clip_id, double opacity) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->opacity = std::clamp(opacity, 0.0, 1.0);
  return p;
}

Project set_clip_transform(Project p, std::string_view clip_id, Transform transform) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->transform = transform;
  return p;
}

// ------------------------------------------------------- generated media --

Project set_text_spec(Project p, std::string_view media_id, TextSpec spec) {
  Media* m = find_media(p, media_id);
  if (m == nullptr || !m->is_text) return p;
  m->text = std::move(spec);
  return p;
}

Project set_matte_color(Project p, std::string_view media_id, std::string color) {
  Media* m = find_media(p, media_id);
  if (m == nullptr || !m->is_color) return p;
  m->color = std::move(color);
  return p;
}

Project set_matte_gradient(Project p, std::string_view media_id,
                           std::optional<MatteGradient> gradient) {
  Media* m = find_media(p, media_id);
  if (m == nullptr || !m->is_color) return p;
  m->gradient = std::move(gradient);
  return p;
}

// ------------------------------------------------------------------ tracks --

Project add_video_track(Project p) {
  Track t;
  t.id = new_id("track");
  t.kind = TrackKind::Video;
  p.sequence().tracks.insert(p.sequence().tracks.begin(), std::move(t));
  return p;
}

Project add_audio_track(Project p) {
  Track t;
  t.id = new_id("track");
  t.kind = TrackKind::Audio;
  p.sequence().tracks.push_back(std::move(t));
  return p;
}

Project set_track_label(Project p, std::string_view track_id, std::string label) {
  Track* t = find_track(p, track_id);
  if (t == nullptr) return p;
  t->label = std::string(trim(label));
  return p;
}

Project update_track(Project p, std::string_view track_id, const TrackPropsPatch& patch) {
  Track* t = find_track(p, track_id);
  if (t == nullptr) return p;
  if (patch.muted.has_value()) t->muted = *patch.muted;
  if (patch.solo.has_value()) t->solo = *patch.solo;
  if (patch.locked.has_value()) t->locked = *patch.locked;
  if (patch.hidden.has_value()) t->hidden = *patch.hidden;
  if (patch.targeted.has_value()) t->targeted = *patch.targeted;
  if (patch.sync_locked.has_value()) t->sync_locked = *patch.sync_locked;
  if (patch.height.has_value()) t->height = *patch.height;
  return p;
}

Project set_track_height(Project p, std::string_view track_id, std::optional<double> height) {
  Track* t = find_track(p, track_id);
  if (t == nullptr) return p;
  t->height = height;
  return p;
}

Project remove_track(Project p, std::string_view track_id) {
  std::erase_if(p.sequence().tracks, [&](const Track& t) { return t.id == track_id; });
  return p;
}

// ----------------------------------------------------------------- markers --

const Marker* marker_near(const Project& p, double time, double tolerance) noexcept {
  const Marker* best = nullptr;
  double best_distance = tolerance;
  for (const Marker& m : p.sequence().markers) {
    const double distance = std::abs(m.time - time);
    if (distance <= best_distance) {
      best_distance = distance;
      best = &m;
    }
  }
  return best;
}

Project add_marker(Project p, double time, std::string label, std::string color) {
  p.sequence().markers.push_back(Marker{
      .id = new_id("mark"),
      .time = time,
      .label = std::move(label),
      .color = std::move(color),
  });
  std::ranges::stable_sort(p.sequence().markers, {}, &Marker::time);
  return p;
}

Project set_marker(Project p, std::string_view marker_id, std::string label,
                   std::string comment, std::string color, double duration) {
  const auto it = std::ranges::find(p.sequence().markers, marker_id, &Marker::id);
  if (it == p.sequence().markers.end()) return p;
  it->label = std::move(label);
  it->comment = std::move(comment);
  it->color = std::move(color);
  // Never negative: a marker that ended before it began would draw backwards
  // and mean nothing. Zero is a point, which is what most markers are.
  it->duration = std::max(0.0, duration);
  return p;
}

Project remove_marker(Project p, std::string_view marker_id) {
  std::erase_if(p.sequence().markers, [&](const Marker& m) { return m.id == marker_id; });
  return p;
}

Project clear_markers(Project p) {
  p.sequence().markers.clear();
  return p;
}

const Marker* next_marker(const Project& p, double time) noexcept {
  const auto it = std::ranges::find_if(p.sequence().markers,
                                       [&](const Marker& m) { return m.time > time + 1e-4; });
  return it == p.sequence().markers.end() ? nullptr : &*it;
}

const Marker* previous_marker(const Project& p, double time) noexcept {
  const Marker* found = nullptr;
  for (const Marker& m : p.sequence().markers) {
    if (m.time < time - 1e-4) found = &m;
  }
  return found;
}

// ------------------------------------------------------------- in and out --

Project set_in_point(Project p, std::optional<double> time) {
  if (!time.has_value()) {
    p.sequence().in_point.reset();
    return p;
  }
  const double at = std::max(0.0, *time);
  p.sequence().in_point = at;
  // Cleared rather than pushed along. Somebody marking an in past the out has
  // moved on to a different span, and dragging the out after it would silently
  // keep a boundary they had stopped caring about.
  if (p.sequence().out_point.has_value() && *p.sequence().out_point <= at) p.sequence().out_point.reset();
  return p;
}

Project set_out_point(Project p, std::optional<double> time) {
  if (!time.has_value()) {
    p.sequence().out_point.reset();
    return p;
  }
  const double at = std::max(0.0, *time);
  p.sequence().out_point = at;
  if (p.sequence().in_point.has_value() && *p.sequence().in_point >= at) p.sequence().in_point.reset();
  return p;
}

Project clear_marks(Project p) {
  p.sequence().in_point.reset();
  p.sequence().out_point.reset();
  return p;
}

// ------------------------------------------------------ marks on a source --

namespace {

/// The media to mark, or null when the id names nothing.
[[nodiscard]] Media* media_to_mark(Project& p, std::string_view media_id) {
  const auto it = std::ranges::find_if(p.media, [&](const Media& m) { return m.id == media_id; });
  return it == p.media.end() ? nullptr : &*it;
}

}  // namespace

Project set_source_in_point(Project p, std::string_view media_id, std::optional<double> time) {
  Media* media = media_to_mark(p, media_id);
  if (media == nullptr) return p;
  if (!time.has_value()) {
    media->in_point.reset();
    return p;
  }
  // Clamped to the source, which the sequence marks have no equivalent of: a
  // sequence grows to hold whatever is put in it, a file is the length it is.
  const double at = std::clamp(*time, 0.0, media->duration);
  media->in_point = at;
  if (media->out_point.has_value() && *media->out_point <= at) media->out_point.reset();
  return p;
}

Project set_source_out_point(Project p, std::string_view media_id, std::optional<double> time) {
  Media* media = media_to_mark(p, media_id);
  if (media == nullptr) return p;
  if (!time.has_value()) {
    media->out_point.reset();
    return p;
  }
  const double at = std::clamp(*time, 0.0, media->duration);
  media->out_point = at;
  if (media->in_point.has_value() && *media->in_point >= at) media->in_point.reset();
  return p;
}

Project clear_source_marks(Project p, std::string_view media_id) {
  if (Media* media = media_to_mark(p, media_id); media != nullptr) {
    media->in_point.reset();
    media->out_point.reset();
  }
  return p;
}

Project set_proxy_path(Project p, std::string_view media_id, std::string path) {
  if (Media* media = find_media(p, media_id); media != nullptr) {
    media->proxy_path = std::move(path);
  }
  return p;
}

Project set_drop_frame(Project p, bool drop_frame) {
  p.sequence().drop_frame = drop_frame;
  return p;
}

Project set_media_label(Project p, std::string_view media_id, std::string color) {
  if (Media* media = find_media(p, media_id); media != nullptr) {
    media->label_color = std::move(color);
  }
  return p;
}

Project set_use_proxies(Project p, bool use) {
  p.use_proxies = use;
  return p;
}

std::size_t proxy_count(const Project& p) noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(p.media, [](const Media& m) { return !m.proxy_path.empty(); }));
}

bool has_marks(const Project& p) noexcept {
  return p.sequence().in_point.has_value() || p.sequence().out_point.has_value();
}

MarkedSpan marked_span(const Project& p) noexcept {
  const double total = timeline_duration(p);
  const double start = std::clamp(p.sequence().in_point.value_or(0.0), 0.0, total);
  const double end = std::clamp(p.sequence().out_point.value_or(total), 0.0, total);
  return MarkedSpan{.start = start, .duration = std::max(0.0, end - start)};
}

MarkedSpan playback_span(const Project& p, double preroll, double postroll) noexcept {
  const MarkedSpan marked = marked_span(p);
  const double total = timeline_duration(p);

  // A negative or non-finite number would move the start past the end and turn
  // a run-up into a skip. Taken as none rather than refused: this comes from a
  // preference file somebody can edit, and there is nothing to fail at here.
  const double before = std::isfinite(preroll) ? std::max(0.0, preroll) : 0.0;
  const double after = std::isfinite(postroll) ? std::max(0.0, postroll) : 0.0;

  const double start = std::max(0.0, marked.start - before);
  const double end = std::min(total, marked.start + marked.duration + after);
  return MarkedSpan{.start = start, .duration = std::max(0.0, end - start)};
}

// --------------------------------------------------------------- factories --

Project empty_project(int video_tracks, int audio_tracks) {
  Project p;
  for (int i = 0; i < video_tracks; ++i) {
    Track t;
    t.id = new_id("track");
    t.kind = TrackKind::Video;
    p.sequence().tracks.push_back(std::move(t));
  }
  for (int i = 0; i < audio_tracks; ++i) {
    Track t;
    t.id = new_id("track");
    t.kind = TrackKind::Audio;
    p.sequence().tracks.push_back(std::move(t));
  }
  return p;
}

}  // namespace cutline::core
