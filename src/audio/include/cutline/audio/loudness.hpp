#pragma once

/// Loudness to ITU-R BS.1770, which is what every delivery specification means
/// by the word.
///
/// Not the same thing as the RMS `Meter` reports. RMS answers "how much energy
/// is in this signal"; loudness answers "how loud does this sound to a person",
/// and the two disagree because hearing does not weigh all frequencies alike.
/// The standard says how to close that gap, and it is worth stating because
/// every part of it is doing something:
///
///  * **K-weighting** — a high shelf that lifts the top end by about 4 dB,
///    then a high-pass at 38 Hz. Together they are a rough model of a head in
///    a sound field: the shelf for the way a head boosts what is in front of
///    it, the high-pass for how little the ear makes of the very bottom.
///  * **400 ms blocks, overlapping by three quarters.** Loudness is a running
///    impression rather than an instant, and the overlap is what stops a loud
///    moment falling between two blocks and going uncounted.
///  * **Two gates.** An absolute one at -70 LUFS throws away silence, and a
///    relative one ten below the ungated average throws away the quiet parts.
///    Without them a programme's number is decided by how much room tone it
///    has, which is exactly the thing nobody is trying to measure.
///
/// The result is **LUFS** — decibels, but referenced so that a full-scale sine
/// reads about -3 and the numbers in a delivery specification are the numbers
/// you see. -14 is what most streaming wants, -23 is broadcast in Europe.

#include "cutline/audio/biquad.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace cutline::audio {

/// Below this a block is silence and is not counted at all.
inline constexpr double kAbsoluteGateLufs = -70.0;

/// How far below the ungated average the second gate sits, in LU.
inline constexpr double kRelativeGateLu = 10.0;

/// What a programme is usually asked to arrive at. Streaming's number; the
/// caller picks, and this is only the one to offer first.
inline constexpr double kDefaultLoudnessTarget = -14.0;

class LoudnessMeter {
 public:
  LoudnessMeter(double sample_rate, int channels);

  /// Measures one interleaved block. Does not change it — like `Meter`, this is
  /// a tap. Blocks may be any length and may be split anywhere.
  void process(std::span<const float> interleaved) noexcept;

  /// The gated integrated loudness over everything measured so far, in LUFS.
  ///
  /// `kAbsoluteGateLufs` when nothing loud enough has been heard, which is the
  /// honest answer for silence and keeps every caller from having to test for
  /// an empty measurement.
  [[nodiscard]] double integrated_lufs() const noexcept;

  /// How many 400 ms blocks have been counted. Zero means the measurement is
  /// too short to mean anything, which a caller may want to say out loud.
  [[nodiscard]] std::size_t blocks() const noexcept { return loudness_.size(); }

  void reset() noexcept;

 private:
  void take_block() noexcept;

  double sample_rate_ = 48000.0;
  std::size_t channels_ = 2;

  /// Two biquads per channel: the shelf, then the high-pass.
  std::vector<Biquad> shelf_;
  std::vector<Biquad> highpass_;

  /// A ring of the last 400 ms of weighted squares, per channel, and how far
  /// through the current 100 ms hop we are.
  std::vector<double> window_;
  std::size_t window_frames_ = 0;
  std::size_t at_ = 0;
  std::size_t filled_ = 0;
  std::size_t hop_frames_ = 0;
  std::size_t since_hop_ = 0;

  /// One entry per completed block: its loudness, and the mean square that
  /// produced it. The gates need both — the first to decide what to keep, the
  /// second to average what was kept.
  std::vector<double> loudness_;
  std::vector<double> power_;
};

/// The gain, in decibels, that would bring `measured` to `target`.
///
/// Trivial arithmetic, and it is a named function because the interesting part
/// is what it refuses: an unmeasurable programme has no number to move, and
/// returning zero rather than something enormous is what stops a silent
/// sequence being amplified into noise.
[[nodiscard]] double loudness_offset_db(double measured, double target) noexcept;

}  // namespace cutline::audio
