#include "cutline/audio/compressor.hpp"

#include "cutline/audio/biquad.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::audio {
namespace {

/// One-pole smoothing coefficient for a time constant in milliseconds. A zero
/// or negative time means "instant", which is coefficient zero.
[[nodiscard]] double coefficient_for(double milliseconds, double sample_rate) noexcept {
  if (!(milliseconds > 0.0) || !(sample_rate > 0.0)) return 0.0;
  return std::exp(-1.0 / (sample_rate * milliseconds / 1000.0));
}

}  // namespace

Compressor::Compressor(CompressorSettings settings, double sample_rate, int channels) noexcept
    : settings_(settings), channels_(std::max(channels, 1)) {
  attack_coefficient_ = coefficient_for(settings_.attack_ms, sample_rate);
  release_coefficient_ = coefficient_for(settings_.release_ms, sample_rate);
  makeup_ = db_to_linear(settings_.makeup_db);
  settings_.ratio = std::max(settings_.ratio, 1.0);
  settings_.knee_db = std::max(settings_.knee_db, 0.0);
}

bool Compressor::transparent() const noexcept {
  return settings_.ratio <= 1.0 && settings_.makeup_db == 0.0;
}

double Compressor::curve_db(double input_db) const noexcept {
  // Distance above the threshold, which is where all the behaviour lives.
  const double over = input_db - settings_.threshold_db;
  const double half_knee = settings_.knee_db / 2.0;

  if (over <= -half_knee) return 0.0;  // below the knee: untouched

  const double slope = 1.0 / settings_.ratio - 1.0;  // negative: dB removed per dB over
  if (over >= half_knee || settings_.knee_db == 0.0) return slope * over;

  // Inside the knee, interpolate quadratically so the curve and its slope are
  // both continuous at each end. A discontinuous slope is what makes a hard
  // knee click on material sitting at the threshold.
  const double knee = over + half_knee;
  return slope * knee * knee / (2.0 * settings_.knee_db);
}

void Compressor::process(std::span<float> interleaved) noexcept {
  if (transparent()) return;

  const auto channels = static_cast<std::size_t>(channels_);
  if (channels == 0 || interleaved.size() < channels) return;

  const std::size_t frames = interleaved.size() / channels;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    const std::size_t base = frame * channels;

    // Linked detection: the loudest channel decides for all of them.
    double peak = 0.0;
    for (std::size_t c = 0; c < channels; ++c) {
      peak = std::max(peak, static_cast<double>(std::abs(interleaved[base + c])));
    }

    const double target_db = curve_db(linear_to_db(peak));

    // Attack when reduction is deepening, release when it is easing off.
    // Comparing targets rather than levels is what keeps a brief dip in a loud
    // passage from re-triggering the attack.
    const double coefficient =
        target_db < envelope_db_ ? attack_coefficient_ : release_coefficient_;
    envelope_db_ = target_db + coefficient * (envelope_db_ - target_db);

    const auto gain = static_cast<float>(db_to_linear(envelope_db_) * makeup_);
    for (std::size_t c = 0; c < channels; ++c) interleaved[base + c] *= gain;
  }
}

void Compressor::reset() noexcept { envelope_db_ = 0.0; }

double Compressor::reduction_db() const noexcept { return envelope_db_; }

}  // namespace cutline::audio
