#pragma once

/// What the Lumetri Color panel does to a project, apart from drawing it.
///
/// The panel edits one Lumetri Color effect on the selected clip, and adds one
/// when the clip has none, as Premiere's does on the first change. Its
/// eyedroppers and its Auto button work from pixels handed in, so all of it
/// can be tested without a picture on screen.

#include "cutline/core/model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// Where the clip's first Lumetri Color sits in its stack, if it has one.
[[nodiscard]] std::optional<std::size_t> lumetri_on(const core::Project& project,
                                                    std::string_view clip_id);

/// The clip with a Lumetri Color on it: the one it has, or a new one at the
/// end of its stack, as Premiere adds one when its panel is first used.
[[nodiscard]] core::Project with_lumetri(core::Project project, std::string_view clip_id,
                                         std::size_t* index = nullptr);

/// One of Lumetri's numbers, keyframed when it is animated, as Effect Controls
/// would set it.
[[nodiscard]] core::Project set_lumetri_param(core::Project project, std::string_view clip_id,
                                              std::string_view key, double value,
                                              double local_t);
/// Several at once, as one edit: Auto, and the eyedroppers.
[[nodiscard]] core::Project set_lumetri_params(
    core::Project project, std::string_view clip_id,
    const std::vector<std::pair<std::string, double>>& values, double local_t);
[[nodiscard]] core::Project set_lumetri_curve(core::Project project, std::string_view clip_id,
                                              std::string_view key,
                                              std::vector<core::CurvePoint> points);
[[nodiscard]] core::Project set_lumetri_file(core::Project project, std::string_view clip_id,
                                             std::string_view key, std::string path);

/// The project as it would be with one of the clip's Lumetri sections, or the
/// whole effect, switched off: what an eyedropper samples, since it measures
/// the picture before the correction it is about to set.
[[nodiscard]] core::Project without_lumetri(core::Project project, std::string_view clip_id,
                                            std::vector<std::string> sections = {});

/// Premiere's white balance selector: the Temperature and Tint that make a
/// colour picked from the picture neutral. 8-bit coded values in.
[[nodiscard]] std::pair<double, double> white_balance_for(std::uint8_t r, std::uint8_t g,
                                                          std::uint8_t b);

/// The HSL secondary's Set Colour: a key round a colour picked from the
/// picture, as parameter values.
[[nodiscard]] std::vector<std::pair<std::string, double>> key_for(std::uint8_t r,
                                                                  std::uint8_t g,
                                                                  std::uint8_t b);

/// Premiere's Auto in Basic Correction: Exposure, Contrast, Whites and Blacks
/// that spread the picture's tones across the range. 8-bit RGBA in.
[[nodiscard]] std::vector<std::pair<std::string, double>> auto_tone(
    const std::uint8_t* pixels, int width, int height);

/// Where looks are kept: Cutline's own, and any .cube put there.
[[nodiscard]] std::filesystem::path default_looks_folder();

/// Writes the looks that come with Cutline into `folder`, those not already
/// there. Its own, made here: Premiere's are Adobe's to ship.
void write_built_in_looks(const std::filesystem::path& folder);

/// Every .cube in a folder, by name.
[[nodiscard]] std::vector<std::filesystem::path> looks_in(const std::filesystem::path& folder);

/// The names of the looks that come with Cutline, in the order they are made.
[[nodiscard]] std::vector<std::string> built_in_look_names();

}  // namespace cutline::editor
