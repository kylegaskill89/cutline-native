#pragma once

/// Premiere's Warp Stabilizer, as a pass, and the analysis it draws on.
///
/// The analysis is corners followed through every frame of the clip, which
/// takes a while and is done once, in the background, as Premiere does it. It
/// is kept in a file of its own, named for what was analysed: the clip's
/// source, which part of it, and at what speed. So the same shot stabilised
/// twice is analysed once, and trimming or retiming it makes the name change
/// and the analysis run again, without the project keeping any record of it.
///
/// What a frame shows is worked out from the analysis and the effect's
/// settings, for the whole clip at once because smoothing looks either side,
/// and kept until the settings change.

#include "cutline/core/model.hpp"
#include "cutline/render/effect_passes.hpp"
#include "cutline/track/stabilize.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cutline::render {

/// Where analyses are kept: Cutline's folder under the user's local
/// application data, or wherever `set_analysis_folder` last said.
[[nodiscard]] std::filesystem::path analysis_folder();
void set_analysis_folder(std::filesystem::path folder);

/// The name of the analysis `effect` on `clip` needs. Empty when the clip has
/// nothing to analyse.
[[nodiscard]] std::string analysis_key(const core::Project& project, const core::Clip& clip,
                                       const core::ClipEffect& effect);
[[nodiscard]] std::filesystem::path analysis_path(const std::string& key);
[[nodiscard]] bool has_analysis(const std::string& key);

/// The effect's controls, as the stabiliser takes them.
[[nodiscard]] track::StabilizeSettings stabilize_settings(const core::ClipEffect& effect);

/// The stabilisation of an analysis with some settings: kept once worked
/// out. `wait` false gives nothing rather than working it out now.
[[nodiscard]] std::shared_ptr<const track::Stabilization> stabilization(
    const std::string& key, const track::StabilizeSettings& settings, bool wait = true);

/// The warp pass's data: columns, rows and the share left showing, then each
/// vertex's source as two fractions.
inline constexpr std::size_t kWarpHeader = 4;

/// Warp Stabilizer's pass for `effect` on `clip` at `local_t`, or none until
/// the clip has been analysed.
void plan_warp_stabilizer(const core::Project& project, const core::Clip& clip,
                          const core::ClipEffect& effect, double local_t,
                          std::vector<EffectPass>& passes);

}  // namespace cutline::render
