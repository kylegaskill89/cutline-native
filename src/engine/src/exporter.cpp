#include "cutline/engine/exporter.hpp"

#include "cutline/core/query.hpp"
#include "cutline/engine/audio_mixer.hpp"
#include "cutline/engine/frame_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <set>
#include <vector>

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

  // Built before the writer, because whether the file gets an audio stream at
  // all depends on whether anything is audible — and a container records its
  // streams in the header, so that has to be known up front.
  std::unique_ptr<AudioMixer> mixer;
  if (settings.audio) {
    auto built = AudioMixer::create(project, {.sample_rate = settings.audio_sample_rate,
                                              .channels = settings.audio_channels});
    if (!built) return std::unexpected(built.error());
    if (!(*built)->silent()) mixer = std::move(*built);
  }

  media::AudioEncodeSettings encode_audio;
  encode_audio.enabled = mixer != nullptr;
  encode_audio.sample_rate = settings.audio_sample_rate;
  encode_audio.channels = settings.audio_channels;
  encode_audio.bitrate = settings.audio_bitrate;

  auto writer = media::MediaWriter::create(settings.path, encode, encode_audio);
  if (!writer) return std::unexpected(writer.error());

  ExportResult result;
  result.encoder = (*writer)->encoder_name();
  if ((*writer)->has_audio()) result.audio_encoder = (*writer)->audio_encoder_name();

  // Deduplicated: a source missing for one frame is missing for all of them,
  // and a per-frame list would be thousands of identical lines.
  std::set<std::string> missing;
  if (mixer) {
    for (const std::string& id : mixer->missing_media()) missing.insert(id);
  }

  // Audio is written a frame's worth at a time, interleaved with the video, so
  // the muxer never has to buffer one stream while it waits for the other.
  const auto channels = static_cast<std::size_t>(settings.audio_channels);
  const double rate = static_cast<double>(settings.audio_sample_rate);
  std::vector<float> audio_block;
  std::int64_t audio_fed = 0;
  const auto audio_total = static_cast<std::int64_t>(std::llround(duration * rate));

  const auto began = std::chrono::steady_clock::now();

  // Prime the limiter. Its look-ahead delays output by a few milliseconds, and
  // the first block out is that much silence; feeding it and throwing the
  // silence away is what keeps audio aligned to picture instead of trailing it.
  if (mixer) {
    const auto latency = std::min(static_cast<std::int64_t>(mixer->latency_frames()),
                                  audio_total);
    if (latency > 0) {
      audio_block.assign(static_cast<std::size_t>(latency) * channels, 0.0f);
      if (auto ok = mixer->mix(start, audio_block); !ok) return std::unexpected(ok.error());
      audio_fed = latency;
    }
  }

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

    if (mixer) {
      // How much audio this frame of video should be accompanied by, computed
      // from the frame index rather than accumulated so the two stay locked
      // together over a long timeline.
      const auto want = std::min(
          audio_total,
          static_cast<std::int64_t>(std::llround(static_cast<double>(frame + 1) / fps * rate)));
      if (const std::int64_t need = want - audio_fed; need > 0) {
        audio_block.assign(static_cast<std::size_t>(need) * channels, 0.0f);
        const double at = start + static_cast<double>(audio_fed) / rate;
        if (auto ok = mixer->mix(at, audio_block); !ok) {
          return std::unexpected(std::format("frame {}: {}", frame, ok.error()));
        }
        if (auto ok = (*writer)->write_audio(audio_block); !ok) {
          return std::unexpected(std::format("frame {}: {}", frame, ok.error()));
        }
        audio_fed = want;
      }
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

  // The last few milliseconds are still inside the limiter's look-ahead;
  // without this they are simply lost off the end of the timeline.
  if (mixer && mixer->latency_frames() > 0) {
    audio_block.assign(mixer->latency_frames() * channels, 0.0f);
    mixer->flush(audio_block);
    if (auto ok = (*writer)->write_audio(audio_block); !ok) {
      return std::unexpected(ok.error());
    }
  }

  if (auto ok = (*writer)->finish(); !ok) return std::unexpected(ok.error());

  const auto ended = std::chrono::steady_clock::now();
  result.audio_frames = (*writer)->audio_frame_count();
  result.frames = total;
  result.duration_seconds = static_cast<double>(total) / fps;
  result.elapsed_seconds = std::chrono::duration<double>(ended - began).count();
  result.missing_media.assign(missing.begin(), missing.end());
  return result;
}

}  // namespace cutline::engine
