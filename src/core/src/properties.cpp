#include "cutline/core/properties.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace cutline::core {
namespace {

[[nodiscard]] Media* find_media(Project& p, std::string_view media_id) noexcept {
  const auto it = std::ranges::find(p.media, media_id, &Media::id);
  return it == p.media.end() ? nullptr : &*it;
}

[[nodiscard]] Track* find_track(Project& p, std::string_view track_id) noexcept {
  const auto it = std::ranges::find(p.tracks, track_id, &Track::id);
  return it == p.tracks.end() ? nullptr : &*it;
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
  for (Track& t : p.tracks) {
    for (Clip& c : t.clips) {
      if (ids.contains(c.id)) c.disabled = !enabled;
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
  const auto found = std::ranges::find(p.tracks, track_id, &Track::id);
  if (found == p.tracks.end() || found->kind != TrackKind::Audio) return nullptr;
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
  p.canvas_w = std::clamp(width, kMinCanvas, kMaxCanvas);
  p.canvas_h = std::clamp(height, kMinCanvas, kMaxCanvas);
  return p;
}

Project set_fps(Project p, double fps) {
  // A rate of zero would divide by nothing in every walk of the timeline, and a
  // negative one is reverse wearing the wrong name.
  p.fps = std::clamp(fps, kMinFps, kMaxFps);
  return p;
}

Project set_master_gain(Project p, double gain) {
  p.master_gain = std::clamp(gain, 0.0, kMaxMasterGain);
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

Project set_clip_speed(Project p, std::string_view clip_id, double speed,
                       std::optional<bool> reverse) {
  const std::vector<std::string> member_ids = group_members(p, clip_id);
  const std::unordered_set<std::string> members(member_ids.begin(), member_ids.end());
  const double clamped = std::clamp(speed, kMinSpeed, kMaxSpeed);

  for (Track& t : p.tracks) {
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
  p.tracks.insert(p.tracks.begin(), std::move(t));
  return p;
}

Project add_audio_track(Project p) {
  Track t;
  t.id = new_id("track");
  t.kind = TrackKind::Audio;
  p.tracks.push_back(std::move(t));
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
  std::erase_if(p.tracks, [&](const Track& t) { return t.id == track_id; });
  return p;
}

// ----------------------------------------------------------------- markers --

const Marker* marker_near(const Project& p, double time, double tolerance) noexcept {
  const Marker* best = nullptr;
  double best_distance = tolerance;
  for (const Marker& m : p.markers) {
    const double distance = std::abs(m.time - time);
    if (distance <= best_distance) {
      best_distance = distance;
      best = &m;
    }
  }
  return best;
}

Project add_marker(Project p, double time, std::string label, std::string color) {
  p.markers.push_back(Marker{
      .id = new_id("mark"),
      .time = time,
      .label = std::move(label),
      .color = std::move(color),
  });
  std::ranges::stable_sort(p.markers, {}, &Marker::time);
  return p;
}

Project remove_marker(Project p, std::string_view marker_id) {
  std::erase_if(p.markers, [&](const Marker& m) { return m.id == marker_id; });
  return p;
}

Project clear_markers(Project p) {
  p.markers.clear();
  return p;
}

const Marker* next_marker(const Project& p, double time) noexcept {
  const auto it = std::ranges::find_if(p.markers,
                                       [&](const Marker& m) { return m.time > time + 1e-4; });
  return it == p.markers.end() ? nullptr : &*it;
}

const Marker* previous_marker(const Project& p, double time) noexcept {
  const Marker* found = nullptr;
  for (const Marker& m : p.markers) {
    if (m.time < time - 1e-4) found = &m;
  }
  return found;
}

// ------------------------------------------------------------- in and out --

Project set_in_point(Project p, std::optional<double> time) {
  if (!time.has_value()) {
    p.in_point.reset();
    return p;
  }
  const double at = std::max(0.0, *time);
  p.in_point = at;
  // Cleared rather than pushed along. Somebody marking an in past the out has
  // moved on to a different span, and dragging the out after it would silently
  // keep a boundary they had stopped caring about.
  if (p.out_point.has_value() && *p.out_point <= at) p.out_point.reset();
  return p;
}

Project set_out_point(Project p, std::optional<double> time) {
  if (!time.has_value()) {
    p.out_point.reset();
    return p;
  }
  const double at = std::max(0.0, *time);
  p.out_point = at;
  if (p.in_point.has_value() && *p.in_point >= at) p.in_point.reset();
  return p;
}

Project clear_marks(Project p) {
  p.in_point.reset();
  p.out_point.reset();
  return p;
}

bool has_marks(const Project& p) noexcept {
  return p.in_point.has_value() || p.out_point.has_value();
}

MarkedSpan marked_span(const Project& p) noexcept {
  const double total = timeline_duration(p);
  const double start = std::clamp(p.in_point.value_or(0.0), 0.0, total);
  const double end = std::clamp(p.out_point.value_or(total), 0.0, total);
  return MarkedSpan{.start = start, .duration = std::max(0.0, end - start)};
}

// --------------------------------------------------------------- factories --

Project empty_project(int video_tracks, int audio_tracks) {
  Project p;
  for (int i = 0; i < video_tracks; ++i) {
    Track t;
    t.id = new_id("track");
    t.kind = TrackKind::Video;
    p.tracks.push_back(std::move(t));
  }
  for (int i = 0; i < audio_tracks; ++i) {
    Track t;
    t.id = new_id("track");
    t.kind = TrackKind::Audio;
    p.tracks.push_back(std::move(t));
  }
  return p;
}

}  // namespace cutline::core
