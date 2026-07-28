#include "cutline/audio/limiter.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::audio {

Limiter::Limiter(LimiterSettings settings, double sample_rate, int channels)
    : settings_(settings), channels_(static_cast<std::size_t>(std::max(channels, 1))) {
  settings_.limit = std::clamp(settings_.limit, 1e-4, 1.0);

  if (sample_rate > 0.0 && settings_.attack_ms > 0.0) {
    lookahead_ = static_cast<std::size_t>(sample_rate * settings_.attack_ms / 1000.0);
  }
  lookahead_ = std::max<std::size_t>(lookahead_, 1);
  delay_.assign(lookahead_ * channels_, 0.0f);

  // Gain falls linearly across the look-ahead, so it arrives at the value a
  // peak needs exactly as that peak emerges -- no sooner, which would duck
  // audio that did not need it, and no later, which would let the peak through.
  attack_step_ = 1.0 / static_cast<double>(lookahead_);

  if (sample_rate > 0.0 && settings_.release_ms > 0.0) {
    release_coefficient_ = std::exp(-1.0 / (sample_rate * settings_.release_ms / 1000.0));
  }
}

std::size_t Limiter::latency_frames() const noexcept { return lookahead_; }

void Limiter::push_requirement(double gain) noexcept {
  // Keep the deque increasing from the front, so the front is always the
  // smallest gain still ahead of us. Anything larger behind a new smaller
  // requirement can never be the window minimum again.
  while (!pending_.empty() && pending_.back().second >= gain) pending_.pop_back();
  pending_.emplace_back(position_, gain);
}

float Limiter::advance(float sample, std::size_t channel) noexcept {
  const std::size_t slot = write_ * channels_ + channel;
  const float delayed = delay_[slot];
  delay_[slot] = sample;
  return delayed;
}

void Limiter::process(std::span<float> interleaved) noexcept {
  if (channels_ == 0 || interleaved.size() < channels_) return;
  const std::size_t frames = interleaved.size() / channels_;

  for (std::size_t frame = 0; frame < frames; ++frame) {
    const std::size_t base = frame * channels_;

    // The gain this incoming frame will need when it emerges. Channels are
    // linked: one gain for the frame, from its loudest channel, so limiting
    // never shifts the stereo image.
    double peak = 0.0;
    for (std::size_t c = 0; c < channels_; ++c) {
      peak = std::max(peak, static_cast<double>(std::abs(interleaved[base + c])));
    }
    push_requirement(peak > settings_.limit ? settings_.limit / peak : 1.0);

    // Retire requirements that have already passed out of the window.
    while (!pending_.empty() && pending_.front().first + lookahead_ <= position_) {
      pending_.pop_front();
    }
    const double required = pending_.empty() ? 1.0 : pending_.front().second;

    if (required < gain_) {
      gain_ = std::max(required, gain_ - attack_step_);
    } else {
      gain_ = required + release_coefficient_ * (gain_ - required);
    }

    // Read the delayed frame out, then clamp against what it actually needs.
    // The smoothing above should already have arrived, but this makes the
    // ceiling a guarantee rather than a tendency.
    double emerging = 0.0;
    for (std::size_t c = 0; c < channels_; ++c) {
      interleaved[base + c] = advance(interleaved[base + c], c);
      emerging = std::max(emerging, static_cast<double>(std::abs(interleaved[base + c])));
    }

    double applied = gain_;
    if (emerging * applied > settings_.limit) applied = settings_.limit / emerging;
    for (std::size_t c = 0; c < channels_; ++c) {
      interleaved[base + c] = static_cast<float>(interleaved[base + c] * applied);
    }

    write_ = (write_ + 1) % lookahead_;
    ++position_;
  }
}

void Limiter::flush(std::span<float> out) noexcept {
  // Feeding silence in pushes the remaining audio out, which is exactly what a
  // flush is.
  std::ranges::fill(out, 0.0f);
  process(out);
}

void Limiter::reset() noexcept {
  std::ranges::fill(delay_, 0.0f);
  pending_.clear();
  write_ = 0;
  position_ = 0;
  gain_ = 1.0;
}

}  // namespace cutline::audio
