#pragma once

/// Writing encoded video to a file.
///
/// The encoder is chosen at runtime rather than fixed at build time, because
/// which one exists depends on the machine: NVENC on NVIDIA, QSV on Intel, AMF
/// on AMD, and x264/x265 everywhere. Hardware is tried first and software is
/// the fallback, so an export always completes — slower on a machine without a
/// supported GPU, rather than failing on one.
///
/// Frames arrive as 8-bit sRGB RGBA, which is what the compositor reads back,
/// and are converted to the encoder's pixel format here. That conversion is on
/// the CPU for now; doing it in a shader and handing the encoder a GPU surface
/// is the obvious next optimisation, and the interface does not assume either.

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::media {

/// Which encoder family to use. `Auto` picks the best available.
enum class EncoderPreference {
  Auto,
  Hardware,  ///< fail rather than silently fall back, for benchmarking
  Software,
};

enum class VideoCodec {
  H264,
  Hevc,
};

[[nodiscard]] std::string_view to_string(VideoCodec codec) noexcept;

struct VideoEncodeSettings {
  int width = 1920;
  int height = 1080;
  double fps = 30.0;

  VideoCodec codec = VideoCodec::H264;
  EncoderPreference preference = EncoderPreference::Auto;

  /// Target bitrate in bits per second. Zero asks the encoder for its own
  /// quality-based default, which is usually what a user wants.
  std::int64_t bitrate = 0;

  /// Quality when `bitrate` is zero: lower is better, in the encoder's own
  /// scale (CRF for x264/x265, CQ for the hardware encoders).
  int quality = 20;
};

/// Encodes frames and muxes them into a container. Video only for now; audio
/// is the next phase and will join here rather than in a separate writer, so
/// that interleaving stays this class's problem.
class MediaWriter {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<MediaWriter>, std::string> create(
      const std::string& path, const VideoEncodeSettings& settings);

  MediaWriter(const MediaWriter&) = delete;
  MediaWriter& operator=(const MediaWriter&) = delete;
  ~MediaWriter();

  /// Appends one frame. `rgba` is row-major 8-bit sRGB, `width * height * 4`
  /// bytes, and frames are expected in presentation order.
  [[nodiscard]] std::expected<void, std::string> write_frame(
      std::span<const std::uint8_t> rgba);

  /// Flushes the encoder and finalises the container. Calling this is what
  /// makes the file playable; a writer destroyed without it leaves a truncated
  /// file, which is why the error is worth checking rather than left to the
  /// destructor.
  [[nodiscard]] std::expected<void, std::string> finish();

  /// The encoder actually in use, for logging — "h264_nvenc", "libx264", ...
  [[nodiscard]] const std::string& encoder_name() const noexcept;

  /// How many frames have been accepted so far.
  [[nodiscard]] std::int64_t frame_count() const noexcept;

 private:
  struct Impl;
  explicit MediaWriter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::media
