#pragma once

/// Inspecting a media file: what streams it has, how long it is, and — the part
/// the reference never reported — what colour it is in.
///
/// The TypeScript version shelled out to ffprobe and parsed its output. This
/// reads the same information straight from libavformat, in-process.

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::media {

/// How a stream's code values map to light. Named after the ITU/SMPTE
/// identifiers so a file's tagging survives round-tripping.
enum class TransferCharacteristic {
  Unknown,
  Bt709,      ///< ordinary SDR
  Smpte2084,  ///< PQ — HDR10
  AribStdB67, ///< HLG
  Other,
};

enum class ColorPrimaries {
  Unknown,
  Bt709,   ///< SDR / sRGB gamut
  Bt2020,  ///< wide gamut, usual companion to PQ and HLG
  Smpte170m,
  Other,
};

/// Everything needed to decide how to bring a stream into linear light.
struct ColorInfo {
  ColorPrimaries primaries = ColorPrimaries::Unknown;
  TransferCharacteristic transfer = TransferCharacteristic::Unknown;
  bool full_range = false;
  int bits_per_component = 8;

  /// Whether this stream carries high dynamic range, which is true exactly when
  /// its transfer function is PQ or HLG. Bit depth alone does not decide it:
  /// 10-bit SDR exists, and so does mistagged footage.
  [[nodiscard]] bool is_hdr() const noexcept {
    return transfer == TransferCharacteristic::Smpte2084 ||
           transfer == TransferCharacteristic::AribStdB67;
  }
};

struct VideoStreamInfo {
  int index = 0;  ///< index within the file, as libav numbers streams
  int width = 0;
  int height = 0;
  /// Average frame rate. Variable-frame-rate sources report their average.
  double fps = 0.0;
  std::string codec;
  std::string pixel_format;
  ColorInfo color;
};

struct AudioStreamInfo {
  int index = 0;
  int sample_rate = 0;
  int channels = 0;
  std::string codec;
};

struct MediaInfo {
  std::string path;
  std::string format;
  double duration = 0.0;  ///< seconds
  long long bit_rate = 0;
  std::vector<VideoStreamInfo> video;
  std::vector<AudioStreamInfo> audio;

  [[nodiscard]] bool has_video() const noexcept { return !video.empty(); }
  [[nodiscard]] const VideoStreamInfo* primary_video() const noexcept {
    return video.empty() ? nullptr : &video.front();
  }
};

/// Reads a file's structure without decoding any frames.
[[nodiscard]] std::expected<MediaInfo, std::string> probe(std::string_view path);

/// Human-readable names, for logs and the media pool.
[[nodiscard]] std::string_view to_string(ColorPrimaries primaries) noexcept;
[[nodiscard]] std::string_view to_string(TransferCharacteristic transfer) noexcept;

}  // namespace cutline::media
