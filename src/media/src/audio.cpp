#include "cutline/media/audio.hpp"

#include "av_common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>

namespace cutline::media {
namespace {

using namespace detail;

/// Appends whatever the resampler produces for one decoded frame. Passing a
/// null frame flushes the samples it has been holding back for its filter
/// delay, which is where the tail of the stream lives.
[[nodiscard]] std::expected<void, std::string> drain_resampler(SwrContext* resampler,
                                                               const AVFrame* frame, int channels,
                                                               std::vector<float>& out) {
  // Worst case is every buffered sample plus this frame's, after rate change.
  const int64_t pending = swr_get_delay(resampler, frame != nullptr ? frame->sample_rate : 48000);
  const int capacity =
      static_cast<int>(pending) + (frame != nullptr ? frame->nb_samples : 0) + 256;

  const std::size_t offset = out.size();
  out.resize(offset + static_cast<std::size_t>(capacity) * static_cast<std::size_t>(channels));

  auto* destination = reinterpret_cast<uint8_t*>(out.data() + offset);
  const int written =
      swr_convert(resampler, &destination, capacity,
                  frame != nullptr ? const_cast<const uint8_t**>(frame->extended_data) : nullptr,
                  frame != nullptr ? frame->nb_samples : 0);
  if (written < 0) {
    return std::unexpected(std::format("resampling failed: {}", av_error_string(written)));
  }

  out.resize(offset + static_cast<std::size_t>(written) * static_cast<std::size_t>(channels));
  return {};
}

}  // namespace

std::expected<AudioBuffer, std::string> decode_audio(std::string_view path, int audio_stream,
                                                     AudioDecodeOptions options) {
  if (options.channels <= 0 || options.sample_rate <= 0) {
    return std::unexpected("target sample rate and channel count must be positive");
  }
  const std::string path_string(path);

  AVFormatContext* raw = nullptr;
  detail::quiet_av_logging();
  if (const int rc = avformat_open_input(&raw, path_string.c_str(), nullptr, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open {}: {}", path_string, av_error_string(rc)));
  }
  FormatContext format(raw);

  if (const int rc = avformat_find_stream_info(format.get(), nullptr); rc < 0) {
    return std::unexpected(
        std::format("cannot read streams in {}: {}", path_string, av_error_string(rc)));
  }

  const int index = audio_stream_index(format.get(), audio_stream);
  if (index < 0) {
    return std::unexpected(
        std::format("{} has no audio stream {}", path_string, audio_stream));
  }

  AVStream* stream = format->streams[index];
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) return std::unexpected("no decoder for this audio codec");

  CodecContext decoder(avcodec_alloc_context3(codec));
  if (!decoder) return std::unexpected("out of memory allocating an audio decoder");
  if (const int rc = avcodec_parameters_to_context(decoder.get(), stream->codecpar); rc < 0) {
    return std::unexpected(std::format("cannot configure decoder: {}", av_error_string(rc)));
  }
  decoder->pkt_timebase = stream->time_base;
  if (const int rc = avcodec_open2(decoder.get(), codec, nullptr); rc < 0) {
    return std::unexpected(std::format("cannot open audio decoder: {}", av_error_string(rc)));
  }

  AVChannelLayout target_layout{};
  av_channel_layout_default(&target_layout, options.channels);

  SwrContext* raw_resampler = nullptr;
  if (const int rc = swr_alloc_set_opts2(&raw_resampler, &target_layout, AV_SAMPLE_FMT_FLT,
                                         options.sample_rate, &decoder->ch_layout,
                                         decoder->sample_fmt, decoder->sample_rate, 0, nullptr);
      rc < 0) {
    av_channel_layout_uninit(&target_layout);
    return std::unexpected(std::format("cannot configure resampler: {}", av_error_string(rc)));
  }
  Resampler resampler(raw_resampler);
  av_channel_layout_uninit(&target_layout);

  if (const int rc = swr_init(resampler.get()); rc < 0) {
    return std::unexpected(std::format("cannot start resampler: {}", av_error_string(rc)));
  }

  Frame frame(av_frame_alloc());
  Packet packet(av_packet_alloc());
  if (!frame || !packet) return std::unexpected("out of memory allocating audio buffers");

  AudioBuffer buffer;
  buffer.sample_rate = options.sample_rate;
  buffer.channels = options.channels;

  const bool ranged = options.duration > 0.0 || options.start > 0.0;
  const double wanted_start = std::max(0.0, options.start);
  const double wanted_end =
      options.duration > 0.0 ? wanted_start + options.duration
                             : std::numeric_limits<double>::infinity();

  if (ranged && wanted_start > 0.0) {
    // Backwards, to the keyframe at or before the target: a compressed audio
    // stream cannot be entered mid-packet, so the decode starts early and the
    // surplus is trimmed off below.
    const auto target =
        static_cast<std::int64_t>(std::llround(wanted_start / av_q2d(stream->time_base)));
    if (av_seek_frame(format.get(), index, target, AVSEEK_FLAG_BACKWARD) >= 0) {
      avcodec_flush_buffers(decoder.get());
    }
  }

  // Where the first decoded sample actually landed, which is at or before what
  // was asked for.
  double decoded_start = wanted_start;
  bool have_start = false;
  bool past_end = false;

  const auto pump = [&]() -> std::expected<void, std::string> {
    while (true) {
      const int rc = avcodec_receive_frame(decoder.get(), frame.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
      if (rc < 0) {
        return std::unexpected(std::format("audio decode failed: {}", av_error_string(rc)));
      }

      if (frame->pts != AV_NOPTS_VALUE) {
        const double at = static_cast<double>(frame->pts) * av_q2d(stream->time_base);
        if (!have_start) {
          decoded_start = at;
          have_start = true;
        }
        if (at >= wanted_end) past_end = true;
      }

      if (auto ok = drain_resampler(resampler.get(), frame.get(), options.channels,
                                    buffer.samples);
          !ok) {
        return ok;
      }
    }
  };

  while (!past_end) {
    const int read = av_read_frame(format.get(), packet.get());
    if (read == AVERROR_EOF) break;
    if (read < 0) {
      return std::unexpected(std::format("read failed: {}", av_error_string(read)));
    }
    if (packet->stream_index == index) {
      const int sent = avcodec_send_packet(decoder.get(), packet.get());
      if (sent < 0) {
        av_packet_unref(packet.get());
        return std::unexpected(std::format("cannot feed decoder: {}", av_error_string(sent)));
      }
      if (auto ok = pump(); !ok) {
        av_packet_unref(packet.get());
        return std::unexpected(ok.error());
      }
    }
    av_packet_unref(packet.get());
  }

  avcodec_send_packet(decoder.get(), nullptr);
  if (auto ok = pump(); !ok) return std::unexpected(ok.error());

  // The resampler holds back samples for its own latency; without this flush
  // the last few milliseconds of every clip would go missing.
  if (auto ok = drain_resampler(resampler.get(), nullptr, options.channels, buffer.samples);
      !ok) {
    return std::unexpected(ok.error());
  }

  if (ranged) {
    const auto lanes = static_cast<std::size_t>(options.channels);
    const auto rate = static_cast<double>(options.sample_rate);

    // Drop what the seek overshot backwards into, so the first sample really is
    // the one asked for.
    if (const double lead = wanted_start - decoded_start; lead > 0.0) {
      const auto extra = std::min(buffer.samples.size(),
                                  static_cast<std::size_t>(lead * rate) * lanes);
      buffer.samples.erase(buffer.samples.begin(),
                           buffer.samples.begin() + static_cast<std::ptrdiff_t>(extra));
    }
    if (options.duration > 0.0) {
      const auto keep = static_cast<std::size_t>(options.duration * rate) * lanes;
      if (buffer.samples.size() > keep) buffer.samples.resize(keep);
    }
    buffer.start_time = wanted_start;
  }

  return buffer;
}

WaveformPeaks compute_peaks(const AudioBuffer& audio, int buckets_per_second) {
  WaveformPeaks peaks;
  peaks.buckets_per_second = std::max(1, buckets_per_second);

  const std::size_t frames = audio.frame_count();
  if (frames == 0 || audio.channels <= 0 || audio.sample_rate <= 0) return peaks;

  const auto frames_per_bucket = static_cast<std::size_t>(
      std::max(1.0, static_cast<double>(audio.sample_rate) / peaks.buckets_per_second));
  const std::size_t bucket_count = (frames + frames_per_bucket - 1) / frames_per_bucket;

  peaks.minimum.reserve(bucket_count);
  peaks.maximum.reserve(bucket_count);

  const auto channels = static_cast<std::size_t>(audio.channels);
  for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
    const std::size_t first = bucket * frames_per_bucket;
    const std::size_t last = std::min(first + frames_per_bucket, frames);

    float low = 0.0f;
    float high = 0.0f;
    bool seen = false;
    for (std::size_t f = first; f < last; ++f) {
      // Channels are folded together so the envelope covers everything audible
      // at that moment rather than one side of a stereo pair.
      for (std::size_t ch = 0; ch < channels; ++ch) {
        const float sample = audio.samples[f * channels + ch];
        if (!seen) {
          low = sample;
          high = sample;
          seen = true;
        } else {
          low = std::min(low, sample);
          high = std::max(high, sample);
        }
      }
    }
    peaks.minimum.push_back(low);
    peaks.maximum.push_back(high);
  }

  return peaks;
}

std::expected<WaveformPeaks, std::string> extract_waveform(std::string_view path,
                                                           int audio_stream,
                                                           int buckets_per_second) {
  const auto audio = decode_audio(path, audio_stream);
  if (!audio) return std::unexpected(audio.error());
  return compute_peaks(*audio, buckets_per_second);
}

}  // namespace cutline::media
