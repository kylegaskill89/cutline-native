#pragma once

/// A feed-forward dynamic range compressor.
///
/// The model exposes only threshold and ratio, which is what the reference
/// implementation exposed. Everything else is fixed at values close to
/// FFmpeg's `acompressor` defaults, since that is what exports were rendered
/// with. This is our own implementation rather than a port: the reference had
/// two — `acompressor` on export and Web Audio's `DynamicsCompressorNode` in
/// the preview, whose defaults differ enough (3 ms attack against 20 ms, a
/// 30 dB knee against 9 dB) that a compressed clip audibly changed when it was
/// exported. One implementation cannot disagree with itself.
///
/// Channels are linked: the gain reduction is computed from the loudest channel
/// and applied to all of them, so a hard pan does not pull the stereo image
/// sideways every time the compressor engages.

#include <cstddef>
#include <span>

namespace cutline::audio {

struct CompressorSettings {
  /// Level above which gain reduction begins, in dBFS.
  double threshold_db = -18.0;
  /// Input-to-output ratio above the threshold; 1 is a no-op.
  double ratio = 4.0;
  double attack_ms = 20.0;
  double release_ms = 250.0;
  /// Width of the soft knee in dB, centred on the threshold. A hard corner is
  /// audible as a click on material that sits right at the threshold.
  double knee_db = 9.0;
  /// Output gain applied after compression, in dB.
  double makeup_db = 0.0;
};

class Compressor {
 public:
  Compressor(CompressorSettings settings, double sample_rate, int channels) noexcept;

  /// Compresses one interleaved block in place. Block boundaries do not affect
  /// the result: the envelope carries across calls, so splitting a buffer in
  /// two gives the same samples as processing it whole.
  void process(std::span<float> interleaved) noexcept;

  /// Takes new settings and keeps the envelope.
  ///
  /// What an animated threshold or ratio needs. Rebuilding the compressor
  /// instead would reset the gain reduction to zero at every block boundary,
  /// which is heard as the compressor letting go and grabbing again several
  /// times a second.
  ///
  /// `sample_rate` is passed again because attack and release are stored as
  /// per-sample coefficients rather than as the times they came from.
  void retune(CompressorSettings settings, double sample_rate) noexcept;

  void reset() noexcept;

  /// The gain reduction currently being applied, in dB (negative or zero).
  /// Meant for a meter in the UI.
  [[nodiscard]] double reduction_db() const noexcept;

  /// True when the settings cannot change the signal, so the caller can skip
  /// the work entirely.
  [[nodiscard]] bool transparent() const noexcept;

 private:
  [[nodiscard]] double curve_db(double input_db) const noexcept;

  CompressorSettings settings_;
  int channels_ = 2;
  double attack_coefficient_ = 0.0;
  double release_coefficient_ = 0.0;
  double makeup_ = 1.0;
  /// Envelope of applied gain reduction in dB, smoothed over time. This, not
  /// the input level, is what attack and release act on, which is what makes
  /// the release time behave the way a mixing engineer expects.
  double envelope_db_ = 0.0;
};

}  // namespace cutline::audio
