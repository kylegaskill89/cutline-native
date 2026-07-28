#include "cutline/media/encoder.hpp"

#include "av_common.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <vector>

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

/// The sample format the encoder wants, preferring planar float since that is
/// what the mixer produces and what AAC takes, so the conversion is a
/// deinterleave rather than a requantisation.
[[nodiscard]] AVSampleFormat preferred_sample_format(const AVCodec* encoder) {
  const AVSampleFormat* formats = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                   reinterpret_cast<const void**>(&formats), &count) < 0 ||
      formats == nullptr || count <= 0) {
    return AV_SAMPLE_FMT_FLTP;
  }

  for (int i = 0; i < count; ++i) {
    if (formats[i] == AV_SAMPLE_FMT_FLTP) return AV_SAMPLE_FMT_FLTP;
  }
  return formats[0];
}

}  // namespace

std::string_view to_string(VideoCodec codec) noexcept {
  return codec == VideoCodec::Hevc ? "hevc" : "h264";
}

struct MediaWriter::Impl {
  detail::OutputFormatContext format;
  detail::Packet packet;

  detail::CodecContext encoder;
  detail::Frame frame;
  detail::Scaler scaler;
  AVStream* stream = nullptr;
  std::string name;
  std::int64_t frames = 0;

  detail::CodecContext audio_encoder;
  detail::Frame audio_frame;
  detail::Resampler audio_resampler;
  AVStream* audio_stream = nullptr;
  std::string audio_name;
  int audio_channels = 0;
  int audio_block = 0;  ///< the encoder's own frame size, in samples per channel
  /// Interleaved float samples accepted but not yet encoded, because the
  /// encoder takes fixed-size blocks and callers should not have to know that.
  std::vector<float> audio_pending;
  std::int64_t audio_frames = 0;

  bool finished = false;

  int width = 0;
  int height = 0;

  /// Drains whatever an encoder has ready. Passing a null frame flushes it.
  ///
  /// `duration` is what to stamp on each packet, in the encoder's time base. A
  /// zero means the encoder's own value is trusted, falling back to its block
  /// size — which is what audio wants, since packets there are not all the same
  /// length once the final short one arrives.
  [[nodiscard]] std::expected<void, std::string> drain(AVCodecContext* codec, AVStream* target,
                                                       AVFrame* input, std::int64_t duration);

  /// Encodes exactly `samples` frames from the front of `audio_pending`.
  [[nodiscard]] std::expected<void, std::string> encode_audio_block(int samples);

  /// Creates the audio encoder and stream. Must run before the header is
  /// written, since that is where a container records its streams.
  [[nodiscard]] std::expected<void, std::string> open_audio(const AudioEncodeSettings& settings);
};

std::expected<void, std::string> MediaWriter::Impl::drain(AVCodecContext* codec,
                                                          AVStream* target, AVFrame* input,
                                                          std::int64_t duration) {
  if (int rc = avcodec_send_frame(codec, input); rc < 0) {
    return std::unexpected(std::format("cannot send a frame: {}", av_error_string(rc)));
  }

  while (true) {
    const int rc = avcodec_receive_packet(codec, packet.get());
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
    if (rc < 0) {
      return std::unexpected(std::format("cannot read a packet: {}", av_error_string(rc)));
    }

    // Video passes 1: one frame long, in a time base of 1/fps, so one tick is
    // exactly one frame.
    //
    // Encoders generally leave this unset, and a container then has to infer
    // each sample's duration from the *next* sample's timestamp. That leaves
    // the final sample with no duration at all, so every file came out one
    // frame short: an eight-second export reported 7.98s and decoded 479 of its
    // 480 frames, and a single-frame file had no duration and no frame rate.
    if (duration > 0) {
      packet->duration = duration;
    } else if (packet->duration <= 0) {
      packet->duration = codec->frame_size;
    }

    // The encoder counts in its own time base; the muxer counts in the
    // stream's. Rescaling here is what keeps timestamps right when they differ,
    // and it carries the duration across with them.
    av_packet_rescale_ts(packet.get(), codec->time_base, target->time_base);
    packet->stream_index = target->index;

    const int written = av_interleaved_write_frame(format.get(), packet.get());
    av_packet_unref(packet.get());
    if (written < 0) {
      return std::unexpected(std::format("cannot write a packet: {}", av_error_string(written)));
    }
  }
}

std::expected<void, std::string> MediaWriter::Impl::open_audio(
    const AudioEncodeSettings& settings) {
  if (settings.sample_rate <= 0 || settings.channels <= 0) {
    return std::unexpected("the audio format must have a positive rate and channel count");
  }

  // AAC rather than a runtime search: unlike video, there is no meaningful
  // hardware audio encoder, it is in every FFmpeg build, and every container
  // this writes takes it.
  const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (aac == nullptr) return std::unexpected("no AAC encoder is available");

  detail::CodecContext context(avcodec_alloc_context3(aac));
  if (!context) return std::unexpected("out of memory");

  context->sample_rate = settings.sample_rate;
  context->sample_fmt = preferred_sample_format(aac);
  context->bit_rate = settings.bitrate > 0 ? settings.bitrate : 192000;
  context->time_base = {1, settings.sample_rate};
  av_channel_layout_default(&context->ch_layout, settings.channels);

  if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  if (int rc = avcodec_open2(context.get(), aac, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open the AAC encoder: {}", av_error_string(rc)));
  }

  audio_encoder = std::move(context);
  audio_name = aac->name;
  audio_channels = settings.channels;
  // AAC encodes 1024-sample blocks. An encoder that does not care reports zero,
  // in which case any block will do and a round number keeps the buffering
  // predictable.
  audio_block = audio_encoder->frame_size > 0 ? audio_encoder->frame_size : 1024;

  audio_stream = avformat_new_stream(format.get(), nullptr);
  if (audio_stream == nullptr) return std::unexpected("cannot create an audio stream");
  audio_stream->time_base = audio_encoder->time_base;
  if (int rc = avcodec_parameters_from_context(audio_stream->codecpar, audio_encoder.get());
      rc < 0) {
    return std::unexpected(
        std::format("cannot describe the audio stream: {}", av_error_string(rc)));
  }

  audio_frame.reset(av_frame_alloc());
  if (!audio_frame) return std::unexpected("out of memory");
  audio_frame->format = audio_encoder->sample_fmt;
  audio_frame->nb_samples = audio_block;
  audio_frame->sample_rate = audio_encoder->sample_rate;
  if (int rc = av_channel_layout_copy(&audio_frame->ch_layout, &audio_encoder->ch_layout);
      rc < 0) {
    return std::unexpected(std::format("cannot set the channel layout: {}", av_error_string(rc)));
  }
  if (int rc = av_frame_get_buffer(audio_frame.get(), 0); rc < 0) {
    return std::unexpected(
        std::format("cannot allocate an audio frame: {}", av_error_string(rc)));
  }

  SwrContext* resampler = nullptr;
  if (int rc = swr_alloc_set_opts2(&resampler, &audio_encoder->ch_layout,
                                   audio_encoder->sample_fmt, audio_encoder->sample_rate,
                                   &audio_encoder->ch_layout, AV_SAMPLE_FMT_FLT,
                                   settings.sample_rate, 0, nullptr);
      rc < 0 || resampler == nullptr) {
    return std::unexpected(
        std::format("cannot create an audio converter: {}", av_error_string(rc)));
  }
  audio_resampler.reset(resampler);
  if (int rc = swr_init(audio_resampler.get()); rc < 0) {
    return std::unexpected(
        std::format("cannot initialise the audio converter: {}", av_error_string(rc)));
  }

  return {};
}

std::expected<void, std::string> MediaWriter::Impl::encode_audio_block(int samples) {
  if (samples <= 0) return {};

  if (int rc = av_frame_make_writable(audio_frame.get()); rc < 0) {
    return std::unexpected(
        std::format("cannot write to the audio frame: {}", av_error_string(rc)));
  }
  audio_frame->nb_samples = samples;

  // Interleaved float in, whatever the encoder wants out — AAC takes planar
  // float, so this is a deinterleave rather than a format change, but going
  // through the resampler keeps it correct for an encoder that wants something
  // else.
  const auto* input = reinterpret_cast<const std::uint8_t*>(audio_pending.data());
  if (int rc = swr_convert(audio_resampler.get(), audio_frame->data, samples, &input, samples);
      rc < 0) {
    return std::unexpected(std::format("cannot convert audio: {}", av_error_string(rc)));
  }

  // Timestamps count samples, because the time base is 1/sample_rate.
  audio_frame->pts = audio_frames;
  audio_frames += samples;

  if (auto ok = drain(audio_encoder.get(), audio_stream, audio_frame.get(), 0); !ok) return ok;

  const auto consumed = static_cast<std::size_t>(samples) *
                        static_cast<std::size_t>(audio_channels);
  audio_pending.erase(audio_pending.begin(),
                      audio_pending.begin() + static_cast<std::ptrdiff_t>(consumed));
  return {};
}

MediaWriter::MediaWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

// The output context's deleter closes the I/O, so an export abandoned without
// `finish` leaves a file that is obviously truncated rather than one that looks
// complete.
MediaWriter::~MediaWriter() = default;

const std::string& MediaWriter::encoder_name() const noexcept { return impl_->name; }
std::int64_t MediaWriter::frame_count() const noexcept { return impl_->frames; }

std::expected<std::unique_ptr<MediaWriter>, std::string> MediaWriter::create(
    const std::string& path, const VideoEncodeSettings& settings,
    const AudioEncodeSettings& audio) {
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

  // Audio is set up before the header, because a container records its streams
  // there: adding one afterwards would not appear in the file.
  if (audio.enabled) {
    if (auto ok = impl->open_audio(audio); !ok) return std::unexpected(ok.error());
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

  return d.drain(d.encoder.get(), d.stream, d.frame.get(), 1);
}

std::expected<void, std::string> MediaWriter::write_audio(std::span<const float> interleaved) {
  Impl& d = *impl_;
  if (d.finished) return std::unexpected("the writer has already been finished");
  if (d.audio_stream == nullptr) {
    return std::unexpected("this writer was created without audio");
  }

  const auto channels = static_cast<std::size_t>(d.audio_channels);
  if (interleaved.size() % channels != 0) {
    return std::unexpected("the audio block is not a whole number of frames");
  }

  d.audio_pending.insert(d.audio_pending.end(), interleaved.begin(), interleaved.end());

  const auto block = static_cast<std::size_t>(d.audio_block) * channels;
  while (d.audio_pending.size() >= block) {
    if (auto ok = d.encode_audio_block(d.audio_block); !ok) return ok;
  }
  return {};
}

bool MediaWriter::has_audio() const noexcept { return impl_->audio_stream != nullptr; }

const std::string& MediaWriter::audio_encoder_name() const noexcept { return impl_->audio_name; }

std::int64_t MediaWriter::audio_frame_count() const noexcept { return impl_->audio_frames; }

std::expected<void, std::string> MediaWriter::finish() {
  Impl& d = *impl_;
  if (d.finished) return {};
  d.finished = true;

  if (d.audio_stream != nullptr) {
    // Whatever is left is shorter than a block; the encoder takes a short final
    // frame. Dropping it would clip up to 21 ms off the end of every export.
    const auto channels = static_cast<std::size_t>(d.audio_channels);
    if (const std::size_t remaining = d.audio_pending.size() / channels; remaining > 0) {
      if (auto ok = d.encode_audio_block(static_cast<int>(remaining)); !ok) return ok;
    }
    if (auto ok = d.drain(d.audio_encoder.get(), d.audio_stream, nullptr, 0); !ok) return ok;
  }

  if (auto ok = d.drain(d.encoder.get(), d.stream, nullptr, 1); !ok) return ok;

  if (int rc = av_write_trailer(d.format.get()); rc < 0) {
    return std::unexpected(std::format("cannot finalise the file: {}", av_error_string(rc)));
  }
  if ((d.format->oformat->flags & AVFMT_NOFILE) == 0 && d.format->pb != nullptr) {
    avio_closep(&d.format->pb);
  }
  return {};
}

}  // namespace cutline::media
