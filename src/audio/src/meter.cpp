#include "cutline/audio/meter.hpp"

#include "cutline/audio/biquad.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::audio {
namespace {

/// A level in decibels, floored rather than allowed to run to negative
/// infinity as a true zero would.
[[nodiscard]] double level_db(double linear) noexcept {
  if (!(linear > 0.0)) return kMeterFloorDb;
  return std::max(kMeterFloorDb, linear_to_db(linear));
}

/// The per-sample multiplier that falls `db_per_second` decibels a second.
[[nodiscard]] double fall_per_sample(double db_per_second, double rate) noexcept {
  if (rate <= 0.0 || db_per_second <= 0.0) return 1.0;
  return std::pow(10.0, -db_per_second / (20.0 * rate));
}

}  // namespace

Meter::Meter(double sample_rate, int channels, MeterSettings settings) noexcept
    : settings_(settings),
      channels_(std::clamp(static_cast<std::size_t>(std::max(0, channels)),
                           std::size_t{1}, kMaxMeterChannels)),
      rate_(sample_rate > 0.0 ? sample_rate : 48000.0) {
  fall_ = fall_per_sample(settings_.fall_db_per_second, rate_);
  hold_fall_ = fall_per_sample(settings_.hold_fall_db_per_second, rate_);

  // One-pole averaging. The window is a time constant rather than a boxcar,
  // which costs one multiply a sample instead of a buffer of history.
  rms_alpha_ = settings_.rms_seconds > 0.0
                   ? 1.0 - std::exp(-1.0 / (rate_ * settings_.rms_seconds))
                   : 1.0;
}

void Meter::process(std::span<const float> interleaved) noexcept {
  if (channels_ == 0) return;
  const std::size_t frames = interleaved.size() / channels_;
  const double step = 1.0 / rate_;

  for (std::size_t i = 0; i < frames; ++i) {
    for (std::size_t c = 0; c < channels_; ++c) {
      State& s = state_[c];
      const double sample = std::abs(static_cast<double>(interleaved[i * channels_ + c]));

      // Rise instantly, fall gradually. A meter that smoothed its rise would
      // under-read exactly the short peaks it exists to catch.
      s.peak = std::max(sample, s.peak * fall_);

      if (s.peak >= s.hold) {
        s.hold = s.peak;
        s.held_for = 0.0;
      } else if (s.held_for < settings_.hold_seconds) {
        s.held_for += step;
      } else {
        s.hold *= hold_fall_;
      }

      s.mean_square += (sample * sample - s.mean_square) * rms_alpha_;
      if (sample >= settings_.over) s.over = true;
    }
  }
}

MeterReading Meter::read() const noexcept {
  MeterReading reading;
  reading.count = static_cast<int>(channels_);
  for (std::size_t c = 0; c < channels_; ++c) {
    const State& s = state_[c];
    reading.channels[c] = ChannelLevel{
        .peak_db = level_db(s.peak),
        .rms_db = level_db(std::sqrt(s.mean_square)),
        .hold_db = level_db(s.hold),
        .over = s.over,
    };
  }
  return reading;
}

void Meter::reset() noexcept { state_.fill(State{}); }

double meter_fraction(double db, double floor_db, double ceiling_db) noexcept {
  if (!(ceiling_db > floor_db)) return 0.0;
  return std::clamp((db - floor_db) / (ceiling_db - floor_db), 0.0, 1.0);
}

}  // namespace cutline::audio
