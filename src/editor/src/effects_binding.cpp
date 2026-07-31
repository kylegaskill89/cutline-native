#include "cutline/editor/effects_binding.hpp"

#include "cutline/audio/chain.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/keyframe.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace cutline::editor {
namespace {

/// The stored value of a parameter, or the catalogue's default when the effect
/// does not carry one.
///
/// A stored effect need not have every parameter: only what was changed is
/// written, and the resolver fills the rest in. The panel has to do the same or
/// a fresh crop would show four sliders at zero regardless of where they are.
[[nodiscard]] double stored_or_default(const core::ClipEffect& effect,
                                       const render::EffectParamSpec& spec) {
  const auto found = effect.params.find(std::string(spec.key));
  return found == effect.params.end() ? spec.fallback : found->second;
}

/// The effect at `index`, or null.
[[nodiscard]] const core::ClipEffect* effect_at(const core::Project& project,
                                                std::string_view clip_id, std::size_t index) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || index >= clip->effects.size()) return nullptr;
  return &clip->effects[index];
}

/// Whether a keyframe sits at `local_t`, within the tolerance an edit at that
/// time would land on.
[[nodiscard]] bool keyed_at(const core::ClipEffect& effect, std::string_view key,
                            double local_t) {
  const auto found = effect.keyframes.find(std::string(key));
  if (found == effect.keyframes.end()) return false;
  return std::ranges::any_of(found->second, [local_t](const core::Keyframe& frame) {
    return std::abs(frame.t - local_t) <= core::kKeyframeMatchEps;
  });
}

}  // namespace

std::vector<EffectRow> clip_effects(const core::Project& project, std::string_view clip_id,
                                    double local_t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return {};

  std::vector<EffectRow> out;
  out.reserve(clip->effects.size());

  for (std::size_t i = 0; i < clip->effects.size(); ++i) {
    const core::ClipEffect& effect = clip->effects[i];
    const render::EffectSpec* spec = render::find_effect_spec(effect.type);

    EffectRow row;
    row.index = i;
    row.type = effect.type;
    row.name = spec != nullptr ? std::string(spec->name) : effect.type;
    row.enabled = effect.enabled;
    row.unknown = spec == nullptr;
    if (spec == nullptr) {
      out.push_back(std::move(row));
      continue;
    }

    for (const render::EffectParamSpec& param : spec->params) {
      const bool animated = core::is_effect_param_animated(effect, param.key);
      row.params.push_back(EffectParamRow{
          .key = std::string(param.key),
          .name = std::string(param.name),
          .range = {.minimum = param.minimum, .maximum = param.maximum},
          // Animated parameters ignore what is stored, so showing the stored
          // value would put a number on screen that nothing is using.
          .value = animated ? core::effect_param_at(effect, param.key, local_t)
                            : stored_or_default(effect, param),
          .fallback = param.fallback,
          .suffix = std::string(param.suffix),
          .toggle = param.toggle,
          .animated = animated,
          .keyed_here = animated && keyed_at(effect, param.key, local_t),
          .interp = core::effect_keyframe_interp_of(effect, param.key)});
    }

    for (const render::EffectColorSpec& color : spec->colors) {
      const auto found = effect.colors.find(std::string(color.key));
      row.colors.push_back(EffectColorRow{
          .key = std::string(color.key),
          .name = std::string(color.name),
          .value = found == effect.colors.end() ? std::string(color.fallback) : found->second});
    }

    out.push_back(std::move(row));
  }

  return out;
}

std::vector<EffectChoice> addable_effects() {
  std::vector<EffectChoice> out;
  for (const render::EffectSpec& spec : render::effect_catalog()) {
    out.push_back(EffectChoice{.type = std::string(spec.type),
                               .name = std::string(spec.name),
                               .category = std::string(render::to_string(spec.category))});
  }
  return out;
}

core::Project add_effect(core::Project project, std::string_view clip_id,
                         std::string_view type) {
  const render::EffectSpec* spec = render::find_effect_spec(type);
  if (spec == nullptr) return project;

  // Every parameter written out, not only the ones that differ from neutral.
  // The panel reads what is stored, and an effect added with an empty map would
  // show its sliders at the catalogue defaults while the *file* said nothing —
  // a difference that only appears after saving and reopening.
  std::map<std::string, double> params;
  for (const render::EffectParamSpec& param : spec->params) {
    params.emplace(std::string(param.key), param.fallback);
  }

  std::map<std::string, std::string> colors;
  for (const render::EffectColorSpec& color : spec->colors) {
    colors.emplace(std::string(color.key), std::string(color.fallback));
  }

  return core::add_clip_effect(std::move(project), clip_id, std::string(spec->type),
                               std::move(params), std::move(colors));
}

core::Project set_effect_parameter(core::Project project, std::string_view clip_id,
                                   std::size_t index, std::string_view key, double value,
                                   double local_t) {
  const core::ClipEffect* effect = effect_at(project, clip_id, index);
  if (effect == nullptr) return project;

  if (core::is_effect_param_animated(*effect, key)) {
    return core::set_effect_keyframe(std::move(project), clip_id, index, std::string(key),
                                     local_t, value);
  }
  return core::set_clip_effect_param(std::move(project), clip_id, index, std::string(key),
                                     value);
}

core::Project set_effect_parameter_animated(core::Project project, std::string_view clip_id,
                                            std::size_t index, std::string_view key,
                                            bool animated, double local_t) {
  const core::ClipEffect* effect = effect_at(project, clip_id, index);
  if (effect == nullptr) return project;

  const bool already = core::is_effect_param_animated(*effect, key);
  if (already == animated) return project;

  // Read before either edit: turning animation on has to keep the value the
  // parameter already had, and turning it off has to keep the one the
  // keyframes were producing.
  const double current = core::effect_param_at(*effect, key, local_t);

  if (animated) {
    return core::set_effect_keyframe(std::move(project), clip_id, index, std::string(key),
                                     local_t, current);
  }

  project = core::clear_effect_keyframes(std::move(project), clip_id, index, key);
  return core::set_clip_effect_param(std::move(project), clip_id, index, std::string(key),
                                     current);
}

core::Project toggle_effect_keyframe(core::Project project, std::string_view clip_id,
                                     std::size_t index, std::string_view key, double local_t) {
  const core::ClipEffect* effect = effect_at(project, clip_id, index);
  if (effect == nullptr) return project;
  // Not animated: there is no list to add to, and silently starting one here
  // would make the stopwatch and this marker mean the same thing.
  if (!core::is_effect_param_animated(*effect, key)) return project;

  if (keyed_at(*effect, key, local_t)) {
    return core::remove_effect_keyframe_at(std::move(project), clip_id, index, key, local_t);
  }
  return core::set_effect_keyframe(std::move(project), clip_id, index, std::string(key), local_t,
                                   core::effect_param_at(*effect, key, local_t));
}

core::Project set_effect_parameter_interp(core::Project project, std::string_view clip_id,
                                          std::size_t index, std::string_view key,
                                          core::Interp mode) {
  const core::ClipEffect* effect = effect_at(project, clip_id, index);
  // Nothing to set it on. A curve without keyframes would be silently
  // discarded the moment the stopwatch was pressed.
  if (effect == nullptr || !core::is_effect_param_animated(*effect, key)) return project;
  return core::set_effect_keyframe_interp(std::move(project), clip_id, index, key, mode);
}

// ------------------------------------------------------------ audio stack --

std::vector<EffectRow> clip_audio_effects(const core::Project& project,
                                          std::string_view clip_id) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return {};

  std::vector<EffectRow> rows;
  rows.reserve(clip->audio_effects.size());

  for (std::size_t i = 0; i < clip->audio_effects.size(); ++i) {
    const core::AudioClipEffect& effect = clip->audio_effects[i];
    const audio::AudioEffectDef* def = audio::audio_effect_def(effect.type);

    EffectRow row{.index = i,
                  .type = effect.type,
                  // The raw type for one this build does not know, so a project
                  // written by a newer version still shows that something is
                  // there rather than a gap in the stack.
                  .name = def != nullptr ? std::string(def->name) : effect.type,
                  .enabled = effect.enabled,
                  .unknown = def == nullptr};
    if (def == nullptr) {
      rows.push_back(std::move(row));
      continue;
    }

    for (const audio::AudioEffectParamDef& param : def->params) {
      row.params.push_back(EffectParamRow{
          .key = std::string(param.key),
          .name = std::string(param.label),
          .range = {.minimum = param.minimum, .maximum = param.maximum, .step = param.step},
          // Through the registry rather than straight off the map, so a
          // parameter the stored effect does not carry reads as its default
          // instead of as zero.
          .value = audio::audio_effect_param(effect, param.key),
          .fallback = param.fallback,
          .suffix = std::string(param.unit),
      });
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<EffectChoice> addable_audio_effects() {
  std::vector<EffectChoice> out;
  for (const audio::AudioEffectDef& def : audio::audio_effect_defs()) {
    out.push_back(EffectChoice{.type = std::string(def.id),
                               .name = std::string(def.name),
                               // One category. All eight are filters of one
                               // sort or another, and inventing groupings the
                               // registry does not have would be a fiction the
                               // menu then has to keep telling.
                               .category = "Audio"});
  }
  return out;
}

core::Project add_audio_effect(core::Project project, std::string_view clip_id,
                               std::string_view type) {
  const audio::AudioEffectDef* def = audio::audio_effect_def(type);
  if (def == nullptr) return project;

  // Every parameter written out, for the same reason the visual stack does it:
  // the panel reads what is stored, and an effect added with an empty map would
  // show its sliders at the registry defaults while the *file* said nothing —
  // a difference that only appears after saving and reopening.
  std::map<std::string, double> params;
  for (const audio::AudioEffectParamDef& param : def->params) {
    params.emplace(std::string(param.key), param.fallback);
  }

  return core::add_audio_effect(std::move(project), clip_id, std::string(def->id),
                                std::move(params));
}

}  // namespace cutline::editor
