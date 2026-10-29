#pragma once

/// Morph Cut's looking: both sides of the cut drawn alone at every frame of
/// the transition, and the motion from one to the other found in each. See
/// `track/morph.hpp` and `render/morph_cut.hpp`.

#include "cutline/core/model.hpp"
#include "cutline/engine/stabilizer_analysis.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/track/morph.hpp"

#include <expected>
#include <memory>
#include <string>

namespace cutline::engine {

/// Analyses the Morph Cut at the out-edge of `clip_id`. An error when there is
/// none, a side cannot be drawn, or the looking was stopped.
[[nodiscard]] std::expected<track::MorphAnalysis, std::string> analyse_morph_cut(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const std::string& clip_id, const AnalysisProgress& progress = {});

}  // namespace cutline::engine
