#pragma once

/// Lining up two recordings of the same event by their sound.
///
/// What Premiere's "Audio" synchronise point does for a multi-camera source
/// sequence: every camera hears the clap, the door and the first word, so the
/// offset at which their sound agrees best is the offset between them. The
/// microphones differ, so the samples themselves do not match; what matches is
/// *when things happen*. Each recording is reduced to an onset envelope, the
/// rises in its loudness, and the two envelopes are cross-correlated.

#include <optional>
#include <span>
#include <vector>

namespace cutline::audio {

/// How many envelope values a second of sound becomes: 2 ms apart, which is a
/// twentieth of a frame at 25 and finer than any cut can land.
inline constexpr int kSyncEnvelopeRate = 500;

/// The rises in loudness of a mono signal, `kSyncEnvelopeRate` values a
/// second, with the average taken off so silence and a steady hum both count
/// for nothing.
[[nodiscard]] std::vector<float> onset_envelope(std::span<const float> mono, int sample_rate);

/// Where one envelope sits against another.
struct SyncMatch {
  /// Where `other` begins on `reference`'s clock, in seconds; negative when it
  /// began first.
  double offset = 0.0;
  /// How far the best agreement stands above the next best that is not the
  /// same moment, as a ratio. Near 1 is a guess.
  double confidence = 0.0;
};

/// The offset at which two envelopes agree best, or nothing when they share
/// too little to say: no overlap, silence, or no agreement that stands out.
[[nodiscard]] std::optional<SyncMatch> match_envelopes(std::span<const float> reference,
                                                       std::span<const float> other);

}  // namespace cutline::audio
