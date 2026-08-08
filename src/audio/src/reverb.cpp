#include "cutline/audio/reverb.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::audio {
namespace {

/// Freeverb's comb and allpass lengths, in frames at 44.1 kHz.
///
/// Kept as the published numbers and scaled to whatever rate the mixer runs at,
/// rather than re-derived. They are mutually inharmonic, which is the whole
/// trick: lengths sharing a factor ring together at their common frequency and
/// give a room with a note in it.
constexpr std::array<std::size_t, 8> kCombLengths{1116, 1188, 1277, 1356,
                                                  1422, 1491, 1557, 1617};
constexpr std::array<std::size_t, 4> kAllpassLengths{556, 441, 341, 225};

/// How far a channel's lines are offset from the first channel's, in frames.
/// Two channels running identical lines are one channel heard twice; a stereo
/// reverb needs its sides to be different rooms of the same size.
constexpr std::size_t kStereoSpread = 23;

constexpr double kReferenceRate = 44100.0;

/// Allpass feedback. Freeverb's, and it is not a control: it sets how much the
/// phase is scrambled, and every value but this one sounds worse.
constexpr float kAllpassFeedback = 0.5f;

/// Keeps the tail from running for ever at size 1, and keeps the whole thing
/// stable. Freeverb's scaling of its room-size control.
constexpr double kMaxFeedback = 0.98;
constexpr double kFeedbackOffset = 0.7;

[[nodiscard]] std::size_t scaled(std::size_t frames, double sample_rate, std::size_t spread) {
  const double at_rate = static_cast<double>(frames + spread) * sample_rate / kReferenceRate;
  return std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(at_rate)));
}

}  // namespace

float Reverb::Comb::process(float in, float feedback, float damp) noexcept {
  if (line.empty()) return in;
  const float out = line[at];
  // A one-pole low-pass in the feedback path: each time round the loop the top
  // end is rolled off a little more, so the tail darkens as it decays the way a
  // real room's does.
  store = out * (1.0f - damp) + store * damp;
  line[at] = in + store * feedback;
  at = (at + 1) % line.size();
  return out;
}

void Reverb::Comb::reset() noexcept {
  std::ranges::fill(line, 0.0f);
  at = 0;
  store = 0.0f;
}

float Reverb::Allpass::process(float in) noexcept {
  if (line.empty()) return in;
  const float held = line[at];
  const float out = held - in;
  line[at] = in + held * kAllpassFeedback;
  at = (at + 1) % line.size();
  return out;
}

void Reverb::Allpass::reset() noexcept {
  std::ranges::fill(line, 0.0f);
  at = 0;
}

Reverb::Reverb(ReverbSettings settings, double sample_rate, int channels)
    : settings_(settings),
      sample_rate_(sample_rate > 0.0 ? sample_rate : kReferenceRate),
      channels_(static_cast<std::size_t>(std::max(channels, 1))) {
  voices_.resize(channels_);
  for (std::size_t c = 0; c < channels_; ++c) {
    const std::size_t spread = c * kStereoSpread;
    for (std::size_t i = 0; i < kCombs; ++i) {
      voices_[c].combs[i].line.assign(scaled(kCombLengths[i], sample_rate_, spread), 0.0f);
    }
    for (std::size_t i = 0; i < kAllpasses; ++i) {
      voices_[c].allpasses[i].line.assign(scaled(kAllpassLengths[i], sample_rate_, spread), 0.0f);
    }
  }
}

void Reverb::retune(ReverbSettings settings, double sample_rate) noexcept {
  // Settings only. The lines are sized for the rate they were built at and
  // resizing them here would allocate on the mixing thread — and the audio in
  // them really did happen, whatever the room has since become.
  settings_ = settings;
  (void)sample_rate;
}

void Reverb::reset() noexcept {
  for (Channel& voice : voices_) {
    for (Comb& comb : voice.combs) comb.reset();
    for (Allpass& allpass : voice.allpasses) allpass.reset();
  }
}

void Reverb::process(std::span<float> interleaved) noexcept {
  if (channels_ == 0 || voices_.empty()) return;

  const auto feedback = static_cast<float>(
      std::clamp(settings_.size, 0.0, 1.0) * (kMaxFeedback - kFeedbackOffset) + kFeedbackOffset);
  const auto damp = static_cast<float>(std::clamp(settings_.damping, 0.0, 1.0) * 0.4);
  const float wet = static_cast<float>(std::clamp(settings_.mix, 0.0, 1.0));
  const float dry = 1.0f - wet;

  // The combs run in parallel and are summed, so eight of them are eight times
  // as loud as one. Scaled back here rather than in each comb, where it would
  // be eight multiplications instead of one.
  constexpr float kCombScale = 1.0f / static_cast<float>(kCombs);

  const std::size_t frames = interleaved.size() / channels_;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (std::size_t c = 0; c < channels_; ++c) {
      const std::size_t at = frame * channels_ + c;
      const float in = interleaved[at];

      float sum = 0.0f;
      for (Comb& comb : voices_[c].combs) sum += comb.process(in, feedback, damp);
      sum *= kCombScale;

      // Then in series, each smearing what the last one left.
      for (Allpass& allpass : voices_[c].allpasses) sum = allpass.process(sum);

      interleaved[at] = in * dry + sum * wet;
    }
  }
}

}  // namespace cutline::audio
