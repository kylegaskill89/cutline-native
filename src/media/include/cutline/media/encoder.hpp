#pragma once

/// Writing encoded video to a file.
///
/// The encoder is chosen at runtime rather than fixed at build time, because
/// which one exists depends on the machine: NVENC on NVIDIA, QSV on Intel, AMF
/// on AMD, and x264/x265 everywhere. Hardware is tried first and software is
/// the fallback, so an export always completes — slower on a machine without a
/// supported GPU, rather than failing on one.
///
/// Frames arrive either as 8-bit sRGB RGBA, which is what the compositor reads
/// back for a still, or already converted on the card to 4:2:0 in two planes,
/// which is what an export sends. The second is the fast way: converting RGBA
/// on the processor cost fourteen milliseconds a 4K frame.

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
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
  /// Apple ProRes 422 HQ: an intermediate, every frame a keyframe, 4:2:2 at
  /// ten bits, for handing a cut on to be graded or finished. In a .mov.
  ProRes,
  /// Avid DNxHR HQ: the same kind of intermediate, at eight bits. In a .mov.
  DnxHr,
};

/// Whether a codec is an intermediate rather than a delivery format: no
/// quality to choose, and a .mov to put it in.
[[nodiscard]] constexpr bool is_intermediate(VideoCodec codec) noexcept {
  return codec == VideoCodec::ProRes || codec == VideoCodec::DnxHr;
}

[[nodiscard]] std::string_view to_string(VideoCodec codec) noexcept;

/// How the picture in the file is encoded: which primaries, which transfer
/// curve, and so which YCbCr matrix. Written into the stream's tags, which is
/// what tells a player how to show it.
enum class VideoColour {
  Bt709,      ///< SDR, as every export was
  Bt2100Pq,   ///< HDR10: Rec.2020 primaries, the ST 2084 curve
  Bt2100Hlg,  ///< Rec.2020 primaries, hybrid log-gamma
};

/// What an HDR10 file says about the display it was mastered on and the
/// brightest it gets, so a display that cannot reach that knows how to map it
/// down. In nits; the primaries are the mastering display's, as CIE xy.
struct HdrMetadata {
  /// Display P3 at D65, which is what nearly every HDR grade is mastered on.
  double red_x = 0.680, red_y = 0.320;
  double green_x = 0.265, green_y = 0.690;
  double blue_x = 0.150, blue_y = 0.060;
  double white_x = 0.3127, white_y = 0.3290;
  double max_luminance = 1000.0;
  double min_luminance = 0.0001;
  /// The brightest pixel anywhere, and the brightest frame on average.
  int max_content_light = 1000;
  int max_frame_average_light = 400;
};

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
  /// 8 or 10. Ten is HEVC only: the hardware encoders do not offer 10-bit
  /// H.264, and Premiere does not either. The x265 in this build is 8-bit, so
  /// ten bits needs a hardware encoder that has them -- NVENC, QSV or AMF --
  /// and a machine without one is told so rather than given eight.
  int bit_depth = 8;
  VideoColour colour = VideoColour::Bt709;
  /// Mastering display and light levels, for a PQ file. Left out of anything
  /// else, since nothing else carries them.
  std::optional<HdrMetadata> hdr;
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

  /// How many audio streams the file carries, all in the same format.
  ///
  /// More than one is how a timeline's tracks are kept apart in the output —
  /// the thing anybody sending a cut on for a mix needs, since a stereo mixdown
  /// cannot be unpicked back into the tracks it came from. Every stream is
  /// written through the same writer because interleaving is a property of the
  /// file rather than of any one stream.
  int streams = 1;
};

/// A frame already converted for an encoder: in the writer's colour encoding at
/// studio range, 4:2:0 in two planes -- luma, then the two colour differences
/// interleaved at half size each way. NV12, or P010 with ten bits at the top of
/// sixteen, which has to match the writer's bit depth. Borrowed for the length
/// of the call that takes it.
struct PlanarFrame {
  bool ten_bit = false;
  const std::uint8_t* luma = nullptr;
  std::size_t luma_stride = 0;
  const std::uint8_t* chroma = nullptr;
  std::size_t chroma_stride = 0;
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
  /// bytes, and frames are expected in presentation order. BT.709 writers
  /// only: eight bits of sRGB is not a picture an HDR file can be made from.
  [[nodiscard]] std::expected<void, std::string> write_frame(
      std::span<const std::uint8_t> rgba);

  /// Appends one frame already in the encoder's shape. Taken as it is when the
  /// encoder accepts NV12, which every hardware encoder and x264 do; put into
  /// planar form for one that does not, which is a shuffle of planes rather
  /// than a colour conversion.
  [[nodiscard]] std::expected<void, std::string> write_frame(const PlanarFrame& frame);

  /// Appends audio. `interleaved` is float samples in [-1, 1], a whole number
  /// of frames of `channels` each.
  ///
  /// Block size is free: samples are buffered until the encoder's own frame
  /// size is reached, because AAC encodes fixed 1024-sample blocks and a caller
  /// should not have to know that. Writing audio to a writer created without it
  /// is an error rather than a silent no-op.
  ///
  /// `stream` picks which of them, for a file carrying more than one. They are
  /// independent: a caller may write a block to one and nothing to another, and
  /// each is padded to its own length only by what it is given.
  [[nodiscard]] std::expected<void, std::string> write_audio(std::span<const float> interleaved,
                                                             int stream = 0);

  /// Whether an audio stream was created.
  [[nodiscard]] bool has_audio() const noexcept;

  /// How many audio streams the file carries.
  [[nodiscard]] int audio_stream_count() const noexcept;

  /// The audio encoder in use, or empty when there is no audio stream.
  [[nodiscard]] const std::string& audio_encoder_name() const noexcept;

  /// How many audio frames — samples per channel — one stream has accepted.
  [[nodiscard]] std::int64_t audio_frame_count(int stream = 0) const noexcept;

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
