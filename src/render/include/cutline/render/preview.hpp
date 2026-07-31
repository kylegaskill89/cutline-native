#pragma once

/// Rendering a sequence at less than its own size.
///
/// A preview does not have to be the resolution of the sequence. Compositing
/// cost is per pixel, so a half-size preview is a quarter of the work and a
/// quarter-size one a sixteenth, which is the difference between scrubbing 4K
/// and waiting on it.
///
/// The picture has to be the *same picture*, only smaller. Almost everything
/// already is: transforms are canvas fractions, crops and vignettes are
/// fractions, and a media's draw size is fitted to whatever canvas it is given.
/// What is not are the few things measured in pixels, and those are what this
/// scales. Missing one is invisible until somebody switches quality mid-edit
/// and the framing moves.

#include "cutline/core/model.hpp"

namespace cutline::render {

/// The same sequence on a canvas `factor` times the size.
///
/// A factor of 1 returns the project untouched. Anything that would leave a
/// canvas smaller than a couple of pixels is refused the same way, since a
/// one-pixel preview is not a preview.
///
/// What gets scaled, and why each one is here:
///
///  * the canvas itself;
///  * **type sizes and stroke widths**, because a title is laid out in canvas
///    pixels — leave them and a caption is twice as big on a half-size
///    preview, which is the one thing anybody would notice immediately;
///  * **blur**, whose amount is a radius in pixels.
///
/// That list is the whole of it today. **A new parameter measured in pixels
/// belongs here too**, and there is no way for a test to find one that is
/// missing — a fraction and a pixel count are both just doubles.
[[nodiscard]] core::Project scaled_canvas(core::Project project, double factor);

}  // namespace cutline::render
