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

/// Audio output. Disabled by default, so a caller that has nothing to write
/// gets a video-only file rather than a silent track.
struct AudioEncodeSettings {
  bool enabled = false;
  int sample_rate = 48000;
  int channels = 2;
  /// Bits per second. AAC at 192 kbps stereo is transparent enough that the
  /// encoder is not what anyone will hear.
  std::int64_t bitrate = 192000;
};

/// Encodes frames and muxes them into a container.
///
/// Audio and video share one writer rather than being written separately,
/// because interleaving is a property of the file: a player reads it in storage
/// order, and a container whose streams are written in two passes makes it seek
/// badly. Keeping both here means the interleaving is one class's problem.
class MediaWriter {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<MediaWriter>, std::string> create(
      const std::string& path, const VideoEncodeSettings& settings,
      const AudioEncodeSettings& audio = {});

  MediaWriter(const MediaWriter&) = delete;
  MediaWriter& operator=(const MediaWriter&) = delete;
  ~MediaWriter();

  /// Appends one frame. `rgba` is row-major 8-bit sRGB, `width * height * 4`
  /// bytes, and frames are expected in presentation order.
  [[nodiscard]] std::expected<void, std::string> write_frame(
      std::span<const std::uint8_t> rgba);

  /// Appends audio. `interleaved` is float samples in [-1, 1], a whole number
  /// of frames of `channels` each.
  ///
  /// Block size is free: samples are buffered until the encoder's own frame
  /// size is reached, because AAC encodes fixed 1024-sample blocks and a caller
  /// should not have to know that. Writing audio to a writer created without it
  /// is an error rather than a silent no-op.
  [[nodiscard]] std::expected<void, std::string> write_audio(std::span<const float> interleaved);

  /// Whether an audio stream was created.
  [[nodiscard]] bool has_audio() const noexcept;

  /// The audio encoder in use, or empty when there is no audio stream.
  [[nodiscard]] const std::string& audio_encoder_name() const noexcept;

  /// How many audio frames — samples per channel — have been accepted.
  [[nodiscard]] std::int64_t audio_frame_count() const noexcept;

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
