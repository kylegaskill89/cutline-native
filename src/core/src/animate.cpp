#include "cutline/core/animate.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>

namespace cutline::core {

Project set_keyframe(Project p, std::string_view clip_id, AnimProp prop, double local_t,
                     double v) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  upsert_keyframe(c->keyframes[anim_prop_index(prop)], local_t, v);
  return p;
}

Interp keyframe_interp_of(const Clip& c, AnimProp prop) noexcept {
  return keyframe_list_interp(c.keyframes[anim_prop_index(prop)]);
}

Project set_keyframe_interp(Project p, std::string_view clip_id, AnimProp prop, Interp mode) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  set_keyframe_list_interp(c->keyframes[anim_prop_index(prop)], mode);
  return p;
}

Project remove_keyframe_at(Project p, std::string_view clip_id, AnimProp prop, double local_t) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  remove_keyframe_near(c->keyframes[anim_prop_index(prop)], local_t);
  return p;
}

Project clear_keyframes(Project p, std::string_view clip_id, AnimProp prop) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->keyframes[anim_prop_index(prop)].clear();
  return p;
}

Project set_gain_keyframe(Project p, std::string_view clip_id, double local_t, double v) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  upsert_keyframe(c->gain_keyframes, local_t, std::clamp(v, 0.0, kMaxGain));
  return p;
}

Interp gain_keyframe_interp_of(const Clip& c) noexcept {
  return keyframe_list_interp(c.gain_keyframes);
}

Project set_gain_keyframe_interp(Project p, std::string_view clip_id, Interp mode) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  set_keyframe_list_interp(c->gain_keyframes, mode);
  return p;
}

Project remove_gain_keyframe_at(Project p, std::string_view clip_id, double local_t) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  remove_keyframe_near(c->gain_keyframes, local_t);
  return p;
}

Project move_gain_keyframe(Project p, std::string_view clip_id, double from_t, double to_t,
                           double v) {
  p = remove_gain_keyframe_at(std::move(p), clip_id, from_t);
  return set_gain_keyframe(std::move(p), clip_id, to_t, v);
}

Project clear_gain_keyframes(Project p, std::string_view clip_id) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->gain_keyframes.clear();
  return p;
}

namespace {

/// The track by id, or null. Its own helper because every function below wants
/// it and `find_clip` has no counterpart in this header's neighbourhood.
[[nodiscard]] Track* find_track_for(Project& p, std::string_view track_id) noexcept {
  const auto found = std::ranges::find(p.sequence().tracks, track_id, &Track::id);
  return found == p.sequence().tracks.end() ? nullptr : &*found;
}

}  // namespace

Project set_track_gain_keyframe(Project p, std::string_view track_id, double time, double v) {
  Track* t = find_track_for(p, track_id);
  if (t == nullptr) return p;
  upsert_keyframe(t->gain_keyframes, std::max(0.0, time), std::clamp(v, 0.0, kMaxGain));
  return p;
}

Project set_track_pan_keyframe(Project p, std::string_view track_id, double time, double v) {
  Track* t = find_track_for(p, track_id);
  if (t == nullptr) return p;
  upsert_keyframe(t->pan_keyframes, std::max(0.0, time), std::clamp(v, -1.0, 1.0));
  return p;
}

Project remove_track_gain_keyframe_at(Project p, std::string_view track_id, double time) {
  Track* t = find_track_for(p, track_id);
  if (t == nullptr) return p;
  remove_keyframe_near(t->gain_keyframes, time);
  return p;
}

Project clear_track_gain_keyframes(Project p, std::string_view track_id) {
  Track* t = find_track_for(p, track_id);
  if (t == nullptr) return p;
  t->gain_keyframes.clear();
  return p;
}

Project clear_track_pan_keyframes(Project p, std::string_view track_id) {
  Track* t = find_track_for(p, track_id);
  if (t == nullptr) return p;
  t->pan_keyframes.clear();
  return p;
}

Project set_track_automation(Project p, std::string_view track_id, AutomationMode mode) {
  Track* t = find_track_for(p, track_id);
  if (t == nullptr) return p;
  t->automation = mode;
  return p;
}

namespace {

/// Lays a pass over a curve, replacing only what it covers.
///
/// Shared by the tracks and the master rather than written twice: punching in
/// means the same thing wherever the fader is, and two copies of that rule is
/// one copy and a future disagreement.
void lay_pass_over(std::vector<Keyframe>& curve, std::span<const Keyframe> pass) {
  if (pass.empty()) return;
  const double from = pass.front().t;
  const double to = pass.back().t;

  // Inclusive at both ends, so a keyframe exactly under where the hand started
  // is replaced rather than left sitting a fraction of a frame from the one
  // that replaced it.
  std::vector<Keyframe> kept;
  kept.reserve(curve.size() + pass.size());
  for (const Keyframe& key : curve) {
    if (key.t < from || key.t > to) kept.push_back(key);
  }
  kept.insert(kept.end(), pass.begin(), pass.end());
  std::ranges::sort(kept, {}, &Keyframe::t);
  curve = std::move(kept);
}

}  // namespace

Project set_master_automation(Project p, AutomationMode mode) {
  p.sequence().master_automation = mode;
  return p;
}

Project clear_master_gain_keyframes(Project p) {
  p.sequence().master_gain_keyframes.clear();
  return p;
}

Project write_master_gain_pass(Project p, std::span<const Keyframe> pass) {
  lay_pass_over(p.sequence().master_gain_keyframes, pass);
  return p;
}

Project write_track_gain_pass(Project p, std::string_view track_id,
                              std::span<const Keyframe> pass) {
  Track* t = find_track_for(p, track_id);
  if (t == nullptr) return p;
  lay_pass_over(t->gain_keyframes, pass);
  return p;
}

}  // namespace cutline::core
