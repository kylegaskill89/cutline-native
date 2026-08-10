#include "ripple.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::core {
namespace {

/// How close two edit points have to be to count as the same instant. The same
/// tolerance the rest of the timeline uses for "these clips touch".
constexpr double kTouchEps = 1e-9;

}  // namespace

Project ripple_after(Project p, std::vector<RippleShift> shifts,
                     const std::unordered_set<std::string>& changed) {
  if (shifts.empty()) return p;

  // Sorted by time and then by delta, so collapsing to the last entry at each
  // instant keeps the largest — which for a slow-down is the one that makes
  // enough room, and for a speed-up is the least of the closings and so the
  // one that cannot pull a neighbour over the clip in front of it.
  std::ranges::sort(shifts);
  std::vector<RippleShift> distinct;
  for (const RippleShift& shift : shifts) {
    if (!distinct.empty() && std::abs(distinct.back().first - shift.first) < kTouchEps) {
      distinct.back().second = shift.second;
    } else {
      distinct.push_back(shift);
    }
  }

  for (Track& t : p.sequence().tracks) {
    const bool holds_target =
        std::ranges::any_of(t.clips, [&](const Clip& c) { return changed.contains(c.id); });
    // A pinned track still carries its own changed clips — sync lock decides
    // whether an edit *elsewhere* moves a track, not whether its own clip may
    // change length and leave the one after it overlapping.
    if (!t.sync_locked && !holds_target) continue;
    for (Clip& c : t.clips) {
      double moved = 0.0;
      for (const auto& [at, delta] : distinct) {
        if (c.start >= at - kTouchEps) moved += delta;
      }
      c.start += moved;
    }
  }

  for (Track& t : p.sequence().tracks) std::ranges::stable_sort(t.clips, {}, &Clip::start);
  return p;
}

}  // namespace cutline::core
