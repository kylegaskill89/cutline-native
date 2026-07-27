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

}  // namespace cutline::core
