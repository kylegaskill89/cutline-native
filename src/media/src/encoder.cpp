#include "cutline/media/encoder.hpp"

#include "av_common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
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

/// The pixel format to give an encoder, from its own list rather than guessed,
/// or nothing when it has none at this depth.
///
/// The two-plane form first, NV12 or P010, because it is what an export
/// converts to on the card and an encoder that takes it takes the frame as it
/// is. Planar 4:2:0 for one that does not, which costs a shuffle of planes.
[[nodiscard]] std::optional<AVPixelFormat> preferred_format(const AVCodec* encoder,
                                                            int bit_depth) {
  const bool ten = bit_depth > 8;
  const AVPixelFormat* formats = nullptr;
  int count = 0;
  if (avcodec_get_supported_config(nullptr, encoder, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                   reinterpret_cast<const void**>(&formats), &count) < 0 ||
      formats == nullptr || count <= 0) {
    return ten ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
  }

  const std::array<AVPixelFormat, 2> wanted =
      ten ? std::array{AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE}
          : std::array{AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P};
  for (const AVPixelFormat format : wanted) {
    for (int i = 0; i < count; ++i) {
      if (formats[i] == format) return format;
    }
  }
  if (ten) return std::nullopt;
  return formats[0];
}

/// Writes the mastering display and light levels into the encoder's side data,
/// where the encoders that put them in the bitstream look, and into the
/// stream's, where the container looks.
[[nodiscard]] bool attach_hdr_metadata(AVCodecContext* context, AVCodecParameters* stream,
                                       const HdrMetadata& hdr) {
  const auto ratio = [](double value, int denominator) {
    return AVRational{static_cast<int>(std::lround(value * denominator)), denominator};
  };
  AVMasteringDisplayMetadata mastering{};
  mastering.display_primaries[0][0] = ratio(hdr.red_x, 50000);
  mastering.display_primaries[0][1] = ratio(hdr.red_y, 50000);
  mastering.display_primaries[1][0] = ratio(hdr.green_x, 50000);
  mastering.display_primaries[1][1] = ratio(hdr.green_y, 50000);
  mastering.display_primaries[2][0] = ratio(hdr.blue_x, 50000);
  mastering.display_primaries[2][1] = ratio(hdr.blue_y, 50000);
  mastering.white_point[0] = ratio(hdr.white_x, 50000);
  mastering.white_point[1] = ratio(hdr.white_y, 50000);
  mastering.max_luminance = ratio(hdr.max_luminance, 10000);
  mastering.min_luminance = ratio(hdr.min_luminance, 10000);
  mastering.has_primaries = 1;
  mastering.has_luminance = 1;

  AVContentLightMetadata light{};
  light.MaxCLL = static_cast<unsigned>(std::max(0, hdr.max_content_light));
  light.MaxFALL = static_cast<unsigned>(std::max(0, hdr.max_frame_average_light));

  if (context != nullptr) {
    AVFrameSideData* a = av_frame_side_data_new(
        &context->decoded_side_data, &context->nb_decoded_side_data,
        AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, sizeof mastering, 0);
    AVFrameSideData* b = av_frame_side_data_new(
        &context->decoded_side_data, &context->nb_decoded_side_data,
        AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, sizeof light, 0);
    if (a == nullptr || b == nullptr) return false;
    std::memcpy(a->data, &mastering, sizeof mastering);
    std::memcpy(b->data, &light, sizeof light);
  }
  if (stream != nullptr) {
    AVPacketSideData* a = av_packet_side_data_new(
        &stream->coded_side_data, &stream->nb_coded_side_data,
        AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof mastering, 0);
    AVPacketSideData* b = av_packet_side_data_new(
        &stream->coded_side_data, &stream->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
        sizeof light, 0);
    if (a == nullptr || b == nullptr) return false;
    std::memcpy(a->data, &mastering, sizeof mastering);
    std::memcpy(b->data, &light, sizeof light);
  }
  return true;
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
  /// From NV12 or P010, for an encoder that wants planar; made the first time
  /// one does.
  detail::Scaler planar;
  bool ten_bit = false;
  bool sdr = true;
  AVStream* stream = nullptr;
  std::string name;
  std::int64_t frames = 0;

  /// One audio stream: its encoder, its buffers and its clock.
  ///
  /// A struct rather than a set of parallel members because a file may carry
  /// several - one per timeline track - and every one of them needs its own
  /// pending samples and its own sample count. Sharing either would interleave
  /// two tracks into one stream.
  struct AudioStream {
    detail::CodecContext encoder;
    detail::Frame frame;
    detail::Resampler resampler;
    AVStream* stream = nullptr;
    /// The encoder's own frame size, in samples per channel.
    int block = 0;
    /// Interleaved float samples accepted but not yet encoded, because the
    /// encoder takes fixed-size blocks and callers should not have to know it.
    std::vector<float> pending;
    std::int64_t frames = 0;
  };
  std::vector<AudioStream> audio;
  std::string audio_name;
  int audio_channels = 0;

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

  /// Encodes exactly `samples` frames from the front of one stream's pending.
  [[nodiscard]] std::expected<void, std::string> encode_audio_block(AudioStream& target,
                                                                    int samples);

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

  AudioStream built;
  built.encoder = std::move(context);
  audio_name = aac->name;
  audio_channels = settings.channels;
  // AAC encodes 1024-sample blocks. An encoder that does not care reports zero,
  // in which case any block will do and a round number keeps the buffering
  // predictable.
  built.block = built.encoder->frame_size > 0 ? built.encoder->frame_size : 1024;

  built.stream = avformat_new_stream(format.get(), nullptr);
  if (built.stream == nullptr) return std::unexpected("cannot create an audio stream");
  built.stream->time_base = built.encoder->time_base;
  if (int rc = avcodec_parameters_from_context(built.stream->codecpar, built.encoder.get());
      rc < 0) {
    return std::unexpected(
        std::format("cannot describe the audio stream: {}", av_error_string(rc)));
  }

  built.frame.reset(av_frame_alloc());
  if (!built.frame) return std::unexpected("out of memory");
  built.frame->format = built.encoder->sample_fmt;
  built.frame->nb_samples = built.block;
  built.frame->sample_rate = built.encoder->sample_rate;
  if (int rc = av_channel_layout_copy(&built.frame->ch_layout, &built.encoder->ch_layout);
      rc < 0) {
    return std::unexpected(std::format("cannot set the channel layout: {}", av_error_string(rc)));
  }
  if (int rc = av_frame_get_buffer(built.frame.get(), 0); rc < 0) {
    return std::unexpected(
        std::format("cannot allocate an audio frame: {}", av_error_string(rc)));
  }

  SwrContext* resampler = nullptr;
  if (int rc = swr_alloc_set_opts2(&resampler, &built.encoder->ch_layout,
                                   built.encoder->sample_fmt, built.encoder->sample_rate,
                                   &built.encoder->ch_layout, AV_SAMPLE_FMT_FLT,
                                   settings.sample_rate, 0, nullptr);
      rc < 0 || resampler == nullptr) {
    return std::unexpected(
        std::format("cannot create an audio converter: {}", av_error_string(rc)));
  }
  built.resampler.reset(resampler);
  if (int rc = swr_init(built.resampler.get()); rc < 0) {
    return std::unexpected(
        std::format("cannot initialise the audio converter: {}", av_error_string(rc)));
  }

  audio.push_back(std::move(built));
  return {};
}

std::expected<void, std::string> MediaWriter::Impl::encode_audio_block(AudioStream& target,
                                                                       int samples) {
  if (samples <= 0) return {};

  if (int rc = av_frame_make_writable(target.frame.get()); rc < 0) {
    return std::unexpected(
        std::format("cannot write to the audio frame: {}", av_error_string(rc)));
  }
  target.frame->nb_samples = samples;

  // Interleaved float in, whatever the encoder wants out — AAC takes planar
  // float, so this is a deinterleave rather than a format change, but going
  // through the resampler keeps it correct for an encoder that wants something
  // else.
  const auto* input = reinterpret_cast<const std::uint8_t*>(target.pending.data());
  if (int rc = swr_convert(target.resampler.get(), target.frame->data, samples, &input, samples);
      rc < 0) {
    return std::unexpected(std::format("cannot convert audio: {}", av_error_string(rc)));
  }

  // Timestamps count samples, because the time base is 1/sample_rate. Kept per
  // stream, so one written less often than another does not inherit its clock.
  target.frame->pts = target.frames;
  target.frames += samples;

  if (auto ok = drain(target.encoder.get(), target.stream, target.frame.get(), 0); !ok) {
    return ok;
  }

  const auto consumed = static_cast<std::size_t>(samples) *
                        static_cast<std::size_t>(audio_channels);
  target.pending.erase(target.pending.begin(),
                       target.pending.begin() + static_cast<std::ptrdiff_t>(consumed));
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
  if (settings.bit_depth != 8 && settings.bit_depth != 10) {
    return std::unexpected("the bit depth must be 8 or 10");
  }
  if (settings.bit_depth == 10 && settings.codec != VideoCodec::Hevc) {
    return std::unexpected("ten bits needs HEVC");
  }
  if (settings.colour != VideoColour::Bt709 && settings.bit_depth != 10) {
    return std::unexpected("HDR needs ten bits");
  }

  auto impl = std::make_unique<Impl>();
  // Encoders overwhelmingly want even dimensions for 4:2:0 chroma, and an odd
  // one fails deep inside the encoder with an unhelpful message.
  impl->width = settings.width & ~1;
  impl->height = settings.height & ~1;

  AVFormatContext* format = nullptr;
  detail::quiet_av_logging();
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

    // One frame across two hardware encoders, where the frame is big enough
    // to be worth splitting and the encoder can. A 4K frame through one NVENC
    // took 5.3 ms, which held an export to 2.3x realtime with everything else
    // waiting on it; split, 2.1 ms. At the same quality setting the file was
    // 3.5% larger and 0.03 dB lower in PSNR against the source. HEVC only, as
    // the encoder offers it, and at 4K and up, which is where NVIDIA's own
    // automatic choice draws the line.
    const bool splittable = std::string_view(name).ends_with("nvenc") &&
                            settings.codec == VideoCodec::Hevc &&
                            static_cast<std::int64_t>(impl->width) * impl->height >=
                                std::int64_t{3840} * 2160;

    const auto pixel_format = preferred_format(encoder, settings.bit_depth);
    if (!pixel_format) {
      if (!attempts.empty()) attempts += ", ";
      attempts += std::format("{} (no {}-bit input)", name, settings.bit_depth);
      continue;
    }

    const auto open = [&](bool split) -> std::expected<detail::CodecContext, std::string> {
      detail::CodecContext context(avcodec_alloc_context3(encoder));
      if (!context) return std::unexpected("out of memory");

      context->width = impl->width;
      context->height = impl->height;
      context->time_base = av_inv_q(rational_fps(settings.fps));
      context->framerate = rational_fps(settings.fps);
      context->pix_fmt = *pixel_format;
      if (settings.colour == VideoColour::Bt709) {
        context->colorspace = AVCOL_SPC_BT709;
        context->color_primaries = AVCOL_PRI_BT709;
        context->color_trc = AVCOL_TRC_BT709;
      } else {
        context->colorspace = AVCOL_SPC_BT2020_NCL;
        context->color_primaries = AVCOL_PRI_BT2020;
        context->color_trc = settings.colour == VideoColour::Bt2100Pq ? AVCOL_TRC_SMPTE2084
                                                                      : AVCOL_TRC_ARIB_STD_B67;
      }
      context->color_range = AVCOL_RANGE_MPEG;
      if (settings.colour == VideoColour::Bt2100Pq && settings.hdr.has_value() &&
          !attach_hdr_metadata(context.get(), nullptr, *settings.hdr)) {
        return std::unexpected("out of memory");
      }

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
      if (split) av_dict_set(&options, "split_encode_mode", "forced", 0);

      const int rc = avcodec_open2(context.get(), encoder, &options);
      av_dict_free(&options);
      if (rc < 0) return std::unexpected(av_error_string(rc));
      return context;
    };

    // A card with one encoder refuses the split when the encoder opens. That
    // is a reason to encode unsplit, not to fall through to software.
    auto opened = open(splittable);
    if (!opened && splittable) opened = open(false);
    if (!opened) {
      if (!attempts.empty()) attempts += ", ";
      attempts += std::format("{} ({})", name, opened.error());
      continue;
    }
    detail::CodecContext context = std::move(*opened);

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
  if (settings.colour == VideoColour::Bt2100Pq && settings.hdr.has_value() &&
      av_packet_side_data_get(impl->stream->codecpar->coded_side_data,
                              impl->stream->codecpar->nb_coded_side_data,
                              AV_PKT_DATA_MASTERING_DISPLAY_METADATA) == nullptr &&
      !attach_hdr_metadata(nullptr, impl->stream->codecpar, *settings.hdr)) {
    return std::unexpected("out of memory");
  }
  impl->ten_bit = settings.bit_depth == 10;
  impl->sdr = settings.colour == VideoColour::Bt709;

  // Audio is set up before the header, because a container records its streams
  // there: adding one afterwards would not appear in the file.
  if (audio.enabled) {
    // One call per stream. Each gets its own encoder rather than sharing one,
    // because an encoder carries the clock and the buffered tail of whatever
    // was last written through it.
    for (int i = 0; i < std::max(1, audio.streams); ++i) {
      if (auto ok = impl->open_audio(audio); !ok) return std::unexpected(ok.error());
    }
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
  if (!d.sdr) return std::unexpected("an HDR writer takes converted frames, not sRGB");

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

std::expected<void, std::string> MediaWriter::write_frame(const PlanarFrame& planes) {
  Impl& d = *impl_;
  if (d.finished) return std::unexpected("the writer has already been finished");
  if (planes.luma == nullptr || planes.chroma == nullptr) {
    return std::unexpected("a frame needs both its planes");
  }
  if (planes.ten_bit != d.ten_bit) {
    return std::unexpected(d.ten_bit ? "this writer takes 10-bit frames"
                                     : "this writer takes 8-bit frames");
  }

  if (int rc = av_frame_make_writable(d.frame.get()); rc < 0) {
    return std::unexpected(std::format("cannot write to the frame: {}", av_error_string(rc)));
  }

  if (d.encoder->pix_fmt == AV_PIX_FMT_NV12 || d.encoder->pix_fmt == AV_PIX_FMT_P010LE) {
    // Copied rather than lent: the planes belong to a readback buffer that is
    // handed back the moment this returns, and an encoder may hold a frame
    // past `avcodec_send_frame`.
    const auto rows = [](std::uint8_t* to, int to_stride, const std::uint8_t* from,
                         std::size_t from_stride, std::size_t bytes, int count) {
      for (int y = 0; y < count; ++y) {
        std::memcpy(to + static_cast<std::size_t>(y) * to_stride,
                    from + static_cast<std::size_t>(y) * from_stride, bytes);
      }
    };
    // Two bytes a sample at ten bits.
    const auto width = static_cast<std::size_t>(d.width) * (d.ten_bit ? 2 : 1);
    rows(d.frame->data[0], d.frame->linesize[0], planes.luma, planes.luma_stride, width,
         d.height);
    rows(d.frame->data[1], d.frame->linesize[1], planes.chroma, planes.chroma_stride, width,
         d.height / 2);
  } else {
    if (!d.planar) {
      d.planar.reset(sws_getContext(d.width, d.height,
                                    d.ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12, d.width,
                                    d.height, d.encoder->pix_fmt, SWS_POINT, nullptr, nullptr,
                                    nullptr));
      if (!d.planar) return std::unexpected("cannot create a plane converter");
    }
    const std::uint8_t* source[4] = {planes.luma, planes.chroma, nullptr, nullptr};
    const int stride[4] = {static_cast<int>(planes.luma_stride),
                           static_cast<int>(planes.chroma_stride), 0, 0};
    sws_scale(d.planar.get(), source, stride, 0, d.height, d.frame->data, d.frame->linesize);
  }

  d.frame->pts = d.frames;
  ++d.frames;
  return d.drain(d.encoder.get(), d.stream, d.frame.get(), 1);
}

std::expected<void, std::string> MediaWriter::write_audio(std::span<const float> interleaved,
                                                         int stream) {
  Impl& d = *impl_;
  if (d.finished) return std::unexpected("the writer has already been finished");
  if (d.audio.empty()) return std::unexpected("this writer was created without audio");
  if (stream < 0 || static_cast<std::size_t>(stream) >= d.audio.size()) {
    return std::unexpected("this writer has no audio stream by that number");
  }

  Impl::AudioStream& target = d.audio[static_cast<std::size_t>(stream)];
  const auto channels = static_cast<std::size_t>(d.audio_channels);
  if (interleaved.size() % channels != 0) {
    return std::unexpected("the audio block is not a whole number of frames");
  }

  target.pending.insert(target.pending.end(), interleaved.begin(), interleaved.end());

  const auto block = static_cast<std::size_t>(target.block) * channels;
  while (target.pending.size() >= block) {
    if (auto ok = d.encode_audio_block(target, target.block); !ok) return ok;
  }
  return {};
}

bool MediaWriter::has_audio() const noexcept { return !impl_->audio.empty(); }

int MediaWriter::audio_stream_count() const noexcept {
  return static_cast<int>(impl_->audio.size());
}

const std::string& MediaWriter::audio_encoder_name() const noexcept { return impl_->audio_name; }

std::int64_t MediaWriter::audio_frame_count(int stream) const noexcept {
  if (stream < 0 || static_cast<std::size_t>(stream) >= impl_->audio.size()) return 0;
  return impl_->audio[static_cast<std::size_t>(stream)].frames;
}

std::expected<void, std::string> MediaWriter::finish() {
  Impl& d = *impl_;
  if (d.finished) return {};
  d.finished = true;

  for (Impl::AudioStream& target : d.audio) {
    // Whatever is left is shorter than a block; the encoder takes a short final
    // frame. Dropping it would clip up to 21 ms off the end of every export.
    const auto channels = static_cast<std::size_t>(d.audio_channels);
    if (const std::size_t remaining = target.pending.size() / channels; remaining > 0) {
      if (auto ok = d.encode_audio_block(target, static_cast<int>(remaining)); !ok) return ok;
    }
    if (auto ok = d.drain(target.encoder.get(), target.stream, nullptr, 0); !ok) return ok;
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
