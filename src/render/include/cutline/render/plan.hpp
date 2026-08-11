#pragma once

/// Turning a project at a moment in time into an ordered list of things to
/// draw.
///
/// This is the layer that decides *what* is on screen; the compositor decides
/// how it gets there, and the media layer supplies the pixels. Keeping the
/// decision separate from both is what lets it be tested exhaustively without a
/// GPU or a video file — draw order, visibility, and which frame of which
/// source each layer wants are all pure functions of the model.
///
/// The plan is deliberately free of GPU types. A caller walks it, fetches
/// whatever frames it names, and hands the compositor its layers.

#include "cutline/core/layout.hpp"
#include "cutline/core/model.hpp"
#include "cutline/core/segments.hpp"

#include <string>
#include <vector>

namespace cutline::render {

/// What a layer draws.
enum class LayerContent {
  Video,       ///< a frame from a source file, at `source_time`
  Still,       ///< an image; `source_time` is meaningless
  Text,        ///< a title, laid out from the media's text spec
  Color,       ///< a colour matte, possibly a gradient
  Adjustment,  ///< draws nothing; its effects apply to everything beneath it
  /// A sequence composited on its own and drawn as one picture.
  ///
  /// The layer's effects run on the *result* of that, which is the whole point
  /// of a nest and the reason it cannot be flattened into the layers around it:
  /// splicing the inner ones in would apply the nest's effects to each of them
  /// separately.
  Nested,
};

/// One entry in the draw order, resolved at a single instant.
struct PlannedLayer {
  /// Both point into the project the plan was built from and do not outlive
  /// it. A plan is a view, not a copy.
  const core::Clip* clip = nullptr;
  const core::Media* media = nullptr;

  LayerContent content = LayerContent::Video;

  /// Where it lands on the canvas, transitions and keyframes already applied.
  core::LayerBox box;
  /// Opacity times fades, already combined.
  double alpha = 1.0;

  /// How much to soften the layer vertically before it is scaled, from 0 to 1.
  /// Premiere's Anti-flicker Filter; see `core::Transform::anti_flicker`.
  double anti_flicker = 0.0;
  core::BlendMode blend = core::BlendMode::Normal;

  /// The media time to display, clamped inside the segment so a rounding error
  /// at a boundary cannot pull in a frame from beyond the trim. Meaningful
  /// only for `Video`.
  double source_time = 0.0;

  /// Which video track this came from, counting from the bottom. Useful for
  /// diagnostics and for the UI; the draw order is already the vector's order.
  int track_index = 0;
};

/// How big a title comes out.
///
/// The model stores what the text says, not how much room it takes — that
/// depends on a font, which is not something a pure layer can know. A caller
/// able to draw text supplies this and its titles are sized to their content; a
/// caller that cannot leaves it empty, and a title falls back to filling the
/// canvas rather than to nothing.
using TextMeasurer = std::function<core::Size(const core::Media& media)>;

/// The layers active at timeline time `t`, in the order they should be drawn:
/// bottom track first, and within a track, earlier-starting segments first so a
/// dissolve's incoming clip lands on top of the outgoing one.
///
/// Hidden tracks and disabled clips are left out. `media_duration_of` supplies
/// each media's source length for handle borrowing, and should return infinity
/// for stills and generated media.
[[nodiscard]] std::vector<PlannedLayer> plan_frame(
    const core::Project& project, double t,
    const std::function<double(std::string_view media_id)>& media_duration_of,
    const TextMeasurer& measure_text = {});

/// As `plan_frame`, using the project's own media list for durations. This is
/// what callers normally want; the callback form exists for tests and for
/// sources whose real duration is not yet known.
[[nodiscard]] std::vector<PlannedLayer> plan_frame(const core::Project& project, double t,
                                                   const TextMeasurer& measure_text = {});

}  // namespace cutline::render
