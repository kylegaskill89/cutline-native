#include "cutline/media/audio.hpp"

#include "av_common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <limits>
#include <vector>

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

/// One pass over an audio stream, resampled to `options`' format.
///
/// `consume` is called after every decoded frame with everything resampled so
/// far sitting in `out`; whatever it leaves behind is carried into the next
/// call. That is the whole point of it: a caller that only needs a *summary* of
/// the audio can fold and clear as it goes rather than holding the stream.
///
/// The alternative — what this used to be — is a single vector the length of
/// the file. A ten-minute recording with four audio streams is about a
/// gigabyte decoded, measured at 1,355 MB peak for one clip, and that is what
/// this application was paying to draw a line on a clip.
///
/// Returns the source time of the first sample handed over, which is at or
/// before what was asked for: a compressed stream cannot be entered
/// mid-packet, so a range seeks backwards and the surplus is trimmed by
/// whoever asked for it.
[[nodiscard]] std::expected<double, std::string> read_audio(
    std::string_view path, int audio_stream, const AudioDecodeOptions& options,
    std::vector<float>& out, const std::function<void()>& consume) {
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

  // Everything else in the file is refused before a single packet is read.
  //
  // `av_read_frame` hands over every stream's packets and the loop below throws
  // away the ones it did not want — which sounds free and is not. The demuxer
  // still parses them, and on the material this is built for that is 4K HEVC
  // interleaved with the audio: reading one ten-minute stream out of a 1.5 GB
  // capture meant parsing the whole 1.5 GB. `AVDISCARD_ALL` makes the demuxer
  // skip those packets at the container level instead.
  for (unsigned i = 0; i < format->nb_streams; ++i) {
    format->streams[i]->discard = static_cast<int>(i) == index ? AVDISCARD_DEFAULT
                                                               : AVDISCARD_ALL;
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

      if (auto ok = drain_resampler(resampler.get(), frame.get(), options.channels, out);
          !ok) {
        return ok;
      }
      consume();
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
  if (auto ok = drain_resampler(resampler.get(), nullptr, options.channels, out); !ok) {
    return std::unexpected(ok.error());
  }
  consume();

  return decoded_start;
}

}  // namespace

std::expected<AudioBuffer, std::string> decode_audio(std::string_view path, int audio_stream,
                                                     AudioDecodeOptions options) {
  if (options.channels <= 0 || options.sample_rate <= 0) {
    return std::unexpected("target sample rate and channel count must be positive");
  }

  AudioBuffer buffer;
  buffer.sample_rate = options.sample_rate;
  buffer.channels = options.channels;

  // Nothing to consume as it arrives: this is the caller that genuinely wants
  // the whole thing, because what it feeds is real-time playback scheduling.
  const auto decoded = read_audio(path, audio_stream, options, buffer.samples, [] {});
  if (!decoded) return std::unexpected(decoded.error());
  const double decoded_start = *decoded;

  const bool ranged = options.duration > 0.0 || options.start > 0.0;
  const double wanted_start = std::max(0.0, options.start);

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

namespace {

/// `compute_peaks`, one block at a time.
///
/// Buckets are counted in frames from the start of the stream, exactly as the
/// whole-buffer version counts them, so the two produce the same envelope —
/// there is a test that says so. What differs is that this never sees more than
/// one decoded block at once.
class PeakFolder {
 public:
  PeakFolder(int sample_rate, int channels, int buckets_per_second)
      : channels_(static_cast<std::size_t>(std::max(1, channels))),
        per_bucket_(static_cast<std::size_t>(
            std::max(1.0, static_cast<double>(sample_rate) /
                              std::max(1, buckets_per_second)))) {
    peaks_.buckets_per_second = std::max(1, buckets_per_second);
  }

  /// Folds a block of interleaved frames. Whole frames only, which is what the
  /// resampler produces.
  void take(std::span<const float> block) {
    const std::size_t frames = block.size() / channels_;
    for (std::size_t f = 0; f < frames; ++f) {
      for (std::size_t ch = 0; ch < channels_; ++ch) {
        // Channels are folded together so the envelope covers everything
        // audible at that moment rather than one side of a stereo pair.
        const float sample = block[f * channels_ + ch];
        if (!seen_) {
          low_ = sample;
          high_ = sample;
          seen_ = true;
        } else {
          low_ = std::min(low_, sample);
          high_ = std::max(high_, sample);
        }
      }
      if (++in_bucket_ == per_bucket_) close();
    }
  }

  /// The envelope, with any part-filled last bucket closed off. A stream whose
  /// length is not a whole number of buckets still ends where it ends.
  [[nodiscard]] WaveformPeaks finish() {
    if (in_bucket_ > 0) close();
    return std::move(peaks_);
  }

 private:
  void close() {
    peaks_.minimum.push_back(low_);
    peaks_.maximum.push_back(high_);
    in_bucket_ = 0;
    seen_ = false;
    low_ = 0.0f;
    high_ = 0.0f;
  }

  std::size_t channels_ = 2;
  std::size_t per_bucket_ = 1;
  std::size_t in_bucket_ = 0;
  float low_ = 0.0f;
  float high_ = 0.0f;
  bool seen_ = false;
  WaveformPeaks peaks_;
};

}  // namespace

std::expected<WaveformPeaks, std::string> extract_waveform(std::string_view path,
                                                           int audio_stream,
                                                           int buckets_per_second) {
  const AudioDecodeOptions options;
  PeakFolder folder(options.sample_rate, options.channels, buckets_per_second);

  // Folded and thrown away block by block. An envelope is the one thing here
  // that reads a whole stream and keeps almost none of it — a ten-minute
  // recording is a gigabyte decoded and about a megabyte of envelope — so
  // holding the decode was paying a thousand times over for the answer.
  std::vector<float> block;
  const auto decoded = read_audio(path, audio_stream, options, block, [&] {
    folder.take(block);
    block.clear();
  });
  if (!decoded) return std::unexpected(decoded.error());

  return folder.finish();
}

std::expected<std::vector<WaveformPeaks>, std::string> extract_waveforms(
    std::string_view path, std::span<const int> audio_streams, int buckets_per_second) {
  if (audio_streams.empty()) return std::vector<WaveformPeaks>{};

  const std::string path_string(path);
  const AudioDecodeOptions options;

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

  /// One stream being read: its decoder, its resampler, and the envelope being
  /// folded. Kept together because the demux loop dispatches on the absolute
  /// index and has to reach all three.
  struct Reading {
    int index = -1;
    CodecContext decoder;
    Resampler resampler;
    std::unique_ptr<PeakFolder> folder;
    std::vector<float> block;
  };

  // Nothing at all until a stream asks for it, so a container's other tracks
  // are skipped by the demuxer rather than parsed and dropped.
  for (unsigned i = 0; i < format->nb_streams; ++i) format->streams[i]->discard = AVDISCARD_ALL;

  std::vector<Reading> readings(audio_streams.size());
  for (std::size_t i = 0; i < audio_streams.size(); ++i) {
    Reading& reading = readings[i];
    // An ordinal the file does not have leaves an empty envelope rather than
    // failing every other stream with it.
    reading.index = audio_stream_index(format.get(), audio_streams[i]);
    if (reading.index < 0) continue;

    AVStream* stream = format->streams[reading.index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
      reading.index = -1;
      continue;
    }
    stream->discard = AVDISCARD_DEFAULT;

    reading.decoder.reset(avcodec_alloc_context3(codec));
    if (!reading.decoder) return std::unexpected("out of memory allocating an audio decoder");
    if (const int rc = avcodec_parameters_to_context(reading.decoder.get(), stream->codecpar);
        rc < 0) {
      return std::unexpected(std::format("cannot configure decoder: {}", av_error_string(rc)));
    }
    reading.decoder->pkt_timebase = stream->time_base;
    if (const int rc = avcodec_open2(reading.decoder.get(), codec, nullptr); rc < 0) {
      return std::unexpected(std::format("cannot open audio decoder: {}", av_error_string(rc)));
    }

    AVChannelLayout target_layout{};
    av_channel_layout_default(&target_layout, options.channels);
    SwrContext* raw_resampler = nullptr;
    const int rc = swr_alloc_set_opts2(&raw_resampler, &target_layout, AV_SAMPLE_FMT_FLT,
                                       options.sample_rate, &reading.decoder->ch_layout,
                                       reading.decoder->sample_fmt, reading.decoder->sample_rate,
                                       0, nullptr);
    av_channel_layout_uninit(&target_layout);
    if (rc < 0) {
      return std::unexpected(std::format("cannot configure resampler: {}", av_error_string(rc)));
    }
    reading.resampler.reset(raw_resampler);
    if (const int opened = swr_init(reading.resampler.get()); opened < 0) {
      return std::unexpected(std::format("cannot open resampler: {}", av_error_string(opened)));
    }
    reading.folder = std::make_unique<PeakFolder>(options.sample_rate, options.channels,
                                                  buckets_per_second);
  }

  Frame frame(av_frame_alloc());
  Packet packet(av_packet_alloc());
  if (!frame || !packet) return std::unexpected("out of memory allocating an audio frame");

  const auto pump = [&](Reading& reading) -> std::expected<void, std::string> {
    for (;;) {
      const int got = avcodec_receive_frame(reading.decoder.get(), frame.get());
      if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return {};
      if (got < 0) return std::unexpected(std::format("decode failed: {}", av_error_string(got)));

      if (auto ok = drain_resampler(reading.resampler.get(), frame.get(), options.channels,
                                    reading.block);
          !ok) {
        return ok;
      }
      reading.folder->take(reading.block);
      reading.block.clear();
    }
  };

  for (;;) {
    const int read = av_read_frame(format.get(), packet.get());
    if (read == AVERROR_EOF) break;
    if (read < 0) {
      return std::unexpected(std::format("read failed: {}", av_error_string(read)));
    }

    for (Reading& reading : readings) {
      if (reading.index != packet->stream_index) continue;
      if (const int sent = avcodec_send_packet(reading.decoder.get(), packet.get()); sent < 0) {
        av_packet_unref(packet.get());
        return std::unexpected(std::format("cannot feed decoder: {}", av_error_string(sent)));
      }
      if (auto ok = pump(reading); !ok) {
        av_packet_unref(packet.get());
        return std::unexpected(ok.error());
      }
      break;
    }
    av_packet_unref(packet.get());
  }

  std::vector<WaveformPeaks> out(audio_streams.size());
  for (std::size_t i = 0; i < readings.size(); ++i) {
    Reading& reading = readings[i];
    if (reading.folder == nullptr) continue;

    avcodec_send_packet(reading.decoder.get(), nullptr);
    if (auto ok = pump(reading); !ok) return std::unexpected(ok.error());
    // The resampler holds samples back for its own latency; without this the
    // last few milliseconds of every stream would go missing.
    if (auto ok = drain_resampler(reading.resampler.get(), nullptr, options.channels,
                                  reading.block);
        !ok) {
      return std::unexpected(ok.error());
    }
    reading.folder->take(reading.block);
    out[i] = reading.folder->finish();
  }
  return out;
}

}  // namespace cutline::media
