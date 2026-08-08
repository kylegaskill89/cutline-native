#pragma once

/// An echo: the signal repeated, fading.
///
/// The first effect in this layer with a *delay line* in it. Everything else
/// here — the filters, the compressor, the limiter — decides what to do with a
/// sample from that sample and a little state; this one needs to have kept the
/// last two seconds of audio, and that difference is why it is its own file
/// rather than another shape in `chain.cpp`.
///
/// Feedback is taken from the line **before** the new sample is written, so a
/// repeat feeds the next repeat rather than itself. Writing first and reading
/// afterwards is the classic way to get a delay that screams at a feedback of
/// one; this one decays as it should and is stable at any setting the registry
/// allows.

#include <cstddef>
#include <span>
#include <vector>

namespace cutline::audio {

struct DelaySettings {
  /// How far behind the echo is, in milliseconds.
  double time_ms = 250.0;
  /// How much of each repeat is fed back in, 0 to 1. At 0 there is one repeat.
  double feedback = 0.35;
  /// How much of the output is delayed signal, 0 to 1. At 0 the effect is
  /// inaudible, which is why the registry's default is not 0.
  double mix = 0.3;
};

/// The longest echo anybody gets. Two seconds is past what is musically useful
/// and short enough that the buffer costs a few hundred kilobytes a channel.
inline constexpr double kMaxDelayMs = 2000.0;

class Delay {
 public:
  Delay(DelaySettings settings, double sample_rate, int channels);

  /// Processes one interleaved block in place. Splitting a buffer across calls
  /// gives the same samples as processing it whole, which is what the mixer
  /// relies on everywhere else.
  void process(std::span<float> interleaved) noexcept;

  /// New settings, keeping the line. The audio already in it really did happen
  /// and is what the next repeat is made of; throwing it away on every retune
  /// would silence an animated delay rather than sweep it.
  void retune(DelaySettings settings, double sample_rate) noexcept;

  void reset() noexcept;

 private:
  DelaySettings settings_;
  double sample_rate_ = 48000.0;
  std::size_t channels_ = 2;

  /// One line per channel, each `capacity_` frames long, written round.
  std::vector<float> line_;
  std::size_t capacity_ = 0;
  std::size_t at_ = 0;
  /// How far back the read head sits, in frames. Held rather than recomputed so
  /// a retune cannot move it between two samples of one block.
  std::size_t behind_ = 0;
};

}  // namespace cutline::audio
