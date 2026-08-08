#include "cutline/editor/effects_binding.hpp"

#include "cutline/audio/chain.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/keyframe.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/roles.hpp"
#include "cutline/editor/transitions.hpp"
#include "cutline/render/effect_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
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

namespace {

/// Fractions are stored, percentages are shown — the same split every other
/// number in this panel keeps.
constexpr double kEffectPercent = 100.0;

/// The smallest half-extent a seeded path starts with, so choosing Free Draw on
/// a mask that had been squashed to nothing still gives something to grab.
constexpr double kMinPathExtent = 0.05;

/// The mask as a panel reads it.
[[nodiscard]] EffectMaskRow mask_row(const core::Mask& mask) {
  return EffectMaskRow{
      .shape = mask.shape,
      .x = mask.x * kEffectPercent,
      .y = mask.y * kEffectPercent,
      .width = mask.width * kEffectPercent,
      .height = mask.height * kEffectPercent,
      .rotation = mask.rotation,
      .feather = mask.feather * kEffectPercent,
      .opacity = mask.opacity * kEffectPercent,
      .inverted = mask.inverted,
  };
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
    row.mask = mask_row(effect.mask);
    row.mask_params = mask_param_rows(effect, local_t);
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

double mask_param_scale(std::string_view key) noexcept {
  // Rotation is degrees on both sides. Everything else is a fraction of the
  // layer, shown as a percentage like every other fraction in this panel.
  return key == "mask.rotation" ? 1.0 : kEffectPercent;
}

std::vector<EffectParamRow> mask_param_rows(const core::ClipEffect& effect, double local_t) {
  if (!effect.mask.active()) return {};

  /// Name, range and reset value per key. The ranges reach past the layer on
  /// purpose for position and size: a mask parked off the edge is how one is
  /// brought in from the side.
  struct Spec {
    std::string_view key;
    std::string_view name;
    double minimum;
    double maximum;
    double fallback;
    std::string_view suffix;
  };
  static constexpr std::array<Spec, 7> kSpecs{{
      {"mask.x", "Mask Position X", -50.0, 150.0, 50.0, "%"},
      {"mask.y", "Mask Position Y", -50.0, 150.0, 50.0, "%"},
      {"mask.width", "Mask Width", 0.0, 100.0, 25.0, "%"},
      {"mask.height", "Mask Height", 0.0, 100.0, 25.0, "%"},
      {"mask.rotation", "Mask Rotation", -180.0, 180.0, 0.0, "\xc2\xb0"},
      {"mask.feather", "Mask Feather", 0.0, 50.0, 0.0, "%"},
      {"mask.opacity", "Mask Opacity", 0.0, 100.0, 100.0, "%"},
  }};

  std::vector<EffectParamRow> rows;
  rows.reserve(kSpecs.size());
  for (const Spec& spec : kSpecs) {
    const bool animated = core::is_effect_param_animated(effect, spec.key);
    const double scale = mask_param_scale(spec.key);
    rows.push_back(EffectParamRow{
        .key = std::string(spec.key),
        .name = std::string(spec.name),
        .range = {.minimum = spec.minimum, .maximum = spec.maximum},
        // Through `effect_param_at` whether or not it is animated: that is the
        // one reader that knows a mask number lives on the mask, and going
        // round it here is how the two would come to disagree.
        .value = core::effect_param_at(effect, spec.key, local_t) * scale,
        .fallback = spec.fallback,
        .suffix = std::string(spec.suffix),
        .animated = animated,
        .keyed_here = animated && keyed_at(effect, spec.key, local_t),
        .interp = core::effect_keyframe_interp_of(effect, spec.key)});
  }
  return rows;
}

std::span<const core::MaskShape> mask_shapes() noexcept {
  static constexpr std::array kShapes{core::MaskShape::None, core::MaskShape::Ellipse,
                                      core::MaskShape::Rectangle, core::MaskShape::Path};
  return kShapes;
}

std::string_view mask_shape_name(core::MaskShape shape) noexcept {
  switch (shape) {
    case core::MaskShape::None: return "None";
    case core::MaskShape::Ellipse: return "Ellipse";
    case core::MaskShape::Rectangle: return "Rectangle";
    case core::MaskShape::Path: return "Free Draw";
  }
  return "None";
}

core::Project set_effect_mask(core::Project project, std::string_view clip_id,
                              std::size_t index, const EffectMaskRow& row) {
  core::Mask mask{
      .shape = row.shape,
      .x = row.x / kEffectPercent,
      .y = row.y / kEffectPercent,
      .width = row.width / kEffectPercent,
      .height = row.height / kEffectPercent,
      .rotation = row.rotation,
      .feather = row.feather / kEffectPercent,
      .opacity = row.opacity / kEffectPercent,
      .inverted = row.inverted,
  };

  // The corners survive a change to any other number, because the panel's row
  // does not carry them and rewriting the mask from it would otherwise erase a
  // path every time its feather was touched.
  if (const core::ClipEffect* effect = effect_at(project, clip_id, index); effect != nullptr) {
    mask.points = effect->mask.points;
  }

  // A path chosen with no corners yet starts as the rectangle it replaces, so
  // there is a shape on the picture to pull about. Premiere hands you a pen and
  // an empty frame; a shape you can drag from the first moment is the same
  // feature with nothing to learn first, and the first corner you move makes it
  // yours.
  if (mask.shape == core::MaskShape::Path && mask.points.size() < 3) {
    const double half_w = std::max(kMinPathExtent, mask.width);
    const double half_h = std::max(kMinPathExtent, mask.height);
    mask.points = {core::MaskPoint{-half_w, -half_h}, core::MaskPoint{half_w, -half_h},
                   core::MaskPoint{half_w, half_h}, core::MaskPoint{-half_w, half_h}};
  }

  return core::set_effect_mask(std::move(project), clip_id, index, std::move(mask));
}

core::Project clear_effect_mask(core::Project project, std::string_view clip_id,
                                std::size_t index) {
  return core::set_effect_mask(std::move(project), clip_id, index, core::Mask{});
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
                                          std::string_view clip_id, double local_t) {
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
          .value = audio::audio_effect_param(effect, param.key, local_t),
          .fallback = param.fallback,
          .suffix = std::string(param.unit),
      });

      EffectParamRow& row_param = row.params.back();
      row_param.animated = audio::audio_effect_param_animated(effect, param.key);
      if (row_param.animated) {
        const auto keys = effect.keyframes.find(std::string(param.key));
        row_param.keyed_here =
            keys != effect.keyframes.end() &&
            std::ranges::any_of(keys->second, [local_t](const core::Keyframe& frame) {
              return std::abs(frame.t - local_t) <= core::kKeyframeMatchEps;
            });
        row_param.interp = core::audio_effect_keyframe_interp_of(effect, param.key);
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

namespace {

/// The audio effect at `index`, or null.
[[nodiscard]] const core::AudioClipEffect* audio_effect_at(const core::Project& project,
                                                           std::string_view clip_id,
                                                           std::size_t index) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || index >= clip->audio_effects.size()) return nullptr;
  return &clip->audio_effects[index];
}

}  // namespace

core::Project set_audio_effect_parameter(core::Project project, std::string_view clip_id,
                                         std::size_t index, std::string_view key, double value,
                                         double local_t) {
  const core::AudioClipEffect* effect = audio_effect_at(project, clip_id, index);
  if (effect == nullptr) return project;

  if (audio::audio_effect_param_animated(*effect, key)) {
    return core::set_audio_effect_keyframe(std::move(project), clip_id, index, std::string(key),
                                           local_t, value);
  }
  return core::set_audio_effect_param(std::move(project), clip_id, index, std::string(key),
                                      value);
}

core::Project set_audio_effect_parameter_animated(core::Project project,
                                                  std::string_view clip_id, std::size_t index,
                                                  std::string_view key, bool animated,
                                                  double local_t) {
  const core::AudioClipEffect* effect = audio_effect_at(project, clip_id, index);
  if (effect == nullptr) return project;

  const bool already = audio::audio_effect_param_animated(*effect, key);
  if (already == animated) return project;

  // Read before either edit, so pressing the stopwatch changes nothing about
  // the sound at this instant — the same contract every other stopwatch keeps.
  const double current = audio::audio_effect_param(*effect, key, local_t);

  if (animated) {
    return core::set_audio_effect_keyframe(std::move(project), clip_id, index, std::string(key),
                                           local_t, current);
  }

  project = core::clear_audio_effect_keyframes(std::move(project), clip_id, index, key);
  return core::set_audio_effect_param(std::move(project), clip_id, index, std::string(key),
                                      current);
}

core::Project toggle_audio_effect_keyframe(core::Project project, std::string_view clip_id,
                                           std::size_t index, std::string_view key,
                                           double local_t) {
  const core::AudioClipEffect* effect = audio_effect_at(project, clip_id, index);
  if (effect == nullptr) return project;
  // Not animated: there is no list to add to, and silently starting one here
  // would make the stopwatch and this marker mean the same thing.
  if (!audio::audio_effect_param_animated(*effect, key)) return project;

  const auto keys = effect->keyframes.find(std::string(key));
  if (keys != effect->keyframes.end() &&
      std::ranges::any_of(keys->second, [local_t](const core::Keyframe& frame) {
        return std::abs(frame.t - local_t) <= core::kKeyframeMatchEps;
      })) {
    return core::remove_audio_effect_keyframe_at(std::move(project), clip_id, index, key,
                                                 local_t);
  }
  return core::set_audio_effect_keyframe(std::move(project), clip_id, index, std::string(key),
                                         local_t, audio::audio_effect_param(*effect, key,
                                                                            local_t));
}

core::Project set_audio_effect_parameter_interp(core::Project project, std::string_view clip_id,
                                                std::size_t index, std::string_view key,
                                                core::Interp mode) {
  const core::AudioClipEffect* effect = audio_effect_at(project, clip_id, index);
  if (effect == nullptr || !audio::audio_effect_param_animated(*effect, key)) return project;
  return core::set_audio_effect_keyframe_interp(std::move(project), clip_id, index, key, mode);
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

core::Project apply_role_preset(core::Project project, std::string_view clip_id,
                                core::AudioRole role) {
  project = core::set_clip_role(std::move(project), clip_id, role);

  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || clip->kind != core::TrackKind::Audio) return project;

  for (const core::AudioClipEffect& wanted : audio::role_preset(role)) {
    const bool already =
        std::ranges::any_of(clip->audio_effects, [&](const core::AudioClipEffect& have) {
          return have.type == wanted.type;
        });
    if (already) continue;

    // Every parameter written out, not only the ones the preset has an opinion
    // about: the panel reads what is stored, and an effect whose map is half
    // empty shows those sliders at the registry defaults while the *file* says
    // nothing — a difference that only appears after saving and reopening.
    std::map<std::string, double> params;
    if (const audio::AudioEffectDef* def = audio::audio_effect_def(wanted.type); def != nullptr) {
      for (const audio::AudioEffectParamDef& param : def->params) {
        params.emplace(std::string(param.key), param.fallback);
      }
    }
    for (const auto& [key, value] : wanted.params) params[key] = value;

    project = core::add_audio_effect(std::move(project), clip_id, wanted.type, std::move(params));
    clip = core::find_clip(project, clip_id);
    if (clip == nullptr) break;
  }
  return project;
}

// ------------------------------------------------------------- copy/paste --

EffectClipboard copy_one_effect(const core::Project& project, std::string_view clip_id,
                                std::size_t index) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return {};

  EffectClipboard clipboard;
  clipboard.kind = clip->kind;

  if (clip->kind == core::TrackKind::Video) {
    if (index >= clip->effects.size()) return {};
    clipboard.video.push_back(clip->effects[index]);
  } else {
    if (index >= clip->audio_effects.size()) return {};
    clipboard.audio.push_back(clip->audio_effects[index]);
  }

  clipboard.filled = true;
  return clipboard;
}

EffectClipboard copy_effects(const core::Project& project, std::string_view clip_id) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return {};
  return EffectClipboard{.kind = clip->kind,
                         .video = clip->effects,
                         .audio = clip->audio_effects,
                         .filled = true};
}

core::Project paste_effects(core::Project project, std::span<const std::string> clip_ids,
                            const EffectClipboard& clipboard) {
  if (!clipboard.filled) return project;

  for (const std::string& clip_id : clip_ids) {
    const core::Clip* clip = core::find_clip(project, clip_id);
    // Its own kind only. Selecting a shot selects both halves of an A/V pair,
    // and a video look pasted over the pair must not take the audio's filters
    // with it.
    if (clip == nullptr || clip->kind != clipboard.kind) continue;

    if (clipboard.kind == core::TrackKind::Video) {
      project = core::clear_clip_effects(std::move(project), clip_id);
      project = core::append_clip_effects(std::move(project), clip_id, clipboard.video);
    } else {
      project = core::clear_audio_effects(std::move(project), clip_id);
      project = core::append_audio_effects(std::move(project), clip_id, clipboard.audio);
    }
  }
  return project;
}

core::Project reset_effect(core::Project project, std::string_view clip_id,
                           std::size_t index) {
  const core::ClipEffect* effect = effect_at(project, clip_id, index);
  if (effect == nullptr) return project;
  const render::EffectSpec* spec = render::find_effect_spec(effect->type);
  if (spec == nullptr) return project;

  for (const render::EffectParamSpec& param : spec->params) {
    // The keyframes first. Setting a parameter that is animated writes a
    // keyframe rather than the stored value, so resetting an animated effect
    // without clearing them would put a keyframe at the playhead holding the
    // default and leave the rest of the curve exactly as it was.
    project = core::clear_effect_keyframes(std::move(project), clip_id, index, param.key);
    project = core::set_clip_effect_param(std::move(project), clip_id, index,
                                          std::string(param.key), param.fallback);
  }
  for (const render::EffectColorSpec& color : spec->colors) {
    project = core::set_clip_effect_color(std::move(project), clip_id, index,
                                          std::string(color.key), std::string(color.fallback));
  }
  return project;
}

core::Project reset_audio_effect(core::Project project, std::string_view clip_id,
                                 std::size_t index) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || index >= clip->audio_effects.size()) return project;
  const audio::AudioEffectDef* def = audio::audio_effect_def(clip->audio_effects[index].type);
  if (def == nullptr) return project;

  for (const audio::AudioEffectParamDef& param : def->params) {
    // Keyframes first. Setting an animated parameter writes a keyframe rather
    // than the stored value, so a reset that left the animation running would
    // put one keyframe at the playhead holding the default and leave the rest
    // of the sweep exactly as it was.
    project = core::clear_audio_effect_keyframes(std::move(project), clip_id, index, param.key);
    project = core::set_audio_effect_param(std::move(project), clip_id, index,
                                           std::string(param.key), param.fallback);
  }
  return project;
}

// ---------------------------------------------------------------- library --

namespace {

/// The three prefixes an id can carry, and what each one means.
constexpr std::string_view kVideoPrefix = "video:";
constexpr std::string_view kAudioPrefix = "audio:";
constexpr std::string_view kTransitionPrefix = "transition:";

/// Splits `id` into its prefix and the type behind it. An id with no prefix
/// this build knows names nothing, which is what a library written by a newer
/// version looks like.
[[nodiscard]] std::optional<std::string_view> behind(std::string_view id,
                                                     std::string_view prefix) {
  if (!id.starts_with(prefix)) return std::nullopt;
  return id.substr(prefix.size());
}

}  // namespace

std::string_view preset_name_of(std::string_view id) noexcept {
  if (!id.starts_with(kPresetPrefix)) return {};
  return id.substr(kPresetPrefix.size());
}

std::vector<LibraryEntry> effect_library(std::span<const std::string> preset_names) {
  std::vector<LibraryEntry> out;

  // Paths, not names. Twenty video effects in five categories under one flat
  // heading each is a list nobody can scan; nested, the panel opens as three
  // headings and you go down to what you want, which is Premiere's arrangement
  // and everybody else's.
  for (const EffectChoice& choice : addable_effects()) {
    out.push_back(LibraryEntry{.id = std::string(kVideoPrefix) + choice.type,
                               .name = choice.name,
                               .folder = "Video Effects/" + choice.category});
  }
  for (const EffectChoice& choice : addable_audio_effects()) {
    out.push_back(LibraryEntry{.id = std::string(kAudioPrefix) + choice.type,
                               .name = choice.name,
                               .folder = "Audio Effects"});
  }
  // Transitions last, and in the same panel, because that is where somebody
  // reaching for a cross-dissolve looks — not in a dropdown three sections down
  // an inspector.
  for (const core::TransitionKind kind : transition_kinds()) {
    out.push_back(LibraryEntry{.id = std::string(kTransitionPrefix) +
                                     std::string(transition_id(kind)),
                               .name = std::string(transition_name(kind)),
                               .folder = "Video Transitions"});
  }

  // Last, and in their own folder, which is where Premiere keeps them. A
  // preset is reached for the same way an effect is, so it belongs in the same
  // panel rather than behind a menu somewhere else.
  for (const std::string& name : preset_names) {
    out.push_back(LibraryEntry{
        .id = std::string(kPresetPrefix) + name, .name = name, .folder = "Presets"});
  }
  return out;
}

bool library_entry_fits(const core::Project& project, std::string_view clip_id,
                        std::string_view id) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return false;

  if (const auto type = behind(id, kVideoPrefix)) {
    return clip->kind == core::TrackKind::Video && render::find_effect_spec(*type) != nullptr;
  }
  if (const auto type = behind(id, kAudioPrefix)) {
    return clip->kind == core::TrackKind::Audio && audio::audio_effect_def(*type) != nullptr;
  }
  if (const auto type = behind(id, kTransitionPrefix)) {
    const std::optional<core::TransitionKind> kind = transition_from_id(*type);
    // A transition needs a cut to sit on. Offering one where nothing abuts the
    // clip would be a control that silently did nothing.
    return kind.has_value() && longest_transition(project, clip_id, *kind) > 0.0;
  }
  return false;
}

core::Project apply_library_entry(core::Project project, std::string_view clip_id,
                                  std::string_view id) {
  if (!library_entry_fits(project, clip_id, id)) return project;

  if (const auto type = behind(id, kVideoPrefix)) {
    return add_effect(std::move(project), clip_id, *type);
  }
  if (const auto type = behind(id, kAudioPrefix)) {
    return add_audio_effect(std::move(project), clip_id, *type);
  }
  if (const auto type = behind(id, kTransitionPrefix)) {
    const std::optional<core::TransitionKind> kind = transition_from_id(*type);
    if (!kind.has_value()) return project;
    return set_transition(std::move(project), clip_id, *kind,
                          default_transition_length(project, clip_id, *kind));
  }
  return project;
}

core::Project add_effect_to(core::Project project, std::span<const std::string> clip_ids,
                            std::string_view type, bool audio) {
  // Built back into a library id so the one rule about what may land on what
  // stays in one place. The alternative is a kind check here kept in step with
  // the one in `library_entry_fits`, and two copies of a rule is one copy and a
  // future disagreement.
  const std::string id = std::string(audio ? kAudioPrefix : kVideoPrefix) + std::string(type);
  for (const std::string& clip_id : clip_ids) {
    project = apply_library_entry(std::move(project), clip_id, id);
  }
  return project;
}

core::Project clear_effects_on(core::Project project, std::span<const std::string> clip_ids) {
  for (const std::string& clip_id : clip_ids) {
    project = core::clear_clip_effects(std::move(project), clip_id);
    project = core::clear_audio_effects(std::move(project), clip_id);
  }
  return project;
}

}  // namespace cutline::editor
