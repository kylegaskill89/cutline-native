#include "cutline/audio/music.hpp"

#include "fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <optional>

namespace cutline::audio {
namespace {

/// A magnitude spectrogram: `frames` rows of `bins` values.
struct Spectrogram {
  std::size_t size = 0;  ///< the transform's length
  std::size_t hop = 0;
  std::size_t bins = 0;
  std::size_t frames = 0;
  double rate = 0.0;
  std::vector<float> magnitude;

  [[nodiscard]] const float* frame(std::size_t t) const { return magnitude.data() + t * bins; }
  [[nodiscard]] double frequency(std::size_t k) const {
    return static_cast<double>(k) * rate / static_cast<double>(size);
  }
  /// Seconds at the middle of frame `t`'s window.
  [[nodiscard]] double time(std::size_t t) const {
    return (static_cast<double>(t * hop) + static_cast<double>(size) / 2.0) / rate;
  }
};

/// About 93 ms of window and an eighth of it as the hop, whatever the rate:
/// long enough to tell the pitch classes apart above the bass, and a hop
/// short enough that a beat is placed to within 12 ms before it is refined.
[[nodiscard]] Spectrogram spectrogram(std::span<const float> mono, int sample_rate) {
  Spectrogram s;
  s.rate = sample_rate;
  s.size = 1;
  while (static_cast<double>(s.size) < 0.093 * sample_rate) s.size <<= 1;
  s.hop = s.size / 8;
  s.bins = s.size / 2;
  if (mono.size() < s.size) return s;
  s.frames = (mono.size() - s.size) / s.hop + 1;
  s.magnitude.resize(s.frames * s.bins);

  std::vector<float> window(s.size);
  for (std::size_t i = 0; i < s.size; ++i) {
    window[i] = static_cast<float>(
        0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(s.size)));
  }
  std::vector<std::complex<float>> buffer(s.size);
  for (std::size_t t = 0; t < s.frames; ++t) {
    const float* in = mono.data() + t * s.hop;
    for (std::size_t i = 0; i < s.size; ++i) buffer[i] = {in[i] * window[i], 0.0f};
    detail::fft(buffer, false);
    float* out = s.magnitude.data() + t * s.bins;
    for (std::size_t k = 0; k < s.bins; ++k) out[k] = std::abs(buffer[k]);
  }
  return s;
}

/// Bin ranges for `count` bands spaced evenly in pitch from `low` to `high` Hz,
/// each at least one bin wide.
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> bands(const Spectrogram& s,
                                                                     int count, double low,
                                                                     double high) {
  high = std::min(high, s.rate / 2.0);
  std::vector<std::pair<std::size_t, std::size_t>> out;
  for (int b = 0; b < count; ++b) {
    const double f0 = low * std::pow(high / low, static_cast<double>(b) / count);
    const double f1 = low * std::pow(high / low, static_cast<double>(b + 1) / count);
    auto k0 = static_cast<std::size_t>(std::round(f0 * static_cast<double>(s.size) / s.rate));
    auto k1 = static_cast<std::size_t>(std::round(f1 * static_cast<double>(s.size) / s.rate));
    k0 = std::clamp<std::size_t>(k0, 1, s.bins - 1);
    k1 = std::clamp<std::size_t>(std::max(k1, k0 + 1), k0 + 1, s.bins);
    out.emplace_back(k0, k1);
  }
  return out;
}

/// How much is newly struck in each frame: the rise in loudness, band by band,
/// summed. Log-compressed first, so a quiet hi-hat counts beside a loud kick,
/// and with the slow swell of the arrangement taken out, so a crescendo is not
/// mistaken for a run of onsets.
[[nodiscard]] std::vector<double> onset_strength(const Spectrogram& s) {
  std::vector<double> onset(s.frames, 0.0);
  if (s.frames < 2) return onset;
  const auto ranges = bands(s, 40, 40.0, 11000.0);

  std::vector<float> energy(s.frames * ranges.size());
  float loudest = 0.0f;
  for (std::size_t t = 0; t < s.frames; ++t) {
    const float* m = s.frame(t);
    for (std::size_t b = 0; b < ranges.size(); ++b) {
      float sum = 0.0f;
      for (std::size_t k = ranges[b].first; k < ranges[b].second; ++k) sum += m[k] * m[k];
      energy[t * ranges.size() + b] = sum;
      loudest = std::max(loudest, sum);
    }
  }
  // Relative to the loudest moment, so the floor that keeps silence from
  // flickering is the same distance down in a quiet master as in a loud one.
  const float floor = std::max(loudest * 1e-5f, 1e-12f);
  for (float& e : energy) e = std::log1p(e / floor);

  for (std::size_t t = 1; t < s.frames; ++t) {
    double rise = 0.0;
    for (std::size_t b = 0; b < ranges.size(); ++b) {
      rise += std::max(0.0f, energy[t * ranges.size() + b] - energy[(t - 1) * ranges.size() + b]);
    }
    onset[t] = rise / static_cast<double>(ranges.size());
  }

  // Less its running mean over about 0.4 s, and never below nothing.
  const auto half = static_cast<std::size_t>(0.2 * s.rate / static_cast<double>(s.hop));
  std::vector<double> sums(onset.size() + 1, 0.0);
  std::partial_sum(onset.begin(), onset.end(), sums.begin() + 1);
  std::vector<double> out(onset.size());
  for (std::size_t t = 0; t < onset.size(); ++t) {
    const std::size_t a = t > half ? t - half : 0;
    const std::size_t b = std::min(onset.size(), t + half + 1);
    out[t] = std::max(0.0, onset[t] - (sums[b] - sums[a]) / static_cast<double>(b - a));
  }
  double square = 0.0;
  for (const double v : out) square += v * v;
  const double spread = std::sqrt(square / static_cast<double>(out.size()));
  if (spread > 0.0) {
    for (double& v : out) v /= spread;
  }
  return out;
}

/// The beat period in frames, fractional: the lag at which the onsets most
/// resemble themselves, between 50 and 200 beats a minute, weighed towards
/// 120 an octave either way. The weighing is what settles the old question of
/// whether a song is at 70 or at 140, the way a listener tapping along would.
///
/// No period at all when the onsets do not recur: hiss has onsets everywhere
/// and a beat nowhere, and a tracker asked to follow it finds one anyway.
[[nodiscard]] double beat_period(const std::vector<double>& onset, double frames_per_second) {
  const auto shortest = static_cast<std::size_t>(std::floor(frames_per_second * 60.0 / 200.0));
  const auto longest = static_cast<std::size_t>(std::ceil(frames_per_second * 60.0 / 50.0));
  if (onset.size() < longest * 2 || shortest < 2) return 0.0;

  // About the mean, so what is measured is the onsets' rhythm and not that
  // there are some.
  const double mean =
      std::accumulate(onset.begin(), onset.end(), 0.0) / static_cast<double>(onset.size());
  const auto correlate = [&](std::size_t lag) {
    double sum = 0.0;
    for (std::size_t t = lag; t < onset.size(); ++t) {
      sum += (onset[t] - mean) * (onset[t - lag] - mean);
    }
    return sum / static_cast<double>(onset.size() - lag);
  };
  const double whole = correlate(0);
  if (!(whole > 0.0)) return 0.0;
  std::vector<double> correlation(longest + 2, 0.0);
  for (std::size_t lag = shortest - 1; lag <= longest + 1; ++lag) correlation[lag] = correlate(lag);
  std::size_t best = 0;
  double best_score = 0.0;
  for (std::size_t lag = shortest; lag <= longest; ++lag) {
    const double bpm = frames_per_second * 60.0 / static_cast<double>(lag);
    const double octaves = std::log2(bpm / 120.0);
    const double score = correlation[lag] * std::exp(-0.5 * octaves * octaves);
    if (score > best_score) {
      best_score = score;
      best = lag;
    }
  }
  // A beat recurs strongly: a tenth of the onsets' whole energy again a beat
  // later is far more than chance gives and far less than any music with a
  // pulse does.
  constexpr double kPulse = 0.1;
  if (best == 0 || correlation[best] < kPulse * whole) return 0.0;
  // Between the frames, from the parabola through the peak and its neighbours:
  // a period rounded to whole frames is a twelfth of a beat out at 120, which
  // over a minute of song is enough to drift onto the off-beat.
  const double a = correlation[best - 1];
  const double b = correlation[best];
  const double c = correlation[best + 1];
  const double bend = a - 2.0 * b + c;
  const double shift = bend < 0.0 ? std::clamp(0.5 * (a - c) / bend, -0.5, 0.5) : 0.0;
  return static_cast<double>(best) + shift;
}

/// Ellis's beat tracker: the onsets nearest a grid of `period` frames, where
/// the grid may bend wherever the music leans on a beat late or early.
[[nodiscard]] std::vector<std::size_t> track_beats(const std::vector<double>& onset,
                                                   double period) {
  const std::size_t n = onset.size();
  // Smoothed a little, so a beat lands on a cluster of onsets rather than on
  // whichever single frame of it rose most.
  std::vector<double> local(n, 0.0);
  const int reach = static_cast<int>(std::ceil(period / 8.0));
  for (std::size_t t = 0; t < n; ++t) {
    double sum = 0.0;
    for (int k = -reach; k <= reach; ++k) {
      const auto at = static_cast<std::ptrdiff_t>(t) + k;
      if (at < 0 || at >= static_cast<std::ptrdiff_t>(n)) continue;
      const double x = k * 32.0 / period;
      sum += onset[static_cast<std::size_t>(at)] * std::exp(-0.5 * x * x);
    }
    local[t] = sum;
  }

  constexpr double kTightness = 100.0;
  std::vector<double> score(n, 0.0);
  std::vector<std::ptrdiff_t> back(n, -1);
  const double strongest = *std::ranges::max_element(local);
  bool started = false;
  for (std::size_t t = 0; t < n; ++t) {
    const auto from = static_cast<std::ptrdiff_t>(t) - static_cast<std::ptrdiff_t>(2.0 * period);
    const auto to = static_cast<std::ptrdiff_t>(t) - static_cast<std::ptrdiff_t>(period / 2.0);
    double best = -std::numeric_limits<double>::infinity();
    std::ptrdiff_t best_at = -1;
    for (std::ptrdiff_t tau = std::max<std::ptrdiff_t>(0, from); tau <= to; ++tau) {
      const double gap = std::log(static_cast<double>(static_cast<std::ptrdiff_t>(t) - tau) / period);
      const double v = score[static_cast<std::size_t>(tau)] - kTightness * gap * gap;
      if (v > best) {
        best = v;
        best_at = tau;
      }
    }
    // Nothing chains back until the music has begun, so a beat is not
    // invented in the silence before the first note.
    if (!started && local[t] > 0.01 * strongest) started = true;
    if (started && best_at >= 0) {
      score[t] = local[t] + best;
      back[t] = best_at;
    } else {
      score[t] = local[t];
    }
  }

  // The last beat is the last peak of the running score that stands up with
  // the rest, not the final frame, which in a fade-out is nothing.
  std::vector<double> peaks;
  for (std::size_t t = 1; t + 1 < n; ++t) {
    if (score[t] > score[t - 1] && score[t] >= score[t + 1]) peaks.push_back(score[t]);
  }
  if (peaks.empty()) return {};
  std::ranges::nth_element(peaks, peaks.begin() + static_cast<std::ptrdiff_t>(peaks.size() / 2));
  const double typical = peaks[peaks.size() / 2];
  std::ptrdiff_t last = -1;
  for (std::size_t t = n - 2; t >= 1; --t) {
    if (score[t] > score[t - 1] && score[t] >= score[t + 1] && score[t] > 0.5 * typical) {
      last = static_cast<std::ptrdiff_t>(t);
      break;
    }
  }
  std::vector<std::size_t> beats;
  for (std::ptrdiff_t t = last; t >= 0; t = back[static_cast<std::size_t>(t)]) {
    beats.push_back(static_cast<std::size_t>(t));
  }
  std::ranges::reverse(beats);

  // Weak beats at either end are the tracker carrying the grid into an intro
  // or a tail with nothing on it, so they are let go.
  if (beats.size() > 2) {
    double square = 0.0;
    for (const std::size_t t : beats) square += local[t] * local[t];
    const double level = 0.5 * std::sqrt(square / static_cast<double>(beats.size()));
    while (beats.size() > 2 && local[beats.front()] < level) beats.erase(beats.begin());
    while (beats.size() > 2 && local[beats.back()] < level) beats.pop_back();
  }
  return beats;
}

/// The exact moment of the attack near `around` seconds: where the 20 ms after
/// a point is loudest against the 20 ms before it, within `reach` of `around`.
///
/// A stretch either side rather than one short window against the next,
/// because a short window is shorter than a cycle of the bass, and a kick's
/// own waveform rose and fell from one to the next by more than the kick did
/// arriving.
[[nodiscard]] double attack_near(std::span<const float> mono, int sample_rate, double around,
                                 double reach) {
  const auto width = static_cast<std::ptrdiff_t>(0.02 * sample_rate);
  const auto step = std::max<std::ptrdiff_t>(1, sample_rate / 2000);
  const auto count = static_cast<std::ptrdiff_t>(mono.size());
  const auto at = [&](double seconds) {
    return static_cast<std::ptrdiff_t>(std::llround(seconds * sample_rate));
  };
  const std::ptrdiff_t lo = std::max(width, at(around - reach));
  const std::ptrdiff_t hi = std::min(count - width, at(around + reach));
  if (hi <= lo) return around;

  const std::ptrdiff_t first = lo - width;
  std::vector<double> sums(static_cast<std::size_t>(hi + width - first) + 1, 0.0);
  for (std::size_t i = 0; i + 1 < sums.size(); ++i) {
    const float v = mono[static_cast<std::size_t>(first) + i];
    sums[i + 1] = sums[i] + static_cast<double>(v) * v;
  }
  const auto energy = [&](std::ptrdiff_t from) {
    const auto a = static_cast<std::size_t>(from - first);
    return sums[a + static_cast<std::size_t>(width)] - sums[a];
  };
  // A floor far below anything heard, so silence after silence is no rise.
  const double floor = 1e-9 * static_cast<double>(width);
  double best = -std::numeric_limits<double>::infinity();
  double best_at = around;
  for (std::ptrdiff_t i = lo; i <= hi; i += step) {
    const double rise = std::log((energy(i) + floor) / (energy(i - width) + floor));
    if (rise > best) {
      best = rise;
      best_at = static_cast<double>(i) / sample_rate;
    }
  }
  return best_at;
}

[[nodiscard]] double distance(const std::array<float, 12>& a, const std::array<float, 12>& b) {
  double sum = 0.0;
  for (std::size_t i = 0; i < 12; ++i) {
    const double d = a[i] - b[i];
    sum += d * d;
  }
  return std::sqrt(sum);
}

[[nodiscard]] double unlikeness(const std::array<float, 12>& a, const std::array<float, 12>& b) {
  double dot = 0.0;
  for (std::size_t i = 0; i < 12; ++i) dot += static_cast<double>(a[i]) * b[i];
  return std::clamp(1.0 - dot, 0.0, 1.0);
}

[[nodiscard]] double median(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2));
  return values[values.size() / 2];
}

}  // namespace

MusicAnalysis analyse_music(std::span<const float> mono, int sample_rate) {
  MusicAnalysis out;
  if (sample_rate <= 0) return out;
  out.duration = static_cast<double>(mono.size()) / sample_rate;

  const Spectrogram s = spectrogram(mono, sample_rate);
  if (s.frames < 8) return out;
  const double frames_per_second = s.rate / static_cast<double>(s.hop);
  const std::vector<double> onset = onset_strength(s);
  const double period = beat_period(onset, frames_per_second);
  if (period <= 0.0) return out;
  out.tempo = frames_per_second * 60.0 / period;

  const std::vector<std::size_t> frames = track_beats(onset, period);
  // A frame's rise is felt once the attack is well into its window, which
  // weighs its leading edge least: three quarters of the way along is where
  // the search for the attack starts, a quarter of a beat either way: as far
  // as the tracker may have leant a beat towards its neighbours, and short of
  // the off-beat.
  const double reach = 0.5 * static_cast<double>(s.size) / s.rate;
  const double lag = 0.25 * static_cast<double>(s.size) / s.rate;
  for (const std::size_t t : frames) {
    out.beats.push_back(attack_near(mono, sample_rate, s.time(t) + lag, reach));
  }
  // Refinement can bring two beats at either side of a rest closer than they
  // were, but never past each other; if it did, the grid is kept.
  for (std::size_t i = 1; i < out.beats.size(); ++i) {
    if (out.beats[i] <= out.beats[i - 1] + 0.1 * period / frames_per_second) {
      out.beats[i] = std::max(out.beats[i], s.time(frames[i]));
    }
  }

  // What each beat sounds like, from the frames whose middles fall in it.
  const auto timbre_bands = bands(s, 12, 60.0, 10000.0);
  const double beat_seconds = period / frames_per_second;
  out.harmony.resize(out.beats.size());
  out.timbre.resize(out.beats.size());
  std::vector<double> bass(out.beats.size(), 0.0);
  for (std::size_t i = 0; i < out.beats.size(); ++i) {
    const double from = out.beats[i];
    const double to = i + 1 < out.beats.size() ? out.beats[i + 1] : from + beat_seconds;
    std::array<double, 12> chroma{};
    std::array<double, 12> energy{};
    int counted = 0;
    for (std::size_t t = 0; t < s.frames; ++t) {
      const double mid = s.time(t);
      if (mid < from) continue;
      if (mid >= to) break;
      ++counted;
      const float* m = s.frame(t);
      // Pitch classes from 150 Hz, below which a bin is wider than a semitone
      // and would smear the bass into every class near it.
      for (std::size_t k = 1; k < s.bins; ++k) {
        const double f = s.frequency(k);
        if (f < 150.0) continue;
        if (f > 4200.0) break;
        const auto semitone = static_cast<int>(std::lround(12.0 * std::log2(f / 440.0))) + 9;
        chroma[static_cast<std::size_t>(((semitone % 12) + 12) % 12)] += m[k];
      }
      for (std::size_t b = 0; b < timbre_bands.size(); ++b) {
        double sum = 0.0;
        for (std::size_t k = timbre_bands[b].first; k < timbre_bands[b].second; ++k) {
          sum += static_cast<double>(m[k]) * m[k];
        }
        energy[b] += sum / static_cast<double>(timbre_bands[b].second - timbre_bands[b].first);
      }
      if (t > 0) {
        // The bass's rise on this beat, which is where a bar's first beat
        // tends to be struck hardest.
        const float* p = s.frame(t - 1);
        for (std::size_t k = 1; k < s.bins && s.frequency(k) < 150.0; ++k) {
          bass[i] += std::max(0.0f, m[k] - p[k]);
        }
      }
    }
    double length = 0.0;
    for (const double c : chroma) length += c * c;
    length = std::sqrt(length);
    for (std::size_t c = 0; c < 12; ++c) {
      out.harmony[i][c] = length > 0.0 ? static_cast<float>(chroma[c] / length) : 0.0f;
      const double mean = counted > 0 ? energy[c] / counted : 0.0;
      out.timbre[i][c] = static_cast<float>(10.0 * std::log10(mean + 1e-10));
    }
  }

  // The bar line: the beat, of each four, that the harmony most often changes
  // on and the bass is struck hardest on. Both measures are scaled by their own
  // spread, so neither drowns the other.
  if (out.beats.size() >= kBeatsPerBar * 2) {
    std::vector<double> change(out.beats.size(), 0.0);
    for (std::size_t i = 1; i < out.beats.size(); ++i) {
      change[i] = unlikeness(out.harmony[i - 1], out.harmony[i]);
    }
    const auto spread = [](const std::vector<double>& v) {
      double mean = std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
      double sum = 0.0;
      for (const double x : v) sum += (x - mean) * (x - mean);
      return std::max(1e-9, std::sqrt(sum / static_cast<double>(v.size())));
    };
    const double change_spread = spread(change);
    const double bass_spread = spread(bass);
    double best = -std::numeric_limits<double>::infinity();
    for (int phase = 0; phase < kBeatsPerBar; ++phase) {
      double sum = 0.0;
      int count = 0;
      for (auto i = static_cast<std::size_t>(phase); i < out.beats.size(); i += kBeatsPerBar) {
        sum += change[i] / change_spread + bass[i] / bass_spread;
        ++count;
      }
      if (count > 0 && sum / count > best) {
        best = sum / count;
        out.bar_phase = phase;
      }
    }
  }
  return out;
}

namespace {

/// A cut from one bar to another, as the search sees it.
struct Jump {
  std::size_t to = 0;  ///< the grid point cut into
  double cost = 0.0;
};

/// The fallback when there is no beat to cut on: the song from its start,
/// stopped at the target or left whole.
[[nodiscard]] RemixPlan cut_short(const MusicAnalysis& analysis, const RemixRequest& request) {
  const double length = std::max(0.0, std::min({analysis.duration, request.target, request.maximum}));
  RemixPlan plan;
  if (length > 0.0) plan.spans.push_back({0.0, length});
  plan.length = length;
  return plan;
}

}  // namespace

RemixPlan plan_remix(const MusicAnalysis& analysis, const RemixRequest& request) {
  const double target = std::min(request.target, request.maximum);
  if (!(target > 0.0)) return {};
  if (!analysis.usable()) return cut_short(analysis, request);

  const std::size_t beat_count = analysis.beats.size();
  const double beat_seconds =
      (analysis.beats.back() - analysis.beats.front()) / static_cast<double>(beat_count - 1);
  // Within half a beat of the song as it is: nothing a cut could improve on.
  if (std::abs(target - analysis.duration) < beat_seconds / 2.0 &&
      analysis.duration <= request.maximum) {
    return {.spans = {{0.0, analysis.duration}}, .length = analysis.duration};
  }

  // The grid: the song's start, each beat, and its end. Point `b + 1` is beat
  // `b`, so a stretch from point p to point q is the song from one to the other.
  std::vector<double> grid;
  grid.push_back(0.0);
  for (const double b : analysis.beats) grid.push_back(std::clamp(b, 0.0, analysis.duration));
  grid.push_back(analysis.duration);
  const std::size_t points = grid.size();
  const std::size_t end = points - 1;
  const auto beat_of = [](std::size_t point) { return point - 1; };
  const auto downbeat = [&](std::size_t point) {
    return point >= 1 && point < end && analysis.is_downbeat(beat_of(point));
  };

  const double share = std::clamp(request.segments, 0, 100) / 100.0;
  const double harmonic = 0.2 + 0.6 * std::clamp(request.variations, 0, 100) / 100.0;
  // Fewer, longer stretches at 0: a cut costs a lot and four bars must play
  // between any two. Many short ones at 100: a cut is cheap and one bar will do.
  const auto bars_between = static_cast<std::size_t>(std::lround(4.0 - 3.0 * share));
  const std::size_t run = bars_between * kBeatsPerBar;
  const double cut_cost = 2.0 - 1.75 * share;
  constexpr double kPhraseCost = 0.5;
  constexpr double kPhrase = 4 * kBeatsPerBar;

  // How unlike two beats are, each measure scaled by how much it usually
  // changes from one beat to the next, so a typical step costs about one.
  std::vector<double> harmony_steps;
  std::vector<double> timbre_steps;
  for (std::size_t i = 1; i < beat_count; ++i) {
    harmony_steps.push_back(unlikeness(analysis.harmony[i - 1], analysis.harmony[i]));
    timbre_steps.push_back(distance(analysis.timbre[i - 1], analysis.timbre[i]));
  }
  const double harmony_scale = std::max(1e-3, median(harmony_steps));
  const double timbre_scale = std::max(1e-3, median(timbre_steps));
  const auto beat_distance = [&](std::size_t a, std::size_t b) {
    return harmonic * unlikeness(analysis.harmony[a], analysis.harmony[b]) / harmony_scale +
           (1.0 - harmonic) * distance(analysis.timbre[a], analysis.timbre[b]) / timbre_scale;
  };

  // Every cut worth considering from each downbeat: into another downbeat,
  // scored by how alike the bar before each and the bar after each are, which
  // is what makes the join sound like the bar that would have come next.
  std::vector<std::vector<Jump>> jumps(points);
  for (std::size_t from = 1; from < end; ++from) {
    if (!downbeat(from)) continue;
    // A first stretch of at least the run, from the first bar line.
    if (beat_of(from) < static_cast<std::size_t>(analysis.bar_phase) + run) continue;
    std::vector<Jump> options;
    for (std::size_t to = 1; to < end; ++to) {
      if (to == from || !downbeat(to)) continue;
      // And somewhere to land with a run still to play before the end.
      if (beat_of(to) + run >= beat_count) continue;
      double sum = 0.0;
      int count = 0;
      for (int k = -kBeatsPerBar; k < kBeatsPerBar; ++k) {
        const auto a = static_cast<std::ptrdiff_t>(beat_of(from)) + k;
        const auto b = static_cast<std::ptrdiff_t>(beat_of(to)) + k;
        if (a < 0 || b < 0 || a >= static_cast<std::ptrdiff_t>(beat_count) ||
            b >= static_cast<std::ptrdiff_t>(beat_count)) {
          continue;
        }
        sum += beat_distance(static_cast<std::size_t>(a), static_cast<std::size_t>(b));
        ++count;
      }
      if (count < kBeatsPerBar) continue;
      const auto apart = static_cast<double>(std::max(from, to) - std::min(from, to));
      const double phrase = std::fmod(apart, kPhrase) == 0.0 ? 0.0 : kPhraseCost;
      options.push_back({to, sum / count + phrase + cut_cost});
    }
    constexpr std::size_t kKept = 16;
    if (options.size() > kKept) {
      std::ranges::partial_sort(options, options.begin() + kKept, {}, &Jump::cost);
      options.resize(kKept);
    }
    jumps[from] = std::move(options);
  }

  // The search, over (grid point, time played so far), in steps of time small
  // enough to land within a beat and coarse enough to stay a few megabytes.
  constexpr double kSlack = 5.0;
  const double longest = std::min(request.maximum, target + kSlack);
  const double step = std::max(0.02, longest / 3000.0);
  const auto slots = static_cast<std::size_t>(std::floor(longest / step)) + 1;
  const auto slot_of = [&](double seconds) { return std::llround(seconds / step); };
  // Measured from the start of the song each time so the rounding does not
  // build up along a straight run.
  const auto span_slots = [&](std::size_t a, std::size_t b) {
    return static_cast<std::size_t>(std::max<long long>(0, slot_of(grid[b]) - slot_of(grid[a])));
  };

  constexpr float kUnreached = std::numeric_limits<float>::infinity();
  std::vector<float> cost(points * slots, kUnreached);
  std::vector<std::int32_t> back(points * slots, -1);
  const auto index = [&](std::size_t point, std::size_t slot) { return point * slots + slot; };
  cost[index(0, 0)] = 0.0f;

  for (std::size_t slot = 0; slot < slots; ++slot) {
    for (std::size_t point = 0; point < end; ++point) {
      const float here = cost[index(point, slot)];
      if (here == kUnreached) continue;
      const auto self = static_cast<std::int32_t>(index(point, slot));

      const std::size_t on = slot + span_slots(point, point + 1);
      if (on < slots && here < cost[index(point + 1, on)]) {
        cost[index(point + 1, on)] = here;
        back[index(point + 1, on)] = self;
      }
      for (const Jump& jump : jumps[point]) {
        // A cut is taken with the run after it, so the next cut cannot follow
        // sooner than Segments allows.
        const std::size_t land = jump.to + run;
        const std::size_t at = slot + span_slots(jump.to, land);
        if (at >= slots) continue;
        const float total = here + static_cast<float>(jump.cost);
        if (total < cost[index(land, at)]) {
          cost[index(land, at)] = total;
          back[index(land, at)] = self;
        }
      }
    }
  }

  // The best way to the end, where every second away from the target counts
  // as much as a cut does at the middle setting.
  std::ptrdiff_t best_slot = -1;
  double best = std::numeric_limits<double>::infinity();
  for (std::size_t slot = 0; slot < slots; ++slot) {
    const float c = cost[index(end, slot)];
    if (c == kUnreached) continue;
    const double length = static_cast<double>(slot) * step;
    if (length > request.maximum + step) continue;
    const double total = c + 1.0 * std::abs(length - target);
    if (total < best) {
      best = total;
      best_slot = static_cast<std::ptrdiff_t>(slot);
    }
  }
  if (best_slot < 0) return cut_short(analysis, request);

  std::vector<std::size_t> path;
  for (std::int32_t at = static_cast<std::int32_t>(index(end, static_cast<std::size_t>(best_slot)));
       at >= 0; at = back[static_cast<std::size_t>(at)]) {
    path.push_back(static_cast<std::size_t>(at) / slots);
  }
  std::ranges::reverse(path);

  // Read back as stretches: a step to the next point plays on; anything else
  // was a cut, from the point it left to the one `run` before where it landed.
  RemixPlan plan;
  double from = 0.0;
  for (std::size_t i = 1; i < path.size(); ++i) {
    if (path[i] == path[i - 1] + 1) continue;
    const std::size_t into = path[i] - run;
    plan.spans.push_back({from, grid[path[i - 1]]});
    from = grid[into];
  }
  plan.spans.push_back({from, analysis.duration});

  // Each join a little before its downbeat on both sides, so the cut is in the
  // gap before the attack. The same amount off both, so nothing changes length
  // but the first stretch, by that much, and the last, by as much again.
  for (std::size_t i = 1; i < plan.spans.size(); ++i) {
    plan.spans[i - 1].to = std::max(plan.spans[i - 1].from, plan.spans[i - 1].to - kRemixLead);
    plan.spans[i].from = std::max(0.0, plan.spans[i].from - kRemixLead);
  }
  for (const MusicSpan& span : plan.spans) plan.length += span.to - span.from;
  return plan;
}

}  // namespace cutline::audio
