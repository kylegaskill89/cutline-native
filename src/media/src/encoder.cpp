#include "cutline/media/encoder.hpp"

#include "av_common.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace cutline::media {
namespace {

using detail::av_error_string;

/// Encoders to try, best first. Hardware first because it is an order of
/// magnitude faster; the software encoders are last because they always exist,
/// which is what makes the fallback a guarantee rather than a hope.
[[nodiscard]] std::vector<const char*> candidates(VideoCodec codec,
                                                  EncoderPreference preference) {
  std::vector<const char*> hardware;
  std::vector<const char*> software;

  if (codec == VideoCodec::Hevc) {
    hardware = {"hevc_nvenc", "hevc_qsv", "hevc_amf"};
    software = {"libx265"};
  } else {
    hardware = {"h264_nvenc", "h264_qsv", "h264_amf"};
    software = {"libx264"};
  }

  switch (preference) {
    case EncoderPreference::Hardware:
      return hardware;
    case EncoderPreference::Software:
      return software;
    default:
      break;
  }

  hardware.insert(hardware.end(), software.begin(), software.end());
  return hardware;
}

[[nodiscard]] AVCodecID codec_id(VideoCodec codec) noexcept {
  return codec == VideoCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

/// True when the encoder names a pixel format it can take; NV12 is preferred by
/// the hardware encoders and YUV420P by the software ones, so rather than
/// guessing, the encoder's own list decides.
[[nodiscard]] AVPixelFormat preferred_format(const AVCodec* encoder) {
  const AVPixelFormat* formats = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   reinterpret_cast<const void**>(&formats), &count) < 0 ||
      formats == nullptr || count <= 0) {
    return AV_PIX_FMT_YUV420P;
  }

  // Both are 8-bit 4:2:0, so either is a faithful target for SDR output.
  for (int i = 0; i < count; ++i) {
    if (formats[i] == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
  }
  for (int i = 0; i < count; ++i) {
    if (formats[i] == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
  }
  return formats[0];
}

/// The frame rate as an exact rational where possible. 29.97 and 59.94 are
/// 30000/1001 and 60000/1001, and writing them as decimals accumulates drift
/// over a long timeline.
[[nodiscard]] AVRational rational_fps(double fps) {
  if (fps <= 0.0) return {30, 1};

  constexpr std::array<std::pair<double, AVRational>, 4> ntsc{{
      {24000.0 / 1001.0, {24000, 1001}},
      {30000.0 / 1001.0, {30000, 1001}},
      {60000.0 / 1001.0, {60000, 1001}},
      {120000.0 / 1001.0, {120000, 1001}},
  }};
  for (const auto& [value, rate] : ntsc) {
    if (std::abs(fps - value) < 1e-4) return rate;
  }

  AVRational rate{};
  av_reduce(&rate.num, &rate.den, static_cast<std::int64_t>(std::llround(fps * 1000.0)), 1000,
            INT_MAX);
  return rate;
}

}  // namespace

std::string_view to_string(VideoCodec codec) noexcept {
  return codec == VideoCodec::Hevc ? "hevc" : "h264";
}

struct MediaWriter::Impl {
  detail::OutputFormatContext format;
  detail::CodecContext encoder;
  detail::Frame frame;
  detail::Packet packet;
  detail::Scaler scaler;

  AVStream* stream = nullptr;
  std::string name;
  std::int64_t frames = 0;
  bool finished = false;

  int width = 0;
  int height = 0;

  /// Drains whatever the encoder has ready. Passing a null frame flushes.
  [[nodiscard]] std::expected<void, std::string> drain(AVFrame* input);
};

std::expected<void, std::string> MediaWriter::Impl::drain(AVFrame* input) {
  if (int rc = avcodec_send_frame(encoder.get(), input); rc < 0) {
    return std::unexpected(std::format("cannot send a frame: {}", av_error_string(rc)));
  }

  while (true) {
    const int rc = avcodec_receive_packet(encoder.get(), packet.get());
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
    if (rc < 0) {
      return std::unexpected(std::format("cannot read a packet: {}", av_error_string(rc)));
    }

    // One frame long, in the encoder's time base — which is 1/fps, so one tick
    // is exactly one frame.
    //
    // Encoders generally leave this unset, and a container then has to infer
    // each sample's duration from the *next* sample's timestamp. That leaves
    // the final sample with no duration at all, so every file came out one
    // frame short: an eight-second export reported 7.98s and decoded 479 of its
    // 480 frames, and a single-frame file had no duration and no frame rate.
    packet->duration = 1;

    // The encoder counts in its own time base; the muxer counts in the
    // stream's. Rescaling here is what keeps timestamps right when they differ,
    // and it carries the duration across with them.
    av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
    packet->stream_index = stream->index;

    const int written = av_interleaved_write_frame(format.get(), packet.get());
    av_packet_unref(packet.get());
    if (written < 0) {
      return std::unexpected(std::format("cannot write a packet: {}", av_error_string(written)));
    }
  }
}

MediaWriter::MediaWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

// The output context's deleter closes the I/O, so an export abandoned without
// `finish` leaves a file that is obviously truncated rather than one that looks
// complete.
MediaWriter::~MediaWriter() = default;

const std::string& MediaWriter::encoder_name() const noexcept { return impl_->name; }
std::int64_t MediaWriter::frame_count() const noexcept { return impl_->frames; }

std::expected<std::unique_ptr<MediaWriter>, std::string> MediaWriter::create(
    const std::string& path, const VideoEncodeSettings& settings) {
  if (settings.width <= 0 || settings.height <= 0) {
    return std::unexpected("the output size must be positive");
  }

  auto impl = std::make_unique<Impl>();
  // Encoders overwhelmingly want even dimensions for 4:2:0 chroma, and an odd
  // one fails deep inside the encoder with an unhelpful message.
  impl->width = settings.width & ~1;
  impl->height = settings.height & ~1;

  AVFormatContext* format = nullptr;
  if (int rc = avformat_alloc_output_context2(&format, nullptr, nullptr, path.c_str());
      rc < 0 || format == nullptr) {
    return std::unexpected(
        std::format("cannot infer a container for {}: {}", path, av_error_string(rc)));
  }
  impl->format.reset(format);

  const AVCodec* chosen = nullptr;
  std::string attempts;
  for (const char* name : candidates(settings.codec, settings.preference)) {
    const AVCodec* encoder = avcodec_find_encoder_by_name(name);
    if (encoder == nullptr) {
      if (!attempts.empty()) attempts += ", ";
      attempts += std::format("{} (not built in)", name);
      continue;
    }

    detail::CodecContext context(avcodec_alloc_context3(encoder));
    if (!context) continue;

    context->width = impl->width;
    context->height = impl->height;
    context->time_base = av_inv_q(rational_fps(settings.fps));
    context->framerate = rational_fps(settings.fps);
    context->pix_fmt = preferred_format(encoder);
    // SDR throughout, matching what the compositor reads back.
    context->colorspace = AVCOL_SPC_BT709;
    context->color_primaries = AVCOL_PRI_BT709;
    context->color_trc = AVCOL_TRC_BT709;
    context->color_range = AVCOL_RANGE_MPEG;

    if ((impl->format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
      context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* options = nullptr;
    if (settings.bitrate > 0) {
      context->bit_rate = settings.bitrate;
    } else {
      // Quality-targeted. The option name differs by encoder family, and
      // setting one the encoder does not know is harmless.
      const std::string quality = std::to_string(settings.quality);
      av_dict_set(&options, "crf", quality.c_str(), 0);
      av_dict_set(&options, "cq", quality.c_str(), 0);
      av_dict_set(&options, "global_quality", quality.c_str(), 0);
      av_dict_set(&options, "qp", quality.c_str(), 0);
    }

    const int rc = avcodec_open2(context.get(), encoder, &options);
    av_dict_free(&options);
    if (rc < 0) {
      if (!attempts.empty()) attempts += ", ";
      attempts += std::format("{} ({})", name, av_error_string(rc));
      continue;
    }

    impl->encoder = std::move(context);
    impl->name = name;
    chosen = encoder;
    break;
  }

  if (chosen == nullptr) {
    return std::unexpected(std::format("no usable {} encoder: {}",
                                       to_string(settings.codec), attempts));
  }

  impl->stream = avformat_new_stream(impl->format.get(), nullptr);
  if (impl->stream == nullptr) return std::unexpected("cannot create an output stream");
  impl->stream->time_base = impl->encoder->time_base;

  if (int rc = avcodec_parameters_from_context(impl->stream->codecpar, impl->encoder.get());
      rc < 0) {
    return std::unexpected(
        std::format("cannot describe the stream: {}", av_error_string(rc)));
  }

  if ((impl->format->oformat->flags & AVFMT_NOFILE) == 0) {
    if (int rc = avio_open(&impl->format->pb, path.c_str(), AVIO_FLAG_WRITE); rc < 0) {
      return std::unexpected(std::format("cannot open {}: {}", path, av_error_string(rc)));
    }
  }

  if (int rc = avformat_write_header(impl->format.get(), nullptr); rc < 0) {
    return std::unexpected(std::format("cannot write the header: {}", av_error_string(rc)));
  }

  impl->frame.reset(av_frame_alloc());
  impl->packet.reset(av_packet_alloc());
  if (!impl->frame || !impl->packet) return std::unexpected("out of memory");

  impl->frame->format = impl->encoder->pix_fmt;
  impl->frame->width = impl->width;
  impl->frame->height = impl->height;
  impl->frame->colorspace = impl->encoder->colorspace;
  impl->frame->color_range = impl->encoder->color_range;
  if (int rc = av_frame_get_buffer(impl->frame.get(), 0); rc < 0) {
    return std::unexpected(std::format("cannot allocate a frame: {}", av_error_string(rc)));
  }

  // RGBA in, the encoder's format out. BT.709 to match how the frames were
  // composited and how the stream is tagged.
  impl->scaler.reset(sws_getContext(impl->width, impl->height, AV_PIX_FMT_RGBA, impl->width,
                                    impl->height, impl->encoder->pix_fmt, SWS_BILINEAR, nullptr,
                                    nullptr, nullptr));
  if (!impl->scaler) return std::unexpected("cannot create a colour converter");

  const int* table = sws_getCoefficients(SWS_CS_ITU709);
  int* inverse = nullptr;
  int source_range = 0;
  int destination_range = 0;
  int brightness = 0;
  int contrast = 0;
  int saturation = 0;
  if (sws_getColorspaceDetails(impl->scaler.get(), &inverse, &source_range,
                               const_cast<int**>(&table), &destination_range, &brightness,
                               &contrast, &saturation) >= 0) {
    // Input is full-range RGB; output is studio-range YUV, which is what the
    // stream is tagged as and what players expect.
    sws_setColorspaceDetails(impl->scaler.get(), sws_getCoefficients(SWS_CS_ITU709), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, brightness, contrast,
                             saturation);
  }

  return std::unique_ptr<MediaWriter>(new MediaWriter(std::move(impl)));
}

std::expected<void, std::string> MediaWriter::write_frame(std::span<const std::uint8_t> rgba) {
  Impl& d = *impl_;
  if (d.finished) return std::unexpected("the writer has already been finished");

  const std::size_t expected = static_cast<std::size_t>(d.width) * d.height * 4;
  if (rgba.size() < expected) {
    return std::unexpected(std::format("frame is {} bytes, expected {}", rgba.size(), expected));
  }

  if (int rc = av_frame_make_writable(d.frame.get()); rc < 0) {
    return std::unexpected(std::format("cannot write to the frame: {}", av_error_string(rc)));
  }

  const std::uint8_t* source[4] = {rgba.data(), nullptr, nullptr, nullptr};
  const int stride[4] = {d.width * 4, 0, 0, 0};
  sws_scale(d.scaler.get(), source, stride, 0, d.height, d.frame->data, d.frame->linesize);

  // Presentation timestamps count frames, because the time base is the inverse
  // of the frame rate. That keeps them exact rather than accumulating a
  // rounding error per frame.
  d.frame->pts = d.frames;
  ++d.frames;

  return d.drain(d.frame.get());
}

std::expected<void, std::string> MediaWriter::finish() {
  Impl& d = *impl_;
  if (d.finished) return {};
  d.finished = true;

  if (auto ok = d.drain(nullptr); !ok) return ok;

  if (int rc = av_write_trailer(d.format.get()); rc < 0) {
    return std::unexpected(std::format("cannot finalise the file: {}", av_error_string(rc)));
  }
  if ((d.format->oformat->flags & AVFMT_NOFILE) == 0 && d.format->pb != nullptr) {
    avio_closep(&d.format->pb);
  }
  return {};
}

}  // namespace cutline::media
