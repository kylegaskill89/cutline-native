#include "cutline/editor/settings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>

namespace cutline::editor {
namespace {

using nlohmann::json;

[[nodiscard]] std::string shown(const std::filesystem::path& path) {
  return path.generic_string();
}

// The spellings are part of the file format, so they are written out rather
// than derived from the enumerator order — inserting a sort would otherwise
// silently reinterpret every file written before it.

[[nodiscard]] std::string_view name_of(BrowserSort sort) noexcept {
  switch (sort) {
    case BrowserSort::Pool: return "pool";
    case BrowserSort::Name: return "name";
    case BrowserSort::Kind: return "kind";
    case BrowserSort::Duration: return "duration";
    case BrowserSort::Uses: return "uses";
  }
  return "pool";
}

[[nodiscard]] BrowserSort sort_named(std::string_view text) noexcept {
  if (text == "name") return BrowserSort::Name;
  if (text == "kind") return BrowserSort::Kind;
  if (text == "duration") return BrowserSort::Duration;
  if (text == "uses") return BrowserSort::Uses;
  // Anything unrecognised reads as the order things were imported in, which is
  // what a project with no preference at all shows.
  return BrowserSort::Pool;
}

[[nodiscard]] std::string_view name_of(ui::BrowserView view) noexcept {
  return view == ui::BrowserView::Icons ? "icons" : "list";
}

[[nodiscard]] ui::BrowserView view_named(std::string_view text) noexcept {
  return text == "icons" ? ui::BrowserView::Icons : ui::BrowserView::List;
}

/// Written and read as an array of colours in `ui::MediaKind` order, padded to
/// the kinds this build has. A map keyed by name would survive a kind being
/// inserted; an array keyed by position would not, so the reader pads and
/// truncates rather than trusting the length.
[[nodiscard]] std::vector<std::string> sized_to(std::vector<std::string> list,
                                                std::size_t count) {
  list.resize(count);
  return list;
}

}  // namespace

std::string_view label_name(const Settings& settings, std::size_t index) {
  const std::span<const ClipLabel> labels = clip_labels();
  if (index >= labels.size()) return {};
  if (index < settings.label_names.size() && !settings.label_names[index].empty()) {
    return settings.label_names[index];
  }
  return labels[index].name;
}

std::string_view label_default(const Settings& settings, ui::MediaKind kind) {
  const auto at = static_cast<std::size_t>(kind);
  if (at >= settings.label_defaults.size()) return {};
  return settings.label_defaults[at];
}

std::string to_json(const Settings& settings, int indent) {
  json out;
  out["version"] = kSettingsSchemaVersion;
  out["theme"] = settings.theme;
  out["snapping"] = settings.snapping;
  out["looping"] = settings.looping;
  out["aspect_locked"] = settings.aspect_locked;
  out["preview_scale"] = settings.preview_scale;
  out["pool_sort"] = name_of(settings.pool_sort);
  out["pool_descending"] = settings.pool_descending;
  out["pool_view"] = name_of(settings.pool_view);
  out["still_length"] = settings.still_length;
  out["transition_length"] = settings.transition_length;
  out["autosave_seconds"] = settings.autosave_seconds;
  out["autosave_versions"] = settings.autosave_versions;
  out["undo_depth"] = settings.undo_depth;
  out["preroll"] = settings.preroll;
  out["postroll"] = settings.postroll;
  out["software_renderer"] = settings.software_renderer;
  // Only when somebody has changed one. A fresh file saying "every label is
  // called what it is already called" is eight lines of nothing.
  if (std::ranges::any_of(settings.label_names, [](const std::string& n) { return !n.empty(); })) {
    out["label_names"] = settings.label_names;
  }
  if (std::ranges::any_of(settings.label_defaults,
                          [](const std::string& c) { return !c.empty(); })) {
    out["label_defaults"] = settings.label_defaults;
  }
  out["proxy_height"] = settings.proxy_height;
  if (!settings.proxy_folder.empty()) out["proxy_folder"] = settings.proxy_folder;
  if (!settings.audio_device.empty()) {
    out["audio_device"] = settings.audio_device;
    out["audio_device_name"] = settings.audio_device_name;
  }
  return out.dump(indent);
}

std::expected<Settings, std::string> settings_from_json(std::string_view text) {
  const json document = json::parse(text, nullptr, false);
  if (document.is_discarded()) return std::unexpected("the settings file is not valid JSON");
  if (!document.is_object()) return std::unexpected("a settings file has to be an object");

  const int version = document.value("version", 0);
  if (version > kSettingsSchemaVersion) {
    return std::unexpected("those settings were written by a newer version of Cutline");
  }

  // Started from the defaults, so a key that is not there is a setting nobody
  // has chosen rather than a setting turned off.
  Settings settings;
  settings.theme = document.value("theme", settings.theme);
  settings.snapping = document.value("snapping", settings.snapping);
  settings.looping = document.value("looping", settings.looping);
  settings.aspect_locked = document.value("aspect_locked", settings.aspect_locked);
  settings.pool_descending = document.value("pool_descending", settings.pool_descending);
  settings.pool_sort = sort_named(document.value("pool_sort", std::string{}));
  settings.pool_view = view_named(document.value("pool_view", std::string{}));

  // Clamped rather than trusted. A hand-edited zero would divide the canvas to
  // nothing and render a frame with no pixels in it.
  settings.preview_scale = std::clamp(document.value("preview_scale", settings.preview_scale),
                                      kMinPreviewScale, kMaxPreviewScale);
  settings.still_length = std::clamp(document.value("still_length", settings.still_length),
                                     kMinStillLength, kMaxStillLength);
  settings.transition_length =
      std::clamp(document.value("transition_length", settings.transition_length),
                 kMinTransitionLength, kMaxTransitionLength);
  settings.autosave_seconds =
      std::clamp(document.value("autosave_seconds", settings.autosave_seconds),
                 kMinAutosaveSeconds, kMaxAutosaveSeconds);
  settings.autosave_versions =
      std::clamp(document.value("autosave_versions", settings.autosave_versions),
                 kMinAutosaveVersions, kMaxAutosaveVersions);
  settings.undo_depth = std::clamp(document.value("undo_depth", settings.undo_depth),
                                   kMinUndoDepth, kMaxUndoDepth);
  settings.preroll =
      std::clamp(document.value("preroll", settings.preroll), kMinRoll, kMaxRoll);
  settings.postroll =
      std::clamp(document.value("postroll", settings.postroll), kMinRoll, kMaxRoll);
  settings.software_renderer =
      document.value("software_renderer", settings.software_renderer);

  // Sized to what this build has rather than to what the file says. A file from
  // a version with one more label would otherwise reach past the palette, and
  // one from a version with fewer would leave the last name unreadable.
  settings.label_names =
      sized_to(document.value("label_names", std::vector<std::string>{}), clip_labels().size());
  settings.label_defaults = sized_to(document.value("label_defaults", std::vector<std::string>{}),
                                     ui::kMediaKindCount);

  settings.proxy_height = std::clamp(document.value("proxy_height", settings.proxy_height),
                                     kMinProxyHeight, kMaxProxyHeight);
  settings.proxy_folder = document.value("proxy_folder", settings.proxy_folder);
  settings.audio_device = document.value("audio_device", settings.audio_device);
  settings.audio_device_name = document.value("audio_device_name", settings.audio_device_name);
  return settings;
}

std::filesystem::path default_settings_path() {
  // Beside the user's other application data, which is where a preference
  // belongs. Falling back to the working directory keeps this usable on a
  // machine where the variable is not set rather than failing to save at all.
  const char* roaming = std::getenv("APPDATA");
  const std::filesystem::path base =
      roaming == nullptr ? std::filesystem::path{"."} : std::filesystem::path{roaming};
  return base / "Cutline" / "settings.json";
}

std::expected<Settings, std::string> read_settings(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  // Not an error: it means nobody has changed anything yet.
  if (!file) return Settings{};

  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad()) return std::unexpected("could not read " + shown(path));

  return settings_from_json(text.str());
}

std::expected<void, std::string> write_settings(const std::filesystem::path& path,
                                                const Settings& settings) {
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return std::unexpected("could not make " + shown(path.parent_path()) + ": " +
                             error.message());
    }
  }

  // Beside the target so the rename stays within one filesystem and is
  // therefore atomic, exactly as a project and the workspaces are written.
  std::filesystem::path staging = path;
  staging += ".saving";

  {
    std::ofstream file(staging, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected("could not write " + shown(staging));
    file << to_json(settings);
    file.flush();
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
