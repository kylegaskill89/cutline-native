#pragma once

/// Warp Stabilizer's first step: following corners through every frame of the
/// clip, as Premiere does in the background when the effect is applied.

#include "cutline/core/model.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/track/stabilize.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace cutline::engine {

/// Told after each frame, with how many there are; false stops it.
using AnalysisProgress = std::function<bool(int done, int total)>;

/// Analyses `clip_id` for its Warp Stabilizer effect at `effect`. Detailed
/// Analysis follows twice the corners at a larger size. An error when the
/// clip cannot be drawn, or when it was stopped.
[[nodiscard]] std::expected<track::Analysis, std::string> analyse_for_stabilizer(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const std::string& clip_id, std::size_t effect, const AnalysisProgress& progress = {});

}  // namespace cutline::engine
