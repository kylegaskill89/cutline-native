#pragma once

/// Export presets: what a file is written as, named, built in or saved.
///
/// The Export window used to start from the same five dropdowns every time,
/// so writing the same kind of file twice meant setting them twice, and there
/// was no way to say "the YouTube one" at all. A preset is those choices with
/// a name: Premiere's own built-ins -- Match Source, the platforms, the
/// intermediates, HDR10 -- and any the person saves.
///
/// What a preset means depends on the sequence it is used with: "match
/// source" is the sequence's size and rate, and a size is given as the
/// picture's short side so a vertical sequence stays vertical. `plan_for`
/// works that out into the numbers an export takes, and says so plainly when
/// a preset cannot be written the way it asks -- HDR in H.264, which Premiere
/// does not do either. Matching an HDR sequence in H.264 is not that: it is
/// written as Rec. 709, tone mapped, as Premiere writes it.

#include "cutline/core/model.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// What the picture is encoded as.
enum class ExportFormat {
  H264,
  Hevc,
  /// HEVC at ten bits: smoother gradients, and what HDR needs.
  Hevc10,
  /// Apple ProRes 422 HQ, the intermediate for a grade or a finish.
  ProRes,
  /// Avid DNxHR HQ, the same for Avid's side of the world.
  DnxHr,
};

[[nodiscard]] std::string_view to_string(ExportFormat format) noexcept;
[[nodiscard]] std::optional<ExportFormat> export_format_from_string(std::string_view text) noexcept;
/// The formats as the Export window offers them, and what each is called.
[[nodiscard]] std::span<const ExportFormat> export_formats() noexcept;
[[nodiscard]] std::string_view format_label(ExportFormat format) noexcept;
/// An intermediate has no quality to choose and is written to a .mov.
[[nodiscard]] bool is_intermediate(ExportFormat format) noexcept;

struct ExportPreset {
  std::string name;
  ExportFormat format = ExportFormat::H264;
  /// The encoder's constant quality, lower being better; unused by the
  /// intermediates, whose quality is their profile.
  int quality = 18;
  /// The picture's short side in pixels -- 1080 for "1080p", whichever way
  /// round the sequence is -- or zero for the sequence's own size.
  int short_side = 0;
  /// Frames a second, or zero for the sequence's own.
  double fps = 0.0;
  /// The colour written, or the sequence's when there is none.
  std::optional<core::WorkingColour> colour;
  bool audio = true;
  int channels = 2;
  /// One stream per audio track rather than a mixdown, for a mix elsewhere.
  bool separate_audio = false;
  /// One of Premiere's, which cannot be deleted or overwritten.
  bool built_in = false;

  friend bool operator==(const ExportPreset&, const ExportPreset&) = default;
};

/// The built-in presets, in the order the Export window lists them.
[[nodiscard]] std::span<const ExportPreset> built_in_export_presets();

/// A preset worked out against a sequence: the numbers an export is given.
struct ExportPlan {
  ExportFormat format = ExportFormat::H264;
  int quality = 18;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  core::WorkingColour colour = core::WorkingColour::Rec709;
  int bit_depth = 8;
  bool audio = true;
  int channels = 2;
  bool separate_audio = false;
  /// ".mp4" or ".mov", with the dot.
  std::string extension;

  friend bool operator==(const ExportPlan&, const ExportPlan&) = default;
};

/// What a preset means for this project's open sequence, or why it cannot be
/// written as it asks.
[[nodiscard]] std::expected<ExportPlan, std::string> plan_for(const ExportPreset& preset,
                                                             const core::Project& project);

/// The presets the person has saved.
struct SavedExportPresets {
  std::vector<ExportPreset> presets;
  friend bool operator==(const SavedExportPresets&, const SavedExportPresets&) = default;
};

/// Every preset there is: the built-ins, then the saved ones.
[[nodiscard]] std::vector<ExportPreset> all_export_presets(const SavedExportPresets& saved);

/// Whether two presets would write the same file, whatever they are called.
[[nodiscard]] bool same_settings(const ExportPreset& a, const ExportPreset& b) noexcept;
/// The name of the preset these settings are, built in or saved, or "Custom"
/// when they have been changed from it -- what Premiere's Preset menu shows.
[[nodiscard]] std::string matching_preset_name(const SavedExportPresets& saved,
                                               const ExportPreset& settings);

/// Saves a preset under its name, replacing a saved one of that name. A
/// built-in's name is refused, as Premiere refuses it, so a built-in always
/// means what it says.
[[nodiscard]] std::expected<void, std::string> save_export_preset(SavedExportPresets& saved,
                                                                  ExportPreset preset);
/// Removes a saved preset by name. A built-in cannot be removed.
bool delete_export_preset(SavedExportPresets& saved, std::string_view name);

[[nodiscard]] std::string to_json(const SavedExportPresets& saved, int indent = 2);
[[nodiscard]] std::expected<SavedExportPresets, std::string> saved_export_presets_from_json(
    std::string_view text);
/// Beside the other preferences: `%APPDATA%\Cutline\export_presets.json`.
[[nodiscard]] std::filesystem::path default_export_presets_path();
/// A missing file is no saved presets, not an error.
[[nodiscard]] std::expected<SavedExportPresets, std::string> read_export_presets(
    const std::filesystem::path& path);
[[nodiscard]] std::expected<void, std::string> write_export_presets(
    const std::filesystem::path& path, const SavedExportPresets& saved);

}  // namespace cutline::editor
