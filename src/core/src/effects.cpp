#include "cutline/core/effects.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace cutline::core {
namespace {

/// The effect at `index` on the clip, or null when either is out of reach.
template <typename StackT>
[[nodiscard]] auto* effect_at(StackT& stack, std::size_t index) {
  return index < stack.size() ? &stack[index] : nullptr;
}

[[nodiscard]] ClipEffect* find_effect(Project& p, std::string_view clip_id, std::size_t index) {
  Clip* c = find_clip(p, clip_id);
  return c == nullptr ? nullptr : effect_at(c->effects, index);
}

[[nodiscard]] AudioClipEffect* find_audio_effect(Project& p, std::string_view clip_id,
                                                 std::size_t index) {
  Clip* c = find_clip(p, clip_id);
  return c == nullptr ? nullptr : effect_at(c->audio_effects, index);
}

/// Swaps the entry at `index` with its neighbour in `direction`.
template <typename StackT>
bool move_within(StackT& stack, std::size_t index, int direction) {
  if (index >= stack.size() || direction == 0) return false;
  const auto target = static_cast<long long>(index) + direction;
  if (target < 0 || target >= static_cast<long long>(stack.size())) return false;
  std::swap(stack[index], stack[static_cast<std::size_t>(target)]);
  return true;
}

}  // namespace

// ----------------------------------------------------------------- queries --

double Mask::* mask_param_field(std::string_view key) noexcept {
  // Written down once, so reading a mask number and writing it cannot come to
  // disagree about which field a name means.
  if (key == "mask.x") return &Mask::x;
  if (key == "mask.y") return &Mask::y;
  if (key == "mask.width") return &Mask::width;
  if (key == "mask.height") return &Mask::height;
  if (key == "mask.rotation") return &Mask::rotation;
  if (key == "mask.feather") return &Mask::feather;
  if (key == "mask.opacity") return &Mask::opacity;
  // Not the shape and not `inverted`: neither is a number, and a keyframe
  // between two of them would have to mean something halfway between an ellipse
  // and a rectangle.
  return nullptr;
}

std::span<const std::string_view> mask_param_keys() noexcept {
  static constexpr std::array<std::string_view, 7> kKeys{
      "mask.x",        "mask.y",       "mask.width",  "mask.height",
      "mask.rotation", "mask.feather", "mask.opacity"};
  return kKeys;
}

bool is_effect_param_animated(const ClipEffect& effect, std::string_view key) noexcept {
  const auto it = effect.keyframes.find(std::string(key));
  return it != effect.keyframes.end() && !it->second.empty();
}

double effect_param_at(const ClipEffect& effect, std::string_view key, double local_t) noexcept {
  const auto animated = effect.keyframes.find(std::string(key));
  if (animated != effect.keyframes.end() && !animated->second.empty()) {
    return eval_keyframes(animated->second, local_t);
  }
  // A mask's numbers live on the mask, which is their one home. Only their
  // keyframes are kept under a parameter name.
  if (double Mask::* field = mask_param_field(key); field != nullptr) {
    return effect.mask.*field;
  }

  const auto stat = effect.params.find(std::string(key));
  return stat != effect.params.end() ? stat->second : 0.0;
}

std::vector<ClipEffect> resolved_effects(const Clip& c, double local_t) {
  std::vector<ClipEffect> out;
  out.reserve(c.effects.size());
  for (const ClipEffect& effect : c.effects) {
    ClipEffect resolved = effect;
    for (const auto& [key, kfs] : effect.keyframes) {
      if (kfs.empty()) continue;
      // A mask's animation is folded onto the mask rather than into the
      // parameters, because that is where everything downstream reads it — the
      // renderer, the shapes drawn on the picture, and the pass planner.
      if (double Mask::* field = mask_param_field(key); field != nullptr) {
        resolved.mask.*field = eval_keyframes(kfs, local_t);
        continue;
      }
      resolved.params[key] = eval_keyframes(kfs, local_t);
    }
    // The renderer reads params only, so the animation is fully folded away.
    resolved.keyframes.clear();
    out.push_back(std::move(resolved));
  }
  return out;
}

bool clip_has_effect_keyframes(const Clip& c) noexcept {
  return std::ranges::any_of(c.effects, [](const ClipEffect& e) {
    return std::ranges::any_of(e.keyframes,
                               [](const auto& entry) { return !entry.second.empty(); });
  });
}

std::vector<double> effect_keyframe_times(const Clip& c) {
  std::vector<double> out;
  for (const ClipEffect& effect : c.effects) {
    for (const auto& [key, kfs] : effect.keyframes) {
      for (const Keyframe& k : kfs) out.push_back(k.t);
    }
  }
  return out;
}

// ------------------------------------------------------------- stack edits --

Project add_clip_effect(Project p, std::string_view clip_id, std::string type,
                        std::map<std::string, double> params,
                        std::map<std::string, std::string> colors) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->effects.push_back(ClipEffect{
      .type = std::move(type),
      .params = std::move(params),
      .colors = std::move(colors),
  });
  return p;
}

Project remove_clip_effect(Project p, std::string_view clip_id, std::size_t index) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr || index >= c->effects.size()) return p;
  c->effects.erase(c->effects.begin() + static_cast<std::ptrdiff_t>(index));
  return p;
}

Project move_clip_effect(Project p, std::string_view clip_id, std::size_t index, int direction) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  move_within(c->effects, index, direction);
  return p;
}

Project append_clip_effects(Project p, std::string_view clip_id,
                            std::span<const ClipEffect> effects) {
  if (effects.empty()) return p;
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->effects.insert(c->effects.end(), effects.begin(), effects.end());
  return p;
}

Project clear_clip_effects(Project p, std::string_view clip_id) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->effects.clear();
  return p;
}

Project toggle_clip_effect(Project p, std::string_view clip_id, std::size_t index) {
  ClipEffect* effect = find_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  effect->enabled = !effect->enabled;
  return p;
}

Project set_clip_effect_param(Project p, std::string_view clip_id, std::size_t index,
                              std::string key, double value) {
  ClipEffect* effect = find_effect(p, clip_id, index);
  if (effect == nullptr) return p;

  // A mask number goes to the mask. Everything that sets an effect parameter
  // comes through here, so this is what lets a mask be animated by exactly the
  // machinery an ordinary parameter is — one reserved name and no new path.
  if (double Mask::* field = mask_param_field(key); field != nullptr) {
    effect->mask.*field = value;
    return p;
  }

  effect->params[std::move(key)] = value;
  return p;
}

Project set_clip_effect_color(Project p, std::string_view clip_id, std::size_t index,
                              std::string key, std::string value) {
  ClipEffect* effect = find_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  effect->colors[std::move(key)] = std::move(value);
  return p;
}

Project set_effect_mask(Project p, std::string_view clip_id, std::size_t index, Mask mask) {
  ClipEffect* effect = find_effect(p, clip_id, index);
  if (effect == nullptr) return p;

  // Clamped where a value has no meaning outside a range. Position is not:
  // a mask parked off the layer is a perfectly ordinary way to animate one in
  // from the edge, and it is the caller's business.
  mask.width = std::max(0.0, mask.width);
  mask.height = std::max(0.0, mask.height);
  mask.feather = std::max(0.0, mask.feather);
  mask.opacity = std::clamp(mask.opacity, 0.0, 1.0);

  effect->mask = std::move(mask);
  return p;
}

// -------------------------------------------------- effect param keyframes --

Project set_effect_keyframe(Project p, std::string_view clip_id, std::size_t index,
                            std::string key, double local_t, double v) {
  ClipEffect* effect = find_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  upsert_keyframe(effect->keyframes[std::move(key)], local_t, v);
  return p;
}

Interp effect_keyframe_interp_of(const ClipEffect& effect, std::string_view key) noexcept {
  const auto it = effect.keyframes.find(std::string(key));
  return it == effect.keyframes.end() ? Interp::Linear : keyframe_list_interp(it->second);
}

Project set_effect_keyframe_interp(Project p, std::string_view clip_id, std::size_t index,
                                   std::string_view key, Interp mode) {
  ClipEffect* effect = find_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  const auto it = effect->keyframes.find(std::string(key));
  if (it == effect->keyframes.end()) return p;
  set_keyframe_list_interp(it->second, mode);
  return p;
}

Project remove_effect_keyframe_at(Project p, std::string_view clip_id, std::size_t index,
                                  std::string_view key, double local_t) {
  ClipEffect* effect = find_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  const auto it = effect->keyframes.find(std::string(key));
  if (it == effect->keyframes.end()) return p;
  remove_keyframe_near(it->second, local_t);
  // An emptied parameter stops being animated, falling back to its static value.
  if (it->second.empty()) effect->keyframes.erase(it);
  return p;
}

Project clear_effect_keyframes(Project p, std::string_view clip_id, std::size_t index,
                               std::string_view key) {
  ClipEffect* effect = find_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  effect->keyframes.erase(std::string(key));
  return p;
}

// -------------------------------------------- audio effect param keyframes --

Project set_audio_effect_keyframe(Project p, std::string_view clip_id, std::size_t index,
                                  std::string key, double local_t, double v) {
  AudioClipEffect* effect = find_audio_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  upsert_keyframe(effect->keyframes[std::move(key)], local_t, v);
  return p;
}

Interp audio_effect_keyframe_interp_of(const AudioClipEffect& effect,
                                       std::string_view key) noexcept {
  const auto it = effect.keyframes.find(std::string(key));
  return it == effect.keyframes.end() ? Interp::Linear : keyframe_list_interp(it->second);
}

Project set_audio_effect_keyframe_interp(Project p, std::string_view clip_id, std::size_t index,
                                         std::string_view key, Interp mode) {
  AudioClipEffect* effect = find_audio_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  const auto it = effect->keyframes.find(std::string(key));
  if (it == effect->keyframes.end()) return p;
  set_keyframe_list_interp(it->second, mode);
  return p;
}

Project remove_audio_effect_keyframe_at(Project p, std::string_view clip_id, std::size_t index,
                                        std::string_view key, double local_t) {
  AudioClipEffect* effect = find_audio_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  const auto it = effect->keyframes.find(std::string(key));
  if (it == effect->keyframes.end()) return p;
  remove_keyframe_near(it->second, local_t);
  // An emptied parameter stops being animated, falling back to its static value.
  if (it->second.empty()) effect->keyframes.erase(it);
  return p;
}

Project clear_audio_effect_keyframes(Project p, std::string_view clip_id, std::size_t index,
                                     std::string_view key) {
  AudioClipEffect* effect = find_audio_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  effect->keyframes.erase(std::string(key));
  return p;
}

// ------------------------------------------------------------ audio stack --

Project add_audio_effect(Project p, std::string_view clip_id, std::string type,
                         std::map<std::string, double> params) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->audio_effects.push_back(AudioClipEffect{
      .type = std::move(type),
      .params = std::move(params),
  });
  return p;
}

Project remove_audio_effect(Project p, std::string_view clip_id, std::size_t index) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr || index >= c->audio_effects.size()) return p;
  c->audio_effects.erase(c->audio_effects.begin() + static_cast<std::ptrdiff_t>(index));
  return p;
}

Project toggle_audio_effect(Project p, std::string_view clip_id, std::size_t index) {
  AudioClipEffect* effect = find_audio_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  effect->enabled = !effect->enabled;
  return p;
}

Project move_audio_effect(Project p, std::string_view clip_id, std::size_t index, int direction) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  move_within(c->audio_effects, index, direction);
  return p;
}

Project set_audio_effect_param(Project p, std::string_view clip_id, std::size_t index,
                               std::string key, double value) {
  AudioClipEffect* effect = find_audio_effect(p, clip_id, index);
  if (effect == nullptr) return p;
  effect->params[std::move(key)] = value;
  return p;
}

Project append_audio_effects(Project p, std::string_view clip_id,
                             std::span<const AudioClipEffect> effects) {
  if (effects.empty()) return p;
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->audio_effects.insert(c->audio_effects.end(), effects.begin(), effects.end());
  return p;
}

Project clear_audio_effects(Project p, std::string_view clip_id) {
  Clip* c = find_clip(p, clip_id);
  if (c == nullptr) return p;
  c->audio_effects.clear();
  return p;
}

}  // namespace cutline::core
