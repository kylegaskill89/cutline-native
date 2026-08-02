#include "cutline/core/keyframe.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cutline::core {

namespace {

/// How close to the wanted x the solve has to get, and how hard it may try.
///
/// A thousandth of a segment is finer than a frame at any rate anybody edits
/// at, and finer than a pixel on any graph this is drawn on. Eight Newton steps
/// reach it from anywhere on a well-behaved curve; the bisection after it is
/// for the ones that are not, where the derivative goes flat and Newton stops
/// moving.
constexpr double kSolveEps = 1e-6;
constexpr int kNewtonSteps = 8;
constexpr int kBisectSteps = 24;

/// One component of a cubic bezier from 0 to 1 with control values `c1`, `c2`.
[[nodiscard]] double bezier_axis(double s, double c1, double c2) noexcept {
  const double r = 1.0 - s;
  return 3.0 * r * r * s * c1 + 3.0 * r * s * s * c2 + s * s * s;
}

[[nodiscard]] double bezier_axis_slope(double s, double c1, double c2) noexcept {
  const double r = 1.0 - s;
  return 3.0 * r * r * c1 + 6.0 * r * s * (c2 - c1) + 3.0 * s * s * (1.0 - c2);
}

}  // namespace

double bezier_fraction(double f, double x1, double y1, double x2, double y2) noexcept {
  if (!(f > 0.0)) return 0.0;
  if (f >= 1.0) return 1.0;

  // Monotonic in x, so there is exactly one parameter for each fraction of the
  // way along. Without this a handle dragged back past its own keyframe would
  // describe a curve that is at two values at the same instant, and which one
  // came out would be whichever the solver happened to land on.
  x1 = std::clamp(x1, 0.0, 1.0);
  x2 = std::clamp(x2, 0.0, 1.0);

  // Newton from the fraction itself, which is the answer exactly when the curve
  // is the straight line and close to it for every gentle one.
  double s = f;
  for (int i = 0; i < kNewtonSteps; ++i) {
    const double error = bezier_axis(s, x1, x2) - f;
    if (std::abs(error) < kSolveEps) return bezier_axis(s, y1, y2);
    const double slope = bezier_axis_slope(s, x1, x2);
    if (std::abs(slope) < 1e-9) break;  // flat: Newton has nothing to follow
    s -= error / slope;
  }

  // Bisection, which cannot stall. Reached only where the curve is flat in x,
  // which is exactly the case a handle pulled hard sideways produces.
  double low = 0.0;
  double high = 1.0;
  s = f;
  for (int i = 0; i < kBisectSteps; ++i) {
    const double x = bezier_axis(s, x1, x2);
    if (std::abs(x - f) < kSolveEps) break;
    if (x < f) {
      low = s;
    } else {
      high = s;
    }
    s = (low + high) / 2.0;
  }
  return bezier_axis(s, y1, y2);
}

double ease_fraction(double f, Interp mode) noexcept {
  switch (mode) {
    case Interp::Hold:
      return 0.0;
    case Interp::Ease:
      return f * f * (3.0 - 2.0 * f);  // smoothstep
    case Interp::Bezier:
      // Shapeless without the keyframe at the far end of the segment. See
      // `segment_fraction`, which is what the evaluator actually calls.
    case Interp::Linear:
      break;
  }
  return f;
}

double segment_fraction(const Keyframe& a, const Keyframe& b, double f) noexcept {
  if (a.e != Interp::Bezier) return ease_fraction(f, a.e);
  // `b.in_*` is measured backwards from `b`, so the second control point is one
  // minus it. That is what makes a keyframe's two handles symmetric — pulling
  // either one "outward" is a larger number.
  return bezier_fraction(f, a.out_x, a.out_y, 1.0 - b.in_x, 1.0 - b.in_y);
}

double eval_keyframes(std::span<const Keyframe> kfs, double local_t) noexcept {
  if (kfs.empty()) return 0.0;
  if (local_t <= kfs.front().t) return kfs.front().v;

  const Keyframe& last = kfs.back();
  if (local_t >= last.t) return last.v;

  for (std::size_t i = 0; i + 1 < kfs.size(); ++i) {
    const Keyframe& a = kfs[i];
    const Keyframe& b = kfs[i + 1];
    if (local_t >= a.t && local_t <= b.t) {
      const double span = b.t - a.t;
      const double f = span > 1e-9 ? (local_t - a.t) / span : 0.0;
      return a.v + (b.v - a.v) * segment_fraction(a, b, f);
    }
  }
  return last.v;
}

void upsert_keyframe(std::vector<Keyframe>& kfs, double t, double v) {
  const auto existing = std::ranges::find_if(
      kfs, [&](const Keyframe& k) { return std::abs(k.t - t) < kKeyframeMatchEps; });
  if (existing != kfs.end()) {
    existing->t = t;
    existing->v = v;  // the keyframe keeps its own interpolation
    return;
  }
  kfs.push_back({.t = t, .v = v, .e = keyframe_list_interp(kfs)});
  std::ranges::stable_sort(kfs, {}, &Keyframe::t);
}

void remove_keyframe_near(std::vector<Keyframe>& kfs, double t) {
  std::erase_if(kfs, [&](const Keyframe& k) { return std::abs(k.t - t) < kKeyframeRemoveEps; });
}

Interp keyframe_list_interp(std::span<const Keyframe> kfs) noexcept {
  return kfs.empty() ? Interp::Linear : kfs.front().e;
}

void set_keyframe_list_interp(std::vector<Keyframe>& kfs, Interp mode) noexcept {
  for (Keyframe& k : kfs) k.e = mode;
}

}  // namespace cutline::core
