/// Measures the claim the rewrite rests on.
///
/// The TypeScript exporter rendered each output frame by seeking the source to
/// that time, which on 4K long-GOP footage cost about 4.5 seconds per frame.
/// This benchmark runs both access patterns over the same file so the
/// difference is a measurement rather than an assertion:
///
///   sequential  — decode straight through, the way export actually walks time
///   per-frame   — seek to each frame's timestamp, the way the old exporter did
///
/// Usage: decode_bench <file> [frames]

#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"

#include <chrono>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>

namespace {

using cutline::media::Acceleration;
using cutline::media::VideoDecoder;
using Clock = std::chrono::steady_clock;

struct Result {
  int frames = 0;
  double seconds = 0.0;

  [[nodiscard]] double fps() const { return seconds > 0.0 ? frames / seconds : 0.0; }
  [[nodiscard]] double ms_per_frame() const {
    return frames > 0 ? seconds * 1000.0 / frames : 0.0;
  }
};

/// Decodes forward from the start, which is how export reads a source.
Result run_sequential(std::string_view path, int limit, Acceleration preferred) {
  auto decoder = VideoDecoder::open(path, {.preferred = preferred});
  if (!decoder) {
    std::println(stderr, "  open failed: {}", decoder.error());
    return {};
  }

  Result result;
  const auto started = Clock::now();
  while (result.frames < limit) {
    const auto got = (*decoder)->next_frame();
    if (!got) {
      std::println(stderr, "  decode failed: {}", got.error());
      break;
    }
    if (!*got) break;  // end of stream
    ++result.frames;
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  return result;
}

/// Seeks to each frame's timestamp before decoding it, reproducing the access
/// pattern that made the old exporter unusable.
Result run_per_frame_seek(std::string_view path, int limit, double fps, Acceleration preferred) {
  auto decoder = VideoDecoder::open(path, {.preferred = preferred});
  if (!decoder) {
    std::println(stderr, "  open failed: {}", decoder.error());
    return {};
  }

  const double frame_duration = fps > 0.0 ? 1.0 / fps : 1.0 / 30.0;
  Result result;
  const auto started = Clock::now();
  for (int i = 0; i < limit; ++i) {
    const double target = i * frame_duration;
    if (const auto sought = (*decoder)->seek(target); !sought) {
      std::println(stderr, "  seek failed: {}", sought.error());
      break;
    }
    // Decode forward from the keyframe until the wanted time is reached, which
    // is the part that costs: the whole GOP is decoded to reach one frame.
    bool reached = false;
    while (!reached) {
      const auto got = (*decoder)->next_frame();
      if (!got || !*got) break;
      if ((*decoder)->timestamp() + 1e-6 >= target) reached = true;
    }
    ++result.frames;
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  return result;
}

void report(std::string_view label, const Result& r) {
  std::println("  {:<22} {:>5} frames in {:>7.2f}s   {:>8.1f} fps   {:>9.2f} ms/frame", label,
               r.frames, r.seconds, r.fps(), r.ms_per_frame());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::println(stderr, "usage: decode_bench <file> [frames]");
    return 2;
  }

  const std::string path = argv[1];
  const int limit = argc > 2 ? std::atoi(argv[2]) : 300;

  const auto info = cutline::media::probe(path);
  if (!info) {
    std::println(stderr, "probe failed: {}", info.error());
    return 1;
  }

  std::println("file      {}", info->path);
  std::println("format    {}   {:.2f}s   {:.1f} Mbps", info->format, info->duration,
               static_cast<double>(info->bit_rate) / 1e6);

  const auto* video = info->primary_video();
  if (video == nullptr) {
    std::println(stderr, "no video stream");
    return 1;
  }

  std::println("video     {} {}x{} @ {:.3f} fps   {}", video->codec, video->width, video->height,
               video->fps, video->pixel_format);
  std::println("colour    {} / {}   {}-bit   {}", to_string(video->color.primaries),
               to_string(video->color.transfer), video->color.bits_per_component,
               video->color.is_hdr() ? "HDR" : "SDR");
  std::println("audio     {} stream(s)", info->audio.size());
  std::println("");

  // Report which path was actually taken; hardware support is not guaranteed.
  {
    auto probe_decoder = VideoDecoder::open(path);
    if (probe_decoder) {
      std::println("decoder   {}", to_string((*probe_decoder)->acceleration()));
    }
  }
  std::println("");

  const Result sequential = run_sequential(path, limit, Acceleration::D3D12Va);
  report("sequential (d3d12)", sequential);

  const Result sequential_11 = run_sequential(path, limit, Acceleration::D3D11Va);
  report("sequential (d3d11)", sequential_11);

  const Result sequential_sw = run_sequential(path, limit, Acceleration::Software);
  report("sequential (cpu)", sequential_sw);

  // Seeking is slow enough that a full run would take far too long, so this
  // samples a smaller number of frames and reports the per-frame cost.
  const int seek_limit = limit < 30 ? limit : 30;
  const Result seeking = run_per_frame_seek(path, seek_limit, video->fps, Acceleration::D3D12Va);
  report("per-frame seek", seeking);

  std::println("");
  if (sequential.ms_per_frame() > 0.0 && seeking.ms_per_frame() > 0.0) {
    std::println("per-frame seeking costs {:.1f}x more per frame than decoding in order",
                 seeking.ms_per_frame() / sequential.ms_per_frame());
  }
  if (sequential.ms_per_frame() > 0.0 && sequential_sw.ms_per_frame() > 0.0) {
    std::println("hardware decode is {:.1f}x faster than software",
                 sequential_sw.ms_per_frame() / sequential.ms_per_frame());
  }
  if (sequential.fps() > 0.0 && video->fps > 0.0) {
    std::println("sequential decode runs at {:.1f}x realtime", sequential.fps() / video->fps);
  }

  return 0;
}
