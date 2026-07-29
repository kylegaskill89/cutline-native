/// Renders a project to a video file.
///
/// The headless exporter: no window, no UI, one command. It is what proves the
/// premise of the rewrite end to end, and it reports the throughput so a
/// regression is visible rather than merely felt.
///
///     export_project <project.json> <out.mp4> [--hevc] [--software]
///                    [--quality N] [--bitrate BPS] [--fps N]
///                    [--width N] [--height N] [--mono] [--no-audio]
///                    [--start S] [--duration S]

#include "cutline/core/serialize.hpp"
#include "cutline/engine/exporter.hpp"
#include "cutline/gpu/device.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>

namespace {

/// Formats seconds as `M:SS`, which is easier to read at a glance than a raw
/// count once an export runs to minutes.
[[nodiscard]] std::string as_clock(double seconds) {
  if (seconds < 0.0 || !std::isfinite(seconds)) return "--:--";
  const auto total = static_cast<long long>(seconds + 0.5);
  return std::format("{}:{:02}", total / 60, total % 60);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::println(stderr,
                 "usage: export_project <project.json> <out.mp4> [--hevc] [--software]\n"
                 "                      [--quality N] [--bitrate BPS] [--fps N]\n"
                 "                      [--width N] [--height N] [--mono] [--no-audio]\n"
                 "                      [--start S] [--duration S]");
    return 2;
  }

  cutline::engine::ExportSettings settings;
  settings.path = argv[2];

  for (int i = 3; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const auto value = [&](double fallback) {
      return i + 1 < argc ? std::strtod(argv[++i], nullptr) : fallback;
    };

    if (flag == "--hevc") {
      settings.codec = cutline::media::VideoCodec::Hevc;
    } else if (flag == "--software") {
      settings.preference = cutline::media::EncoderPreference::Software;
    } else if (flag == "--hardware") {
      settings.preference = cutline::media::EncoderPreference::Hardware;
    } else if (flag == "--quality") {
      settings.quality = static_cast<int>(value(settings.quality));
    } else if (flag == "--bitrate") {
      settings.bitrate = static_cast<std::int64_t>(value(0));
    } else if (flag == "--fps") {
      settings.fps = value(0);
    } else if (flag == "--width") {
      settings.width = static_cast<int>(value(0));
    } else if (flag == "--height") {
      settings.height = static_cast<int>(value(0));
    } else if (flag == "--mono") {
      settings.audio_channels = 1;
    } else if (flag == "--no-audio") {
      settings.audio = false;
    } else if (flag == "--start") {
      settings.start = value(0);
    } else if (flag == "--duration") {
      settings.duration = value(0);
    } else {
      std::println(stderr, "unknown option: {}", flag);
      return 2;
    }
  }

  std::ifstream in(argv[1]);
  if (!in) {
    std::println(stderr, "cannot open {}", argv[1]);
    return 1;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();

  const auto loaded = cutline::core::from_json(buffer.str());
  if (!loaded) {
    std::println(stderr, "cannot read the project: {}", loaded.error());
    return 1;
  }
  for (const std::string& warning : loaded->warnings) {
    std::println(stderr, "warning: {}", warning);
  }

  auto device = cutline::gpu::Device::create({.allow_software = true});
  if (!device) {
    std::println(stderr, "cannot create a device: {}", device.error());
    return 1;
  }

  const cutline::core::Project& project = loaded->project;
  std::println("{}x{} @ {:.3f} fps on {}",
               settings.width > 0 ? settings.width : project.canvas_w,
               settings.height > 0 ? settings.height : project.canvas_h,
               settings.fps > 0.0 ? settings.fps : project.fps, (*device)->adapter_name());

  auto last_report = std::chrono::steady_clock::now();
  const auto progress = [&](const cutline::engine::ExportProgress& p) {
    // Rate-limited: a line per frame would drown the useful output and cost
    // more than the rendering on a short export.
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report < std::chrono::milliseconds(500) && p.frame != p.total) return true;
    last_report = now;

    std::print("\r  frame {}/{}  ({:.1f}%)  t={}   ", p.frame, p.total,
               100.0 * static_cast<double>(p.frame) / static_cast<double>(p.total),
               as_clock(p.timeline_seconds));
    std::fflush(stdout);
    return true;
  };

  const auto result = cutline::engine::export_project(*device, project, settings, progress);
  std::println("");

  if (!result) {
    std::println(stderr, "export failed: {}", result.error());
    return 1;
  }

  for (const std::string& id : result->missing_media) {
    std::println(stderr, "warning: media {} could not be read; its clips are missing", id);
  }

  std::println("{} frames ({}) in {:.2f}s — {:.1f} fps, {:.2f}x realtime, via {}",
               result->frames, as_clock(result->duration_seconds), result->elapsed_seconds,
               result->frames_per_second(),
               result->elapsed_seconds > 0.0 ? result->duration_seconds / result->elapsed_seconds
                                             : 0.0,
               result->encoder);
  std::println("wrote {}", settings.path);
  return 0;
}
