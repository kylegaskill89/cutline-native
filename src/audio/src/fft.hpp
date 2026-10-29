#pragma once

/// A radix-2 FFT, shared by the synchroniser's cross-correlation and the
/// spectral repair effects. Small and plain: the sizes here are a few
/// thousand points, and a dependency for them would cost more than it saved.

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace cutline::audio::detail {

/// In place; `values.size()` must be a power of two. The inverse is scaled by
/// 1/n, so a forward and an inverse give back what went in.
template <typename Real>
void fft(std::vector<std::complex<Real>>& values, bool inverse) {
  using Complex = std::complex<Real>;
  const std::size_t n = values.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; (j & bit) != 0; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(values[i], values[j]);
  }
  for (std::size_t length = 2; length <= n; length <<= 1) {
    const double angle = 2.0 * std::numbers::pi / static_cast<double>(length) * (inverse ? 1 : -1);
    // The twiddle is carried in double whatever the samples are, so its
    // rounding does not build up across a long butterfly in float.
    const std::complex<double> step(std::cos(angle), std::sin(angle));
    for (std::size_t i = 0; i < n; i += length) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < length / 2; ++k) {
        const Complex u = values[i + k];
        const Complex v = values[i + k + length / 2] *
                          Complex(static_cast<Real>(w.real()), static_cast<Real>(w.imag()));
        values[i + k] = u + v;
        values[i + k + length / 2] = u - v;
        w *= step;
      }
    }
  }
  if (inverse) {
    for (Complex& v : values) v /= static_cast<Real>(n);
  }
}

}  // namespace cutline::audio::detail
