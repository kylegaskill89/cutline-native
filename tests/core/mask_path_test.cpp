/// The pen's arithmetic: turning points with handles into the corners that get
/// filled, splitting an edge without moving the shape, and finding the edge a
/// click meant.
///
/// The property that matters most is the dull one — a path with no handles on
/// it comes back exactly as it went in. Every path drawn before the pen existed
/// is that path, and every test elsewhere in the suite that uses a polygon mask
/// is relying on it.

#include "cutline/core/mask_path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace cutline::core {
namespace {

[[nodiscard]] std::vector<MaskPoint> square() {
  return {MaskPoint{.x = -0.2, .y = -0.2}, MaskPoint{.x = 0.2, .y = -0.2},
          MaskPoint{.x = 0.2, .y = 0.2}, MaskPoint{.x = -0.2, .y = 0.2}};
}

// ------------------------------------------------------------- flattening --

TEST(MaskPath, APathOfSharpCornersIsItsOwnOutline) {
  const std::vector<MaskPoint> points = square();
  EXPECT_EQ(flatten_mask_path(points), points);
}

TEST(MaskPath, TooFewPointsToEncloseAnythingComeBackUntouched) {
  const std::vector<MaskPoint> one{MaskPoint{.x = 0.1, .y = 0.1}};
  EXPECT_EQ(flatten_mask_path(one), one);
  EXPECT_TRUE(flatten_mask_path({}).empty());
}

TEST(MaskPath, ACurvedEdgeGainsPointsAlongIt) {
  std::vector<MaskPoint> points = square();
  // Bow the first edge outwards.
  points[0].out_y = -0.2;
  points[1].in_y = -0.2;

  const std::vector<MaskPoint> flat = flatten_mask_path(points);
  ASSERT_GT(flat.size(), points.size());

  // Every point somebody placed is still there, in order.
  for (const MaskPoint& placed : points) {
    const bool kept = std::ranges::any_of(flat, [&](const MaskPoint& out) {
      return std::abs(out.x - placed.x) < 1e-9 && std::abs(out.y - placed.y) < 1e-9;
    });
    EXPECT_TRUE(kept) << "a placed corner was dropped in favour of a sample";
  }

  // And the samples bow the way the handles pull: the first edge runs along
  // y = -0.2, and the curve should sit above it.
  const bool bowed = std::ranges::any_of(
      flat, [](const MaskPoint& out) { return out.y < -0.2 - 1e-6; });
  EXPECT_TRUE(bowed) << "the edge was flattened straight through its own curve";
}

TEST(MaskPath, TheBudgetIsNeverExceeded) {
  std::vector<MaskPoint> points = square();
  for (MaskPoint& point : points) {
    point.in_x = -0.1;
    point.in_y = -0.1;
    point.out_x = 0.1;
    point.out_y = 0.1;
  }
  for (const std::size_t budget : {std::size_t{4}, std::size_t{8}, std::size_t{64}}) {
    EXPECT_LE(flatten_mask_path(points, budget).size(), budget) << "budget " << budget;
  }
}

TEST(MaskPath, MorePointsThanTheBudgetKeepsThePlacedOnes) {
  // A path can be longer than the budget, and then there is no room for curve
  // samples at all. Handing back the corners that were placed beats inventing
  // any, because those are the shape and the samples only approximate it.
  std::vector<MaskPoint> points;
  for (int i = 0; i < 10; ++i) {
    points.push_back(MaskPoint{.x = 0.01 * i, .y = 0.02 * i, .out_x = 0.05, .out_y = 0.05});
  }
  const std::vector<MaskPoint> flat = flatten_mask_path(points, 6);
  ASSERT_EQ(flat.size(), 6u);
  for (std::size_t i = 0; i < flat.size(); ++i) {
    EXPECT_DOUBLE_EQ(flat[i].x, points[i].x);
    EXPECT_DOUBLE_EQ(flat[i].y, points[i].y);
  }
}

TEST(MaskPath, TheBudgetGoesWhereTheBendIs) {
  // Two curved edges, one bent far more than the other. The sharp one should
  // get more of the samples, which is the whole reason the split is weighted
  // rather than even.
  std::vector<MaskPoint> points = square();
  points[0].out_y = -0.6;  // a big bow on the first edge
  points[1].in_y = -0.6;
  points[2].out_y = 0.22;  // a slight one on the third
  points[3].in_y = 0.22;

  const std::vector<MaskPoint> flat = flatten_mask_path(points, 24);

  // Count what landed clearly outside the square on each side.
  int above = 0;
  int below = 0;
  for (const MaskPoint& out : flat) {
    if (out.y < -0.2 - 1e-6) ++above;
    if (out.y > 0.2 + 1e-6) ++below;
  }
  EXPECT_GT(above, below) << "the tighter bend should have been sampled more finely";
}

// --------------------------------------------------------------- splitting --

TEST(MaskPath, SplittingAnEdgeDoesNotMoveTheShape) {
  std::vector<MaskPoint> points = square();
  points[0].out_x = 0.1;
  points[0].out_y = -0.25;
  points[1].in_x = -0.1;
  points[1].in_y = -0.25;

  const std::vector<MaskPoint> split = split_mask_path(points, 0, 0.5);
  ASSERT_EQ(split.size(), points.size() + 1);

  // The curve either side of the new point traces the original: sampling the
  // first edge before, and the two halves after, has to agree everywhere.
  for (int step = 0; step <= 10; ++step) {
    const double t = static_cast<double>(step) / 10.0;
    const MaskPoint was = mask_path_point_at(points[0], points[1], t);
    // The same place, now expressed on whichever half it falls in.
    const MaskPoint now = t <= 0.5 ? mask_path_point_at(split[0], split[1], t * 2.0)
                                   : mask_path_point_at(split[1], split[2], (t - 0.5) * 2.0);
    EXPECT_NEAR(now.x, was.x, 1e-9) << "at t = " << t;
    EXPECT_NEAR(now.y, was.y, 1e-9) << "at t = " << t;
  }
}

TEST(MaskPath, SplittingAStraightEdgePutsAPointOnIt) {
  const std::vector<MaskPoint> split = split_mask_path(square(), 1, 0.5);
  ASSERT_EQ(split.size(), 5u);
  // The second edge runs from (0.2, -0.2) to (0.2, 0.2), so its middle is
  // (0.2, 0) and the new point is sharp like its neighbours.
  EXPECT_NEAR(split[2].x, 0.2, 1e-9);
  EXPECT_NEAR(split[2].y, 0.0, 1e-9);
  EXPECT_TRUE(split[2].sharp());
}

TEST(MaskPath, TheLastEdgeWrapsBackToTheFirstPoint) {
  const std::vector<MaskPoint> split = split_mask_path(square(), 3, 0.5);
  ASSERT_EQ(split.size(), 5u);
  // From (-0.2, 0.2) back to (-0.2, -0.2): the middle is (-0.2, 0), and it goes
  // on the end because that edge leaves the last point.
  EXPECT_NEAR(split[4].x, -0.2, 1e-9);
  EXPECT_NEAR(split[4].y, 0.0, 1e-9);
}

TEST(MaskPath, SplittingAnEdgeThatIsNotThereChangesNothing) {
  const std::vector<MaskPoint> points = square();
  EXPECT_EQ(split_mask_path(points, 9, 0.5), points);
}

// ---------------------------------------------------------------- nearest --

TEST(MaskPath, TheNearestEdgeToAPlaceIsTheOneUnderIt) {
  const std::vector<MaskPoint> points = square();

  // Just outside the middle of the top edge, which is the one leaving point 0.
  const std::optional<MaskPathHit> top = nearest_on_mask_path(points, 0.0, -0.21);
  ASSERT_TRUE(top.has_value());
  EXPECT_EQ(top->segment, 0u);
  EXPECT_NEAR(top->t, 0.5, 0.05);
  EXPECT_NEAR(top->distance, 0.01, 1e-3);

  // And the left edge, which is the one that wraps.
  const std::optional<MaskPathHit> left = nearest_on_mask_path(points, -0.21, 0.0);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->segment, 3u);
}

TEST(MaskPath, NothingToBeNearIsNotAnAnswer) {
  EXPECT_FALSE(nearest_on_mask_path({}, 0.0, 0.0).has_value());
  const std::vector<MaskPoint> one{MaskPoint{}};
  EXPECT_FALSE(nearest_on_mask_path(one, 0.0, 0.0).has_value());
}

// --------------------------------------------------------------- smoothing --

TEST(MaskPath, SmoothingMirrorsTheHandleThatWasKept) {
  MaskPoint point{.x = 0.1, .y = 0.2, .in_x = 9.0, .in_y = 9.0, .out_x = 0.05, .out_y = -0.03};
  smooth_mask_point(point, true);
  EXPECT_DOUBLE_EQ(point.in_x, -0.05);
  EXPECT_DOUBLE_EQ(point.in_y, 0.03);
  // And the other way round, keeping the incoming one.
  point.out_x = 9.0;
  smooth_mask_point(point, false);
  EXPECT_DOUBLE_EQ(point.out_x, 0.05);
  EXPECT_DOUBLE_EQ(point.out_y, -0.03);
}

TEST(MaskPath, ASharpPointKnowsItIsSharp) {
  const MaskPoint corner{.x = 0.3, .y = 0.4};
  const MaskPoint curved{.x = 0.3, .y = 0.4, .out_x = 0.01};
  EXPECT_TRUE(corner.sharp());
  EXPECT_FALSE(curved.sharp());
}

}  // namespace
}  // namespace cutline::core
