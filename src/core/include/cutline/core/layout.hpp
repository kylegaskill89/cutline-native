#pragma once

/// Where a clip lands on the canvas, and how opaque it is when it gets there.
///
/// This is the geometry the compositor draws with, kept here rather than in the
/// GPU layer because it is exact numeric behaviour carried over from the
/// reference implementation, and because it is worth testing without a device.
/// The shader receives a box and an alpha; it does not know what a keyframe or
/// a transition is.

#include "cutline/core/model.hpp"
#include "cutline/core/segments.hpp"

namespace cutline::core {

struct Size {
  double width = 0.0;
  double height = 0.0;

  friend bool operator==(const Size&, const Size&) = default;
};

/// A media's draw size at scale 1, in canvas pixels: aspect-fit to the canvas,
/// so scale 1 means "fills the canvas as much as it can without distortion"
/// rather than "native pixels". That is what keeps a project's transforms
/// independent of the resolution it is exported at.
///
/// Text is measured, not derived, so callers that have laid a title out pass
/// the result in `measured_text`; core cannot shape glyphs. A zero measurement
/// falls back to the media's stored dimensions.
[[nodiscard]] Size natural_size(const Media* media, double canvas_w, double canvas_h,
                                Size measured_text = {}) noexcept;

/// A resolved draw rectangle in canvas pixels, centred on `center_x, center_y`
/// and rotated clockwise about that centre.
struct LayerBox {
  double center_x = 0.0;
  double center_y = 0.0;
  double width = 0.0;
  double height = 0.0;
  double rotation_deg = 0.0;

  friend bool operator==(const LayerBox&, const LayerBox&) = default;
};

/// How far a layer's centre sits from its anchor point, in canvas pixels.
///
/// Position places the *anchor*, and the compositor draws a rectangle about its
/// centre, so something has to convert between the two. That something is this,
/// in one place, because three callers need the answer: the compositor's box,
/// the monitor's handles, and the drag that writes a position back.
///
/// `drawn` is the layer at the size it is actually drawn — scale already in it.
/// Rotation turns the offset, which is what makes the anchor the point the
/// layer spins about: the anchor stays where it was put and the rest swings
/// around it.
///
/// Pixels rather than canvas fractions on purpose. A rotation is only a
/// rotation in square units; the same turn applied to an offset expressed in
/// fractions of a 16:9 canvas shears it instead.
struct Offset {
  double dx = 0.0;
  double dy = 0.0;

  friend bool operator==(const Offset&, const Offset&) = default;
};

[[nodiscard]] Offset anchor_offset(const Transform& transform, Size drawn) noexcept;

/// The box a clip draws into at timeline time `t`, with keyframed transform
/// applied. Stored `x, y` are canvas fractions naming where the anchor lands,
/// so (0.5, 0.5) puts the anchor in the middle of the frame.
[[nodiscard]] LayerBox layer_box(const Clip& clip, const Media* media, double canvas_w,
                                 double canvas_h, double t, Size measured_text = {}) noexcept;

/// As `layer_box`, but including the horizontal offset a push or slide
/// transition contributes at `t`. Segments without a geometric transition give
/// the same answer as `layer_box`.
[[nodiscard]] LayerBox segment_box(const VideoSeg& seg, const Media* media, double canvas_w,
                                   double canvas_h, double t, Size measured_text = {}) noexcept;

/// A segment's alpha at timeline time `t`: keyframed opacity multiplied by the
/// fade ramps. Manual fades and transition ramps are not additive — the longer
/// of the two wins on each edge, so setting a fade on a clip that already has a
/// dissolve does not double up.
[[nodiscard]] double segment_alpha(const VideoSeg& seg, double t) noexcept;

}  // namespace cutline::core
