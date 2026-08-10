#pragma once

/// Making room after clips have changed length in place.
///
/// Internal to `cutline_core`: two operations change how long clips are without
/// moving them — a retime and a frame-rate conform — and both leave whatever
/// followed either overlapped or floating in a gap. The rule for closing that
/// is the same in both, and it is fiddly enough (collapse the shifts, respect
/// sync lock, but never pin a track against its own clips) that a second copy
/// of it would be a second set of edge cases.

#include "cutline/core/model.hpp"

#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cutline::core {

/// One place the sequence has to open or close, and by how much: the time a
/// clip *used to* end, and how much longer it is now.
using RippleShift = std::pair<double, double>;

/// Moves everything starting at or after each shift along by its delta.
///
/// `changed` names the clips that grew or shrank. A track holding one of them
/// is rippled whether or not it is sync locked — the lock decides whether an
/// edit *elsewhere* moves a track, not whether its own clip may change length
/// and leave the next one overlapping it.
///
/// Shifts are collapsed to one per instant, keeping the largest: a linked pair
/// is two clips ending at the same time by the same amount, and counting both
/// would open the sequence twice for one edit.
[[nodiscard]] Project ripple_after(Project p, std::vector<RippleShift> shifts,
                                   const std::unordered_set<std::string>& changed);

}  // namespace cutline::core
