#pragma once

/// Lining a multi-camera shoot up by its sound, which needs the sound decoded
/// and so belongs here rather than in core beside the other ways of doing it.

#include "cutline/core/model.hpp"

#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace cutline::engine {

/// Told after each source is read, with how many there are; false stops it.
using SyncProgress = std::function<bool(int done, int total)>;

/// Where each source starts so that its sound agrees with the first's, in
/// seconds, the earliest at zero: what `core::create_multicam_sequence` takes.
/// An error naming a source with no sound, or one whose sound could not be
/// found in the first's.
[[nodiscard]] std::expected<std::vector<double>, std::string> multicam_audio_starts(
    const core::Project& project, std::span<const std::string> media_ids,
    const SyncProgress& progress = {});

}  // namespace cutline::engine
