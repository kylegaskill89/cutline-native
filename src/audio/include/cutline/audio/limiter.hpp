#pragma once

/// A look-ahead peak limiter for the master bus.
///
/// Mixing is a plain sum — the reference used `amix` with `normalize=0`, so
/// four tracks at unity really are four times as loud — which means the mix can
/// exceed full scale whenever several loud clips land together. Something has
/// to catch that, and hard clipping is the one option that sounds like a fault
/// rather than like mixing.
///
/// Look-ahead is what separates a limiter from a fast compressor: the gain is
/// pulled down *before* the peak arrives rather than after it, so the attack
/// does not let the transient through. That costs a delay equal to the
/// look-ahead, which the mixer accounts for by flushing at the end.

#include <cstddef>
#include <deque>
#include <span>
#include <vector>

namespace cutline::audio {

struct LimiterSettings {
  /// Peak ceiling in linear amplitude. 0.95 leaves a little headroom for the
  /// overshoot that lossy encoding can add, and is what the reference used.
  double limit = 0.95;
  /// Look-ahead and gain-fall time, in milliseconds.
  double attack_ms = 5.0;
  /// How quickly gain returns to unity once the peak has passed. Too fast
  /// sounds like pumping; too slow ducks material that follows a transient.
  double release_ms = 50.0;
};

class Limiter {
 public:
  Limiter(LimiterSettings settings, double sample_rate, int channels);

  /// Limits one interleaved block in place.
  ///
  /// Output lags input by the look-ahead, so the first `latency_frames()`
  /// frames out are silence and the last block of input is still inside when
  /// the caller runs out. `flush` retrieves it.
  void process(std::span<float> interleaved) noexcept;

  /// Drains the look-ahead buffer into `out`, which should hold
  /// `latency_frames()` frames. Without this the tail of a timeline is lost.
  void flush(std::span<float> out) noexcept;

  /// Frames of delay the look-ahead introduces.
  [[nodiscard]] std::size_t latency_frames() const noexcept;

  void reset() noexcept;

 private:
  [[nodiscard]] float advance(float sample, std::size_t channel) noexcept;
  void push_requirement(double gain) noexcept;

  LimiterSettings settings_;
  std::size_t channels_ = 2;
  std::size_t lookahead_ = 0;
  double release_coefficient_ = 0.0;
  double attack_step_ = 1.0;

  /// Interleaved ring of the samples still inside the look-ahead.
  std::vector<float> delay_;
  std::size_t write_ = 0;

  /// Monotonic queue of the gain each pending frame will require, so the
  /// minimum over the window is available in constant time rather than by
  /// rescanning it every sample.
  std::deque<std::pair<std::size_t, double>> pending_;
  std::size_t position_ = 0;

  double gain_ = 1.0;
};

}  // namespace cutline::audio
