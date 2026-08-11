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
  /// One plane of 8-bit RGBA, sRGB-coded, with **premultiplied** alpha. What a
  /// rasteriser produces: titles, and anything else drawn rather than decoded.
  ///
  /// Premultiplied because it is filtered. Bilinear sampling of straight alpha
  /// mixes the colour of fully transparent pixels into the edge of a glyph,
  /// which is how text ends up with a dark halo around it. The shader divides
  /// the alpha back out before the effects, since those are defined on plain
  /// coded values.
  Rgba8,
  /// A composited scene, already on the card: sRGB-coded R'G'B' with
  /// **straight** alpha, in a plain UNORM target.
  ///
  /// What a nested sequence arrives as. Straight rather than premultiplied is
  /// the whole difference from `Rgba8` and the reason it is its own layout: the
  /// present pass encodes the colour and carries alpha through untouched, so
  /// dividing alpha back out — which `Rgba8` does, because a rasteriser
  /// premultiplies — would wash out everything a nest leaves transparent.
  ///
  /// Only ever texture-resident. There is no upload path for it, because the
  /// thing it describes was produced by a compositor on this same device and
  /// there would be nothing to gain by bringing it down and putting it back.
  Scene,
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

/// A frame that never left the graphics card.
///
/// A hardware decoder given the compositor's own device writes into that
/// device's memory, so the picture is already where it needs to be: there is
/// nothing to copy down and nothing to upload back. The compositor makes views
/// over the resource's plane slices and samples it exactly as it samples the
/// planes it uploaded itself.
///
/// Untyped so d3d12.h stays out of this header, which is the same bargain the
/// decoder's `HardwareTexture` makes and for the same reason.
struct SourceTexture {
  /// `ID3D12Resource*`, NV12. Borrowed: whoever decoded it keeps it alive, and
  /// `compose` waits for the GPU before returning, so it only has to outlive
  /// the call.
  void* resource = nullptr;
  /// Array slice, when the decoder's frame pool is a texture array.
  int subresource = 0;

  /// `ID3D12Fence*` the decoder signals when this frame is finished, and the
  /// value to wait for. Sampling before that reads a half-decoded picture —
  /// intermittently, depending on how busy the card is, which is the worst kind
  /// of fault to be left with.
  void* fence = nullptr;
  unsigned long long fence_value = 0;

  [[nodiscard]] bool empty() const noexcept { return resource == nullptr; }
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

  /// Set when the pixels are already on the compositor's device, in which case
  /// `planes` says nothing and nothing is uploaded.
  SourceTexture texture;
};

}  // namespace cutline::gpu
