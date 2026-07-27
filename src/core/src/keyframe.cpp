#include "cutline/core/keyframe.hpp"

#include <cstddef>

namespace cutline::core {

double ease_fraction(double f, Interp mode) noexcept {
  switch (mode) {
    case Interp::Hold:
      return 0.0;
    case Interp::Ease:
      return f * f * (3.0 - 2.0 * f);  // smoothstep
    case Interp::Linear:
      break;
  }
  return f;
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
      return a.v + (b.v - a.v) * ease_fraction(f, a.e);
    }
  }
  return last.v;
}

}  // namespace cutline::core
