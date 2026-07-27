#pragma once

/// A decoded frame described without reference to how it was decoded.
///
/// The GPU layer never sees libav types; callers hand it plane pointers and the
/// colour tagging that came off the file. That keeps the renderer testable and
/// keeps the media layer replaceable.

#include <cstdint>

namespace cutline::gpu {

enum class PixelLayout {
  Nv12,     ///< Y plane plus interleaved UV at half resolution
  Yuv420p,  ///< separate Y, U, V planes
};

/// Which luma coefficients the encoder used.
enum class ColorSpace {
  Bt709,
  Bt601,
  Bt2020,
};

/// How coded values map to light.
enum class TransferFunction {
  Bt709,      ///< SDR
  Smpte2084,  ///< PQ, HDR10
  AribStdB67, ///< HLG
};

struct PlaneView {
  const std::uint8_t* data = nullptr;
  int stride = 0;
};

struct FrameView {
  int width = 0;
  int height = 0;
  PixelLayout layout = PixelLayout::Nv12;
  ColorSpace space = ColorSpace::Bt709;
  TransferFunction transfer = TransferFunction::Bt709;
  /// Studio-range video leaves headroom and footroom; expanding it is what
  /// keeps blacks from crushing and highlights from clipping.
  bool full_range = false;

  PlaneView planes[3];
};

}  // namespace cutline::gpu
