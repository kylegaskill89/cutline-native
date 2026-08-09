#include "cutline/render/preview.hpp"

#include "cutline/render/effect_catalog.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::render {
namespace {

/// The smallest canvas worth rendering into. Below this the preview says
/// nothing about the framing, which is what a preview is for.
constexpr int kSmallestCanvas = 16;

[[nodiscard]] int scaled(int value, double factor) noexcept {
  return std::max(1, static_cast<int>(std::lround(value * factor)));
}

/// Whether an effect's parameter is a distance in pixels, and so has to shrink
/// with the canvas.
///
/// Asked of the catalogue rather than listed here. The catalogue already says
/// which parameters are in pixels, because that is what puts "px" after the
/// number on the slider — so the one declaration that makes a parameter *read*
/// as a length is the same one that makes it *scale* as a length, and there is
/// no second list to keep in step.
///
/// It used to be a hard-coded test for `blur`, and the two effects added after
/// it were both missed: a directional blur's amount and a sharpen's radius are
/// the same kind of pixel distance, and at half preview quality both were drawn
/// twice as wide as the export would draw them. The header for this file warned
/// that this would happen and that no test could catch it. One can now:
/// `EveryPixelParameterInTheCatalogueIsScaled`.
[[nodiscard]] bool measured_in_pixels(std::string_view type, std::string_view key) {
  const EffectSpec* spec = find_effect_spec(type);
  if (spec == nullptr) return false;
  for (const EffectParamSpec& param : spec->params) {
    if (param.key == key) return param.suffix == "px";
  }
  return false;
}

}  // namespace

core::Project scaled_canvas(core::Project project, double factor) {
  if (!(factor > 0.0) || factor == 1.0) return project;

  const int width = scaled(project.canvas_w, factor);
  const int height = scaled(project.canvas_h, factor);
  if (width < kSmallestCanvas || height < kSmallestCanvas) return project;

  // Taken from the rounded result rather than from `factor`, so everything
  // else is scaled by exactly what the canvas was: a 1919-pixel canvas at a
  // half rounds to 960, and a title scaled by 0.5 instead would be a fraction
  // of a pixel out of step with it. Invisible once; not once it is animated.
  const double actual = static_cast<double>(width) / project.canvas_w;

  project.canvas_w = width;
  project.canvas_h = height;

  for (core::Media& media : project.media) {
    if (!media.text.has_value()) continue;
    media.text->font_size *= actual;
    media.text->stroke_width *= actual;
  }

  for (core::Track& track : project.tracks) {
    for (core::Clip& clip : track.clips) {
      for (core::ClipEffect& effect : clip.effects) {
        // The stored value and the keyframes both: an animated blur reads its
        // keyframes and ignores the stored one, so scaling only one of them
        // leaves whichever is in use unscaled half the time.
        for (auto& [key, value] : effect.params) {
          if (measured_in_pixels(effect.type, key)) value *= actual;
        }
        for (auto& [key, frames] : effect.keyframes) {
          if (!measured_in_pixels(effect.type, key)) continue;
          for (core::Keyframe& frame : frames) frame.v *= actual;
        }
      }
    }
  }

  return project;
}

}  // namespace cutline::render
