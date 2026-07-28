#include "cutline/audio/biquad.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace cutline::audio {
namespace {

/// Whether a filter can be built at all. A cutoff at or above Nyquist has no
/// meaning, and a non-positive Q divides by zero in `alpha`.
[[nodiscard]] bool usable(double sample_rate, double freq, double q) noexcept {
  return std::isfinite(sample_rate) && std::isfinite(freq) && std::isfinite(q) &&
         sample_rate > 0.0 && freq > 0.0 && q > 0.0 && freq < sample_rate * 0.5;
}

struct Coefficients {
  double b0, b1, b2, a0, a1, a2;
};

/// Divides through by a0 and packs the result. Kept in one place because
/// forgetting the normalisation is the classic way to get a filter that is
/// almost right.
[[nodiscard]] Coefficients normalise(Coefficients c) noexcept {
  if (c.a0 == 0.0 || !std::isfinite(c.a0)) return {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
  return {c.b0 / c.a0, c.b1 / c.a0, c.b2 / c.a0, 1.0, c.a1 / c.a0, c.a2 / c.a0};
}

}  // namespace

double db_to_linear(double db) noexcept { return std::pow(10.0, db / 20.0); }

double linear_to_db(double linear) noexcept {
  // Floored rather than -infinity: a level meter wants a number it can draw.
  constexpr double kSilence = 1e-9;
  return 20.0 * std::log10(std::max(linear, kSilence));
}

Biquad Biquad::identity() noexcept { return Biquad{}; }

// The cookbook proper. `w0` is the cutoff in radians per sample and `alpha`
// sets the bandwidth; every section below is those two plus a gain term.

Biquad Biquad::low_pass(double sample_rate, double freq, double q) noexcept {
  if (!usable(sample_rate, freq, q)) return identity();
  const double w0 = 2.0 * std::numbers::pi * freq / sample_rate;
  const double cos_w0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);

  const auto c = normalise({(1.0 - cos_w0) / 2.0, 1.0 - cos_w0, (1.0 - cos_w0) / 2.0,
                            1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha});
  Biquad f;
  f.b0_ = c.b0, f.b1_ = c.b1, f.b2_ = c.b2, f.a1_ = c.a1, f.a2_ = c.a2;
  return f;
}

Biquad Biquad::high_pass(double sample_rate, double freq, double q) noexcept {
  if (!usable(sample_rate, freq, q)) return identity();
  const double w0 = 2.0 * std::numbers::pi * freq / sample_rate;
  const double cos_w0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);

  const auto c = normalise({(1.0 + cos_w0) / 2.0, -(1.0 + cos_w0), (1.0 + cos_w0) / 2.0,
                            1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha});
  Biquad f;
  f.b0_ = c.b0, f.b1_ = c.b1, f.b2_ = c.b2, f.a1_ = c.a1, f.a2_ = c.a2;
  return f;
}

Biquad Biquad::low_shelf(double sample_rate, double freq, double gain_db, double q) noexcept {
  if (!usable(sample_rate, freq, q)) return identity();
  // Shelves use the square root of the gain, because the shelf is built from a
  // pole and a zero that each carry half of it.
  const double a = db_to_linear(gain_db / 2.0);
  const double w0 = 2.0 * std::numbers::pi * freq / sample_rate;
  const double cos_w0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double beta = 2.0 * std::sqrt(a) * alpha;

  const auto c = normalise({a * ((a + 1.0) - (a - 1.0) * cos_w0 + beta),
                            2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0),
                            a * ((a + 1.0) - (a - 1.0) * cos_w0 - beta),
                            (a + 1.0) + (a - 1.0) * cos_w0 + beta,
                            -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0),
                            (a + 1.0) + (a - 1.0) * cos_w0 - beta});
  Biquad f;
  f.b0_ = c.b0, f.b1_ = c.b1, f.b2_ = c.b2, f.a1_ = c.a1, f.a2_ = c.a2;
  return f;
}

Biquad Biquad::high_shelf(double sample_rate, double freq, double gain_db, double q) noexcept {
  if (!usable(sample_rate, freq, q)) return identity();
  const double a = db_to_linear(gain_db / 2.0);
  const double w0 = 2.0 * std::numbers::pi * freq / sample_rate;
  const double cos_w0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);
  const double beta = 2.0 * std::sqrt(a) * alpha;

  const auto c = normalise({a * ((a + 1.0) + (a - 1.0) * cos_w0 + beta),
                            -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0),
                            a * ((a + 1.0) + (a - 1.0) * cos_w0 - beta),
                            (a + 1.0) - (a - 1.0) * cos_w0 + beta,
                            2.0 * ((a - 1.0) - (a + 1.0) * cos_w0),
                            (a + 1.0) - (a - 1.0) * cos_w0 - beta});
  Biquad f;
  f.b0_ = c.b0, f.b1_ = c.b1, f.b2_ = c.b2, f.a1_ = c.a1, f.a2_ = c.a2;
  return f;
}

Biquad Biquad::peaking(double sample_rate, double freq, double gain_db, double q) noexcept {
  if (!usable(sample_rate, freq, q)) return identity();
  const double a = db_to_linear(gain_db / 2.0);
  const double w0 = 2.0 * std::numbers::pi * freq / sample_rate;
  const double cos_w0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);

  const auto c = normalise({1.0 + alpha * a, -2.0 * cos_w0, 1.0 - alpha * a, 1.0 + alpha / a,
                            -2.0 * cos_w0, 1.0 - alpha / a});
  Biquad f;
  f.b0_ = c.b0, f.b1_ = c.b1, f.b2_ = c.b2, f.a1_ = c.a1, f.a2_ = c.a2;
  return f;
}

Biquad Biquad::band_reject(double sample_rate, double freq, double q) noexcept {
  if (!usable(sample_rate, freq, q)) return identity();
  const double w0 = 2.0 * std::numbers::pi * freq / sample_rate;
  const double cos_w0 = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * q);

  const auto c = normalise(
      {1.0, -2.0 * cos_w0, 1.0, 1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha});
  Biquad f;
  f.b0_ = c.b0, f.b1_ = c.b1, f.b2_ = c.b2, f.a1_ = c.a1, f.a2_ = c.a2;
  return f;
}

float Biquad::process(float x) noexcept {
  const double in = static_cast<double>(x);
  const double out = b0_ * in + z1_;
  z1_ = b1_ * in - a1_ * out + z2_;
  z2_ = b2_ * in - a2_ * out;
  return static_cast<float>(out);
}

void Biquad::process(std::span<float> samples) noexcept {
  for (float& sample : samples) sample = process(sample);
}

void Biquad::reset() noexcept {
  z1_ = 0.0;
  z2_ = 0.0;
}

double Biquad::magnitude_at(double freq, double sample_rate) const noexcept {
  if (sample_rate <= 0.0) return 1.0;
  // Evaluate the transfer function on the unit circle: z = e^(j*w).
  const double w = 2.0 * std::numbers::pi * freq / sample_rate;
  const std::complex<double> z = std::polar(1.0, -w);
  const std::complex<double> z2 = z * z;

  const std::complex<double> numerator = b0_ + b1_ * z + b2_ * z2;
  const std::complex<double> denominator = 1.0 + a1_ * z + a2_ * z2;
  if (std::abs(denominator) == 0.0) return 0.0;
  return std::abs(numerator / denominator);
}

double Biquad::gain_db_at(double freq, double sample_rate) const noexcept {
  return linear_to_db(magnitude_at(freq, sample_rate));
}

}  // namespace cutline::audio
