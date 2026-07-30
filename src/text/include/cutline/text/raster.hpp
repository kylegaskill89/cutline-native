#pragma once

/// Turning a title into pixels.
///
/// A library of its own rather than part of the UI or of the engine, because
/// both of them need it and neither should own it: the engine composites a
/// title into a frame, and the interface will eventually want to show the same
/// text while it is being typed. It depends on Skia and on the core model, and
/// on nothing else — no GPU, no FFmpeg, no widgets.
///
/// The output is deliberately the shape the compositor already understands: one
/// plane of 8-bit RGBA, sRGB-coded, premultiplied. Premultiplied because it is
/// filtered on the way to the screen, and bilinear sampling of straight alpha
/// mixes the colour of transparent pixels into the edge of a glyph — which is
/// exactly how text acquires a dark halo.
///
/// Nothing here wraps text. `TextSpec` has no width to wrap against, so lines
/// are broken where the content breaks them and nowhere else; a title that runs
/// off the canvas is a title that needs a newline.

#include "cutline/core/model.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace cutline::text {

/// A rasterised title.
struct Raster {
  int width = 0;
  int height = 0;
  /// `width * height * 4` bytes, row-major, no padding between rows.
  std::vector<std::uint8_t> pixels;

  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
  [[nodiscard]] int stride() const noexcept { return width * 4; }
};

struct Size {
  double width = 0.0;
  double height = 0.0;

  friend bool operator==(const Size&, const Size&) = default;
};

/// How much room the text needs, without drawing it.
///
/// This is what the layout uses to size a title's quad, so it has to agree with
/// what `rasterise` produces or the text will be stretched to fit a box of the
/// wrong shape. Both go through the same measurement.
[[nodiscard]] Size measure(const core::TextSpec& spec);

/// Draws the title at its own font size.
///
/// The image is exactly as large as the text needs, including room for a stroke
/// and a shadow, so the caller can treat it as a picture of known size rather
/// than having to know anything about fonts.
///
/// Fails only when there is no text to draw or no surface to draw it on. An
/// unavailable font is not a failure: the system substitutes one, which is what
/// every other application does and better than refusing to draw a title
/// because a project named a font this machine does not have.
[[nodiscard]] std::expected<Raster, std::string> rasterise(const core::TextSpec& spec);

}  // namespace cutline::text
