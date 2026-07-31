#include "cutline/render/scopes.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::render {
namespace {

/// BT.601 chroma, the same matrix the spec gives for the chroma keyer. Centred
/// on zero and reaching about +-128, which is what maps onto the square.
struct Chroma {
  double cb = 0.0;
  double cr = 0.0;
};

[[nodiscard]] Chroma chroma_of(double r, double g, double b) noexcept {
  return Chroma{.cb = -0.169 * r - 0.331 * g + 0.5 * b,
                .cr = 0.5 * r - 0.419 * g - 0.081 * b};
}

/// How far out the square reaches, in chroma.
///
/// Not 128. A fully saturated primary is further from grey than either axis
/// alone suggests — green and magenta both sit at a radius of about 136 — so a
/// square that stopped at 128 would clamp the very colours a vectorscope exists
/// to show, and pile every one of them onto the edge. This reaches past the
/// furthest of them, which puts 100% saturation just inside the rim, where a
/// vectorscope's outer ring has always been.
constexpr double kChromaReach = 140.0;

/// Chroma to a position across the square, 0 to 1, with grey at the centre.
[[nodiscard]] double across(double chroma) noexcept {
  return 0.5 + 0.5 * (chroma / kChromaReach);
}

/// The value a channel contributes, so the waveform's four modes are one loop.
[[nodiscard]] double channel_of(ScopeChannel channel, std::uint8_t r, std::uint8_t g,
                                std::uint8_t b) noexcept {
  switch (channel) {
    case ScopeChannel::Red: return r;
    case ScopeChannel::Green: return g;
    case ScopeChannel::Blue: return b;
    case ScopeChannel::Luma: break;
  }
  return luma_of(r, g, b);
}

/// A level clamped into the bins there actually are. Rounding at 255.5 would
/// otherwise index one past the end for a pixel at full white.
[[nodiscard]] std::size_t level_of(double value) noexcept {
  const auto index = static_cast<long long>(std::lround(value));
  return static_cast<std::size_t>(std::clamp<long long>(index, 0, 255));
}

}  // namespace

double luma_of(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
  return 0.299 * r + 0.587 * g + 0.114 * b;
}

std::uint32_t Histogram::peak() const noexcept {
  std::uint32_t most = 0;
  for (const auto* channel : {&red, &green, &blue, &luma}) {
    most = std::max(most, *std::ranges::max_element(*channel));
  }
  return most;
}

Histogram compute_histogram(const ScopeImage& image) {
  Histogram out;
  if (image.empty()) return out;

  for (int y = 0; y < image.height; ++y) {
    const std::uint8_t* row = image.pixels + static_cast<std::ptrdiff_t>(y) * image.row_bytes();
    for (int x = 0; x < image.width; ++x) {
      const std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
      ++out.red[pixel[0]];
      ++out.green[pixel[1]];
      ++out.blue[pixel[2]];
      ++out.luma[level_of(luma_of(pixel[0], pixel[1], pixel[2]))];
    }
  }
  return out;
}

std::uint32_t Waveform::at(int column, std::size_t level) const noexcept {
  if (column < 0 || column >= columns || level >= kLevels) return 0;
  return cells[static_cast<std::size_t>(column) * kLevels + level];
}

std::uint32_t Waveform::peak() const noexcept {
  if (cells.empty()) return 0;
  return *std::ranges::max_element(cells);
}

Waveform compute_waveform(const ScopeImage& image, ScopeChannel channel) {
  Waveform out;
  if (image.empty()) return out;

  out.columns = image.width;
  out.cells.assign(static_cast<std::size_t>(image.width) * Waveform::kLevels, 0);

  for (int y = 0; y < image.height; ++y) {
    const std::uint8_t* row = image.pixels + static_cast<std::ptrdiff_t>(y) * image.row_bytes();
    for (int x = 0; x < image.width; ++x) {
      const std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
      const std::size_t level = level_of(channel_of(channel, pixel[0], pixel[1], pixel[2]));
      ++out.cells[static_cast<std::size_t>(x) * Waveform::kLevels + level];
    }
  }
  return out;
}

Parade compute_parade(const ScopeImage& image) {
  return Parade{.red = compute_waveform(image, ScopeChannel::Red),
                .green = compute_waveform(image, ScopeChannel::Green),
                .blue = compute_waveform(image, ScopeChannel::Blue)};
}

std::uint32_t Vectorscope::at(int x, int y) const noexcept {
  if (cells.empty() || x < 0 || y < 0 || x >= kSize || y >= kSize) return 0;
  return cells[static_cast<std::size_t>(y) * kSize + x];
}

std::uint32_t Vectorscope::peak() const noexcept {
  if (cells.empty()) return 0;
  return *std::ranges::max_element(cells);
}

Vectorscope compute_vectorscope(const ScopeImage& image) {
  Vectorscope out;
  if (image.empty()) return out;

  out.cells.assign(static_cast<std::size_t>(Vectorscope::kSize) * Vectorscope::kSize, 0);

  for (int y = 0; y < image.height; ++y) {
    const std::uint8_t* row = image.pixels + static_cast<std::ptrdiff_t>(y) * image.row_bytes();
    for (int x = 0; x < image.width; ++x) {
      const std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
      const Chroma chroma = chroma_of(pixel[0], pixel[1], pixel[2]);

      // Cb runs across and Cr *up*, so the square arrives in the orientation it
      // is drawn in: red toward the top, blue toward the bottom.
      const auto cx = static_cast<long long>(std::lround(across(chroma.cb) * Vectorscope::kSize));
      const auto cy =
          static_cast<long long>(std::lround(across(-chroma.cr) * Vectorscope::kSize));
      const auto column = std::clamp<long long>(cx, 0, Vectorscope::kSize - 1);
      const auto scanline = std::clamp<long long>(cy, 0, Vectorscope::kSize - 1);
      ++out.cells[static_cast<std::size_t>(scanline) * Vectorscope::kSize +
                  static_cast<std::size_t>(column)];
    }
  }
  return out;
}

std::array<VectorTarget, 6> vector_targets() noexcept {
  // Fully saturated primaries and secondaries, put through the same matrix the
  // scatter uses — worked out rather than written down, so a change to the
  // matrix moves the targets with it instead of leaving them lying about the
  // old one.
  constexpr std::array<std::array<double, 3>, 6> corners{{
      {255.0, 0.0, 0.0},      // red
      {255.0, 255.0, 0.0},    // yellow
      {0.0, 255.0, 0.0},      // green
      {0.0, 255.0, 255.0},    // cyan
      {0.0, 0.0, 255.0},      // blue
      {255.0, 0.0, 255.0},    // magenta
  }};

  std::array<VectorTarget, 6> out{};
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const Chroma chroma = chroma_of(corners[i][0], corners[i][1], corners[i][2]);
    out[i] = VectorTarget{.x = across(chroma.cb), .y = across(-chroma.cr)};
  }
  return out;
}

}  // namespace cutline::render
