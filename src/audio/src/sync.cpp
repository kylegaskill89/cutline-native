#include "cutline/audio/sync.hpp"

#include "fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <numeric>

namespace cutline::audio {
namespace {

using Complex = std::complex<double>;

using detail::fft;

}  // namespace

std::vector<float> onset_envelope(std::span<const float> mono, int sample_rate) {
  if (sample_rate <= 0 || mono.empty()) return {};
  const auto block = static_cast<std::size_t>(std::max(1, sample_rate / kSyncEnvelopeRate));
  const std::size_t count = mono.size() / block;
  std::vector<float> loudness(count);
  for (std::size_t b = 0; b < count; ++b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < block; ++i) {
      const double s = mono[b * block + i];
      sum += s * s;
    }
    // Compressed rather than linear, so a clap near one camera and far from
    // another still rises in both; rather than a plain logarithm, so the
    // flicker of a quiet room's noise floor does not count as events.
    loudness[b] = static_cast<float>(std::log1p(100.0 * std::sqrt(sum / block)));
  }
  std::vector<float> onsets(count, 0.0f);
  for (std::size_t b = 1; b < count; ++b) {
    onsets[b] = std::max(0.0f, loudness[b] - loudness[b - 1]);
  }
  if (count > 0) {
    const double mean = std::accumulate(onsets.begin(), onsets.end(), 0.0) / count;
    for (float& v : onsets) v -= static_cast<float>(mean);
  }
  return onsets;
}

std::optional<SyncMatch> match_envelopes(std::span<const float> reference,
                                         std::span<const float> other) {
  const std::size_t n = reference.size();
  const std::size_t m = other.size();
  if (n < 2 || m < 2) return std::nullopt;

  std::size_t size = 1;
  while (size < n + m) size <<= 1;
  std::vector<Complex> a(size);
  std::vector<Complex> b(size);
  for (std::size_t i = 0; i < n; ++i) a[i] = reference[i];
  for (std::size_t i = 0; i < m; ++i) b[i] = other[i];
  fft(a, false);
  fft(b, false);
  for (std::size_t i = 0; i < size; ++i) a[i] *= std::conj(b[i]);
  fft(a, true);

  // The agreement with `other` moved `lag` values along `reference`, for
  // every lag at which they overlap by enough to mean something: two seconds,
  // or all of the shorter one.
  const auto overlap_needed =
      static_cast<long long>(std::min<std::size_t>(2 * kSyncEnvelopeRate, std::min(n, m)));
  const auto first = -static_cast<long long>(m) + overlap_needed;
  const auto last = static_cast<long long>(n) - overlap_needed;
  if (first > last) return std::nullopt;
  const auto at = [&](long long lag) {
    const auto index = static_cast<std::size_t>(lag < 0 ? lag + static_cast<long long>(size) : lag);
    return a[index].real();
  };

  long long best = first;
  for (long long lag = first; lag <= last; ++lag) {
    if (at(lag) > at(best)) best = lag;
  }
  if (!(at(best) > 0.0)) return std::nullopt;

  // The next best that is not the same moment, 50 ms either side of it.
  constexpr long long kApart = kSyncEnvelopeRate / 20;
  double runner_up = 0.0;
  for (long long lag = first; lag <= last; ++lag) {
    if (std::abs(lag - best) > kApart) runner_up = std::max(runner_up, at(lag));
  }
  const double confidence = runner_up > 0.0 ? at(best) / runner_up : 100.0;
  if (confidence < 1.2) return std::nullopt;

  // Between envelope values, from the curve through the peak and its two
  // neighbours.
  double fraction = 0.0;
  if (best > first && best < last) {
    const double l = at(best - 1);
    const double c = at(best);
    const double r = at(best + 1);
    const double bend = l - 2.0 * c + r;
    if (bend < 0.0) fraction = std::clamp(0.5 * (l - r) / bend, -0.5, 0.5);
  }
  return SyncMatch{.offset = (static_cast<double>(best) + fraction) / kSyncEnvelopeRate,
                   .confidence = confidence};
}

}  // namespace cutline::audio
