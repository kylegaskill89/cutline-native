#pragma once

/// Auto Reframe's looking: the clip's frames, ten a second, each asked where
/// a window of the sequence's shape is best placed, and where the shots
/// change. See `track/reframe.hpp`.

#include "cutline/core/model.hpp"
#include "cutline/engine/stabilizer_analysis.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/track/reframe.hpp"

#include <expected>
#include <memory>
#include <string>

namespace cutline::engine {

/// How many frames a second are looked at. A subject does not cross a frame
/// in a tenth of a second, and the path is smoothed over longer than that.
inline constexpr double kReframeAnalysisRate = 10.0;

/// Analyses `clip_id` for its Auto Reframe. An error when the clip cannot be
/// drawn, has no Auto Reframe, or the looking was stopped.
[[nodiscard]] std::expected<track::ReframeAnalysis, std::string> analyse_for_reframe(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const std::string& clip_id, const AnalysisProgress& progress = {});

}  // namespace cutline::engine
