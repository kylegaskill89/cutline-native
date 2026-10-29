#pragma once

/// Premiere's dialogue repair: DeNoise, DeReverb, DeHummer, DeEsser and the
/// high-pass behind Essential Sound's Reduce Rumble.
///
/// Classical signal processing rather than a trained model, which is what
/// Premiere's own effects of these names are; Enhance Speech, the model, is a
/// different thing. Each processes interleaved blocks in place with its state
/// carried across calls, like every other stage of a clip's effect stack, so a
/// buffer split in two gives the same samples as the whole.
///
/// DeNoise and DeReverb work on the spectrum, a frame at a time, and so hand
/// back each sample a fixed number of frames after it went in: `latency()`.
/// What runs them is expected to feed them that far ahead, which the mixer
/// does, so the sound stays where the picture is.

#include "cutline/audio/biquad.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace cutline::audio {

namespace detail {
class Spectral;
}

struct DenoiseSettings {
  /// How hard, from 0 (nothing) to 1. Decides both how far below the noise a
  /// sound has to be to be taken out and how far it is taken down.
  double amount = 0.4;
  /// Output gain in dB, since taking the noise out leaves the voice sounding
  /// quieter than it is.
  double gain_db = 0.0;
};

/// Adaptive noise reduction. The noise is learned as it plays, from the
/// quietest each frequency gets, so steady noise — a fan, air conditioning,
/// a camera's own hiss — is found without being pointed at, and taken out
/// where the voice is not above it.
class Denoiser {
 public:
  Denoiser(DenoiseSettings settings, double sample_rate, int channels);
  ~Denoiser();
  Denoiser(Denoiser&&) noexcept;
  Denoiser& operator=(Denoiser&&) noexcept;

  void process(std::span<float> interleaved) noexcept;
  void retune(DenoiseSettings settings) noexcept;
  void reset() noexcept;
  [[nodiscard]] std::size_t latency() const noexcept;

 private:
  struct State;
  DenoiseSettings settings_;
  std::unique_ptr<detail::Spectral> spectral_;
  std::unique_ptr<State> state_;
};

struct DereverbSettings {
  /// How hard, from 0 to 1.
  double amount = 0.4;
  double gain_db = 0.0;
};

/// Late reverberation taken down. A room's tail is a decaying copy of what
/// was said a moment ago, so its strength at each frequency is predicted from
/// what that frequency held a little earlier, and what is no louder than the
/// prediction is reduced.
class Dereverberator {
 public:
  Dereverberator(DereverbSettings settings, double sample_rate, int channels);
  ~Dereverberator();
  Dereverberator(Dereverberator&&) noexcept;
  Dereverberator& operator=(Dereverberator&&) noexcept;

  void process(std::span<float> interleaved) noexcept;
  void retune(DereverbSettings settings) noexcept;
  void reset() noexcept;
  [[nodiscard]] std::size_t latency() const noexcept;

 private:
  struct State;
  DereverbSettings settings_;
  std::unique_ptr<detail::Spectral> spectral_;
  std::unique_ptr<State> state_;
};

struct DehumSettings {
  /// The mains frequency, 50 or 60 Hz, and how many of its harmonics.
  double frequency = 60.0;
  int harmonics = 4;
  /// How far each is cut, in dB.
  double reduction_db = 30.0;
  /// How narrow each cut is. Narrow enough to leave a voice's own low notes.
  double q = 30.0;
};

/// Mains hum taken out: a narrow cut at the frequency and each harmonic.
class Dehummer {
 public:
  Dehummer(DehumSettings settings, double sample_rate, int channels);
  void process(std::span<float> interleaved) noexcept;
  void retune(DehumSettings settings) noexcept;
  void reset() noexcept;

 private:
  [[nodiscard]] std::vector<Biquad> shapes() const;
  DehumSettings settings_;
  double sample_rate_;
  std::size_t channels_;
  /// One cut per harmonic per channel, the channel's cuts together.
  std::vector<Biquad> cuts_;
};

struct DeessSettings {
  /// Level of the sibilant band above which it is reduced, in dBFS.
  double threshold_db = -30.0;
  /// Where the sibilance is: the split between the band that is reduced and
  /// the rest, which is left alone.
  double frequency = 6000.0;
  /// The most it is reduced by, in dB.
  double reduction_db = 12.0;
};

/// Harsh "s" sounds reduced: the band above `frequency` is turned down only
/// while it is loud, so the rest of the voice and the quiet top of it are not
/// dulled.
class Deesser {
 public:
  Deesser(DeessSettings settings, double sample_rate, int channels);
  void process(std::span<float> interleaved) noexcept;
  void retune(DeessSettings settings) noexcept;
  void reset() noexcept;
  /// The reduction applied to the band just now, in dB, zero or negative.
  [[nodiscard]] double reduction_db() const noexcept { return -applied_db_; }

 private:
  void shape();
  DeessSettings settings_;
  double sample_rate_;
  std::size_t channels_;
  /// A high shelf per channel, turned down as far as the band is too loud,
  /// and a high-pass on the channels' sum that listens to the band.
  std::vector<Biquad> shelves_;
  Biquad listen_ = Biquad::identity();
  std::size_t since_ = 0;
  double envelope_ = 0.0;
  double attack_ = 0.0;
  double release_ = 0.0;
  double applied_db_ = 0.0;
};

/// Essential Sound's Reduce Rumble: a steep high-pass whose corner rises with
/// `amount`, from 40 Hz to 120 Hz, below which a voice has nothing to lose.
class RumbleFilter {
 public:
  RumbleFilter(double amount, double sample_rate, int channels);
  void process(std::span<float> interleaved) noexcept;
  void retune(double amount) noexcept;
  void reset() noexcept;
  [[nodiscard]] static double corner(double amount) noexcept;

 private:
  [[nodiscard]] std::vector<Biquad> shapes() const;
  double amount_;
  double sample_rate_;
  std::size_t channels_;
  /// Two Butterworth sections per channel, making a fourth-order slope.
  std::vector<Biquad> sections_;
};

}  // namespace cutline::audio
