#include "cutline/editor/keyframes.hpp"

#include "cutline/core/query.hpp"
#include "cutline/render/effect_catalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace cutline::editor {
namespace {

/// How close a press has to be, in seconds, to count as being on a keyframe.
/// The view converts a distance in pixels into one of these before asking.
constexpr double kReach = core::kKeyframeRemoveEps;

/// Two keyframes closer than this are the same instant, and a move that would
/// produce them is refused. The looser of the model's two tolerances, so a drag
/// cannot squeeze two points closer together than an edit could tell apart.
constexpr double kApart = core::kKeyframeRemoveEps;

/// The transform properties a lane can exist for, in the order the inspector
/// lists them. Not `kAnimProps`, which is in storage order.
constexpr std::array<ClipParam, 8> kMotionParams{
    ClipParam::Opacity, ClipParam::X,       ClipParam::Y,       ClipParam::ScaleX,
    ClipParam::ScaleY,  ClipParam::Rotation, ClipParam::AnchorX, ClipParam::AnchorY,
};

[[nodiscard]] std::optional<core::AnimProp> anim_prop_of(ClipParam param) noexcept {
  switch (param) {
    case ClipParam::Opacity: return core::AnimProp::Opacity;
    case ClipParam::X: return core::AnimProp::X;
    case ClipParam::Y: return core::AnimProp::Y;
    case ClipParam::ScaleX: return core::AnimProp::ScaleX;
    case ClipParam::ScaleY: return core::AnimProp::ScaleY;
    case ClipParam::Rotation: return core::AnimProp::Rotation;
    case ClipParam::AnchorX: return core::AnimProp::AnchorX;
    case ClipParam::AnchorY: return core::AnimProp::AnchorY;
    case ClipParam::Pan: return core::AnimProp::Pan;
    default: return std::nullopt;
  }
}

/// The list a reference names, or null when it names nothing that exists.
///
/// One function rather than one per caller, and non-const so the edits can use
/// it too — every one of them is "find the list, change it, put the clip back",
/// and finding it is the only part that differs between a transform property
/// and an effect's.
[[nodiscard]] std::vector<core::Keyframe>* lane_keys(core::Clip& clip, const ParamRef& ref) {
  if (ref.motion()) {
    if (ref.param == ClipParam::Gain) return &clip.gain_keyframes;
    const auto prop = anim_prop_of(ref.param);
    if (!prop.has_value()) return nullptr;
    return &clip.keyframes[core::anim_prop_index(*prop)];
  }

  if (ref.effect >= clip.effects.size()) return nullptr;
  const auto found = clip.effects[ref.effect].keyframes.find(ref.key);
  if (found == clip.effects[ref.effect].keyframes.end()) return nullptr;
  return &found->second;
}

/// The index of the keyframe nearest `t`, if one is within `kReach`.
[[nodiscard]] std::optional<std::size_t> nearest(std::span<const core::Keyframe> keys,
                                                 double t) noexcept {
  std::optional<std::size_t> best;
  double closest = kReach;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const double distance = std::abs(keys[i].t - t);
    if (distance <= closest) {
      closest = distance;
      best = i;
    }
  }
  return best;
}

/// Applies `edit` to the list `ref` names on `clip_id`, and reports whether
/// anything came of it. Every edit below is this shape.
template <typename Fn>
[[nodiscard]] core::Project edited(core::Project project, std::string_view clip_id,
                                   const ParamRef& ref, Fn&& edit) {
  core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  std::vector<core::Keyframe>* keys = lane_keys(*clip, ref);
  if (keys == nullptr || keys->empty()) return project;

  // Against a copy, so a refused edit leaves the project as it was rather than
  // half-applied. Every operation here can decline.
  std::vector<core::Keyframe> next = *keys;
  if (!edit(next)) return project;

  *keys = std::move(next);
  return project;
}

}  // namespace

KeyframeModel clip_keyframes(const core::Project& project, std::string_view clip_id) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return {};

  KeyframeModel model;
  model.duration = core::clip_duration(*clip);

  const auto add = [&model](ParamRef ref, std::string name,
                            const std::vector<core::Keyframe>& keys) {
    if (keys.empty()) return;
    model.lanes.push_back(
        KeyframeLane{.ref = std::move(ref), .name = std::move(name), .keys = keys});
  };

  for (const ClipParam param : kMotionParams) {
    const auto prop = anim_prop_of(param);
    if (!prop.has_value()) continue;
    add(ParamRef{.param = param}, std::string(param_name(param)),
        clip->keyframes[core::anim_prop_index(*prop)]);
  }
  // The two audio properties, after the picture ones and in the order the
  // inspector shows them. Gain is not in `kMotionParams` because it does not
  // live in the keyframe array; pan is not because it is not motion.
  add(ParamRef{.param = ClipParam::Gain}, std::string(param_name(ClipParam::Gain)),
      clip->gain_keyframes);
  add(ParamRef{.param = ClipParam::Pan}, std::string(param_name(ClipParam::Pan)),
      clip->keyframes[core::anim_prop_index(core::AnimProp::Pan)]);

  for (std::size_t i = 0; i < clip->effects.size(); ++i) {
    const core::ClipEffect& effect = clip->effects[i];
    const render::EffectSpec* spec = render::find_effect_spec(effect.type);
    const std::string effect_name = spec != nullptr ? std::string(spec->name) : effect.type;

    // Walked through the spec rather than through the map, so lanes come out in
    // the order the inspector shows the parameters rather than alphabetically.
    // An effect the registry no longer has contributes nothing, which is the
    // same answer its parameter rows give.
    if (spec == nullptr) continue;
    for (const render::EffectParamSpec& param : spec->params) {
      const auto found = effect.keyframes.find(std::string(param.key));
      if (found == effect.keyframes.end()) continue;
      // Qualified, because "Amount" says nothing when three effects have one.
      add(ParamRef{.effect = i, .key = std::string(param.key)},
          effect_name + " — " + std::string(param.name), found->second);
    }
  }

  return model;
}

core::Project move_keyframe(core::Project project, std::string_view clip_id,
                            const ParamRef& ref, double from, double to) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;
  // Clamped here, where the clip is in hand: a keyframe dragged past either end
  // is one that can never be reached again.
  const double limit = core::clip_duration(*clip);
  const double target = std::clamp(to, 0.0, limit);

  return edited(std::move(project), clip_id, ref,
                [from, target](std::vector<core::Keyframe>& keys) {
                  const auto index = nearest(keys, from);
                  if (!index.has_value()) return false;
                  if (keys[*index].t == target) return false;

                  // Onto another one is refused rather than merged. Two
                  // keyframes at the same instant have no meaningful order, and
                  // which survived would be whichever the sort happened to
                  // keep.
                  for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (i != *index && std::abs(keys[i].t - target) < kApart) return false;
                  }

                  keys[*index].t = target;
                  std::ranges::sort(keys, {}, &core::Keyframe::t);
                  return true;
                });
}

core::Project set_keyframe_interp(core::Project project, std::string_view clip_id,
                                  const ParamRef& ref, double at, core::Interp mode) {
  return edited(std::move(project), clip_id, ref,
                [at, mode](std::vector<core::Keyframe>& keys) {
                  const auto index = nearest(keys, at);
                  if (!index.has_value() || keys[*index].e == mode) return false;
                  keys[*index].e = mode;
                  return true;
                });
}

core::Project remove_keyframe(core::Project project, std::string_view clip_id,
                              const ParamRef& ref, double at) {
  return edited(std::move(project), clip_id, ref, [at](std::vector<core::Keyframe>& keys) {
    // The last one is refused. A property with animation on and no keyframes
    // evaluates to zero, which is not what removing a point should mean;
    // turning animation off is the stopwatch's job.
    if (keys.size() <= 1) return false;
    const auto index = nearest(keys, at);
    if (!index.has_value()) return false;
    keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(*index));
    return true;
  });
}

KeyframeClipboard copy_keyframes(const core::Project& project, std::string_view clip_id,
                                 std::span<const KeyframeAddress> addresses) {
  // Const, so the lookup goes through a copy of the clip rather than the
  // project. `lane_keys` has to be non-const to serve the edits as well, and
  // this is the one caller that is only reading.
  const core::Clip* found = core::find_clip(project, clip_id);
  if (found == nullptr || addresses.empty()) return {};
  core::Clip clip = *found;

  KeyframeClipboard clipboard;
  double earliest = std::numeric_limits<double>::max();

  for (const KeyframeAddress& address : addresses) {
    const std::vector<core::Keyframe>* keys = lane_keys(clip, address.ref);
    if (keys == nullptr) continue;
    const auto index = nearest(*keys, address.t);
    if (!index.has_value()) continue;

    // Grouped by property, so a selection spanning three lanes comes back as
    // three lanes rather than as one list nobody could put anywhere.
    const auto lane = std::ranges::find_if(clipboard.lanes, [&address](const auto& entry) {
      return entry.ref == address.ref;
    });
    core::Keyframe copied = (*keys)[*index];
    earliest = std::min(earliest, copied.t);
    if (lane == clipboard.lanes.end()) {
      clipboard.lanes.push_back(KeyframeClipboard::Lane{.ref = address.ref, .keys = {copied}});
    } else {
      lane->keys.push_back(copied);
    }
  }

  if (clipboard.lanes.empty()) return {};

  // Relative to the earliest, which is what makes pasting at the playhead mean
  // anything at all.
  for (KeyframeClipboard::Lane& lane : clipboard.lanes) {
    for (core::Keyframe& key : lane.keys) key.t -= earliest;
    std::ranges::sort(lane.keys, {}, &core::Keyframe::t);
  }
  return clipboard;
}

core::Project paste_keyframes(core::Project project, std::string_view clip_id,
                              const KeyframeClipboard& clipboard, double at) {
  if (clipboard.empty()) return project;

  core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;
  const double limit = core::clip_duration(*clip);

  for (const KeyframeClipboard::Lane& lane : clipboard.lanes) {
    std::vector<core::Keyframe>* keys = lane_keys(*clip, lane.ref);
    // Not animated any more, so there is no list to paste into. Switching
    // animation on here would change the picture in a way nobody asked for.
    if (keys == nullptr || keys->empty()) continue;

    for (const core::Keyframe& key : lane.keys) {
      const double when = at + key.t;
      // Dropped rather than clamped. Clamping would pile a whole curve onto
      // the clip's last frame, which is worse than losing the part that did
      // not fit.
      if (when < 0.0 || when > limit) continue;
      core::upsert_keyframe(*keys, when, key.v);
      // `upsert_keyframe` gives a new keyframe the list's mode, so the curve
      // that was copied has to be put back on top of it.
      const auto index = nearest(*keys, when);
      if (index.has_value()) (*keys)[*index].e = key.e;
    }
  }

  // Nothing is written when nothing fits, so this is the project exactly as it
  // arrived — which is what the session needs in order to skip the entry.
  return project;
}

std::optional<double> keyframe_before(std::span<const core::Keyframe> keys, double t) noexcept {
  std::optional<double> best;
  for (const core::Keyframe& key : keys) {
    // Strictly before, and by more than the match tolerance, so pressing the
    // button repeatedly walks the list instead of sticking on the one it just
    // landed on.
    if (key.t < t - core::kKeyframeMatchEps && (!best.has_value() || key.t > *best)) {
      best = key.t;
    }
  }
  return best;
}

std::optional<double> keyframe_after(std::span<const core::Keyframe> keys, double t) noexcept {
  std::optional<double> best;
  for (const core::Keyframe& key : keys) {
    if (key.t > t + core::kKeyframeMatchEps && (!best.has_value() || key.t < *best)) {
      best = key.t;
    }
  }
  return best;
}

}  // namespace cutline::editor
