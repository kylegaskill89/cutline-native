#include "cutline/media/thumbnail.hpp"

#include "av_common.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cutline::media {
namespace {

using namespace detail;

struct Extractor {
  FormatContext format;
  CodecContext decoder;
  Frame frame;
  Packet packet;
  Scaler scaler;
  int stream_index = -1;
  AVRational time_base{1, 1};
  int width = 0;
  int height = 0;
  AVPixelFormat scaler_source = AV_PIX_FMT_NONE;
};

/// Decodes forward until a frame at or after `target` appears, then returns it.
/// Returns null at end of stream.
[[nodiscard]] std::expected<const AVFrame*, std::string> decode_until(Extractor& ex,
                                                                     double target) {
  while (true) {
    const int rc = avcodec_receive_frame(ex.decoder.get(), ex.frame.get());
    if (rc == 0) {
      const int64_t pts = ex.frame->best_effort_timestamp != AV_NOPTS_VALUE
                              ? ex.frame->best_effort_timestamp
                              : ex.frame->pts;
      const double seconds =
          pts == AV_NOPTS_VALUE ? 0.0 : static_cast<double>(pts) * av_q2d(ex.time_base);
      if (seconds + 1e-6 >= target) return ex.frame.get();
      continue;  // still before the wanted time
    }
    if (rc == AVERROR_EOF) return nullptr;
    if (rc != AVERROR(EAGAIN)) {
      return std::unexpected(std::format("decode failed: {}", av_error_string(rc)));
    }

    const int read = av_read_frame(ex.format.get(), ex.packet.get());
    if (read == AVERROR_EOF) {
      avcodec_send_packet(ex.decoder.get(), nullptr);
      // Drain whatever the decoder still holds; if none of it reaches the
      // target, the next receive reports EOF and we give up.
      continue;
    }
    if (read < 0) {
      return std::unexpected(std::format("read failed: {}", av_error_string(read)));
    }
    if (ex.packet->stream_index == ex.stream_index) {
      const int sent = avcodec_send_packet(ex.decoder.get(), ex.packet.get());
      if (sent < 0) {
        av_packet_unref(ex.packet.get());
        return std::unexpected(std::format("cannot feed decoder: {}", av_error_string(sent)));
      }
    }
    av_packet_unref(ex.packet.get());
  }
}

[[nodiscard]] std::expected<Thumbnail, std::string> scale_to_rgba(Extractor& ex,
                                                                 const AVFrame* frame,
                                                                 double timestamp) {
  const auto source_format = static_cast<AVPixelFormat>(frame->format);

  // A source can change format mid-stream, so the scaler is rebuilt when it
  // does rather than quietly producing garbage.
  if (!ex.scaler || ex.scaler_source != source_format) {
    ex.scaler.reset(sws_getContext(frame->width, frame->height, source_format, ex.width,
                                   ex.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
                                   nullptr));
    if (!ex.scaler) return std::unexpected("cannot create a scaler for this pixel format");
    ex.scaler_source = source_format;
  }

  Thumbnail thumbnail;
  thumbnail.width = ex.width;
  thumbnail.height = ex.height;
  thumbnail.timestamp = timestamp;
  thumbnail.rgba.resize(static_cast<std::size_t>(ex.width) * ex.height * 4);

  std::uint8_t* destination[4] = {thumbnail.rgba.data(), nullptr, nullptr, nullptr};
  int stride[4] = {ex.width * 4, 0, 0, 0};
  sws_scale(ex.scaler.get(), frame->data, frame->linesize, 0, frame->height, destination, stride);

  return thumbnail;
}

}  // namespace

std::expected<std::vector<Thumbnail>, std::string> extract_thumbnails(std::string_view path,
                                                                     int count,
                                                                     ThumbnailOptions options) {
  if (count <= 0) return std::vector<Thumbnail>{};
  if (options.height <= 0) return std::unexpected("thumbnail height must be positive");

  const std::string path_string(path);
  Extractor ex;

  AVFormatContext* raw = nullptr;
  detail::quiet_av_logging();
  if (const int rc = avformat_open_input(&raw, path_string.c_str(), nullptr, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open {}: {}", path_string, av_error_string(rc)));
  }
  ex.format.reset(raw);

  if (const int rc = avformat_find_stream_info(ex.format.get(), nullptr); rc < 0) {
    return std::unexpected(
        std::format("cannot read streams in {}: {}", path_string, av_error_string(rc)));
  }

  const AVCodec* codec = nullptr;
  ex.stream_index = av_find_best_stream(ex.format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (ex.stream_index < 0 || codec == nullptr) {
    return std::unexpected(std::format("{} has no decodable video stream", path_string));
  }

  AVStream* stream = ex.format->streams[ex.stream_index];
  ex.time_base = stream->time_base;

  ex.decoder.reset(avcodec_alloc_context3(codec));
  if (!ex.decoder) return std::unexpected("out of memory allocating a decoder");
  if (const int rc = avcodec_parameters_to_context(ex.decoder.get(), stream->codecpar); rc < 0) {
    return std::unexpected(std::format("cannot configure decoder: {}", av_error_string(rc)));
  }
  // Software decoding, deliberately: these frames have to reach the CPU to be
  // scaled, so putting them on the GPU first would only add a download.
  ex.decoder->thread_count = 0;
  if (const int rc = avcodec_open2(ex.decoder.get(), codec, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open decoder: {}", av_error_string(rc)));
  }

  const int source_width = stream->codecpar->width;
  const int source_height = stream->codecpar->height;
  if (source_width <= 0 || source_height <= 0) {
    return std::unexpected("video stream has no usable dimensions");
  }
  ex.height = options.height;
  ex.width = std::max(1, static_cast<int>(std::lround(static_cast<double>(options.height) *
                                                      source_width / source_height)));

  ex.frame.reset(av_frame_alloc());
  ex.packet.reset(av_packet_alloc());
  if (!ex.frame || !ex.packet) return std::unexpected("out of memory allocating frame buffers");

  const double duration = ex.format->duration == AV_NOPTS_VALUE
                              ? 0.0
                              : static_cast<double>(ex.format->duration) / AV_TIME_BASE;
  const double start = std::max(0.0, options.start);
  const double end = options.end > start ? options.end : std::max(start, duration);
  const double span = std::max(0.0, end - start);

  std::vector<Thumbnail> thumbnails;
  thumbnails.reserve(static_cast<std::size_t>(count));

  for (int i = 0; i < count; ++i) {
    // Sampled at bucket centres, so a filmstrip represents the whole span
    // rather than clustering at its start.
    const double fraction = count == 1 ? 0.5 : (i + 0.5) / count;
    const double target = start + span * fraction;

    const auto seek_target = static_cast<int64_t>(target / av_q2d(ex.time_base));
    if (av_seek_frame(ex.format.get(), ex.stream_index, seek_target, AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(ex.decoder.get());
    }

    const auto frame = decode_until(ex, target);
    if (!frame) return std::unexpected(frame.error());
    if (*frame == nullptr) break;  // ran off the end of the stream

    auto thumbnail = scale_to_rgba(ex, *frame, target);
    if (!thumbnail) return std::unexpected(thumbnail.error());
    thumbnails.push_back(std::move(*thumbnail));
  }

  return thumbnails;
}

}  // namespace cutline::media
