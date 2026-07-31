/// The video scopes.
///
/// All arithmetic and no pixels to look at, which is exactly why these can be
/// checked properly: a frame of one known colour has a histogram anybody can
/// work out by hand, and a scope that disagrees with it is wrong rather than
/// merely different.

#include "cutline/render/scopes.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

namespace cutline::render {
namespace {

/// A frame of one colour.
struct Frame {
  Frame(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
      : width(w), height(h) {
    pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
      pixels[i] = r;
      pixels[i + 1] = g;
      pixels[i + 2] = b;
      pixels[i + 3] = a;
    }
  }

  [[nodiscard]] ScopeImage view() const {
    return ScopeImage{.pixels = pixels.data(), .width = width, .height = height};
  }

  void set(int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4;
    pixels[at] = r;
    pixels[at + 1] = g;
    pixels[at + 2] = b;
  }

  int width;
  int height;
  std::vector<std::uint8_t> pixels;
};

[[nodiscard]] std::uint32_t total(const std::array<std::uint32_t, 256>& bins) {
  return std::accumulate(bins.begin(), bins.end(), std::uint32_t{0});
}

// --------------------------------------------------------------- nothing --

TEST(Scopes, AnEmptyImageMeasuresNothing) {
  const ScopeImage nothing;
  EXPECT_EQ(compute_histogram(nothing).peak(), 0u);
  EXPECT_TRUE(compute_waveform(nothing, ScopeChannel::Luma).empty());
  EXPECT_TRUE(compute_vectorscope(nothing).empty());
  EXPECT_TRUE(compute_parade(nothing).red.empty());
}

// ------------------------------------------------------------- the luma --

TEST(Scopes, LumaIsTheReferencesBT601) {
  EXPECT_DOUBLE_EQ(luma_of(255, 255, 255), 255.0 * (0.299 + 0.587 + 0.114));
  EXPECT_DOUBLE_EQ(luma_of(0, 0, 0), 0.0);
  // Green weighs most, which is the whole character of this matrix.
  EXPECT_GT(luma_of(0, 255, 0), luma_of(255, 0, 0));
  EXPECT_GT(luma_of(255, 0, 0), luma_of(0, 0, 255));
}

// --------------------------------------------------------- the histogram --

TEST(Scopes, EveryPixelIsCountedOnce) {
  const Frame frame(16, 8, 10, 20, 30);
  const Histogram histogram = compute_histogram(frame.view());
  const std::uint32_t pixels = 16 * 8;

  EXPECT_EQ(total(histogram.red), pixels);
  EXPECT_EQ(total(histogram.green), pixels);
  EXPECT_EQ(total(histogram.blue), pixels);
  EXPECT_EQ(total(histogram.luma), pixels);
}

TEST(Scopes, AFlatColourLandsInOneBinPerChannel) {
  const Frame frame(4, 4, 10, 20, 30);
  const Histogram histogram = compute_histogram(frame.view());

  EXPECT_EQ(histogram.red[10], 16u);
  EXPECT_EQ(histogram.green[20], 16u);
  EXPECT_EQ(histogram.blue[30], 16u);
  EXPECT_EQ(histogram.peak(), 16u);
}

TEST(Scopes, BlackAndWhiteSitAtTheEnds) {
  const Frame black(2, 2, 0, 0, 0);
  EXPECT_EQ(compute_histogram(black.view()).luma[0], 4u);

  const Frame white(2, 2, 255, 255, 255);
  EXPECT_EQ(compute_histogram(white.view()).luma[255], 4u)
      << "full white must not round past the last bin";
}

TEST(Scopes, TheLumaTraceUsesTheSameMatrixAsLumaOf) {
  const Frame frame(2, 2, 200, 100, 50);
  const Histogram histogram = compute_histogram(frame.view());
  const auto expected = static_cast<std::size_t>(std::lround(luma_of(200, 100, 50)));
  EXPECT_EQ(histogram.luma[expected], 4u);
}

// ---------------------------------------------------------- the waveform --

TEST(Scopes, AWaveformHasAColumnPerColumnOfThePicture) {
  const Frame frame(12, 5, 128, 128, 128);
  const Waveform wave = compute_waveform(frame.view(), ScopeChannel::Luma);

  EXPECT_EQ(wave.columns, 12);
  EXPECT_EQ(wave.cells.size(), 12u * Waveform::kLevels);
  // Every pixel of a column at one level, and that column is five tall.
  EXPECT_EQ(wave.at(0, 128), 5u);
  EXPECT_EQ(wave.peak(), 5u);
}

// The reason a waveform exists rather than a second histogram: it says *where*
// across the picture the levels are.
TEST(Scopes, AWaveformSaysWhichColumnIsBright) {
  Frame frame(4, 2, 0, 0, 0);
  frame.set(2, 0, 255, 255, 255);
  frame.set(2, 1, 255, 255, 255);

  const Waveform wave = compute_waveform(frame.view(), ScopeChannel::Luma);
  EXPECT_EQ(wave.at(2, 255), 2u);
  EXPECT_EQ(wave.at(0, 255), 0u);
  EXPECT_EQ(wave.at(0, 0), 2u);
}

TEST(Scopes, EachChannelMeasuresItsOwn) {
  const Frame frame(3, 1, 200, 100, 50);

  EXPECT_EQ(compute_waveform(frame.view(), ScopeChannel::Red).at(0, 200), 1u);
  EXPECT_EQ(compute_waveform(frame.view(), ScopeChannel::Green).at(0, 100), 1u);
  EXPECT_EQ(compute_waveform(frame.view(), ScopeChannel::Blue).at(0, 50), 1u);
}

TEST(Scopes, ReadingOutsideAWaveformIsHarmless) {
  const Frame frame(2, 2, 0, 0, 0);
  const Waveform wave = compute_waveform(frame.view(), ScopeChannel::Luma);

  EXPECT_EQ(wave.at(-1, 0), 0u);
  EXPECT_EQ(wave.at(99, 0), 0u);
  EXPECT_EQ(wave.at(0, Waveform::kLevels), 0u);
}

TEST(Scopes, AParadeIsTheThreeColourWaveforms) {
  const Frame frame(3, 2, 200, 100, 50);
  const Parade parade = compute_parade(frame.view());

  EXPECT_EQ(parade.red.at(0, 200), 2u);
  EXPECT_EQ(parade.green.at(0, 100), 2u);
  EXPECT_EQ(parade.blue.at(0, 50), 2u);
}

// ------------------------------------------------------- the vectorscope --

TEST(Scopes, GreyLandsAtTheCentre) {
  // Any grey: chroma is zero whatever the brightness, which is the whole
  // premise of reading a cast off this scope.
  for (const std::uint8_t level : {std::uint8_t{0}, std::uint8_t{128}, std::uint8_t{255}}) {
    const Frame frame(4, 4, level, level, level);
    const Vectorscope scope = compute_vectorscope(frame.view());
    EXPECT_EQ(scope.at(128, 128), 16u) << "grey at " << static_cast<int>(level);
    EXPECT_EQ(scope.peak(), 16u);
  }
}

TEST(Scopes, ASaturatedColourReachesOutFromTheCentre) {
  const Frame red(4, 4, 255, 0, 0);
  const Vectorscope scope = compute_vectorscope(red.view());

  // Somewhere well away from the middle, and only in one place.
  std::size_t occupied = 0;
  int found_x = -1;
  int found_y = -1;
  for (int y = 0; y < Vectorscope::kSize; ++y) {
    for (int x = 0; x < Vectorscope::kSize; ++x) {
      if (scope.at(x, y) == 0) continue;
      ++occupied;
      found_x = x;
      found_y = y;
    }
  }
  EXPECT_EQ(occupied, 1u);
  EXPECT_GT(std::abs(found_x - 128) + std::abs(found_y - 128), 60);
}

// Red toward the top and blue toward the bottom, which is the orientation every
// vectorscope is read in.
TEST(Scopes, RedIsAboveTheCentreAndBlueBelow) {
  const auto centre_of = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const Frame frame(2, 2, r, g, b);
    const Vectorscope scope = compute_vectorscope(frame.view());
    for (int y = 0; y < Vectorscope::kSize; ++y) {
      for (int x = 0; x < Vectorscope::kSize; ++x) {
        if (scope.at(x, y) > 0) return y;
      }
    }
    return -1;
  };

  EXPECT_LT(centre_of(255, 0, 0), 128) << "red should be above the middle";
  EXPECT_GT(centre_of(0, 0, 255), 128) << "blue should be below it";
}

TEST(Scopes, ReadingOutsideTheVectorscopeIsHarmless) {
  const Frame frame(2, 2, 0, 0, 0);
  const Vectorscope scope = compute_vectorscope(frame.view());

  EXPECT_EQ(scope.at(-1, 0), 0u);
  EXPECT_EQ(scope.at(0, -1), 0u);
  EXPECT_EQ(scope.at(Vectorscope::kSize, 0), 0u);
}

// The targets are worked out through the same matrix the scatter uses, so a
// colour must land on its own target rather than near it.
TEST(Scopes, EachPrimaryLandsOnItsOwnTarget) {
  const std::array<std::array<std::uint8_t, 3>, 6> corners{{
      {255, 0, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255}, {0, 0, 255}, {255, 0, 255},
  }};
  const std::array<VectorTarget, 6> targets = vector_targets();

  for (std::size_t i = 0; i < corners.size(); ++i) {
    const Frame frame(2, 2, corners[i][0], corners[i][1], corners[i][2]);
    const Vectorscope scope = compute_vectorscope(frame.view());

    const auto x = static_cast<int>(std::lround(targets[i].x * Vectorscope::kSize));
    const auto y = static_cast<int>(std::lround(targets[i].y * Vectorscope::kSize));
    EXPECT_GT(scope.at(x, y), 0u) << "corner " << i << " missed its target";
  }
}

TEST(Scopes, TheTargetsRingTheCentre) {
  for (const VectorTarget& target : vector_targets()) {
    const double dx = target.x - 0.5;
    const double dy = target.y - 0.5;
    EXPECT_GT(std::hypot(dx, dy), 0.2) << "a target sat too near the middle to aim at";
    EXPECT_LT(std::hypot(dx, dy), 0.5) << "and one fell off the square";
  }
}

// ------------------------------------------------------------- the stride --

TEST(Scopes, APaddedRowIsReadAtItsOwnPitch) {
  // What a decoded frame usually looks like: rows wider than the picture.
  constexpr int kWidth = 3;
  constexpr int kHeight = 2;
  constexpr int kStride = kWidth * 4 + 12;

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kStride) * kHeight, 0);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      pixels[static_cast<std::size_t>(y) * kStride + static_cast<std::size_t>(x) * 4] = 200;
    }
    // Junk in the padding, which must not be counted.
    for (int b = kWidth * 4; b < kStride; ++b) {
      pixels[static_cast<std::size_t>(y) * kStride + static_cast<std::size_t>(b)] = 99;
    }
  }

  const ScopeImage image{
      .pixels = pixels.data(), .width = kWidth, .height = kHeight, .stride = kStride};
  const Histogram histogram = compute_histogram(image);

  EXPECT_EQ(total(histogram.red), static_cast<std::uint32_t>(kWidth * kHeight));
  EXPECT_EQ(histogram.red[200], 6u);
  EXPECT_EQ(histogram.red[99], 0u) << "the padding was measured";
}

}  // namespace
}  // namespace cutline::render
