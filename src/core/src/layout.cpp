#include "cutline/core/layout.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace cutline::core {

Size natural_size(const Media* media, double canvas_w, double canvas_h,
                  Size measured_text) noexcept {
  if (media != nullptr && media->is_text && measured_text.width > 0.0 &&
      measured_text.height > 0.0) {
    return measured_text;
  }

  // A media with no dimensions of its own — a colour matte, an adjustment
  // layer — is canvas-sized, so it covers exactly the frame at scale 1.
  const auto dimension = [](const std::optional<int>& value, double fallback) {
    return (value && *value > 0) ? static_cast<double>(*value) : fallback;
  };
  const double media_w = media != nullptr ? dimension(media->width, canvas_w) : canvas_w;
  const double media_h = media != nullptr ? dimension(media->height, canvas_h) : canvas_h;
  if (media_w <= 0.0 || media_h <= 0.0) return {canvas_w, canvas_h};

  const double fit = std::min(canvas_w / media_w, canvas_h / media_h);
  return {media_w * fit, media_h * fit};
}

Offset anchor_offset(const Transform& transform, Size drawn) noexcept {
  // From the anchor to the centre, before the layer is turned.
  const double dx = (0.5 - transform.anchor_x) * drawn.width;
  const double dy = (0.5 - transform.anchor_y) * drawn.height;

  const double radians = transform.rotation * std::numbers::pi / 180.0;
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  return {.dx = dx * c - dy * s, .dy = dx * s + dy * c};
}

LayerBox layer_box(const Clip& clip, const Media* media, double canvas_w, double canvas_h,
                   double t, Size measured_text) noexcept {
  const Size natural = natural_size(media, canvas_w, canvas_h, measured_text);
  const Transform transform = animated_transform(clip, t - clip.start);

  const Size drawn{natural.width * transform.scale_x, natural.height * transform.scale_y};
  const Offset shift = anchor_offset(transform, drawn);

  // The anchor is what position names; the compositor wants a centre. Nothing
  // below this line knows an anchor exists, which is why the shader and the
  // preview were untouched by the whole feature.
  return {
      .center_x = transform.x * canvas_w + shift.dx,
      .center_y = transform.y * canvas_h + shift.dy,
      .width = drawn.width,
      .height = drawn.height,
      .rotation_deg = transform.rotation,
  };
}

LayerBox segment_box(const VideoSeg& seg, const Media* media, double canvas_w, double canvas_h,
                     double t, Size measured_text) noexcept {
  if (seg.clip == nullptr) return {};

  LayerBox box = layer_box(*seg.clip, media, canvas_w, canvas_h, t, measured_text);
  // The slide offset is a fraction of canvas width, so a push travels the same
  // visual distance whatever the export resolution.
  box.center_x += seg_slide_offset_x(seg, t) * canvas_w;
  return box;
}

double segment_alpha(const VideoSeg& seg, double t) noexcept {
  if (seg.clip == nullptr) return 0.0;
  const Clip& clip = *seg.clip;

  double alpha = animated_opacity(clip, t - clip.start);

  // Fades run over the *segment*, not the clip: a transition has already
  // widened the segment past the clip's own bounds, and the ramp belongs to
  // what is drawn.
  const double local = t - seg.start;
  const double length = seg.end - seg.start;
  const double fade_in = std::max(clip.fade_in, seg.x_in);
  const double fade_out = std::max(clip.fade_out, seg.x_out);

  if (fade_in > 0.0 && local < fade_in) alpha *= std::max(0.0, local / fade_in);

  const double tail = length - local;
  if (fade_out > 0.0 && tail < fade_out) alpha *= std::max(0.0, tail / fade_out);

  return alpha;
}

}  // namespace cutline::core
