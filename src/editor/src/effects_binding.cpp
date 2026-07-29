#include "cutline/editor/effects_binding.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/query.hpp"

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

}  // namespace

std::vector<EffectRow> clip_effects(const core::Project& project, std::string_view clip_id) {
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
      row.params.push_back(EffectParamRow{
          .key = std::string(param.key),
          .name = std::string(param.name),
          .range = {.minimum = param.minimum, .maximum = param.maximum},
          .value = stored_or_default(effect, param),
          .fallback = param.fallback,
          .suffix = std::string(param.suffix),
          .toggle = param.toggle,
          .animated = core::is_effect_param_animated(effect, param.key)});
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

}  // namespace cutline::editor
