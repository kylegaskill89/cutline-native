#pragma once

/// Turning graphic layers into pixels.
///
/// A library of its own rather than part of the UI or of the engine, because
/// both of them need it and neither should own it: the engine composites a
/// graphic into a frame, and the interface measures a layer to put handles
/// round it on the Program Monitor. It depends on Skia and on the core model,
/// and on nothing else — no GPU, no FFmpeg, no widgets.
///
/// The output is deliberately the shape the compositor already understands: one
/// plane of 8-bit RGBA, sRGB-coded, premultiplied. Premultiplied because it is
/// filtered on the way to the screen, and bilinear sampling of straight alpha
/// mixes the colour of transparent pixels into the edge of a glyph — which is
/// exactly how text acquires a dark halo.
///
/// One layer at a time, at scale 1. Placing the layers, scaling and turning
/// them, is the compositor's, which is what lets a layer move every frame
/// without being drawn again.

#include "cutline/core/graphic.hpp"
#include "cutline/core/model.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace cutline::text {

/// A drawn layer: its content and the margins round it, as `measure` says.
struct Raster {
  int width = 0;
  int height = 0;
  /// `width * height * 4` bytes, row-major, no padding between rows.
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
  [[nodiscard]] int stride() const noexcept { return width * 4; }
};

/// How much room a layer needs, without drawing it.
///
/// What `core::place_graphic` places layers by, so it has to agree with what
/// `rasterise` produces or the drawing would be stretched to fit a box of the
/// wrong shape. Both go through the same layout.
[[nodiscard]] core::LayerExtent measure(const core::GraphicLayer& layer);

/// Draws a layer at scale 1: an image exactly `measure(layer).drawn()` in
/// size, rounded up to whole pixels.
///
/// `characters` draws only the first so many characters of a text layer,
/// where the whole text would put them: a typewriter reveal.
///
/// Fails only when there is nothing to draw or no surface to draw it on. An
/// unavailable font is not a failure: the system substitutes one, which is what
/// every other application does and better than refusing to draw a title
/// because a project named a font this machine does not have. An image that
/// cannot be read draws nothing, and says so.
[[nodiscard]] std::expected<Raster, std::string> rasterise(
    const core::GraphicLayer& layer, std::size_t characters = core::kAllCharacters);

/// The font families installed on this machine, sorted, for a font menu.
[[nodiscard]] std::vector<std::string> font_families();

}  // namespace cutline::text
