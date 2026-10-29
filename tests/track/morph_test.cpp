/// Morph Cut's looking, on pictures drawn here: a patterned wall with a
/// patterned head in front of it, the head a little elsewhere in the second
/// picture, as either side of a jump cut. The field has to move the head and
/// leave the wall.

#include "cutline/track/morph.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

namespace cutline::track {
namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 180;

/// A wall with detail all over it, and a head — a disc with its own,
/// different detail — centred at (`hx`, `hy`).
[[nodiscard]] Image interview(double hx, double hy) {
  Image image{kWidth, kHeight, std::vector<float>(static_cast<std::size_t>(kWidth * kHeight))};
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      double v = 0.4 + 0.1 * std::sin(x * 0.21) * std::cos(y * 0.17) +
                 0.06 * std::sin((x + 2 * y) * 0.09);
      const double dx = x - hx;
      const double dy = y - hy;
      if (dx * dx + dy * dy <= 40.0 * 40.0) {
        // The head's pattern travels with it: measured from its centre.
        v = 0.7 + 0.12 * std::sin(dx * 0.33) * std::sin(dy * 0.29) + 0.05 * std::cos(dx * 0.11);
      }
      image.pixels[static_cast<std::size_t>(y * kWidth + x)] = static_cast<float>(v);
    }
  }
  return image;
}

TEST(DenseFlow, MovesTheHeadAndLeavesTheWall) {
  const FlowField field = dense_flow(interview(150, 90), interview(166, 96));
  const Point head = field.at(150.0 / kWidth, 90.0 / kHeight);
  EXPECT_NEAR(head.x * kWidth, 16.0, 3.0);
  EXPECT_NEAR(head.y * kHeight, 6.0, 3.0);
  const Point wall = field.at(0.9, 0.2);
  EXPECT_NEAR(wall.x * kWidth, 0.0, 1.5);
  EXPECT_NEAR(wall.y * kHeight, 0.0, 1.5);
}

TEST(DenseFlow, FindsAWholeFrameThatHasMoved) {
  // The camera nudged between the two halves: everything moves together.
  Image from = interview(150, 90);
  Image to = from;
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      to.pixels[static_cast<std::size_t>(y * kWidth + x)] = from.sample(x - 5.0, y + 3.0);
    }
  }
  const FlowField field = dense_flow(from, to);
  const Point middle = field.at(0.5, 0.5);
  EXPECT_NEAR(middle.x * kWidth, 5.0, 1.0);
  EXPECT_NEAR(middle.y * kHeight, -3.0, 1.0);
}

TEST(DenseFlow, IsStillBetweenTwoOfTheSamePicture) {
  const Image same = interview(150, 90);
  const FlowField field = dense_flow(same, same);
  for (const Point& p : field.moves) {
    EXPECT_NEAR(p.x * kWidth, 0.0, 0.05);
    EXPECT_NEAR(p.y * kHeight, 0.0, 0.05);
  }
}

TEST(DenseFlow, IsNotThrownByTheShotBrighteningBetweenHalves) {
  Image brighter = interview(166, 96);
  for (float& p : brighter.pixels) p += 0.08f;
  const Point head = dense_flow(interview(150, 90), brighter).at(150.0 / kWidth, 90.0 / kHeight);
  EXPECT_NEAR(head.x * kWidth, 16.0, 3.0);
  EXPECT_NEAR(head.y * kHeight, 6.0, 3.0);
}

TEST(MorphAnalysisFile, IsSavedAndReadBack) {
  MorphAnalysis saved{.fps = 24.0};
  saved.frames.push_back(dense_flow(interview(150, 90), interview(160, 90)));
  saved.frames.push_back(FlowField{.columns = 2, .rows = 1, .moves = std::vector<Point>(6)});
  const auto path = std::filesystem::temp_directory_path() / "cutline_morph_test.cmca";
  ASSERT_TRUE(save_morph(saved, path));
  const auto loaded = load_morph(path);
  std::filesystem::remove(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_DOUBLE_EQ(loaded->fps, 24.0);
  ASSERT_EQ(loaded->frames.size(), 2u);
  EXPECT_EQ(loaded->frames[1].columns, 2);
  ASSERT_EQ(loaded->frames[0].moves.size(), saved.frames[0].moves.size());
  EXPECT_NEAR(loaded->frames[0].moves[100].x, saved.frames[0].moves[100].x, 1e-6);
}

}  // namespace
}  // namespace cutline::track
