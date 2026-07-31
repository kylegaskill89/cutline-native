#include "cutline/render/preview.hpp"

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
        if (effect.type != "blur") continue;

        // The stored value and the keyframes both: an animated blur reads its
        // keyframes and ignores the stored one, so scaling only one of them
        // leaves whichever is in use unscaled half the time.
        if (const auto found = effect.params.find("amount"); found != effect.params.end()) {
          found->second *= actual;
        }
        if (const auto keyed = effect.keyframes.find("amount"); keyed != effect.keyframes.end()) {
          for (core::Keyframe& frame : keyed->second) frame.v *= actual;
        }
      }
    }
  }

  return project;
}

}  // namespace cutline::render
