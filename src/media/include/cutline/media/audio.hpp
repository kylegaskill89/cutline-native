#pragma once

/// Audio decoding and waveform peaks.
///
/// A clip addresses its audio by *ordinal* — the N in "the third audio stream
/// of this file" — because that is what survives the file being remuxed. The
/// media layer maps that onto libav's absolute stream index.

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::media {

/// Fully decoded audio, interleaved and normalised to [-1, 1].
struct AudioBuffer {
  int sample_rate = 48000;
  int channels = 2;
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
};

/// Decodes one audio stream in full, resampled to the requested format.
///
/// This holds the whole stream in memory — ten minutes of 48 kHz stereo is
/// roughly 230 MB — which is what real-time playback scheduling wants and what
/// the reference did. Streaming decode is a later concern, for export.
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

}  // namespace cutline::media
