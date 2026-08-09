#include "cutline/core/mask_path.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::core {
namespace {

/// Whether the edge from `from` to `to` is a straight line.
///
/// It is the *outgoing* handle of one end and the *incoming* handle of the
/// other that shape an edge, so a point can be sharp on one side and curved on
/// the other — which is exactly what a corner joining a straight run to a bend
/// looks like.
[[nodiscard]] bool straight(const MaskPoint& from, const MaskPoint& to) noexcept {
  return from.out_x == 0.0 && from.out_y == 0.0 && to.in_x == 0.0 && to.in_y == 0.0;
}

[[nodiscard]] double distance(double x1, double y1, double x2, double y2) noexcept {
  return std::hypot(x2 - x1, y2 - y1);
}

/// How much an edge bends: how much longer the path through its two control
/// points is than the straight line between its ends.
///
/// Zero for a straight edge, and larger the further the handles pull away from
/// the chord. It is not the mathematical arc length and does not need to be —
/// it only has to rank the edges against each other so the budget goes where
/// the shape needs it.
[[nodiscard]] double bulge(const MaskPoint& from, const MaskPoint& to) noexcept {
  const double c1x = from.x + from.out_x;
  const double c1y = from.y + from.out_y;
  const double c2x = to.x + to.in_x;
  const double c2y = to.y + to.in_y;
  const double along = distance(from.x, from.y, c1x, c1y) + distance(c1x, c1y, c2x, c2y) +
                       distance(c2x, c2y, to.x, to.y);
  return std::max(0.0, along - distance(from.x, from.y, to.x, to.y));
}

}  // namespace

MaskPoint mask_path_point_at(const MaskPoint& from, const MaskPoint& to, double t) {
  const double u = 1.0 - t;
  const double a = u * u * u;
  const double b = 3.0 * u * u * t;
  const double c = 3.0 * u * t * t;
  const double d = t * t * t;
  return MaskPoint{
      .x = a * from.x + b * (from.x + from.out_x) + c * (to.x + to.in_x) + d * to.x,
      .y = a * from.y + b * (from.y + from.out_y) + c * (to.y + to.in_y) + d * to.y,
  };
}

void smooth_mask_point(MaskPoint& point, bool keep_out) noexcept {
  if (keep_out) {
    point.in_x = -point.out_x;
    point.in_y = -point.out_y;
  } else {
    point.out_x = -point.in_x;
    point.out_y = -point.in_y;
  }
}

std::optional<MaskPathHit> nearest_on_mask_path(std::span<const MaskPoint> points, double x,
                                                double y) {
  const std::size_t count = points.size();
  if (count < 2) return std::nullopt;

  // Enough samples that a click lands on the edge it looks like it landed on.
  // A curve is only ever a few per cent of the frame across, so this is finer
  // than the pointer can be aimed.
  constexpr int kSamples = 24;

  MaskPathHit best;
  double best_distance = -1.0;
  for (std::size_t i = 0; i < count; ++i) {
    const MaskPoint& from = points[i];
    const MaskPoint& to = points[(i + 1) % count];
    for (int step = 0; step <= kSamples; ++step) {
      const double t = static_cast<double>(step) / static_cast<double>(kSamples);
      const MaskPoint at = mask_path_point_at(from, to, t);
      const double away = distance(at.x, at.y, x, y);
      if (best_distance < 0.0 || away < best_distance) {
        best_distance = away;
        best = MaskPathHit{.segment = i, .t = t, .distance = away};
      }
    }
  }
  if (best_distance < 0.0) return std::nullopt;
  return best;
}

std::vector<MaskPoint> split_mask_path(std::span<const MaskPoint> points, std::size_t segment,
                                       double t) {
  const std::size_t count = points.size();
  if (count < 2 || segment >= count) return {points.begin(), points.end()};

  std::vector<MaskPoint> out{points.begin(), points.end()};
  const std::size_t next = (segment + 1) % count;

  MaskPoint& from = out[segment];
  MaskPoint& to = out[next];

  // A straight edge splits into two straight edges, and the honest way to say
  // so is a plain corner. De Casteljau would give the same *shape* but express
  // it with handles running along the line, which is a point that reports
  // itself curved, draws a pair of grips on top of its own edge, and rounds off
  // the moment it is nudged. Splitting the rectangle everyone starts from is
  // the common case, so this is the common case.
  if (straight(from, to)) {
    const MaskPoint inserted{.x = from.x + t * (to.x - from.x),
                             .y = from.y + t * (to.y - from.y)};
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(segment) + 1, inserted);
    return out;
  }

  // de Casteljau, on the four control points of this edge.
  const double p0x = from.x;
  const double p0y = from.y;
  const double c1x = from.x + from.out_x;
  const double c1y = from.y + from.out_y;
  const double c2x = to.x + to.in_x;
  const double c2y = to.y + to.in_y;
  const double p3x = to.x;
  const double p3y = to.y;

  const double a1x = p0x + t * (c1x - p0x);
  const double a1y = p0y + t * (c1y - p0y);
  const double mx = c1x + t * (c2x - c1x);
  const double my = c1y + t * (c2y - c1y);
  const double b2x = c2x + t * (p3x - c2x);
  const double b2y = c2y + t * (p3y - c2y);

  const double a2x = a1x + t * (mx - a1x);
  const double a2y = a1y + t * (my - a1y);
  const double b1x = mx + t * (b2x - mx);
  const double b1y = my + t * (b2y - my);

  const double px = a2x + t * (b1x - a2x);
  const double py = a2y + t * (b1y - a2y);

  // The ends keep their far handles and take new near ones; the new point sits
  // on the curve with the two that hold it there.
  from.out_x = a1x - p0x;
  from.out_y = a1y - p0y;
  to.in_x = b2x - p3x;
  to.in_y = b2y - p3y;

  const MaskPoint inserted{
      .x = px, .y = py, .in_x = a2x - px, .in_y = a2y - py, .out_x = b1x - px,
      .out_y = b1y - py};

  out.insert(out.begin() + static_cast<std::ptrdiff_t>(segment) + 1, inserted);
  return out;
}

std::vector<MaskPoint> flatten_mask_path(std::span<const MaskPoint> points,
                                         std::size_t budget) {
  const std::size_t count = points.size();
  if (count < 2) return {points.begin(), points.end()};

  // More points than there is room for. Handing back the first `budget` of them
  // is the honest answer: they are the corners somebody placed, and inventing
  // samples between them would only crowd out more of the ones that are real.
  if (count >= budget) return {points.begin(), points.begin() + static_cast<long>(budget)};

  // Closed, so the edge from the last point back to the first is an edge like
  // any other and gets its share.
  std::vector<double> weight(count, 0.0);
  double total = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    const MaskPoint& from = points[i];
    const MaskPoint& to = points[(i + 1) % count];
    if (straight(from, to)) continue;
    weight[i] = bulge(from, to);
    total += weight[i];
  }

  if (total <= 0.0) return {points.begin(), points.end()};

  // What is left once every placed point has its place.
  const std::size_t spare = budget - count;

  std::vector<MaskPoint> out;
  out.reserve(budget);
  std::size_t used = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const MaskPoint& from = points[i];
    const MaskPoint& to = points[(i + 1) % count];
    out.push_back(MaskPoint{.x = from.x, .y = from.y});

    if (weight[i] <= 0.0) continue;

    // This edge's share of what is spare, and at least one extra sample —
    // otherwise a gentle curve among sharp ones rounds down to nothing and
    // draws as the straight line it is not.
    auto extra = static_cast<std::size_t>(
        std::llround(static_cast<double>(spare) * (weight[i] / total)));
    extra = std::max<std::size_t>(extra, 1);
    extra = std::min(extra, spare > used ? spare - used : std::size_t{0});
    used += extra;

    for (std::size_t step = 1; step <= extra; ++step) {
      const double t = static_cast<double>(step) / static_cast<double>(extra + 1);
      out.push_back(mask_path_point_at(from, to, t));
    }
  }
  return out;
}

}  // namespace cutline::core
