#pragma once

/// A room around the sound.
///
/// Schroeder's arrangement in the form Freeverb settled on: eight comb filters
/// in parallel, whose sum goes through four allpass filters in series. The
/// combs make the density and the tail; the allpasses smear what would
/// otherwise be an obvious series of echoes into something that sounds like a
/// space rather than a corridor.
///
/// Chosen over a convolution reverb because it needs no impulse response to
/// ship, no FFT, and no file for somebody to lose — and because a room is a
/// thing you dial rather than a thing you pick, which is what a size control is
/// and what a list of hall names is not. Premiere ships both; this is the one
/// that earns its place first.
///
/// The comb lengths are prime-ish and mutually inharmonic on purpose: lengths
/// that share factors ring together at their common frequency, and the result
/// is a room with a note in it.

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace cutline::audio {

struct ReverbSettings {
  /// How big the room is, 0 to 1. It is the combs' feedback, so it is also how
  /// long the tail runs.
  double size = 0.5;
  /// How much the walls absorb the top end, 0 to 1. At 0 the tail keeps its
  /// brightness, which is a tiled bathroom; at 1 it darkens as it decays,
  /// which is nearly everything else.
  double damping = 0.5;
  /// How much of the output is reverb, 0 to 1.
  double mix = 0.25;
};

class Reverb {
 public:
  Reverb(ReverbSettings settings, double sample_rate, int channels);

  void process(std::span<float> interleaved) noexcept;
  /// New settings, keeping the tail. What is in the lines really did happen.
  void retune(ReverbSettings settings, double sample_rate) noexcept;
  void reset() noexcept;

 private:
  /// One comb: a delay line fed back through a one-pole low-pass, which is what
  /// makes the tail darken as it decays.
  struct Comb {
    std::vector<float> line;
    std::size_t at = 0;
    float store = 0.0f;

    [[nodiscard]] float process(float in, float feedback, float damp) noexcept;
    void reset() noexcept;
  };

  /// One allpass: flat in magnitude, and it is the phase it scrambles that
  /// turns a row of echoes into a room.
  struct Allpass {
    std::vector<float> line;
    std::size_t at = 0;

    [[nodiscard]] float process(float in) noexcept;
    void reset() noexcept;
  };

  static constexpr std::size_t kCombs = 8;
  static constexpr std::size_t kAllpasses = 4;

  struct Channel {
    std::array<Comb, kCombs> combs;
    std::array<Allpass, kAllpasses> allpasses;
  };

  ReverbSettings settings_;
  double sample_rate_ = 48000.0;
  std::size_t channels_ = 2;
  std::vector<Channel> voices_;
};

}  // namespace cutline::audio
