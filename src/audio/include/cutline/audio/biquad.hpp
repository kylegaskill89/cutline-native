#pragma once

/// Second-order IIR filters — the RBJ Audio EQ Cookbook sections that every
/// audio effect in the registry except the compressor is built from.
///
/// The coefficient formulas match FFmpeg's `af_biquads.c`, which is what the
/// reference implementation actually rendered exports with. The preview there
/// used Web Audio's `BiquadFilterNode`, which is the same cookbook but picks
/// different default Q values, so preview and export never quite agreed. Owning
/// the DSP removes that split: one filter now serves both.
///
/// Filtering runs in transposed direct form II, which needs two state variables
/// per channel rather than four and is better behaved at low cutoff frequencies,
/// where direct form I loses precision in the input history.

#include <cstddef>
#include <span>

namespace cutline::audio {

/// Butterworth Q. FFmpeg's `highpass`/`lowpass` default to this, giving a
/// maximally flat passband with no resonant peak at the corner.
inline constexpr double kButterworthQ = 0.707;

/// FFmpeg's default width for the `bass` and `treble` shelves.
inline constexpr double kDefaultShelfQ = 0.5;

[[nodiscard]] double db_to_linear(double db) noexcept;
[[nodiscard]] double linear_to_db(double linear) noexcept;

/// One biquad section, carrying its own filter state.
///
/// A section is single-channel: stereo needs one per channel, since the state
/// is a memory of that channel's own past samples. `EffectChain` handles that.
class Biquad {
 public:
  /// Passes everything through unchanged. Also what a constructor with
  /// nonsensical arguments (zero sample rate, cutoff above Nyquist) yields, so
  /// a bad parameter is silent rather than an explosion.
  [[nodiscard]] static Biquad identity() noexcept;

  [[nodiscard]] static Biquad low_pass(double sample_rate, double freq,
                                       double q = kButterworthQ) noexcept;
  [[nodiscard]] static Biquad high_pass(double sample_rate, double freq,
                                        double q = kButterworthQ) noexcept;
  [[nodiscard]] static Biquad low_shelf(double sample_rate, double freq, double gain_db,
                                        double q = kDefaultShelfQ) noexcept;
  [[nodiscard]] static Biquad high_shelf(double sample_rate, double freq, double gain_db,
                                         double q = kDefaultShelfQ) noexcept;
  [[nodiscard]] static Biquad peaking(double sample_rate, double freq, double gain_db,
                                      double q) noexcept;
  [[nodiscard]] static Biquad band_reject(double sample_rate, double freq, double q) noexcept;

  /// Filters one sample, advancing the state.
  [[nodiscard]] float process(float x) noexcept;

  /// Filters a contiguous run of single-channel samples in place.
  void process(std::span<float> samples) noexcept;

  /// Takes `shape`'s coefficients and keeps its own history.
  ///
  /// What an animated parameter needs: the filter has to become a different
  /// filter without forgetting the samples it has already seen, because those
  /// samples really did precede the ones about to arrive. Assigning a freshly
  /// built filter over this one would zero the state instead, which is heard as
  /// a click every time the block boundary moves the cutoff.
  void retune(const Biquad& shape) noexcept;

  /// Forgets the filter's history. Needed whenever playback jumps, since the
  /// state describes samples that are no longer what came before.
  void reset() noexcept;

  /// |H(f)|, the gain this filter applies to a sine at `freq`.
  ///
  /// This is how the filters are tested — asserting a high-pass attenuates
  /// 20 Hz and passes 1 kHz says what the filter *is*, where comparing
  /// coefficients only says what it was typed as. The UI's EQ curve will want
  /// the same function.
  [[nodiscard]] double magnitude_at(double freq, double sample_rate) const noexcept;

  /// `magnitude_at` in decibels.
  [[nodiscard]] double gain_db_at(double freq, double sample_rate) const noexcept;

 private:
  // Numerator over denominator, already normalised by a0, in FFmpeg's naming:
  // y = b0*x + b1*x[-1] + b2*x[-2] - a1*y[-1] - a2*y[-2].
  double b0_ = 1.0;
  double b1_ = 0.0;
  double b2_ = 0.0;
  double a1_ = 0.0;
  double a2_ = 0.0;

  double z1_ = 0.0;
  double z2_ = 0.0;
};

}  // namespace cutline::audio
