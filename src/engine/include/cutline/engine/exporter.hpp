#pragma once

/// Rendering a whole project to a file.
///
/// This is the payoff of the rewrite, and it is deliberately thin: walk the
/// timeline forwards, render each frame through the same `FrameRenderer` the
/// preview uses, hand it to the encoder. There is no separate export
/// compositor, no filter-graph compiler, and no "frame-accurate mode" — those
/// existed in the TypeScript version because preview and export were different
/// machines that had to be kept agreeing. Here there is one machine.
///
/// Walking forwards is not incidental. It is the access pattern sequential
/// decoding is built for, and the difference between ~1.6 ms and ~28 ms per
/// frame of source.

#include "cutline/core/model.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/media/encoder.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace cutline::engine {

struct ExportSettings {
  std::string path;

  media::VideoCodec codec = media::VideoCodec::H264;
  media::EncoderPreference preference = media::EncoderPreference::Auto;
  std::int64_t bitrate = 0;
  int quality = 20;

  /// Output frame rate. Zero takes the project's.
  double fps = 0.0;

  /// The span to render. A zero or negative `duration` takes the whole
  /// timeline, which is what exporting normally means.
  double start = 0.0;
  double duration = 0.0;
};

struct ExportProgress {
  std::int64_t frame = 0;
  std::int64_t total = 0;
  double timeline_seconds = 0.0;
};

struct ExportResult {
  std::int64_t frames = 0;
  double duration_seconds = 0.0;  ///< of the output, not of the export
  double elapsed_seconds = 0.0;
  std::string encoder;
  /// Media ids that could not be read, deduplicated. An export completes
  /// despite these, with holes where they would have been.
  std::vector<std::string> missing_media;

  /// Output frames per second of wall-clock time. Above the project's frame
  /// rate means the export ran faster than realtime.
  [[nodiscard]] double frames_per_second() const noexcept {
    return elapsed_seconds > 0.0 ? static_cast<double>(frames) / elapsed_seconds : 0.0;
  }
};

/// Returning false from the progress callback cancels the export, which leaves
/// a truncated file rather than a complete one.
using ProgressCallback = std::function<bool(const ExportProgress&)>;

[[nodiscard]] std::expected<ExportResult, std::string> export_project(
    const std::shared_ptr<gpu::Device>& device, const core::Project& project,
    const ExportSettings& settings, const ProgressCallback& on_progress = {});

}  // namespace cutline::engine
