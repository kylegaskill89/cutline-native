#pragma once

/// Changing how long audio lasts without changing what it sounds like.
///
/// Retiming a clip cannot be done by resampling. Reading a source faster makes
/// it shorter *and* raises its pitch, which is a tape-speed effect, not a speed
/// change — a voice at 2x becomes a chipmunk. The reference used FFmpeg's
/// `atempo`, which preserves pitch, so anything less here would be an audible
/// regression on every retimed clip.
///
/// The method is WSOLA: cut the input into overlapping windows, lay them back
/// down at a different spacing, and choose each window's exact position by
/// looking for the one that best continues the waveform already written. That
/// last part is what separates it from plain overlap-add — without the
/// similarity search, successive windows land at arbitrary phase and cancel
/// each other into a hollow, flanged sound.

#include <cstddef>
#include <span>
#include <vector>

namespace cutline::audio {

/// Window length in samples at 48 kHz, about 21 ms.
///
/// The trade is fundamental to the method: longer windows preserve pitch and
/// harmonic detail but smear transients, shorter ones keep drums crisp and make
/// sustained tones warble. This sits where speech and music both hold up.
inline constexpr std::size_t kStretchWindow = 1024;

/// Stretches interleaved audio by `factor`: the result is `factor` times as
/// long, at the same pitch. A factor of 2 makes it twice as long, which is what
/// a clip at half speed needs.
///
/// A factor of 1, a non-positive factor, or an input too short to window is
/// returned unchanged, so the caller does not have to special-case them.
[[nodiscard]] std::vector<float> time_stretch(std::span<const float> interleaved, int channels,
                                              double factor);

}  // namespace cutline::audio
