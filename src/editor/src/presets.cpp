#include "cutline/editor/presets.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/serialize.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

namespace cutline::editor {
namespace {

using nlohmann::json;

[[nodiscard]] std::string shown(const std::filesystem::path& path) {
  return path.generic_string();
}

}  // namespace

bool save_preset(Presets& presets, const core::Project& project, std::string_view clip_id,
                 std::string name) {
  if (name.empty()) return false;

  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return false;
  // A preset that applies nothing is indistinguishable from one that failed to
  // save, and the button that made it would look broken.
  if (clip->effects.empty() && clip->audio_effects.empty()) return false;

  EffectPreset preset{
      .name = std::move(name), .video = clip->effects, .audio = clip->audio_effects};

  // Saving over one keeps its place in the list. A preset you have just
  // refined jumping to the end is a preset you then have to go and find.
  const auto existing = std::ranges::find(presets.named, preset.name, &EffectPreset::name);
  if (existing != presets.named.end()) {
    *existing = std::move(preset);
  } else {
    presets.named.push_back(std::move(preset));
  }
  return true;
}

bool remove_preset(Presets& presets, std::string_view name) {
  const auto found = std::ranges::find(presets.named, name, &EffectPreset::name);
  if (found == presets.named.end()) return false;
  presets.named.erase(found);
  return true;
}

const EffectPreset* find_preset(const Presets& presets, std::string_view name) noexcept {
  const auto found = std::ranges::find(presets.named, name, &EffectPreset::name);
  return found == presets.named.end() ? nullptr : &*found;
}

core::Project apply_preset(core::Project project, std::string_view clip_id,
                           const EffectPreset& preset) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || preset.empty()) return project;

  // Only the half that fits. A preset carrying both is what a clip carrying
  // both produced, and putting a colour correction on a waveform because the
  // same preset also held an equaliser is not what anybody meant.
  if (clip->kind == core::TrackKind::Video) {
    if (preset.video.empty()) return project;
    return core::append_clip_effects(std::move(project), clip_id, preset.video);
  }

  if (preset.audio.empty()) return project;
  return core::append_audio_effects(std::move(project), clip_id, preset.audio);
}

// ------------------------------------------------------------- persistence --

std::string to_json(const Presets& presets, int indent) {
  json out;
  out["version"] = kPresetSchemaVersion;

  json named = json::array();
  for (const EffectPreset& preset : presets.named) {
    json entry;
    entry["name"] = preset.name;
    // Through `core`, so a preset holds effects in exactly the shape a project
    // holds them. Parsed straight back out of the string it produced, which
    // costs one pass over a settings file and buys one encoder rather than two.
    entry["stacks"] = json::parse(
        core::effects_to_json(core::EffectStacks{preset.video, preset.audio}, -1));
    named.push_back(std::move(entry));
  }
  out["presets"] = std::move(named);

  return out.dump(indent);
}

std::expected<Presets, std::string> presets_from_json(std::string_view text) {
  const json parsed = json::parse(text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return std::unexpected("presets file is not a JSON object");
  }

  const int version = parsed.value("version", 0);
  if (version > kPresetSchemaVersion) {
    return std::unexpected("presets were written by a newer version of Cutline");
  }

  Presets presets;
  const auto named = parsed.find("presets");
  if (named == parsed.end() || !named->is_array()) return presets;

  for (const json& entry : *named) {
    if (!entry.is_object()) continue;

    EffectPreset preset;
    preset.name = entry.value("name", std::string{});
    // A preset with no name cannot be offered or found again, so there is
    // nothing to keep.
    if (preset.name.empty()) continue;

    if (const auto stacks = entry.find("stacks"); stacks != entry.end()) {
      if (auto read = core::effects_from_json(stacks->dump()); read.has_value()) {
        preset.video = std::move(read->video);
        preset.audio = std::move(read->audio);
      }
    }
    if (preset.empty()) continue;

    presets.named.push_back(std::move(preset));
  }
  return presets;
}

std::filesystem::path default_presets_path() {
  // Beside the user's other application data, which is where a preference
  // belongs. Falling back to the working directory keeps this usable on a
  // machine where the variable is not set rather than failing to save at all.
  const char* roaming = std::getenv("APPDATA");
  std::filesystem::path base =
      roaming == nullptr ? std::filesystem::path{"."} : std::filesystem::path{roaming};
  return base / "Cutline" / "presets.json";
}

std::expected<Presets, std::string> read_presets(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  // Not an error: it means nobody has saved one yet.
  if (!file) return Presets{};

  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad()) return std::unexpected("could not read " + shown(path));

  return presets_from_json(text.str());
}

std::expected<void, std::string> write_presets(const std::filesystem::path& path,
                                               const Presets& presets) {
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return std::unexpected("could not make " + shown(path.parent_path()) + ": " +
                             error.message());
    }
  }

  // Beside the target so the rename stays within one filesystem and is
  // therefore atomic, exactly as a project and a workspace are written.
  std::filesystem::path staging = path;
  staging += ".saving";
  {
    std::ofstream file(staging, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected("could not write " + shown(staging));
    file << to_json(presets);
    if (!file) return std::unexpected("could not write " + shown(staging));
  }

  std::filesystem::rename(staging, path, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return std::unexpected("could not replace " + shown(path) + ": " + error.message());
  }
  return {};
}

}  // namespace cutline::editor
