#pragma once

/// Video scopes: what a frame is made of, measured rather than judged by eye.
///
/// Pure arithmetic over a block of pixels, with no GPU and no model anywhere
/// near it — which is the point. A scope never affects what is rendered or
/// exported; it only reads. That makes every one of these exhaustively testable
/// against a frame built in three lines, and it means a wrong reading can only
/// ever be a wrong reading rather than a wrong export.
///
/// These are fed a *downscaled* copy of the output. A scope is a statistical
/// picture and a quarter of the pixels tell the same story as all of them for a
/// fraction of the work, which matters when it happens every frame.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cutline::render {

/// A borrowed block of 8-bit RGBA, top row first.
///
/// Its own type rather than `ui::ImageView` because this layer knows nothing
/// about the widget layer and should not start now — the four numbers are the
/// whole of what a scope needs.
struct ScopeImage {
  const std::uint8_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  /// Bytes per row. Zero means tightly packed, which is `width * 4`.
  int stride = 0;

  [[nodiscard]] bool empty() const noexcept {
    return pixels == nullptr || width <= 0 || height <= 0;
  }
  [[nodiscard]] int row_bytes() const noexcept { return stride > 0 ? stride : width * 4; }
};

/// BT.601 luma, which is what the reference used and what every scope here
/// measures brightness with. Kept in one place so the histogram's luma trace
/// and the waveform cannot disagree about what grey is.
[[nodiscard]] double luma_of(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept;

/// How many pixels fall at each of the 256 levels, per channel.
struct Histogram {
  static constexpr std::size_t kBins = 256;

  std::array<std::uint32_t, kBins> red{};
  std::array<std::uint32_t, kBins> green{};
  std::array<std::uint32_t, kBins> blue{};
  std::array<std::uint32_t, kBins> luma{};

  /// The fullest bin in any channel, which is what a drawing scales against.
  /// Zero for an empty histogram, and callers must not divide by it blindly.
  [[nodiscard]] std::uint32_t peak() const noexcept;

  friend bool operator==(const Histogram&, const Histogram&) = default;
};

[[nodiscard]] Histogram compute_histogram(const ScopeImage& image);

/// Which channel a waveform is measuring.
enum class ScopeChannel { Luma, Red, Green, Blue };

/// For each column of the frame, how many pixels sit at each level.
///
/// Column-per-column rather than a single distribution, because that is what
/// makes a waveform useful: it says *where* across the picture the highlights
/// are, which a histogram cannot.
struct Waveform {
  static constexpr std::size_t kLevels = 256;

  int columns = 0;
  /// `columns * kLevels`, column-major: all of column 0's levels, then column
  /// 1's. Column-major because that is the order it is both filled and drawn
  /// in, and a scope is rebuilt every frame.
  std::vector<std::uint32_t> cells;

  [[nodiscard]] std::uint32_t at(int column, std::size_t level) const noexcept;
  /// The fullest cell, which is what the drawing scales its brightness against.
  [[nodiscard]] std::uint32_t peak() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return columns <= 0 || cells.empty(); }

  friend bool operator==(const Waveform&, const Waveform&) = default;
};

[[nodiscard]] Waveform compute_waveform(const ScopeImage& image, ScopeChannel channel);

/// The three colour waveforms that make up a parade, in the order they are
/// drawn: red, then green, then blue, side by side.
struct Parade {
  Waveform red;
  Waveform green;
  Waveform blue;

  friend bool operator==(const Parade&, const Parade&) = default;
};

[[nodiscard]] Parade compute_parade(const ScopeImage& image);

/// Chroma as a scatter, in BT.601 Cb/Cr.
///
/// Grey sits at the centre and saturation reaches toward the rim, so a cast
/// shows as the whole cloud leaning one way — which is the one thing no other
/// scope says plainly.
struct Vectorscope {
  static constexpr int kSize = 256;

  /// `kSize * kSize` counts, row-major, with Cb across and Cr *up* — so the
  /// square is in the orientation it is drawn in and the drawing does not have
  /// to remember to flip it.
  std::vector<std::uint32_t> cells;

  [[nodiscard]] std::uint32_t at(int x, int y) const noexcept;
  [[nodiscard]] std::uint32_t peak() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return cells.empty(); }

  friend bool operator==(const Vectorscope&, const Vectorscope&) = default;
};

[[nodiscard]] Vectorscope compute_vectorscope(const ScopeImage& image);

/// Where a fully saturated colour lands on the vectorscope, 0 to 1 across the
/// square. What the graticule's target boxes are drawn at.
struct VectorTarget {
  double x = 0.0;
  double y = 0.0;
};

/// The six primaries and secondaries, in the order a vectorscope's graticule
/// names them: red, yellow, green, cyan, blue, magenta.
[[nodiscard]] std::array<VectorTarget, 6> vector_targets() noexcept;

}  // namespace cutline::render
