#include "cutline/engine/exporter.hpp"

#include "cutline/core/query.hpp"
#include "cutline/engine/frame_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <set>

namespace cutline::engine {

std::expected<ExportResult, std::string> export_project(
    const std::shared_ptr<gpu::Device>& device, const core::Project& project,
    const ExportSettings& settings, const ProgressCallback& on_progress) {
  if (!device) return std::unexpected("an export needs a device");
  if (settings.path.empty()) return std::unexpected("an export needs an output path");

  const double fps = settings.fps > 0.0 ? settings.fps : project.fps;
  if (fps <= 0.0) return std::unexpected("the output frame rate must be positive");

  const double timeline = core::timeline_duration(project);
  const double start = std::max(0.0, settings.start);
  const double duration =
      settings.duration > 0.0 ? settings.duration : std::max(0.0, timeline - start);

  if (duration <= 0.0) {
    return std::unexpected("there is nothing to export: the timeline is empty");
  }

  // Round rather than truncate, so a timeline that is a whole number of frames
  // long does not lose its last one to floating-point error.
  const auto total = static_cast<std::int64_t>(std::llround(duration * fps));
  if (total <= 0) return std::unexpected("the export range is shorter than one frame");

  auto renderer = FrameRenderer::create(device, project.canvas_w, project.canvas_h);
  if (!renderer) return std::unexpected(renderer.error());

  media::VideoEncodeSettings encode;
  encode.width = project.canvas_w;
  encode.height = project.canvas_h;
  encode.fps = fps;
  encode.codec = settings.codec;
  encode.preference = settings.preference;
  encode.bitrate = settings.bitrate;
  encode.quality = settings.quality;

  auto writer = media::MediaWriter::create(settings.path, encode);
  if (!writer) return std::unexpected(writer.error());

  ExportResult result;
  result.encoder = (*writer)->encoder_name();

  // Deduplicated: a source missing for one frame is missing for all of them,
  // and a per-frame list would be thousands of identical lines.
  std::set<std::string> missing;

  const auto began = std::chrono::steady_clock::now();

  for (std::int64_t frame = 0; frame < total; ++frame) {
    // Time is computed from the frame index rather than accumulated, so a
    // rounding error cannot drift over a long timeline.
    const double t = start + static_cast<double>(frame) / fps;

    if (auto ok = (*renderer)->render(project, t); !ok) {
      return std::unexpected(std::format("frame {}: {}", frame, ok.error()));
    }
    for (const std::string& id : (*renderer)->missing_media()) missing.insert(id);

    const auto image = (*renderer)->read_back();
    if (!image) return std::unexpected(std::format("frame {}: {}", frame, image.error()));

    if (auto ok = (*writer)->write_frame(image->pixels); !ok) {
      return std::unexpected(std::format("frame {}: {}", frame, ok.error()));
    }

    if (on_progress) {
      const ExportProgress progress{.frame = frame + 1, .total = total, .timeline_seconds = t};
      if (!on_progress(progress)) {
        // Cancelled. The file is deliberately left unfinalised, so it is
        // obviously incomplete rather than a short but valid export.
        return std::unexpected("export cancelled");
      }
    }
  }

  if (auto ok = (*writer)->finish(); !ok) return std::unexpected(ok.error());

  const auto ended = std::chrono::steady_clock::now();
  result.frames = total;
  result.duration_seconds = static_cast<double>(total) / fps;
  result.elapsed_seconds = std::chrono::duration<double>(ended - began).count();
  result.missing_media.assign(missing.begin(), missing.end());
  return result;
}

}  // namespace cutline::engine
