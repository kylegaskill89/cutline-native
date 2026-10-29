/// Auto Reframe's looking, on frames drawn here: a figure somewhere in a
/// plain room, a figure walking while the camera pans across a patterned
/// wall, and nothing at all. The window should find the figure, ignore the
/// pan, and leave an empty frame framed as it was shot.

#include "cutline/track/reframe.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <vector>

namespace cutline::track {
namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 180;
/// A 9:16 window of a 16:9 frame: the whole height, under a third of the width.
constexpr double kVertical = (9.0 / 16.0) / (16.0 / 9.0);

struct Frame {
  std::vector<std::uint8_t> rgba = std::vector<std::uint8_t>(kWidth * kHeight * 4, 0);

  void fill(int r, int g, int b) {
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
      rgba[i] = static_cast<std::uint8_t>(r);
      rgba[i + 1] = static_cast<std::uint8_t>(g);
      rgba[i + 2] = static_cast<std::uint8_t>(b);
      rgba[i + 3] = 255;
    }
  }

  /// A wall of vertical stripes, shifted `offset` pixels, which a pan moves.
  void stripes(int offset) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const int band = ((x + offset) / 12) % 2;
        const auto v = static_cast<std::uint8_t>(band != 0 ? 150 : 110);
        std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * kWidth + x) * 4];
        p[0] = v;
        p[1] = v;
        p[2] = v;
        p[3] = 255;
      }
    }
  }

  /// A figure: a skin-toned head over a blue body, centred at `cx`.
  void figure(int cx) {
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * kWidth + x) * 4];
        const double hx = (x - cx) / 14.0;
        const double hy = (y - 50) / 18.0;
        if (hx * hx + hy * hy <= 1.0) {
          p[0] = 224;
          p[1] = 172;
          p[2] = 140;
        } else if (std::abs(x - cx) <= 22 && y > 68 && y < 170) {
          p[0] = 40;
          p[1] = 60;
          p[2] = 170;
        }
      }
    }
  }
};

TEST(SubjectFinder, PutsAVerticalWindowOverAFigureStandingToOneSide) {
  SubjectFinder finder(kVertical, 1.0);
  Frame frame;
  frame.fill(90, 90, 90);
  frame.figure(250);
  const Point at = finder.add(frame.rgba, kWidth, kHeight, true);
  EXPECT_NEAR(at.x, 250.0 / kWidth, 0.05);
}

TEST(SubjectFinder, FindsASubjectFarSmallerThanTheFrame) {
  // Under a twentieth of the frame: a scale set by the 95th percentile alone
  // put it with the background and scaled the lot to nothing.
  SubjectFinder finder(kVertical, 1.0);
  Frame frame;
  frame.fill(90, 90, 90);
  for (int y = 80; y < 100; ++y) {
    for (int x = 50; x < 70; ++x) {
      std::uint8_t* p = &frame.rgba[(static_cast<std::size_t>(y) * kWidth + x) * 4];
      p[0] = 224;
      p[1] = 172;
      p[2] = 140;
    }
  }
  const Point at = finder.add(frame.rgba, kWidth, kHeight, true);
  EXPECT_NEAR(at.x, 60.0 / kWidth, 0.03);
}

TEST(SubjectFinder, FollowsTheFigureAndNotTheCamerasPan) {
  SubjectFinder finder(kVertical, 1.0);
  Point at;
  // The camera pans right three pixels a frame while the figure walks left
  // across the wall, from the right third towards the middle.
  for (int i = 0; i < 20; ++i) {
    Frame frame;
    frame.stripes(i * 3);
    frame.figure(260 - i * 4);
    at = finder.add(frame.rgba, kWidth, kHeight, i == 0);
  }
  EXPECT_NEAR(at.x, (260.0 - 19 * 4) / kWidth, 0.06);
}

TEST(SubjectFinder, LeavesAFrameWithNothingInItFramedAsItWasShot) {
  SubjectFinder finder(kVertical, 1.0);
  Frame frame;
  frame.fill(120, 120, 120);
  const Point at = finder.add(frame.rgba, kWidth, kHeight, true);
  EXPECT_NEAR(at.x, 0.5, 0.02);
  EXPECT_NEAR(at.y, 0.5, 0.02);
}

/// Two seconds of subjects at ten a second.
[[nodiscard]] ReframeAnalysis analysis_of(std::vector<Point> subjects, std::vector<int> cuts = {}) {
  return ReframeAnalysis{.fps = 10.0,
                         .window_width = kVertical,
                         .window_height = 1.0,
                         .subjects = std::move(subjects),
                         .cuts = std::move(cuts)};
}

TEST(ReframePath, SlowerMotionHoldsStillWhileTheSubjectStaysInTheWindow) {
  std::vector<Point> jittering;
  for (int i = 0; i < 40; ++i) jittering.push_back({0.6 + 0.03 * std::sin(i * 1.7), 0.5});
  const std::vector<Point> path = reframe_path(analysis_of(jittering), ReframeMotion::Slower);
  for (const Point& p : path) EXPECT_DOUBLE_EQ(p.x, path.front().x);
  EXPECT_NEAR(path.front().x, 0.6, 0.03);
}

TEST(ReframePath, FasterMotionKeepsUpWithASubjectThatMoves) {
  std::vector<Point> walking;
  for (int i = 0; i < 40; ++i) walking.push_back({0.25 + 0.5 * i / 39.0, 0.5});
  const std::vector<Point> faster = reframe_path(analysis_of(walking), ReframeMotion::Faster);
  const std::vector<Point> slower = reframe_path(analysis_of(walking), ReframeMotion::Slower);
  // Mid-walk, the faster path is about where the subject is.
  EXPECT_NEAR(faster[20].x, walking[20].x, 0.02);
  // And it travels the whole way more nearly than the slower one does.
  EXPECT_GT(faster[39].x - faster[0].x, slower[39].x - slower[0].x);
}

TEST(ReframePath, JumpsAtACutRatherThanDriftingAcrossIt) {
  std::vector<Point> two_shots(40, Point{0.25, 0.5});
  for (std::size_t i = 20; i < 40; ++i) two_shots[i] = {0.75, 0.5};
  for (const ReframeMotion motion :
       {ReframeMotion::Slower, ReframeMotion::Default, ReframeMotion::Faster}) {
    const std::vector<Point> path = reframe_path(analysis_of(two_shots, {20}), motion);
    EXPECT_NEAR(path[19].x, 0.25, 1e-6);
    EXPECT_NEAR(path[20].x, 0.75, 1e-6);
  }
}

TEST(ReframePath, NeverPutsTheWindowPartlyOutsideTheFrame) {
  const std::vector<Point> path =
      reframe_path(analysis_of(std::vector<Point>(20, Point{0.98, 0.5})), ReframeMotion::Faster);
  for (const Point& p : path) EXPECT_LE(p.x + kVertical / 2.0, 1.0 + 1e-9);
}

TEST(ReframeAnalysisFile, IsSavedAndReadBack) {
  const ReframeAnalysis saved = analysis_of({{0.1, 0.2}, {0.3, 0.4}, {0.5, 0.6}}, {2});
  const auto path = std::filesystem::temp_directory_path() / "cutline_reframe_test.cara";
  ASSERT_TRUE(save_reframe(saved, path));
  const auto loaded = load_reframe(path);
  std::filesystem::remove(path);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_DOUBLE_EQ(loaded->window_width, saved.window_width);
  EXPECT_EQ(loaded->cuts, saved.cuts);
  ASSERT_EQ(loaded->subjects.size(), 3u);
  EXPECT_NEAR(loaded->subjects[2].y, 0.6, 1e-6);
}

}  // namespace
}  // namespace cutline::track
