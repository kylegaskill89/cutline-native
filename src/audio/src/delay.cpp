#include "cutline/audio/delay.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::audio {
namespace {

[[nodiscard]] std::size_t frames_for(double ms, double sample_rate) noexcept {
  const double frames = std::max(0.0, ms) * 0.001 * sample_rate;
  return static_cast<std::size_t>(std::llround(frames));
}

}  // namespace

Delay::Delay(DelaySettings settings, double sample_rate, int channels)
    : settings_(settings),
      sample_rate_(sample_rate > 0.0 ? sample_rate : 48000.0),
      channels_(static_cast<std::size_t>(std::max(channels, 1))) {
  // Sized for the longest echo the registry allows rather than for the one
  // asked for, so a retune to a longer time does not have to reallocate on the
  // mixing thread — which is the one thread that must not.
  capacity_ = std::max<std::size_t>(1, frames_for(kMaxDelayMs, sample_rate_) + 1);
  line_.assign(capacity_ * channels_, 0.0f);
  behind_ = std::clamp<std::size_t>(frames_for(settings_.time_ms, sample_rate_), 1,
                                    capacity_ - 1);
}

void Delay::retune(DelaySettings settings, double sample_rate) noexcept {
  settings_ = settings;
  if (sample_rate > 0.0) sample_rate_ = sample_rate;
  behind_ = std::clamp<std::size_t>(frames_for(settings_.time_ms, sample_rate_), 1,
                                    capacity_ == 0 ? 1 : capacity_ - 1);
}

void Delay::reset() noexcept {
  std::ranges::fill(line_, 0.0f);
  at_ = 0;
}

void Delay::process(std::span<float> interleaved) noexcept {
  if (capacity_ == 0 || channels_ == 0 || line_.empty()) return;

  const float feedback = static_cast<float>(std::clamp(settings_.feedback, 0.0, 0.95));
  const float wet = static_cast<float>(std::clamp(settings_.mix, 0.0, 1.0));
  const float dry = 1.0f - wet;

  const std::size_t frames = interleaved.size() / channels_;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    // Read before write, so a repeat feeds the *next* repeat rather than
    // itself. The other order is how a delay screams at a feedback of one.
    const std::size_t read = (at_ + capacity_ - behind_) % capacity_;

    for (std::size_t c = 0; c < channels_; ++c) {
      const std::size_t at = frame * channels_ + c;
      const float in = interleaved[at];
      const float echoed = line_[read * channels_ + c];

      line_[at_ * channels_ + c] = in + echoed * feedback;
      interleaved[at] = in * dry + echoed * wet;
    }

    at_ = (at_ + 1) % capacity_;
  }
}

}  // namespace cutline::audio
