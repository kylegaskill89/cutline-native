/// The stabiliser on analyses made here: corners on a shaking camera, whose
/// shake is known, and the warp that takes it out.

#include "cutline/track/stabilize.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <random>

namespace cutline::track {
namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 360;
constexpr std::size_t kFrames = 96;

/// A camera panning steadily right, two pixels a frame, with a shake of a
/// few pixels on top. Returns the shake per frame as well.
struct Shot {
  Analysis analysis;
  std::vector<Point> pan;
  std::vector<Point> shake;
};

/// `depth` gives each corner a second, nearer plane: those move `depth` times
/// as far as the far ones, as a scene with parallax does.
[[nodiscard]] Shot shaky(double readout = 0.0, double depth = 1.0) {
  Shot shot;
  shot.analysis.width = kWidth;
  shot.analysis.height = kHeight;
  shot.analysis.fps = 24.0;
  std::mt19937 random(11);
  std::uniform_real_distribution<double> x(-200.0, kWidth + 200.0);
  std::uniform_real_distribution<double> y(20.0, kHeight - 20.0);
  std::normal_distribution<double> jitter(0.0, 3.0);
  std::vector<Point> world;
  for (int k = 0; k < 400; ++k) world.push_back({x(random), y(random)});
  Point previous_offset{};
  for (std::size_t i = 0; i < kFrames; ++i) {
    const Point pan{-2.0 * static_cast<double>(i), 0.0};
    const Point shake{jitter(random), jitter(random)};
    shot.pan.push_back(pan);
    shot.shake.push_back(shake);
    const Point offset{pan.x + shake.x, pan.y + shake.y};
    const Point speed = i == 0 ? Point{-2.0, 0.0}
                               : Point{offset.x - previous_offset.x, offset.y - previous_offset.y};
    previous_offset = offset;
    std::vector<TrackPoint> frame;
    for (std::size_t k = 0; k < world.size(); ++k) {
      const double near = k % 2 == 0 ? 1.0 : depth;
      Point p{world[k].x + offset.x * near, world[k].y + offset.y * near};
      const double late = readout * (p.y / kHeight - 0.5);
      p = {p.x + speed.x * late, p.y + speed.y * late};
      if (p.x < 2 || p.y < 2 || p.x > kWidth - 2 || p.y > kHeight - 2) continue;
      frame.push_back(TrackPoint{.id = static_cast<std::uint32_t>(k),
                                 .x = static_cast<float>(p.x),
                                 .y = static_cast<float>(p.y)});
    }
    shot.analysis.frames.push_back(std::move(frame));
  }
  return shot;
}

/// Where the warp draws the middle of the frame from, in pixels.
[[nodiscard]] Point middle_source(const WarpGrid& grid) {
  const Point s = grid.at(0.5, 0.5);
  return {s.x * kWidth, s.y * kHeight};
}

/// The output place a source point is drawn at, found by walking back
/// through the warp.
[[nodiscard]] Point shown_at(const WarpGrid& grid, Point source) {
  Point out{source.x / kWidth, source.y / kHeight};
  for (int k = 0; k < 40; ++k) {
    const Point s = grid.at(out.x, out.y);
    out.x -= s.x - source.x / kWidth;
    out.y -= s.y - source.y / kHeight;
  }
  return {out.x * kWidth, out.y * kHeight};
}

/// How much a run of places jumps about: the root of the mean square of its
/// second differences.
[[nodiscard]] double jumpiness(const std::vector<Point>& places) {
  double sum = 0.0;
  for (std::size_t i = 1; i + 1 < places.size(); ++i) {
    const double ax = places[i + 1].x - 2 * places[i].x + places[i - 1].x;
    const double ay = places[i + 1].y - 2 * places[i].y + places[i - 1].y;
    sum += ax * ax + ay * ay;
  }
  return std::sqrt(sum / static_cast<double>(places.size() - 2));
}

[[nodiscard]] StabilizeSettings settings(StabilizeMethod method,
                                         Framing framing = Framing::StabilizeOnly) {
  return StabilizeSettings{.method = method, .framing = framing};
}

TEST(Stabilize, TheShakeIsTakenOutAndThePanKept) {
  const Shot shot = shaky();
  const Stabilization result = stabilize(shot.analysis, settings(StabilizeMethod::Position));
  ASSERT_EQ(result.frames.size(), kFrames);
  // The wanted path is the camera less its correction: the source the middle
  // is drawn from is the middle moved by the shake, less what smoothing kept.
  std::vector<Point> camera;
  std::vector<Point> wanted;
  for (std::size_t i = 0; i < kFrames; ++i) {
    const Point s = middle_source(result.frames[i]);
    const Point off{shot.pan[i].x + shot.shake[i].x, shot.pan[i].y + shot.shake[i].y};
    camera.push_back(off);
    wanted.push_back({off.x - (s.x - kWidth / 2.0), off.y - (s.y - kHeight / 2.0)});
  }
  EXPECT_LT(jumpiness(wanted), jumpiness(camera) / 8.0);
  // The pan is still there: the wanted path still moves right over the shot.
  EXPECT_LT(wanted.back().x - wanted.front().x, -150.0);
}

TEST(Stabilize, NoMotionHoldsTheMiddleFrameStill) {
  const Shot shot = shaky();
  StabilizeSettings s = settings(StabilizeMethod::Position);
  s.result = StabilizeResult::NoMotion;
  const Stabilization result = stabilize(shot.analysis, s);
  const std::size_t mid = kFrames / 2;
  for (const std::size_t i : {std::size_t{0}, std::size_t{30}, kFrames - 1}) {
    const Point source = middle_source(result.frames[i]);
    const double expected = kWidth / 2.0 + (shot.pan[i].x + shot.shake[i].x) -
                            (shot.pan[mid].x + shot.shake[mid].x);
    EXPECT_NEAR(source.x, expected, 0.5) << i;
  }
}

TEST(Stabilize, AutoScaleZoomsJustEnoughToCoverTheShake) {
  const Shot shot = shaky();
  const Stabilization only = stabilize(shot.analysis, settings(StabilizeMethod::Position));
  EXPECT_EQ(only.auto_scale, 1.0);
  const Stabilization scaled = stabilize(
      shot.analysis, settings(StabilizeMethod::Position, Framing::StabilizeCropAutoScale));
  EXPECT_GT(scaled.auto_scale, 1.0);
  EXPECT_LT(scaled.auto_scale, 1.2);
  // And every frame then draws from inside the picture at its corners.
  for (const WarpGrid& grid : scaled.frames) {
    for (const Point corner : {grid.at(0, 0), grid.at(1, 0), grid.at(0, 1), grid.at(1, 1)}) {
      EXPECT_GE(corner.x, -0.002);
      EXPECT_LE(corner.x, 1.002);
      EXPECT_GE(corner.y, -0.002);
      EXPECT_LE(corner.y, 1.002);
    }
  }
}

TEST(Stabilize, CropLeavesTheFrameUnscaledAndCutsItInstead) {
  const Shot shot = shaky();
  const Stabilization cropped =
      stabilize(shot.analysis, settings(StabilizeMethod::Position, Framing::StabilizeCrop));
  EXPECT_EQ(cropped.auto_scale, 1.0);
  EXPECT_LT(cropped.visible, 1.0);
}

TEST(Stabilize, MaximumScaleLimitsTheZoom) {
  const Shot shot = shaky();
  StabilizeSettings s = settings(StabilizeMethod::Position, Framing::StabilizeCropAutoScale);
  s.maximum_scale = 1.01;
  EXPECT_DOUBLE_EQ(stabilize(shot.analysis, s).auto_scale, 1.01);
}

TEST(Stabilize, ARollingShuttersLeanIsFound) {
  const Shot shot = shaky(0.6);
  StabilizeSettings s = settings(StabilizeMethod::Position);
  s.rolling_shutter = RollingShutter::Enhanced;
  EXPECT_NEAR(stabilize(shot.analysis, s).readout, 0.6, 0.1);
  // And footage with none is given none.
  EXPECT_EQ(stabilize(shaky().analysis, s).readout, 0.0);
}

TEST(Stabilize, SubspaceWarpSteadiesAScenesDepthWhereOneTransformCannot) {
  // Near corners move half as far again as far ones: no single transform
  // steadies both.
  const Shot shot = shaky(0.0, 1.5);
  const auto steadiness = [&](StabilizeMethod method) {
    const Stabilization result = stabilize(shot.analysis, settings(method));
    double total = 0.0;
    int counted = 0;
    // Twenty corners of each depth, followed through the whole shot.
    for (std::uint32_t id = 0; id < 40; ++id) {
      std::vector<Point> shown;
      for (std::size_t i = 0; i < kFrames; ++i) {
        const auto& frame = shot.analysis.frames[i];
        const auto found = std::ranges::find(frame, id, &TrackPoint::id);
        if (found == frame.end()) break;
        shown.push_back(shown_at(result.frames[i], {found->x, found->y}));
      }
      if (shown.size() < 40) continue;
      total += jumpiness(shown);
      ++counted;
    }
    EXPECT_GT(counted, 4);
    return total / std::max(counted, 1);
  };
  const double perspective = steadiness(StabilizeMethod::Perspective);
  const double subspace = steadiness(StabilizeMethod::SubspaceWarp);
  EXPECT_LT(subspace, perspective * 0.7);
}

TEST(Stabilize, AnAnalysisIsWrittenAndReadBack) {
  const Shot shot = shaky();
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("cutline_analysis_" + std::to_string(_getpid()) + ".cwsa");
  ASSERT_TRUE(save_analysis(shot.analysis, path));
  const std::optional<Analysis> read = load_analysis(path);
  std::filesystem::remove(path);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(*read, shot.analysis);
}

TEST(Stabilize, AFileThatIsNotAnAnalysisIsRefused) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("cutline_not_" + std::to_string(_getpid()) + ".cwsa");
  { std::ofstream(path) << "hello"; }
  EXPECT_FALSE(load_analysis(path).has_value());
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace cutline::track
