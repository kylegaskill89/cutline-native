/// Scene Edit Detection's looking, on footage made here: textured shots that
/// pan, cut together at known frames, with the things that are not cuts — a
/// fast pan, a flash, grain — put in to be ignored.

#include "cutline/track/scenes.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace cutline::track {
namespace {

constexpr int kWidth = 256;
constexpr int kHeight = 144;
constexpr double kFps = 30.0;

/// A shot: soft blobs over a tint, wider than the frame so it can pan.
struct Shot {
  int width = kWidth * 3;
  std::vector<float> rgb;
};

[[nodiscard]] Shot shot(unsigned seed, float r, float g, float b) {
  Shot out;
  out.rgb.resize(static_cast<std::size_t>(out.width) * kHeight * 3);
  std::mt19937 random(seed);
  std::uniform_real_distribution<float> shade(-40.0F, 40.0F);
  std::uniform_int_distribution<int> x(0, out.width - 1);
  std::uniform_int_distribution<int> y(0, kHeight - 1);
  for (int j = 0; j < kHeight; ++j) {
    for (int i = 0; i < out.width; ++i) {
      const std::size_t at = (static_cast<std::size_t>(j) * out.width + i) * 3;
      out.rgb[at] = r;
      out.rgb[at + 1] = g;
      out.rgb[at + 2] = b;
    }
  }
  for (int blob = 0; blob < 300; ++blob) {
    const int cx = x(random);
    const int cy = y(random);
    const float s = shade(random);
    for (int j = std::max(0, cy - 8); j < std::min(kHeight, cy + 8); ++j) {
      for (int i = std::max(0, cx - 8); i < std::min(out.width, cx + 8); ++i) {
        const float d2 = static_cast<float>((i - cx) * (i - cx) + (j - cy) * (j - cy));
        const float k = s * std::exp(-d2 / 18.0F);
        const std::size_t at = (static_cast<std::size_t>(j) * out.width + i) * 3;
        for (int c = 0; c < 3; ++c) out.rgb[at + c] += k;
      }
    }
  }
  return out;
}

/// The frame of `s` seen from `offset` pixels along, with grain on it.
[[nodiscard]] std::vector<std::uint8_t> frame(const Shot& s, int offset, std::mt19937& grain,
                                              float noise = 3.0F) {
  std::normal_distribution<float> n(0.0F, noise);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(kWidth) * kHeight * 4);
  for (int j = 0; j < kHeight; ++j) {
    for (int i = 0; i < kWidth; ++i) {
      const int from = std::clamp(i + offset, 0, s.width - 1);
      const std::size_t at = (static_cast<std::size_t>(j) * s.width + from) * 3;
      const std::size_t to = (static_cast<std::size_t>(j) * kWidth + i) * 4;
      for (int c = 0; c < 3; ++c) {
        out[to + c] = static_cast<std::uint8_t>(std::clamp(s.rgb[at + c] + n(grain), 0.0F, 255.0F));
      }
      out[to + 3] = 255;
    }
  }
  return out;
}

}  // namespace

TEST(SceneDetection, FindsEachHardCutOnTheFrameItHappens) {
  const Shot a = shot(1, 120, 90, 60);
  const Shot b = shot(2, 60, 110, 150);
  const Shot c = shot(3, 170, 160, 150);
  std::mt19937 grain(9);
  SceneDetector detector(kFps);
  for (int i = 0; i < 90; ++i) {
    const Shot& s = i < 30 ? a : i < 55 ? b : c;
    detector.add(frame(s, i * 2, grain), kWidth, kHeight);
  }
  EXPECT_EQ(detector.cuts(), (std::vector<int>{30, 55}));
}

TEST(SceneDetection, AFastPanIsOneShot) {
  const Shot a = shot(4, 100, 120, 90);
  std::mt19937 grain(10);
  SceneDetector detector(kFps);
  for (int i = 0; i < 60; ++i) detector.add(frame(a, i * 8, grain), kWidth, kHeight);
  EXPECT_TRUE(detector.cuts().empty());
}

TEST(SceneDetection, AFlashIsNotACut) {
  const Shot a = shot(5, 90, 90, 110);
  const Shot white = shot(6, 250, 250, 250);
  std::mt19937 grain(11);
  SceneDetector detector(kFps);
  for (int i = 0; i < 40; ++i) {
    detector.add(frame(i == 20 ? white : a, i, grain), kWidth, kHeight);
  }
  EXPECT_TRUE(detector.cuts().empty());
}

TEST(SceneDetection, ACutBetweenTwoDimShotsIsStillFound) {
  const Shot a = shot(7, 20, 25, 45);
  const Shot b = shot(8, 25, 45, 20);
  std::mt19937 grain(12);
  SceneDetector detector(kFps);
  for (int i = 0; i < 50; ++i) detector.add(frame(i < 25 ? a : b, i, grain), kWidth, kHeight);
  EXPECT_EQ(detector.cuts(), (std::vector<int>{25}));
}

TEST(SceneDetection, GrainInAStillShotIsNotACut) {
  const Shot a = shot(9, 128, 128, 128);
  std::mt19937 grain(13);
  SceneDetector detector(kFps);
  for (int i = 0; i < 60; ++i) detector.add(frame(a, 0, grain, 8.0F), kWidth, kHeight);
  EXPECT_TRUE(detector.cuts().empty());
}

TEST(SceneDetection, ScoresTheFirstFrameAsNoChange) {
  const Shot a = shot(10, 100, 100, 100);
  std::mt19937 grain(14);
  SceneDetector detector(kFps);
  detector.add(frame(a, 0, grain), kWidth, kHeight);
  ASSERT_EQ(detector.scores().size(), 1u);
  EXPECT_EQ(detector.scores().front(), 0.0);
  EXPECT_TRUE(detector.cuts().empty());
}

}  // namespace cutline::track
