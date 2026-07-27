#include "cutline/media/probe.hpp"

#include <format>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

namespace cutline::media {
namespace {

/// Closes a format context on any exit path.
struct FormatContextDeleter {
  void operator()(AVFormatContext* ctx) const noexcept {
    if (ctx != nullptr) avformat_close_input(&ctx);
  }
};
using FormatContext = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

[[nodiscard]] std::string av_error_string(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

[[nodiscard]] ColorPrimaries map_primaries(AVColorPrimaries primaries) noexcept {
  switch (primaries) {
    case AVCOL_PRI_BT709:
      return ColorPrimaries::Bt709;
    case AVCOL_PRI_BT2020:
      return ColorPrimaries::Bt2020;
    case AVCOL_PRI_SMPTE170M:
      return ColorPrimaries::Smpte170m;
    case AVCOL_PRI_UNSPECIFIED:
      return ColorPrimaries::Unknown;
    default:
      return ColorPrimaries::Other;
  }
}

[[nodiscard]] TransferCharacteristic map_transfer(AVColorTransferCharacteristic transfer) noexcept {
  switch (transfer) {
    case AVCOL_TRC_BT709:
      return TransferCharacteristic::Bt709;
    case AVCOL_TRC_SMPTE2084:
      return TransferCharacteristic::Smpte2084;
    case AVCOL_TRC_ARIB_STD_B67:
      return TransferCharacteristic::AribStdB67;
    case AVCOL_TRC_UNSPECIFIED:
      return TransferCharacteristic::Unknown;
    default:
      return TransferCharacteristic::Other;
  }
}

[[nodiscard]] double rational_to_double(AVRational r) noexcept {
  return r.den == 0 ? 0.0 : static_cast<double>(r.num) / static_cast<double>(r.den);
}

[[nodiscard]] std::string codec_name(AVCodecID id) {
  const char* name = avcodec_get_name(id);
  return name == nullptr ? std::string{} : std::string(name);
}

[[nodiscard]] int bits_per_component(AVPixelFormat format) noexcept {
  const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
  return descriptor == nullptr ? 8 : descriptor->comp[0].depth;
}

}  // namespace

std::string_view to_string(ColorPrimaries primaries) noexcept {
  switch (primaries) {
    case ColorPrimaries::Bt709:
      return "bt709";
    case ColorPrimaries::Bt2020:
      return "bt2020";
    case ColorPrimaries::Smpte170m:
      return "smpte170m";
    case ColorPrimaries::Other:
      return "other";
    case ColorPrimaries::Unknown:
      break;
  }
  return "unknown";
}

std::string_view to_string(TransferCharacteristic transfer) noexcept {
  switch (transfer) {
    case TransferCharacteristic::Bt709:
      return "bt709";
    case TransferCharacteristic::Smpte2084:
      return "smpte2084";
    case TransferCharacteristic::AribStdB67:
      return "arib-std-b67";
    case TransferCharacteristic::Other:
      return "other";
    case TransferCharacteristic::Unknown:
      break;
  }
  return "unknown";
}

std::expected<MediaInfo, std::string> probe(std::string_view path) {
  const std::string path_string(path);

  AVFormatContext* raw = nullptr;
  if (const int rc = avformat_open_input(&raw, path_string.c_str(), nullptr, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open {}: {}", path_string, av_error_string(rc)));
  }
  FormatContext context(raw);

  if (const int rc = avformat_find_stream_info(context.get(), nullptr); rc < 0) {
    return std::unexpected(
        std::format("cannot read streams in {}: {}", path_string, av_error_string(rc)));
  }

  MediaInfo info;
  info.path = path_string;
  info.format = context->iformat != nullptr && context->iformat->name != nullptr
                    ? context->iformat->name
                    : "";
  info.duration = context->duration == AV_NOPTS_VALUE
                      ? 0.0
                      : static_cast<double>(context->duration) / AV_TIME_BASE;
  info.bit_rate = context->bit_rate;

  for (unsigned i = 0; i < context->nb_streams; ++i) {
    const AVStream* stream = context->streams[i];
    const AVCodecParameters* codec = stream->codecpar;

    if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      const auto pixel_format = static_cast<AVPixelFormat>(codec->format);
      const char* pixel_name = av_get_pix_fmt_name(pixel_format);

      VideoStreamInfo video;
      video.index = static_cast<int>(i);
      video.width = codec->width;
      video.height = codec->height;
      // avg_frame_rate is what a variable-rate source can honestly report;
      // r_frame_rate is a guessed upper bound and overstates such files.
      video.fps = rational_to_double(stream->avg_frame_rate);
      video.codec = codec_name(codec->codec_id);
      video.pixel_format = pixel_name == nullptr ? "" : pixel_name;
      video.color = ColorInfo{
          .primaries = map_primaries(codec->color_primaries),
          .transfer = map_transfer(codec->color_trc),
          .full_range = codec->color_range == AVCOL_RANGE_JPEG,
          .bits_per_component = bits_per_component(pixel_format),
      };
      info.video.push_back(std::move(video));
    } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
      info.audio.push_back(AudioStreamInfo{
          .index = static_cast<int>(i),
          .sample_rate = codec->sample_rate,
          .channels = codec->ch_layout.nb_channels,
          .codec = codec_name(codec->codec_id),
      });
    }
  }

  return info;
}

}  // namespace cutline::media
