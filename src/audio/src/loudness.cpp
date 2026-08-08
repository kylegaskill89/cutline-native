#include "cutline/audio/loudness.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cutline::audio {
namespace {

/// BS.1770's K-weighting, as the two filters it is defined by.
///
/// The standard publishes coefficients at 48 kHz. These are the *shapes* those
/// coefficients describe, built at whatever rate is running — which is what
/// makes a measurement at 44.1 kHz agree with one at 48 rather than being
/// silently a little wrong.
constexpr double kShelfFreq = 1681.97;
constexpr double kShelfGainDb = 3.9999;
constexpr double kShelfQ = 0.7071752;
constexpr double kHighPassFreq = 38.135;
constexpr double kHighPassQ = 0.5003271;

/// The offset that makes a full-scale sine read where the standard says.
constexpr double kLoudnessOffset = -0.691;

/// 400 ms measured every 100 ms: three quarters of each block overlaps the
/// last, which is what stops a loud moment falling between two of them.
constexpr double kBlockSeconds = 0.4;
constexpr double kHopSeconds = 0.1;

/// The weight each channel carries. Left and right count for one; the surround
/// channels count for more, because a sound behind you is harder to hear and
/// has to be louder to be as loud. Beyond five channels the standard says
/// nothing and one is the honest guess.
[[nodiscard]] double channel_weight(std::size_t channel) noexcept {
  return channel == 3 || channel == 4 ? 1.41 : 1.0;
}

[[nodiscard]] double loudness_of(double weighted_power) noexcept {
  if (!(weighted_power > 0.0)) return -std::numeric_limits<double>::infinity();
  return kLoudnessOffset + 10.0 * std::log10(weighted_power);
}

}  // namespace

LoudnessMeter::LoudnessMeter(double sample_rate, int channels)
    : sample_rate_(sample_rate > 0.0 ? sample_rate : 48000.0),
      channels_(static_cast<std::size_t>(std::max(channels, 1))) {
  shelf_.assign(channels_, Biquad::high_shelf(sample_rate_, kShelfFreq, kShelfGainDb, kShelfQ));
  highpass_.assign(channels_, Biquad::high_pass(sample_rate_, kHighPassFreq, kHighPassQ));

  window_frames_ = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(kBlockSeconds * sample_rate_)));
  hop_frames_ = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::llround(kHopSeconds * sample_rate_)));
  window_.assign(window_frames_ * channels_, 0.0);
}

void LoudnessMeter::reset() noexcept {
  for (Biquad& f : shelf_) f.reset();
  for (Biquad& f : highpass_) f.reset();
  std::ranges::fill(window_, 0.0);
  at_ = 0;
  filled_ = 0;
  since_hop_ = 0;
  loudness_.clear();
  power_.clear();
}

void LoudnessMeter::take_block() noexcept {
  // The mean square of each channel across the window, weighted and summed.
  double weighted = 0.0;
  for (std::size_t c = 0; c < channels_; ++c) {
    double sum = 0.0;
    for (std::size_t i = 0; i < window_frames_; ++i) sum += window_[i * channels_ + c];
    weighted += channel_weight(c) * (sum / static_cast<double>(window_frames_));
  }

  const double level = loudness_of(weighted);
  // Silence is not a quiet block, it is no block. Keeping it would drag the
  // ungated average down and move the relative gate with it.
  if (!std::isfinite(level)) return;
  loudness_.push_back(level);
  power_.push_back(weighted);
}

void LoudnessMeter::process(std::span<const float> interleaved) noexcept {
  if (channels_ == 0 || window_.empty()) return;
  const std::size_t frames = interleaved.size() / channels_;

  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (std::size_t c = 0; c < channels_; ++c) {
      // K-weighted first: the shelf, then the high-pass, in that order.
      const double filtered =
          highpass_[c].process(shelf_[c].process(interleaved[frame * channels_ + c]));
      window_[at_ * channels_ + c] = filtered * filtered;
    }

    at_ = (at_ + 1) % window_frames_;
    if (filled_ < window_frames_) ++filled_;

    // A block every hop, once there is a whole window to take one from.
    ++since_hop_;
    if (since_hop_ >= hop_frames_ && filled_ >= window_frames_) {
      since_hop_ = 0;
      take_block();
    }
  }
}

double LoudnessMeter::integrated_lufs() const noexcept {
  if (loudness_.empty()) return kAbsoluteGateLufs;

  // The absolute gate: silence is thrown away outright.
  double sum = 0.0;
  std::size_t counted = 0;
  for (std::size_t i = 0; i < loudness_.size(); ++i) {
    if (loudness_[i] <= kAbsoluteGateLufs) continue;
    sum += power_[i];
    ++counted;
  }
  if (counted == 0) return kAbsoluteGateLufs;

  // Then the relative one, ten below what is left. This is what stops a
  // programme's number being decided by how much room tone it has.
  const double ungated = loudness_of(sum / static_cast<double>(counted));
  const double gate = ungated - kRelativeGateLu;

  double kept = 0.0;
  std::size_t keeping = 0;
  for (std::size_t i = 0; i < loudness_.size(); ++i) {
    if (loudness_[i] <= kAbsoluteGateLufs || loudness_[i] <= gate) continue;
    kept += power_[i];
    ++keeping;
  }
  if (keeping == 0) return ungated;

  return loudness_of(kept / static_cast<double>(keeping));
}

double loudness_offset_db(double measured, double target) noexcept {
  // Nothing to move. A programme that measured as silence has no loudness to
  // correct, and amplifying it by the difference would turn its noise floor
  // into the programme.
  if (!std::isfinite(measured) || measured <= kAbsoluteGateLufs) return 0.0;
  if (!std::isfinite(target)) return 0.0;
  return target - measured;
}

}  // namespace cutline::audio
