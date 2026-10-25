#pragma once

/// Graphics: where each layer of a graphic is at a moment, and the old title
/// turned into one.
///
/// Placement is worked out here, apart from drawing, because three things need
/// the same answer: the renderer, which draws each layer where this says; the
/// Program Monitor, which puts handles round the layer being dragged; and the
/// tests. Nothing here draws, so the size of a layer's content -- which only
/// something that can lay out text knows -- comes in through a measurer.

#include "cutline/core/layout.hpp"
#include "cutline/core/model.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::core {

/// What a layer measures at scale 1: its content -- the text's lines, the
/// shape -- and how far its drawing reaches past that on each side, for a
/// stroke, a background or a shadow. The drawing's own image is the content
/// plus those margins.
struct LayerExtent {
  Size content;
  double left = 0.0;
  double top = 0.0;
  double right = 0.0;
  double bottom = 0.0;

  [[nodiscard]] Size drawn() const noexcept {
    return {content.width + left + right, content.height + top + bottom};
  }
  friend bool operator==(const LayerExtent&, const LayerExtent&) = default;
};

/// Every character of a text layer, for a reveal that is not typing it out.
inline constexpr std::size_t kAllCharacters = static_cast<std::size_t>(-1);

/// Measures a layer. Always the whole of it: a typewriter reveal draws some of
/// the characters where the whole text puts them, so the words already shown
/// stay still while the rest arrive.
using LayerMeasurer = std::function<LayerExtent(const GraphicLayer& layer)>;

/// How far a layer's drawing reaches past its content on every side: its
/// strokes, a text background's padding, and its shadow. One formula, used by
/// the drawing and by every measurer, so the two always agree.
[[nodiscard]] double drawing_margin(const GraphicLayer& layer) noexcept;

/// The measurer for everything that does not need a font: shapes are their
/// size, text and images nothing, and every margin is `drawing_margin`. What
/// tests and the planner use when nothing better is to hand.
[[nodiscard]] LayerExtent measure_without_fonts(const GraphicLayer& layer);

/// An axis-aligned box in canvas pixels.
struct Bounds {
  double left = 0.0;
  double top = 0.0;
  double width = 0.0;
  double height = 0.0;

  [[nodiscard]] double right() const noexcept { return left + width; }
  [[nodiscard]] double bottom() const noexcept { return top + height; }
  friend bool operator==(const Bounds&, const Bounds&) = default;
};

/// One layer, placed.
struct PlacedLayer {
  const GraphicLayer* layer = nullptr;
  /// Its index in the graphic's list, top first.
  std::size_t index = 0;
  /// The layer's content in canvas pixels, scaled but not turned: what pins
  /// are measured against and what the monitor draws handles round.
  Bounds content;
  /// The whole drawing -- content and margins -- as a quad: centre, size,
  /// turn. What the compositor is given.
  LayerBox box;
  double opacity = 1.0;
  /// How much of the text a typewriter reveal shows, or `kAllCharacters`.
  std::size_t characters = kAllCharacters;
  /// How much of the drawing a wipe has uncovered, from the left, 0..1.
  double wipe = 1.0;
  /// A shape's size after responsive stretching, which the drawing has to
  /// match. The layer's own otherwise.
  double width = 0.0;
  double height = 0.0;
};

/// Where every visible layer of a graphic is, `local_t` seconds into a clip
/// `duration` long, on a canvas that size. Bottom layer first, which is the
/// order they are drawn in.
[[nodiscard]] std::vector<PlacedLayer> place_graphic(const Graphic& graphic, double local_t,
                                                     double duration, double canvas_w,
                                                     double canvas_h,
                                                     const LayerMeasurer& measure);

/// The time a graphic's keyframes are read at, `local_t` into a clip
/// `duration` long: the intro and outro keep their own timing however the clip
/// is trimmed, and the middle stretches or shrinks. Premiere's responsive
/// design, time.
[[nodiscard]] double graphic_time(const Graphic& graphic, double local_t, double duration) noexcept;

/// A layer's property at a time, keyframed or not.
[[nodiscard]] double layer_value(const GraphicLayer& layer, std::string_view property,
                                 double t) noexcept;

/// How many characters a piece of UTF-8 text has, and the first `count` of
/// them. A typewriter reveal counts letters, not bytes.
[[nodiscard]] std::size_t character_count(std::string_view text) noexcept;
[[nodiscard]] std::string first_characters(std::string_view text, std::size_t count);

/// The text a layer shows: its own, in capitals when the style says so.
[[nodiscard]] std::string displayed_text(const GraphicLayer& layer);

/// A title from before graphics, as a graphic of one text layer that looks
/// the same: centred in the frame, the same words, font and colours.
[[nodiscard]] Graphic graphic_from_text(const TextSpec& spec);

/// A graphic with everything measured in canvas pixels multiplied by
/// `factor`, for a preview drawn smaller than the sequence.
[[nodiscard]] Graphic scaled_graphic(Graphic graphic, double factor);

/// The layer with an id, or null.
[[nodiscard]] const GraphicLayer* find_layer(const Graphic& graphic, std::string_view id) noexcept;
[[nodiscard]] GraphicLayer* find_layer(Graphic& graphic, std::string_view id) noexcept;

}  // namespace cutline::core
