#include "cutline/media/transcode.hpp"

#include "av_common.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <vector>

namespace cutline::media {
namespace {

using namespace detail;

/// The most output frames one source frame may be repeated into.
///
/// Repetition is how a gap in a variable-rate source keeps its timing, and a
/// real gap can be seconds long — a locked-off camera writing nothing while
/// nothing moves. But a file with broken timestamps can claim a gap of hours,
/// and the loop filling it would look exactly like a hang. Ten seconds is longer
/// than any honest gap and short enough that a dishonest one ends.
constexpr int kMaxRepeats = 600;

/// A frame rate to write at, from what the container claims.
///
/// `avg_frame_rate` is what the file worked out to, `r_frame_rate` is the base
/// rate its timestamps are expressed in, and a variable-rate file has an honest
/// average and a nonsense base. Preferring the average is what makes the proxy
/// the same length as the original.
[[nodiscard]] double frame_rate_of(const AVStream* stream) noexcept {
  const double average = av_q2d(stream->avg_frame_rate);
  if (average > 0.0 && average < 1000.0) return average;
  const double base = av_q2d(stream->r_frame_rate);
  if (base > 0.0 && base < 1000.0) return base;
  return 30.0;
}

struct Encode {
  FormatContext format;
  CodecContext decoder;
  Frame frame;
  Packet packet;
  Scaler scaler;
  AVPixelFormat scaler_source = AV_PIX_FMT_NONE;
  int stream_index = -1;
  AVRational time_base{1, 1};
  int width = 0;
  int height = 0;

  /// The most recently decoded frame, already scaled, waiting to find out how
  /// long it lasts. A frame's duration is only known once the next one arrives,
  /// so writing is always one frame behind decoding.
  std::vector<std::uint8_t> held;
  bool holding = false;
};

[[nodiscard]] std::expected<void, std::string> scale_into_held(Encode& encode,
                                                               const AVFrame* frame) {
  const auto source_format = static_cast<AVPixelFormat>(frame->format);
  if (!encode.scaler || encode.scaler_source != source_format) {
    // A source can change format mid-stream, and a scaler built for the old one
    // would quietly produce garbage rather than fail.
    encode.scaler.reset(sws_getContext(frame->width, frame->height, source_format, encode.width,
                                       encode.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                       nullptr, nullptr));
    if (!encode.scaler) return std::unexpected("cannot create a scaler for this pixel format");
    encode.scaler_source = source_format;
  }

  std::uint8_t* destination[4] = {encode.held.data(), nullptr, nullptr, nullptr};
  int stride[4] = {encode.width * 4, 0, 0, 0};
  sws_scale(encode.scaler.get(), frame->data, frame->linesize, 0, frame->height, destination,
            stride);
  encode.holding = true;
  return {};
}

/// The time a decoded frame is presented at, in seconds from the start of the
/// stream.
[[nodiscard]] double frame_time(const Encode& encode, const AVFrame* frame) noexcept {
  const std::int64_t pts =
      frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
  if (pts == AV_NOPTS_VALUE) return 0.0;
  return static_cast<double>(pts) * av_q2d(encode.time_base);
}

}  // namespace

std::string default_proxy_path(std::string_view source) {
  std::filesystem::path path(source);
  std::filesystem::path folder = path.parent_path() / "Proxies";
  return (folder / path.filename()).replace_extension(".mp4").string();
}

std::expected<ProxyResult, std::string> write_proxy(std::string_view source,
                                                    std::string_view destination,
                                                    const ProxyOptions& options) {
  if (options.height <= 0) return std::unexpected("proxy height must be positive");

  const std::string source_path(source);
  const std::string destination_path(destination);
  Encode encode;

  AVFormatContext* raw = nullptr;
  quiet_av_logging();
  if (const int rc = avformat_open_input(&raw, source_path.c_str(), nullptr, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open {}: {}", source_path, av_error_string(rc)));
  }
  encode.format.reset(raw);

  if (const int rc = avformat_find_stream_info(encode.format.get(), nullptr); rc < 0) {
    return std::unexpected(
        std::format("cannot read streams in {}: {}", source_path, av_error_string(rc)));
  }

  const AVCodec* codec = nullptr;
  encode.stream_index =
      av_find_best_stream(encode.format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (encode.stream_index < 0 || codec == nullptr) {
    return std::unexpected(std::format("{} has no decodable video stream", source_path));
  }

  AVStream* stream = encode.format->streams[encode.stream_index];
  encode.time_base = stream->time_base;

  encode.decoder.reset(avcodec_alloc_context3(codec));
  if (!encode.decoder) return std::unexpected("out of memory allocating a decoder");
  if (const int rc = avcodec_parameters_to_context(encode.decoder.get(), stream->codecpar);
      rc < 0) {
    return std::unexpected(std::format("cannot configure decoder: {}", av_error_string(rc)));
  }
  // Software decoding, deliberately: every frame has to reach the CPU to be
  // scaled down, so decoding onto the GPU would only add a download per frame.
  encode.decoder->thread_count = std::max(0, options.threads);
  if (const int rc = avcodec_open2(encode.decoder.get(), codec, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open decoder: {}", av_error_string(rc)));
  }

  const int source_width = stream->codecpar->width;
  const int source_height = stream->codecpar->height;
  if (source_width <= 0 || source_height <= 0) {
    return std::unexpected("video stream has no usable dimensions");
  }

  // Never upscale. A proxy for a file already smaller than the proxy size would
  // cost more to decode than the original, which is the whole thing backwards.
  const int wanted_height = std::min(options.height, source_height);
  const int wanted_width = std::max(
      2, static_cast<int>(std::lround(static_cast<double>(wanted_height) * source_width /
                                      source_height)));
  // Even in both directions: 4:2:0 chroma is half resolution, and an odd side
  // has no whole chroma sample to describe its last row or column.
  encode.width = wanted_width - (wanted_width % 2);
  encode.height = std::max(2, wanted_height - (wanted_height % 2));

  encode.frame.reset(av_frame_alloc());
  encode.packet.reset(av_packet_alloc());
  if (!encode.frame || !encode.packet) {
    return std::unexpected("out of memory allocating frame buffers");
  }
  encode.held.resize(static_cast<std::size_t>(encode.width) * encode.height * 4);

  const double fps = frame_rate_of(stream);
  const double duration = encode.format->duration == AV_NOPTS_VALUE
                              ? 0.0
                              : static_cast<double>(encode.format->duration) / AV_TIME_BASE;

  std::error_code ignored;
  std::filesystem::create_directories(std::filesystem::path(destination_path).parent_path(),
                                      ignored);

  auto writer = MediaWriter::create(destination_path,
                                    {.width = encode.width,
                                     .height = encode.height,
                                     .fps = fps,
                                     .codec = options.codec,
                                     .preference = EncoderPreference::Auto,
                                     .bitrate = 0,
                                     .quality = options.quality});
  if (!writer) return std::unexpected(writer.error());

  /// Everything that leaves a partial file behind removes it. A proxy that
  /// exists is a proxy something will attach and cut against, and one that ends
  /// early is worse than one that was never made.
  const auto abandon = [&](std::string message) -> std::expected<ProxyResult, std::string> {
    writer->reset();
    std::error_code ec;
    std::filesystem::remove(destination_path, ec);
    return std::unexpected(std::move(message));
  };

  std::int64_t written = 0;
  double first_time = 0.0;
  bool have_first = false;
  bool cancelled = false;

  /// Writes the held frame until the output has reached `until` seconds. This is
  /// what holds the proxy to the original's timing: a source frame that lasted
  /// three output frames is written three times, and one that lasted none is
  /// written none.
  const auto catch_up_to = [&](double until) -> std::expected<void, std::string> {
    if (!encode.holding) return {};
    int repeats = 0;
    while (static_cast<double>(written) / fps < until - 1e-9 && repeats < kMaxRepeats) {
      if (auto ok = (*writer)->write_frame(encode.held); !ok) return std::unexpected(ok.error());
      ++written;
      ++repeats;
    }
    return {};
  };

  bool draining = false;
  while (!cancelled) {
    const int rc = avcodec_receive_frame(encode.decoder.get(), encode.frame.get());
    if (rc == 0) {
      const double time = frame_time(encode, encode.frame.get());
      if (!have_first) {
        // Against the first frame rather than against zero: a stream whose
        // timestamps start late would otherwise open with seconds of the first
        // frame repeated, and run that much long.
        first_time = time;
        have_first = true;
      }
      if (auto ok = catch_up_to(time - first_time); !ok) return abandon(ok.error());
      if (auto ok = scale_into_held(encode, encode.frame.get()); !ok) return abandon(ok.error());

      if (options.on_progress) {
        const double done =
            duration > 0.0 ? std::clamp((time - first_time) / duration, 0.0, 1.0) : 0.0;
        if (!options.on_progress(done)) cancelled = true;
      }
      continue;
    }
    if (rc == AVERROR_EOF) break;
    if (rc != AVERROR(EAGAIN)) {
      return abandon(std::format("decode failed: {}", av_error_string(rc)));
    }
    if (draining) break;

    const int read = av_read_frame(encode.format.get(), encode.packet.get());
    if (read == AVERROR_EOF) {
      avcodec_send_packet(encode.decoder.get(), nullptr);
      draining = true;
      continue;
    }
    if (read < 0) return abandon(std::format("read failed: {}", av_error_string(read)));

    if (encode.packet->stream_index == encode.stream_index) {
      const int sent = avcodec_send_packet(encode.decoder.get(), encode.packet.get());
      if (sent < 0) {
        av_packet_unref(encode.packet.get());
        return abandon(std::format("cannot feed decoder: {}", av_error_string(sent)));
      }
    }
    av_packet_unref(encode.packet.get());
  }

  if (cancelled) {
    writer->reset();
    std::error_code ec;
    std::filesystem::remove(destination_path, ec);
    return ProxyResult::Cancelled;
  }

  // The last frame lasts until the file does, and there is no frame after it to
  // say so. A source shorter than one output frame still gets that frame, or the
  // proxy is empty and reads as a file that will not decode.
  if (auto ok = catch_up_to(std::max(duration, 1.0 / fps)); !ok) return abandon(ok.error());
  if (written == 0 && encode.holding) {
    if (auto ok = (*writer)->write_frame(encode.held); !ok) return abandon(ok.error());
    ++written;
  }
  if (written == 0) return abandon(std::format("{} decoded no frames", source_path));

  if (auto ok = (*writer)->finish(); !ok) return abandon(ok.error());
  if (options.on_progress) options.on_progress(1.0);
  return ProxyResult::Written;
}

}  // namespace cutline::media
