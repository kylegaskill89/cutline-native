#pragma once

/// Audio decoding and waveform peaks.
///
/// A clip addresses its audio by *ordinal* — the N in "the third audio stream
/// of this file" — because that is what survives the file being remuxed. The
/// media layer maps that onto libav's absolute stream index.

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::media {

/// Decoded audio, interleaved and normalised to [-1, 1].
struct AudioBuffer {
  int sample_rate = 48000;
  int channels = 2;
  /// Source time of the first sample, in seconds. Non-zero when only a range
  /// was decoded, so a buffer says where it came from rather than leaving the
  /// caller to remember.
  double start_time = 0.0;
  std::vector<float> samples;

  [[nodiscard]] std::size_t frame_count() const noexcept {
    return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
  }

  [[nodiscard]] double duration() const noexcept {
    return sample_rate > 0 ? static_cast<double>(frame_count()) / sample_rate : 0.0;
  }
};

struct AudioDecodeOptions {
  int sample_rate = 48000;
  int channels = 2;

  /// Source range to decode, in seconds. A non-positive `duration` takes
  /// everything from `start` to the end of the stream.
  ///
  /// Asking for a range is not merely an optimisation at the scale this is used
  /// at: the reference captures run ten minutes with four audio streams each,
  /// where decoding a whole stream to place a twenty-second clip costs about
  /// four seconds and 230 MB — per clip, per stream.
  double start = 0.0;
  double duration = 0.0;
};

/// Decodes one audio stream, or a range of it, resampled to the requested
/// format.
///
/// The result is held in memory rather than streamed, which is what real-time
/// playback scheduling wants and what the reference did. Bounding the range is
/// what keeps that affordable.
[[nodiscard]] std::expected<AudioBuffer, std::string> decode_audio(
    std::string_view path, int audio_stream = 0, AudioDecodeOptions options = {});

/// Min/max envelope per time bucket, which is what a timeline waveform draws.
/// Keeping both bounds rather than an average is what makes a transient visible
/// instead of averaging it away.
struct WaveformPeaks {
  int buckets_per_second = 100;
  std::vector<float> minimum;
  std::vector<float> maximum;

  [[nodiscard]] std::size_t size() const noexcept { return minimum.size(); }
  [[nodiscard]] bool empty() const noexcept { return minimum.empty(); }
};

/// Reduces a decoded buffer to peaks. Channels are combined, so the envelope
/// spans everything audible at that moment rather than one side of a stereo
/// pair. Pure, and tested without any media file.
[[nodiscard]] WaveformPeaks compute_peaks(const AudioBuffer& audio, int buckets_per_second = 100);

/// Decodes a stream and reduces it to peaks in one step.
[[nodiscard]] std::expected<WaveformPeaks, std::string> extract_waveform(
    std::string_view path, int audio_stream = 0, int buckets_per_second = 100);

/// Peaks for several of a file's audio streams, read in **one pass**.
///
/// The reason this exists rather than calling `extract_waveform` per stream:
/// getting one stream's samples means demuxing the whole container, because
/// that is how the packets are interleaved. Doing that once per stream reads
/// the file once per stream — and the material this is built for is a 1.5 GB
/// capture with four audio streams in it, so the difference is reading a
/// gigabyte and a half instead of six.
///
/// Streams are given as ordinals among the file's audio streams, the same
/// numbering `decode_audio` and `Clip::audio_stream` use. The result is in the
/// order asked for. An ordinal the file does not have gives an empty envelope
/// in its place rather than failing the lot, since one absent stream should not
/// cost the others their waveforms.
[[nodiscard]] std::expected<std::vector<WaveformPeaks>, std::string> extract_waveforms(
    std::string_view path, std::span<const int> audio_streams, int buckets_per_second = 100);

}  // namespace cutline::media
