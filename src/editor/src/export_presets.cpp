#include "cutline/editor/export_presets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>

namespace cutline::editor {
namespace {

using nlohmann::json;
using core::WorkingColour;

constexpr int kSchemaVersion = 1;

constexpr std::array kFormats{ExportFormat::H264, ExportFormat::Hevc, ExportFormat::Hevc10,
                              ExportFormat::ProRes, ExportFormat::DnxHr};

/// Premiere's built-ins, in the spirit rather than the letter: the same
/// purposes, at the qualities this encoder reaches them with.
[[nodiscard]] std::vector<ExportPreset> make_built_ins() {
  std::vector<ExportPreset> out;
  const auto add = [&out](ExportPreset preset) {
    preset.built_in = true;
    out.push_back(std::move(preset));
  };
  add({.name = "Match Source - High Quality", .format = ExportFormat::H264, .quality = 18});
  add({.name = "Match Source - HEVC", .format = ExportFormat::Hevc, .quality = 18});
  add({.name = "Match Source - HEVC 10-bit", .format = ExportFormat::Hevc10, .quality = 18});
  add({.name = "YouTube 1080p Full HD", .format = ExportFormat::H264, .quality = 18,
       .short_side = 1080});
  add({.name = "YouTube 2160p 4K Ultra HD", .format = ExportFormat::H264, .quality = 18,
       .short_side = 2160});
  add({.name = "Vimeo 1080p Full HD", .format = ExportFormat::H264, .quality = 18,
       .short_side = 1080});
  add({.name = "Social Media 720p", .format = ExportFormat::H264, .quality = 21,
       .short_side = 720});
  add({.name = "HDR10 - HEVC 10-bit", .format = ExportFormat::Hevc10, .quality = 18,
       .colour = WorkingColour::Rec2100Pq});
  add({.name = "Apple ProRes 422 HQ", .format = ExportFormat::ProRes});
  add({.name = "Avid DNxHR HQ", .format = ExportFormat::DnxHr});
  return out;
}

[[nodiscard]] std::string_view colour_name(WorkingColour colour) noexcept {
  switch (colour) {
    case WorkingColour::Rec2100Pq: return "rec2100_pq";
    case WorkingColour::Rec2100Hlg: return "rec2100_hlg";
    case WorkingColour::Rec709: break;
  }
  return "rec709";
}

[[nodiscard]] std::optional<WorkingColour> colour_named(std::string_view name) noexcept {
  if (name == "rec709") return WorkingColour::Rec709;
  if (name == "rec2100_pq") return WorkingColour::Rec2100Pq;
  if (name == "rec2100_hlg") return WorkingColour::Rec2100Hlg;
  return std::nullopt;
}

[[nodiscard]] int even(double value) { return std::max(2, static_cast<int>(std::llround(value)) & ~1); }

[[nodiscard]] json to_json_value(const ExportPreset& preset) {
  json out;
  out["name"] = preset.name;
  out["format"] = std::string(to_string(preset.format));
  out["quality"] = preset.quality;
  out["short_side"] = preset.short_side;
  out["fps"] = preset.fps;
  out["colour"] = preset.colour ? std::string(colour_name(*preset.colour)) : std::string("match");
  out["audio"] = preset.audio;
  out["channels"] = preset.channels;
  out["separate_audio"] = preset.separate_audio;
  return out;
}

[[nodiscard]] std::optional<ExportPreset> preset_from(const json& value) {
  if (!value.is_object()) return std::nullopt;
  ExportPreset preset;
  preset.name = value.value("name", std::string{});
  if (preset.name.empty()) return std::nullopt;
  const auto format = export_format_from_string(value.value("format", std::string{}));
  if (!format) return std::nullopt;
  preset.format = *format;
  // Clamped rather than trusted, as every preference is.
  preset.quality = std::clamp(value.value("quality", preset.quality), 0, 51);
  preset.short_side = std::clamp(value.value("short_side", 0), 0, 8640);
  preset.fps = std::clamp(value.value("fps", 0.0), 0.0, 240.0);
  preset.colour = colour_named(value.value("colour", std::string("match")));
  preset.audio = value.value("audio", true);
  preset.channels = std::clamp(value.value("channels", 2), 1, 2);
  preset.separate_audio = value.value("separate_audio", false);
  return preset;
}

[[nodiscard]] std::string shown(const std::filesystem::path& path) { return path.string(); }

}  // namespace

std::string_view to_string(ExportFormat format) noexcept {
  switch (format) {
    case ExportFormat::Hevc: return "hevc";
    case ExportFormat::Hevc10: return "hevc10";
    case ExportFormat::ProRes: return "prores";
    case ExportFormat::DnxHr: return "dnxhr";
    case ExportFormat::H264: break;
  }
  return "h264";
}

std::optional<ExportFormat> export_format_from_string(std::string_view text) noexcept {
  for (const ExportFormat format : kFormats) {
    if (to_string(format) == text) return format;
  }
  return std::nullopt;
}

std::span<const ExportFormat> export_formats() noexcept { return kFormats; }

std::string_view format_label(ExportFormat format) noexcept {
  switch (format) {
    case ExportFormat::Hevc: return "HEVC (H.265)";
    case ExportFormat::Hevc10: return "HEVC 10-bit";
    case ExportFormat::ProRes: return "Apple ProRes 422 HQ";
    case ExportFormat::DnxHr: return "Avid DNxHR HQ";
    case ExportFormat::H264: break;
  }
  return "H.264";
}

bool is_intermediate(ExportFormat format) noexcept {
  return format == ExportFormat::ProRes || format == ExportFormat::DnxHr;
}

std::span<const ExportPreset> built_in_export_presets() {
  static const std::vector<ExportPreset> presets = make_built_ins();
  return presets;
}

std::expected<ExportPlan, std::string> plan_for(const ExportPreset& preset,
                                                const core::Project& project) {
  const core::Sequence& sequence = project.sequence();
  ExportPlan plan;
  plan.format = preset.format;
  plan.quality = preset.quality;
  plan.fps = preset.fps > 0.0 ? preset.fps : sequence.fps;
  // Matching an HDR sequence in a format with no HDR is Rec. 709, tone mapped
  // on the way, as Premiere writes H.264 from an HDR sequence. Only asking
  // for HDR outright in one is refused.
  const bool carries_hdr = preset.format != ExportFormat::H264 &&
                           preset.format != ExportFormat::DnxHr;
  plan.colour = preset.colour.value_or(carries_hdr ? sequence.colour : WorkingColour::Rec709);
  plan.audio = preset.audio;
  plan.channels = preset.channels;
  plan.separate_audio = preset.separate_audio;
  plan.extension = is_intermediate(preset.format) ? ".mov" : ".mp4";

  const bool hdr = plan.colour != WorkingColour::Rec709;
  switch (preset.format) {
    case ExportFormat::Hevc10:
    case ExportFormat::ProRes: plan.bit_depth = 10; break;
    case ExportFormat::Hevc: plan.bit_depth = hdr ? 10 : 8; break;
    case ExportFormat::H264:
    case ExportFormat::DnxHr: plan.bit_depth = 8; break;
  }
  if (hdr && !carries_hdr) {
    return std::unexpected(std::string("HDR is written as HEVC or ProRes, not ") +
                           std::string(format_label(preset.format)));
  }

  // The short side is the one named, so a vertical sequence at "1080p" is
  // 1080 wide and stays vertical, as a phone expects.
  const double width = std::max(2, sequence.canvas_w);
  const double height = std::max(2, sequence.canvas_h);
  if (preset.short_side > 0) {
    const double scale = static_cast<double>(preset.short_side) / std::min(width, height);
    plan.width = even(width * scale);
    plan.height = even(height * scale);
  } else {
    plan.width = even(width);
    plan.height = even(height);
  }
  if (preset.format == ExportFormat::DnxHr && (plan.width < 256 || plan.height < 120)) {
    return std::unexpected("DNxHR needs a picture at least 256 by 120");
  }
  return plan;
}

std::vector<ExportPreset> all_export_presets(const SavedExportPresets& saved) {
  std::vector<ExportPreset> out(built_in_export_presets().begin(), built_in_export_presets().end());
  out.insert(out.end(), saved.presets.begin(), saved.presets.end());
  return out;
}

bool same_settings(const ExportPreset& a, const ExportPreset& b) noexcept {
  // The quality is only compared where there is one to choose.
  return a.format == b.format && (is_intermediate(a.format) || a.quality == b.quality) &&
         a.short_side == b.short_side && a.fps == b.fps && a.colour == b.colour &&
         a.audio == b.audio && a.channels == b.channels && a.separate_audio == b.separate_audio;
}

std::string matching_preset_name(const SavedExportPresets& saved, const ExportPreset& settings) {
  // The one it is named after first, so two presets that happen to agree do
  // not have the menu jump from one name to the other.
  for (const ExportPreset& preset : all_export_presets(saved)) {
    if (preset.name == settings.name && same_settings(preset, settings)) return preset.name;
  }
  return "Custom";
}

std::expected<void, std::string> save_export_preset(SavedExportPresets& saved,
                                                    ExportPreset preset) {
  if (preset.name.empty()) return std::unexpected("A preset needs a name.");
  if (std::ranges::any_of(built_in_export_presets(),
                          [&](const ExportPreset& p) { return p.name == preset.name; })) {
    return std::unexpected("\"" + preset.name + "\" is a built-in preset; save it as another name.");
  }
  preset.built_in = false;
  const auto found = std::ranges::find(saved.presets, preset.name, &ExportPreset::name);
  if (found != saved.presets.end()) {
    *found = std::move(preset);
  } else {
    saved.presets.push_back(std::move(preset));
  }
  return {};
}

bool delete_export_preset(SavedExportPresets& saved, std::string_view name) {
  return std::erase_if(saved.presets, [&](const ExportPreset& p) { return p.name == name; }) > 0;
}

std::string to_json(const SavedExportPresets& saved, int indent) {
  json out;
  out["version"] = kSchemaVersion;
  json list = json::array();
  for (const ExportPreset& preset : saved.presets) list.push_back(to_json_value(preset));
  out["presets"] = std::move(list);
  return out.dump(indent);
}

std::expected<SavedExportPresets, std::string> saved_export_presets_from_json(
    std::string_view text) {
  const json document = json::parse(text, nullptr, false);
  if (document.is_discarded()) return std::unexpected("the export presets file is not valid JSON");
  if (!document.is_object()) return std::unexpected("an export presets file has to be an object");
  if (document.value("version", 0) > kSchemaVersion) {
    return std::unexpected("those export presets were written by a newer version of Cutline");
  }
  SavedExportPresets saved;
  if (const auto list = document.find("presets"); list != document.end() && list->is_array()) {
    for (const json& entry : *list) {
      // A preset this build cannot read -- a format it does not have -- is
      // passed over rather than costing the rest.
      if (auto preset = preset_from(entry); preset.has_value()) {
        (void)save_export_preset(saved, std::move(*preset));
      }
    }
  }
  return saved;
}

std::filesystem::path default_export_presets_path() {
  const char* roaming = std::getenv("APPDATA");
  const std::filesystem::path base =
      roaming == nullptr ? std::filesystem::path{"."} : std::filesystem::path{roaming};
  return base / "Cutline" / "export_presets.json";
}

std::expected<SavedExportPresets, std::string> read_export_presets(
    const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return SavedExportPresets{};
  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad()) return std::unexpected("could not read " + shown(path));
  return saved_export_presets_from_json(text.str());
}

std::expected<void, std::string> write_export_presets(const std::filesystem::path& path,
                                                      const SavedExportPresets& saved) {
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return std::unexpected("could not make " + shown(path.parent_path()) + ": " +
                             error.message());
    }
  }
  std::filesystem::path staging = path;
  staging += ".saving";
  {
    std::ofstream file(staging, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected("could not write " + shown(staging));
    file << to_json(saved);
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
