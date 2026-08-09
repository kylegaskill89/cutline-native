#pragma once

/// Turning a free-drawn mask's points into the straight segments everything
/// downstream actually uses.
///
/// A `MaskPoint` carries a handle either side of it, so the edge between two
/// points is a cubic. Nothing that consumes a mask understands a cubic: the
/// shader fills a polygon by the even-odd rule, and the monitor strokes a run
/// of lines. Both go through here, and that is the point of the file — the
/// outline you can see and the region that gets filled are the same list of
/// corners, so they cannot drift apart.
///
/// The budget is a real constraint rather than a tidiness measure. The shader
/// walks every corner for every pixel it shades, so corners are per-pixel cost,
/// and the buffer they travel in is sized once. Curves are therefore sampled
/// where the curvature is: a nearly straight edge gets one segment and a tight
/// bend gets many, out of a fixed total.

#include "cutline/core/model.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cutline::core {

/// The path as corners, ready to fill or stroke.
///
/// A path whose points are all sharp comes back unchanged — which is every path
/// written before handles existed, and the common case for a shape traced round
/// a sign. Fewer than two points cannot describe an edge and come back as they
/// went in.
///
/// `budget` is the most corners to produce. It is never exceeded, and the
/// original points are always present: a curve loses detail before a corner the
/// user placed is dropped.
[[nodiscard]] std::vector<MaskPoint> flatten_mask_path(std::span<const MaskPoint> points,
                                                       std::size_t budget = kMaxMaskPoints);

/// The point on the cubic between `from` and `to` at `t` in [0, 1].
///
/// Exposed because the monitor needs it to place a handle's grip on the curve
/// it belongs to, and because a second implementation of the same four lines is
/// how the drawn shape and the filled one start disagreeing.
[[nodiscard]] MaskPoint mask_path_point_at(const MaskPoint& from, const MaskPoint& to, double t);

/// Makes `point`'s two handles mirror each other about it, keeping the one
/// named by `keep_out` and reflecting it into the other.
///
/// What "smooth" means: the curve passes through without a crease, because the
/// two handles are collinear with the point and the same length. Premiere calls
/// the other kind a corner point, and reaches it by breaking the pair.
void smooth_mask_point(MaskPoint& point, bool keep_out) noexcept;

/// Where on the path a place is nearest to, for putting a point there.
struct MaskPathHit {
  /// The edge, named by the point it leaves. Wraps, so the last edge is the one
  /// from the last point back to the first.
  std::size_t segment = 0;
  /// How far along that edge, in [0, 1].
  double t = 0.0;
  /// How far the place was from the path, in the same units the path is in.
  double distance = 0.0;
};

/// The point on the path closest to (`x`, `y`).
///
/// Nothing when there is no path to be near. Sampled rather than solved: the
/// closest point on a cubic is a fifth-degree root-find, and this exists to
/// answer "which edge did they click, and roughly where" — a question a click
/// cannot ask more precisely than a pixel.
[[nodiscard]] std::optional<MaskPathHit> nearest_on_mask_path(std::span<const MaskPoint> points,
                                                              double x, double y);

/// Splits the edge leaving `segment` at `t`, returning the path with one more
/// point in it.
///
/// **The shape does not change.** A cubic split by de Casteljau is two cubics
/// that trace exactly the same curve, so the new point lands *on* the outline
/// and the handles either side of it are the ones that keep it there. Inserting
/// a point that moved the shape would make adding detail destructive, which is
/// the opposite of what it is for.
[[nodiscard]] std::vector<MaskPoint> split_mask_path(std::span<const MaskPoint> points,
                                                     std::size_t segment, double t);

}  // namespace cutline::core
