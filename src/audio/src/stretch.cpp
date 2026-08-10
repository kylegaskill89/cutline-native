#include "cutline/audio/stretch.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cutline::audio {
namespace {

/// Half the window, so consecutive windows overlap by half. Two Hann windows at
/// that spacing sum to exactly one, which is what makes the overlap-add
/// transparent rather than rippling in amplitude.
constexpr std::size_t kSynthesisHop = kStretchWindow / 2;

/// The overlap region — also how far the similarity search may move a window.
/// Half a window is enough to find the matching phase of anything down to about
/// 45 Hz at 48 kHz, and searching further mostly finds a later period of the
/// same waveform for more work.
constexpr std::size_t kOverlap = kStretchWindow - kSynthesisHop;
constexpr std::size_t kSearch = kOverlap / 2;

/// A factor this close to 1 is not worth the artefacts of processing.
constexpr double kUnityEpsilon = 1e-3;

[[nodiscard]] std::vector<float> hann_window() {
  std::vector<float> window(kStretchWindow);
  for (std::size_t i = 0; i < kStretchWindow; ++i) {
    window[i] = static_cast<float>(
        0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(kStretchWindow)));
  }
  return window;
}

/// How well the input at `offset` continues `reference`, summed across
/// channels. Plain cross-correlation rather than a normalised one: the two
/// candidates being compared come from nearby parts of the same signal, so
/// their energies are close enough that normalising only costs a square root
/// per candidate.
[[nodiscard]] double similarity(std::span<const float> input, std::size_t offset,
                                std::span<const float> reference, std::size_t channels) {
  double sum = 0.0;
  const std::size_t count = reference.size();
  if (offset * channels + count > input.size()) return -1e30;

  for (std::size_t i = 0; i < count; ++i) {
    sum += static_cast<double>(input[offset * channels + i]) * reference[i];
  }
  return sum;
}

}  // namespace

std::vector<float> time_stretch(std::span<const float> interleaved, int channels,
                                double factor) {
  const auto lanes = static_cast<std::size_t>(std::max(channels, 1));
  const std::vector<float> unchanged(interleaved.begin(), interleaved.end());

  if (!(factor > 0.0) || std::abs(factor - 1.0) < kUnityEpsilon) return unchanged;
  if (interleaved.size() < kStretchWindow * lanes * 2) return unchanged;

  const std::size_t input_frames = interleaved.size() / lanes;
  const auto output_frames =
      static_cast<std::size_t>(static_cast<double>(input_frames) * factor);
  if (output_frames < kStretchWindow) return unchanged;

  // Input advances by this much per window; output always advances by the
  // synthesis hop. Their ratio is the whole of the time change.
  const double analysis_hop = static_cast<double>(kSynthesisHop) / factor;

  const std::vector<float> window = hann_window();

  // Padded so the final window can be written whole rather than clipped.
  std::vector<float> out((output_frames + kStretchWindow) * lanes, 0.0f);

  /// Sum of the window gains landing on each output frame. In the middle this
  /// is exactly 1, since two Hann halves at 50% overlap add to unity; at the
  /// first and last window only one contributes, so dividing by it is what
  /// stops a retimed clip from fading in and out over its own edges.
  std::vector<float> envelope(output_frames + kStretchWindow, 0.0f);

  /// The samples that naturally followed the window just written. The next
  /// window is chosen to look as much like these as possible.
  std::vector<float> expected(kOverlap * lanes, 0.0f);

  double analysis = 0.0;
  std::size_t written = 0;
  bool first = true;

  while (written < output_frames) {
    const auto centre = static_cast<std::size_t>(analysis);
    if (centre + kStretchWindow >= input_frames) break;

    std::size_t chosen = centre;
    if (!first) {
      // Search a window either side of where the timing says to read, and take
      // the position whose waveform best continues what is already down.
      const std::size_t lowest = centre > kSearch ? centre - kSearch : 0;
      const std::size_t highest =
          std::min(centre + kSearch, input_frames - kStretchWindow - 1);

      double best = -1e30;
      for (std::size_t candidate = lowest; candidate <= highest; ++candidate) {
        const double score = similarity(interleaved, candidate, expected, lanes);
        if (score > best) {
          best = score;
          chosen = candidate;
        }
      }
    }
    first = false;

    for (std::size_t i = 0; i < kStretchWindow; ++i) {
      const float gain = window[i];
      envelope[written + i] += gain;
      for (std::size_t c = 0; c < lanes; ++c) {
        out[(written + i) * lanes + c] +=
            interleaved[(chosen + i) * lanes + c] * gain;
      }
    }

    // What followed this window in the source, for the next search to match.
    const std::size_t tail = chosen + kSynthesisHop;
    for (std::size_t i = 0; i < kOverlap; ++i) {
      for (std::size_t c = 0; c < lanes; ++c) {
        const std::size_t index = (tail + i) * lanes + c;
        expected[i * lanes + c] = index < interleaved.size() ? interleaved[index] : 0.0f;
      }
    }

    analysis += analysis_hop;
    written += kSynthesisHop;
  }

  // Below this the window sum carries no real signal, so dividing by it would
  // amplify nothing into something.
  constexpr float kFloor = 1e-3f;
  for (std::size_t i = 0; i < output_frames; ++i) {
    if (envelope[i] <= kFloor) continue;
    for (std::size_t c = 0; c < lanes; ++c) out[i * lanes + c] /= envelope[i];
  }

  out.resize(output_frames * lanes);
  return out;
}

std::vector<float> resample_by(std::span<const float> interleaved, int channels, double factor) {
  const auto lanes = static_cast<std::size_t>(std::max(1, channels));
  if (interleaved.empty() || factor <= 0.0 || factor == 1.0) {
    return {interleaved.begin(), interleaved.end()};
  }

  const std::size_t frames = interleaved.size() / lanes;
  if (frames == 0) return {};

  const auto output_frames = static_cast<std::size_t>(
      std::llround(static_cast<double>(frames) * factor));
  if (output_frames == 0) return {};

  std::vector<float> out(output_frames * lanes, 0.0f);
  // The read position advances by 1/factor per output frame: slower than one
  // for a stretch, which is a conform to a lower rate and the drop in pitch
  // that goes with it.
  const double step = 1.0 / factor;

  for (std::size_t i = 0; i < output_frames; ++i) {
    const double at = static_cast<double>(i) * step;
    const auto whole = static_cast<std::size_t>(at);
    if (whole >= frames) break;

    const auto next = std::min(whole + 1, frames - 1);
    const auto fraction = static_cast<float>(at - static_cast<double>(whole));
    for (std::size_t c = 0; c < lanes; ++c) {
      const float a = interleaved[whole * lanes + c];
      const float b = interleaved[next * lanes + c];
      out[i * lanes + c] = a + (b - a) * fraction;
    }
  }

  return out;
}

}  // namespace cutline::audio
