/// The tracker on pictures made here: a texture moved by a known amount, and
/// the amount found again.

#include "cutline/track/tracker.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <random>

namespace cutline::track {
namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

/// Soft blobs of every size at random places: detail in every direction, as a
/// real picture has, and nothing repeating to be confused by.
[[nodiscard]] Image texture() {
  Image image{.width = kWidth, .height = kHeight};
  image.pixels.assign(static_cast<std::size_t>(kWidth) * kHeight, 0.5f);
  std::mt19937 random(7);
  std::uniform_real_distribution<double> x(0.0, kWidth);
  std::uniform_real_distribution<double> y(0.0, kHeight);
  std::uniform_real_distribution<double> size(2.0, 9.0);
  std::uniform_real_distribution<double> shade(-0.25, 0.25);
  for (int blob = 0; blob < 500; ++blob) {
    const double cx = x(random);
    const double cy = y(random);
    const double r = size(random);
    const double s = shade(random);
    for (int j = static_cast<int>(cy - 3 * r); j <= static_cast<int>(cy + 3 * r); ++j) {
      for (int i = static_cast<int>(cx - 3 * r); i <= static_cast<int>(cx + 3 * r); ++i) {
        if (i < 0 || j < 0 || i >= kWidth || j >= kHeight) continue;
        const double d2 = (i - cx) * (i - cx) + (j - cy) * (j - cy);
        image.pixels[static_cast<std::size_t>(j) * kWidth + i] +=
            static_cast<float>(s * std::exp(-d2 / (2 * r * r)));
      }
    }
  }
  return image;
}

/// The picture with `motion` applied: each pixel read from where the motion
/// would have brought it from.
[[nodiscard]] Image moved(const Image& image, const Similarity& motion) {
  Image out{.width = image.width, .height = image.height};
  out.pixels.resize(image.pixels.size());
  const double c = std::cos(-motion.rotation);
  const double s = std::sin(-motion.rotation);
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const double px = x - motion.origin.x - motion.dx;
      const double py = y - motion.origin.y - motion.dy;
      const double sx = motion.origin.x + (c * px - s * py) / motion.scale;
      const double sy = motion.origin.y + (s * px + c * py) / motion.scale;
      out.pixels[static_cast<std::size_t>(y) * image.width + x] = image.sample(sx, sy);
    }
  }
  return out;
}

[[nodiscard]] std::vector<Point> box(double x0, double y0, double x1, double y1) {
  return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

constexpr Point kCentre{160.0, 120.0};

TEST(Tracker, CornersAreFoundInsideTheRegionAndApart) {
  const std::vector<Point> region = box(100, 80, 220, 160);
  const std::vector<Point> corners = find_features(texture(), region, 50, 6.0);
  ASSERT_GE(corners.size(), 20u);
  for (std::size_t i = 0; i < corners.size(); ++i) {
    EXPECT_TRUE(inside(region, corners[i]));
    for (std::size_t j = i + 1; j < corners.size(); ++j) {
      EXPECT_GE(std::hypot(corners[i].x - corners[j].x, corners[i].y - corners[j].y), 6.0);
    }
  }
}

TEST(Tracker, AFlatPictureHasNoCorners) {
  Image flat{.width = kWidth, .height = kHeight};
  flat.pixels.assign(static_cast<std::size_t>(kWidth) * kHeight, 0.4f);
  EXPECT_TRUE(find_features(flat, {}, 50, 6.0).empty());
}

TEST(Tracker, AMoveOfAFewPixelsIsFoundToATenthOfOne) {
  const Image a = texture();
  const Image b = moved(a, Similarity{.origin = kCentre, .dx = 3.4, .dy = -2.2});
  const auto fit = track_region(Pyramid(a), Pyramid(b), box(80, 60, 240, 180),
                                Motion::Translation, kCentre);
  ASSERT_TRUE(fit.has_value());
  EXPECT_NEAR(fit->dx, 3.4, 0.1);
  EXPECT_NEAR(fit->dy, -2.2, 0.1);
}

TEST(Tracker, ALargeMoveIsFoundThroughThePyramid) {
  const Image a = texture();
  const Image b = moved(a, Similarity{.origin = kCentre, .dx = -21.0, .dy = 14.0});
  const auto fit = track_region(Pyramid(a), Pyramid(b), box(80, 60, 240, 180),
                                Motion::Translation, kCentre);
  ASSERT_TRUE(fit.has_value());
  EXPECT_NEAR(fit->dx, -21.0, 0.2);
  EXPECT_NEAR(fit->dy, 14.0, 0.2);
}

TEST(Tracker, ATurnAndAScaleAreFoundWhenAskedFor) {
  const Image a = texture();
  const double turn = 4.0 * std::numbers::pi / 180.0;
  const Image b = moved(
      a, Similarity{.origin = kCentre, .dx = 2.0, .dy = 1.0, .rotation = turn, .scale = 1.05});
  const auto fit = track_region(Pyramid(a), Pyramid(b), box(70, 50, 250, 190),
                                Motion::Similarity, kCentre);
  ASSERT_TRUE(fit.has_value());
  EXPECT_NEAR(fit->rotation, turn, 0.004);
  EXPECT_NEAR(fit->scale, 1.05, 0.005);
  EXPECT_NEAR(fit->dx, 2.0, 0.25);
  EXPECT_NEAR(fit->dy, 1.0, 0.25);

  // Position only: no turn and no scale, whatever the picture did.
  const auto only = track_region(Pyramid(a), Pyramid(b), box(70, 50, 250, 190),
                                 Motion::Translation, kCentre);
  ASSERT_TRUE(only.has_value());
  EXPECT_EQ(only->rotation, 0.0);
  EXPECT_EQ(only->scale, 1.0);
}

TEST(Tracker, PointsThatDisagreeDoNotMoveTheAnswer) {
  // Thirty points moved by (5, 0) and eight carried off somewhere else.
  std::vector<Point> from;
  std::vector<Point> to;
  for (int i = 0; i < 30; ++i) {
    from.push_back({10.0 + i * 7.0, 20.0 + (i % 5) * 9.0});
    to.push_back({from.back().x + 5.0, from.back().y});
  }
  for (int i = 0; i < 8; ++i) {
    from.push_back({50.0 + i, 90.0});
    to.push_back({from.back().x - 30.0, from.back().y + 40.0});
  }
  const auto fit = fit_motion(from, to, Motion::Similarity, {100.0, 50.0});
  ASSERT_TRUE(fit.has_value());
  EXPECT_NEAR(fit->dx, 5.0, 1e-6);
  EXPECT_NEAR(fit->dy, 0.0, 1e-6);
  EXPECT_NEAR(fit->scale, 1.0, 1e-6);
}

TEST(Tracker, TooFewPointsIsNoAnswer) {
  const std::vector<Point> two{{0, 0}, {1, 1}};
  EXPECT_FALSE(fit_motion(two, two, Motion::Translation, {}).has_value());
}

}  // namespace
}  // namespace cutline::track
