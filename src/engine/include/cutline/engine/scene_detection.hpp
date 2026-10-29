#pragma once

/// Scene Edit Detection's looking: every frame of a clip, compared with the
/// one before, to find where the footage was cut before it arrived.

#include "cutline/core/model.hpp"
#include "cutline/engine/stabilizer_analysis.hpp"
#include "cutline/gpu/device.hpp"

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace cutline::engine {

/// The timeline times inside `clip_id` where a new shot begins, at the
/// sequence's frames. The clip is looked at as it plays, so a retimed or
/// reversed clip's cuts are where they are seen. An error when the clip has no
/// picture, or when it was stopped.
[[nodiscard]] std::expected<std::vector<double>, std::string> detect_scene_edits(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const std::string& clip_id, const AnalysisProgress& progress = {});

}  // namespace cutline::engine
