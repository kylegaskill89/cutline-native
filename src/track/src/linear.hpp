#pragma once

/// Solving small dense systems, for the fits here: eight unknowns for a
/// perspective, and a few hundred for a warp mesh. Gaussian elimination with
/// partial pivoting, which is all a system that size needs.

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace cutline::track::detail {

/// Solves `a` x = `b` for an `n` by `n` system, row by row, in place. False
/// when the system has no single answer.
[[nodiscard]] inline bool solve(std::vector<double>& a, std::vector<double>& b, std::size_t n) {
  for (std::size_t col = 0; col < n; ++col) {
    std::size_t pivot = col;
    for (std::size_t row = col + 1; row < n; ++row) {
      if (std::abs(a[row * n + col]) > std::abs(a[pivot * n + col])) pivot = row;
    }
    if (std::abs(a[pivot * n + col]) < 1e-12) return false;
    if (pivot != col) {
      for (std::size_t k = 0; k < n; ++k) std::swap(a[col * n + k], a[pivot * n + k]);
      std::swap(b[col], b[pivot]);
    }
    for (std::size_t row = col + 1; row < n; ++row) {
      const double f = a[row * n + col] / a[col * n + col];
      if (f == 0.0) continue;
      for (std::size_t k = col; k < n; ++k) a[row * n + k] -= f * a[col * n + k];
      b[row] -= f * b[col];
    }
  }
  for (std::size_t i = n; i-- > 0;) {
    double sum = b[i];
    for (std::size_t k = i + 1; k < n; ++k) sum -= a[i * n + k] * b[k];
    b[i] = sum / a[i * n + i];
  }
  return true;
}

}  // namespace cutline::track::detail
