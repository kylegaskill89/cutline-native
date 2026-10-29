#include "cutline/audio/repair.hpp"

#include "fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace cutline::audio {

// --------------------------------------------------------------- spectral --

namespace detail {

/// Overlapping frames, each windowed, transformed, reshaped and put back.
///
/// Frames of about 40 ms, which is long enough to tell a voice's harmonics
/// from the noise between them and short enough that a word's onset is not
/// smeared ahead of itself; four to a frame's length, with a square-root Hann
/// window going in and coming out, which sums back to exactly what went in
/// when nothing is changed. Every channel is shaped by one call, so an effect
/// can decide its gains from all of them together and keep a stereo image
/// still.
class Spectral {
 public:
  using Complex = std::complex<float>;

  Spectral(double sample_rate, int channels)
      : channels_(static_cast<std::size_t>(std::max(channels, 1))) {
    size_ = 256;
    while (static_cast<double>(size_) < sample_rate * 0.04) size_ <<= 1;
    hop_ = size_ / 4;
    window_.resize(size_);
    for (std::size_t n = 0; n < size_; ++n) {
      window_[n] = static_cast<float>(
          std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * n / size_)));
    }
    in_.assign(channels_, std::vector<float>(size_, 0.0f));
    out_.assign(channels_, std::vector<float>(size_, 0.0f));
    spectra_.assign(channels_, std::vector<Complex>(size_));
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t hop() const noexcept { return hop_; }
  [[nodiscard]] std::size_t bins() const noexcept { return size_ / 2 + 1; }
  [[nodiscard]] std::size_t latency() const noexcept { return size_; }
  [[nodiscard]] std::size_t channels() const noexcept { return channels_; }

  /// `shape` is given every channel's spectrum and changes bins 0 to N/2;
  /// the mirror half is made to agree afterwards, so the result is real.
  template <typename Shape>
  void process(std::span<float> interleaved, Shape&& shape) {
    const std::size_t frames = interleaved.size() / channels_;
    for (std::size_t f = 0; f < frames; ++f) {
      for (std::size_t c = 0; c < channels_; ++c) {
        float& sample = interleaved[f * channels_ + c];
        in_[c][size_ - hop_ + position_] = sample;
        sample = out_[c][position_];
      }
      if (++position_ == hop_) {
        position_ = 0;
        frame(shape);
      }
    }
  }

  void reset() noexcept {
    for (std::vector<float>& v : in_) std::ranges::fill(v, 0.0f);
    for (std::vector<float>& v : out_) std::ranges::fill(v, 0.0f);
    position_ = 0;
  }

 private:
  template <typename Shape>
  void frame(Shape&& shape) {
    for (std::size_t c = 0; c < channels_; ++c) {
      for (std::size_t n = 0; n < size_; ++n) spectra_[c][n] = Complex(in_[c][n] * window_[n], 0);
      fft(spectra_[c], false);
    }
    shape(spectra_);
    // Four frames overlap at every sample, and the two windows multiply to a
    // Hann, whose overlapped sum is two.
    constexpr float kOverlap = 0.5f;
    for (std::size_t c = 0; c < channels_; ++c) {
      std::vector<Complex>& bins = spectra_[c];
      for (std::size_t k = 1; k < size_ / 2; ++k) bins[size_ - k] = std::conj(bins[k]);
      fft(bins, true);
      std::vector<float>& out = out_[c];
      std::copy(out.begin() + static_cast<std::ptrdiff_t>(hop_), out.end(), out.begin());
      std::fill(out.end() - static_cast<std::ptrdiff_t>(hop_), out.end(), 0.0f);
      for (std::size_t n = 0; n < size_; ++n) out[n] += bins[n].real() * window_[n] * kOverlap;
      std::vector<float>& in = in_[c];
      std::copy(in.begin() + static_cast<std::ptrdiff_t>(hop_), in.end(), in.begin());
    }
  }

  std::size_t channels_;
  std::size_t size_ = 0;
  std::size_t hop_ = 0;
  std::size_t position_ = 0;
  std::vector<float> window_;
  std::vector<std::vector<float>> in_;
  std::vector<std::vector<float>> out_;
  std::vector<std::vector<Complex>> spectra_;
};

}  // namespace detail

namespace {

/// Each channel's power at a bin, averaged, so the gains are decided once for
/// all of them.
[[nodiscard]] double power_at(const std::vector<std::vector<std::complex<float>>>& spectra,
                              std::size_t k) noexcept {
  double sum = 0.0;
  for (const auto& channel : spectra) sum += std::norm(channel[k]);
  return sum / static_cast<double>(spectra.size());
}

/// A gain curve softened across neighbouring bins, which is most of what
/// keeps an isolated bin from flickering on and off — the "musical noise"
/// that cheap noise reduction is known by.
void soften(std::vector<double>& gains, std::vector<double>& scratch) {
  scratch = gains;
  for (std::size_t k = 1; k + 1 < gains.size(); ++k) {
    gains[k] = 0.25 * scratch[k - 1] + 0.5 * scratch[k] + 0.25 * scratch[k + 1];
  }
}

void apply(std::vector<std::vector<std::complex<float>>>& spectra, const std::vector<double>& gains,
           double scale) {
  for (auto& channel : spectra) {
    for (std::size_t k = 0; k < gains.size(); ++k) {
      channel[k] *= static_cast<float>(gains[k] * scale);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- denoise --

/// How fast the noise estimate is allowed to climb, in dB a second. Slow
/// enough that a sustained vowel is not mistaken for noise, fast enough that
/// noise which really did get louder is followed within a few seconds.
constexpr double kNoiseRiseDbPerSecond = 4.0;
/// The quietest a bin gets sits below its noise's average; this is how far.
constexpr double kMinimumBias = 2.5;
/// How much of the last frame's clean estimate the next one starts from:
/// the decision-directed weighting, which trades a little response for far
/// less flicker.
constexpr double kDecisionDirected = 0.98;

struct Denoiser::State {
  std::vector<double> smooth;
  std::vector<double> noise;
  std::vector<double> clean;
  std::vector<double> gains;
  std::vector<double> scratch;
  double rise = 1.0;
  bool started = false;
};

Denoiser::Denoiser(DenoiseSettings settings, double sample_rate, int channels)
    : settings_(settings),
      spectral_(std::make_unique<detail::Spectral>(sample_rate, channels)),
      state_(std::make_unique<State>()) {
  const std::size_t bins = spectral_->bins();
  state_->smooth.assign(bins, 0.0);
  state_->noise.assign(bins, 0.0);
  state_->clean.assign(bins, 0.0);
  state_->gains.assign(bins, 1.0);
  state_->rise = std::pow(10.0, kNoiseRiseDbPerSecond * static_cast<double>(spectral_->hop()) /
                                    sample_rate / 10.0);
}

Denoiser::~Denoiser() = default;
Denoiser::Denoiser(Denoiser&&) noexcept = default;
Denoiser& Denoiser::operator=(Denoiser&&) noexcept = default;

void Denoiser::retune(DenoiseSettings settings) noexcept { settings_ = settings; }

std::size_t Denoiser::latency() const noexcept { return spectral_->latency(); }

void Denoiser::reset() noexcept {
  spectral_->reset();
  std::ranges::fill(state_->clean, 0.0);
  std::ranges::fill(state_->gains, 1.0);
  // The noise learned is kept: it is a fact about the recording, and a jump
  // in playback is no reason to forget it and let the first second through.
}

void Denoiser::process(std::span<float> interleaved) noexcept {
  const double amount = std::clamp(settings_.amount, 0.0, 1.0);
  const double floor = std::pow(10.0, -amount * 30.0 / 20.0);
  const double over = 1.0 + amount;
  const double scale = std::pow(10.0, settings_.gain_db / 20.0);
  State& s = *state_;
  spectral_->process(interleaved, [&](auto& spectra) {
    for (std::size_t k = 0; k < s.gains.size(); ++k) {
      const double power = power_at(spectra, k);
      if (!s.started) {
        s.smooth[k] = power;
        s.noise[k] = power;
      } else {
        s.smooth[k] = 0.85 * s.smooth[k] + 0.15 * power;
        s.noise[k] = s.smooth[k] < s.noise[k] ? s.smooth[k] : s.noise[k] * s.rise;
      }
      const double noise = std::max(s.noise[k] * kMinimumBias * over, 1e-20);
      const double posterior = power / noise;
      const double prior = kDecisionDirected * s.clean[k] / noise +
                           (1.0 - kDecisionDirected) * std::max(posterior - 1.0, 0.0);
      const double gain = std::max(prior / (1.0 + prior), floor);
      s.gains[k] = gain;
      s.clean[k] = gain * gain * power;
    }
    s.started = true;
    soften(s.gains, s.scratch);
    apply(spectra, s.gains, scale);
  });
}

// ---------------------------------------------------------------- dereverb --

/// When the late tail begins, after the sound that caused it: the early
/// reflections before this are part of how a voice sounds in a room and are
/// left alone.
constexpr double kTailDelay = 0.05;
/// The room the prediction assumes, in seconds to fall 60 dB. A real one is
/// not measured; `amount` says how much of what the prediction finds is taken.
constexpr double kAssumedDecay = 0.8;

struct Dereverberator::State {
  std::vector<std::vector<double>> history;
  std::size_t at = 0;
  std::vector<double> smooth;
  std::vector<double> gains;
  std::vector<double> scratch;
  double fall = 0.0;
};

Dereverberator::Dereverberator(DereverbSettings settings, double sample_rate, int channels)
    : settings_(settings),
      spectral_(std::make_unique<detail::Spectral>(sample_rate, channels)),
      state_(std::make_unique<State>()) {
  const std::size_t bins = spectral_->bins();
  const double hop_seconds = static_cast<double>(spectral_->hop()) / sample_rate;
  const auto delay = std::max<std::size_t>(1, static_cast<std::size_t>(
                                                  std::lround(kTailDelay / hop_seconds)));
  state_->history.assign(delay, std::vector<double>(bins, 0.0));
  state_->smooth.assign(bins, 0.0);
  state_->gains.assign(bins, 1.0);
  // A power that falls 60 dB in the assumed decay, over the delay.
  state_->fall = std::pow(10.0, -6.0 * static_cast<double>(delay) * hop_seconds / kAssumedDecay);
}

Dereverberator::~Dereverberator() = default;
Dereverberator::Dereverberator(Dereverberator&&) noexcept = default;
Dereverberator& Dereverberator::operator=(Dereverberator&&) noexcept = default;

void Dereverberator::retune(DereverbSettings settings) noexcept { settings_ = settings; }

std::size_t Dereverberator::latency() const noexcept { return spectral_->latency(); }

void Dereverberator::reset() noexcept {
  spectral_->reset();
  for (std::vector<double>& h : state_->history) std::ranges::fill(h, 0.0);
  std::ranges::fill(state_->smooth, 0.0);
  std::ranges::fill(state_->gains, 1.0);
}

void Dereverberator::process(std::span<float> interleaved) noexcept {
  const double amount = std::clamp(settings_.amount, 0.0, 1.0);
  const double floor = std::pow(10.0, -amount * 18.0 / 10.0);  // in power
  const double strength = amount * 2.0;
  const double scale = std::pow(10.0, settings_.gain_db / 20.0);
  State& s = *state_;
  spectral_->process(interleaved, [&](auto& spectra) {
    std::vector<double>& earlier = s.history[s.at];
    for (std::size_t k = 0; k < s.gains.size(); ++k) {
      const double power = power_at(spectra, k);
      s.smooth[k] = 0.5 * s.smooth[k] + 0.5 * power;
      const double tail = s.fall * earlier[k];
      const double kept = std::max(floor, 1.0 - strength * tail / std::max(power, 1e-20));
      // Let go more slowly than it grips, so the gain does not flutter
      // through a decaying syllable.
      s.gains[k] = std::max(std::sqrt(kept), 0.6 * s.gains[k]);
      earlier[k] = s.smooth[k];
    }
    s.at = (s.at + 1) % s.history.size();
    soften(s.gains, s.scratch);
    apply(spectra, s.gains, scale);
  });
}

// ------------------------------------------------------------------ dehum --

Dehummer::Dehummer(DehumSettings settings, double sample_rate, int channels)
    : settings_(settings),
      sample_rate_(sample_rate),
      channels_(static_cast<std::size_t>(std::max(channels, 1))) {
  const std::vector<Biquad> one = shapes();
  for (std::size_t c = 0; c < channels_; ++c) cuts_.insert(cuts_.end(), one.begin(), one.end());
}

std::vector<Biquad> Dehummer::shapes() const {
  std::vector<Biquad> out;
  const int harmonics = std::clamp(settings_.harmonics, 1, 16);
  for (int h = 1; h <= harmonics; ++h) {
    const double at = settings_.frequency * h;
    if (at >= sample_rate_ * 0.45) break;
    out.push_back(Biquad::peaking(sample_rate_, at, -std::max(0.0, settings_.reduction_db),
                                  std::max(1.0, settings_.q)));
  }
  return out;
}

void Dehummer::process(std::span<float> interleaved) noexcept {
  if (cuts_.empty()) return;
  const std::size_t per = cuts_.size() / channels_;
  const std::size_t frames = interleaved.size() / channels_;
  for (std::size_t f = 0; f < frames; ++f) {
    for (std::size_t c = 0; c < channels_; ++c) {
      float& sample = interleaved[f * channels_ + c];
      for (std::size_t h = 0; h < per; ++h) sample = cuts_[c * per + h].process(sample);
    }
  }
}

void Dehummer::retune(DehumSettings settings) noexcept {
  const bool same_count = settings.harmonics == settings_.harmonics &&
                          settings.frequency == settings_.frequency;
  settings_ = settings;
  const std::vector<Biquad> one = shapes();
  if (!same_count || one.size() * channels_ != cuts_.size()) {
    cuts_.clear();
    for (std::size_t c = 0; c < channels_; ++c) cuts_.insert(cuts_.end(), one.begin(), one.end());
    return;
  }
  for (std::size_t c = 0; c < channels_; ++c) {
    for (std::size_t h = 0; h < one.size(); ++h) cuts_[c * one.size() + h].retune(one[h]);
  }
}

void Dehummer::reset() noexcept {
  for (Biquad& cut : cuts_) cut.reset();
}

// ------------------------------------------------------------------- deess --

Deesser::Deesser(DeessSettings settings, double sample_rate, int channels)
    : settings_(settings),
      sample_rate_(sample_rate),
      channels_(static_cast<std::size_t>(std::max(channels, 1))) {
  attack_ = 1.0 - std::exp(-1.0 / (0.0015 * sample_rate));
  release_ = 1.0 - std::exp(-1.0 / (0.08 * sample_rate));
  shelves_.assign(channels_, Biquad::identity());
  shape();
}

/// How often the shelf follows the envelope, in frames: often enough that it
/// moves with an "s" two milliseconds in, seldom enough that its coefficients
/// are not worked out every sample.
constexpr std::size_t kDeessStep = 32;

void Deesser::shape() {
  const double at = std::clamp(settings_.frequency, 1000.0, sample_rate_ * 0.4);
  listen_.retune(Biquad::high_pass(sample_rate_, at));
  const Biquad shelf = Biquad::high_shelf(sample_rate_, at, -applied_db_);
  for (Biquad& b : shelves_) b.retune(shelf);
}

void Deesser::retune(DeessSettings settings) noexcept {
  settings_ = settings;
  shape();
}

void Deesser::reset() noexcept {
  for (Biquad& b : shelves_) b.reset();
  listen_.reset();
  envelope_ = 0.0;
  applied_db_ = 0.0;
  since_ = 0;
  shape();
}

void Deesser::process(std::span<float> interleaved) noexcept {
  const double most = std::max(0.0, settings_.reduction_db);
  const std::size_t frames = interleaved.size() / channels_;
  for (std::size_t f = 0; f < frames; ++f) {
    float sum = 0.0f;
    for (std::size_t c = 0; c < channels_; ++c) sum += interleaved[f * channels_ + c];
    const double band = listen_.process(sum / static_cast<float>(channels_));
    const double power = band * band;
    envelope_ += (power > envelope_ ? attack_ : release_) * (power - envelope_);
    // As a sine's level, so a threshold reads as the peak a meter shows.
    const double level_db = 10.0 * std::log10(std::max(envelope_ * 2.0, 1e-12));
    // A shelf rather than a band split and summed back: a split's two halves
    // are out of phase near the corner, so turning one down does not take
    // that much away, where a shelf cuts by what it is told above it.
    if (++since_ >= kDeessStep) {
      since_ = 0;
      const double wanted = std::clamp((level_db - settings_.threshold_db) * 0.75, 0.0, most);
      if (std::abs(wanted - applied_db_) > 0.05) {
        applied_db_ = wanted;
        shape();
      }
    }
    for (std::size_t c = 0; c < channels_; ++c) {
      float& sample = interleaved[f * channels_ + c];
      sample = shelves_[c].process(sample);
    }
  }
}

// ------------------------------------------------------------------ rumble --

RumbleFilter::RumbleFilter(double amount, double sample_rate, int channels)
    : amount_(amount),
      sample_rate_(sample_rate),
      channels_(static_cast<std::size_t>(std::max(channels, 1))) {
  const std::vector<Biquad> one = shapes();
  for (std::size_t c = 0; c < channels_; ++c) sections_.insert(sections_.end(), one.begin(), one.end());
}

double RumbleFilter::corner(double amount) noexcept {
  return 40.0 + 80.0 * std::clamp(amount, 0.0, 1.0);
}

std::vector<Biquad> RumbleFilter::shapes() const {
  // The two quality factors of a fourth-order Butterworth.
  const double at = corner(amount_);
  return {Biquad::high_pass(sample_rate_, at, 0.5412), Biquad::high_pass(sample_rate_, at, 1.3066)};
}

void RumbleFilter::process(std::span<float> interleaved) noexcept {
  const std::size_t frames = interleaved.size() / channels_;
  for (std::size_t f = 0; f < frames; ++f) {
    for (std::size_t c = 0; c < channels_; ++c) {
      float& sample = interleaved[f * channels_ + c];
      sample = sections_[c * 2].process(sample);
      sample = sections_[c * 2 + 1].process(sample);
    }
  }
}

void RumbleFilter::retune(double amount) noexcept {
  amount_ = amount;
  const std::vector<Biquad> one = shapes();
  for (std::size_t c = 0; c < channels_; ++c) {
    sections_[c * 2].retune(one[0]);
    sections_[c * 2 + 1].retune(one[1]);
  }
}

void RumbleFilter::reset() noexcept {
  for (Biquad& b : sections_) b.reset();
}

}  // namespace cutline::audio
