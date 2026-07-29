#include "cutline/ui/monitor.hpp"

#include <algorithm>
#include <utility>

namespace cutline::ui {

MonitorView::MonitorView() {
  // The picture is letterboxed inside the panel, so nothing should ever run
  // over the edge; clipping is cheap insurance against a rounding error at the
  // boundary drawing a line of video across the panel beside it.
  set_clips_children(true);
}

void MonitorView::set_frame(const ImageView& frame) {
  frame_ = frame;
  texture_ = TextureView{};
}

void MonitorView::set_texture(const TextureView& frame) {
  texture_ = frame;
  frame_ = ImageView{};
}

void MonitorView::clear_frame() {
  frame_ = ImageView{};
  texture_ = TextureView{};
}

void MonitorView::set_canvas_aspect(double aspect) noexcept {
  if (aspect > 0.0) canvas_aspect_ = aspect;
}

Rect MonitorView::picture() const {
  // The frame's own shape when there is one, the sequence's when there is not.
  // Using the panel's shape while empty would make the picture jump into a
  // different rectangle the moment the first frame arrived.
  double aspect = canvas_aspect_;
  if (!frame_.empty()) aspect = frame_.aspect();
  else if (!texture_.empty()) aspect = texture_.aspect();
  return fit_aspect(bounds(), aspect);
}

std::optional<std::pair<double, double>> MonitorView::to_picture(double x, double y) const {
  const Rect area = picture();
  if (area.empty()) return std::nullopt;
  // Deliberately unclamped: a transform being dragged past the edge of the
  // frame is a normal thing to do, and clamping here would stop it dead at the
  // boundary.
  return std::pair{(x - area.x) / area.width, (y - area.y) / area.height};
}

void MonitorView::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = picture();
  if (area.empty()) return;

  const SurfaceStyle& style = theme.style(part(), state());

  if (!has_picture()) {
    // The shape of the sequence, drawn as a well, so it is obvious that this
    // is where the picture goes rather than looking like a broken panel.
    const SurfaceStyle& empty = theme.style(Part::Input, State::Disabled);
    paint_surface(painter, area, empty);
    if (!placeholder_.empty()) {
      painter.text(text_run(area, placeholder_, empty, theme.metrics.small_font_size,
                            TextAlign::Center, false));
    }
    return;
  }

  if (!texture_.empty()) {
    painter.texture(area, texture_);
  } else {
    painter.image(area, frame_);
  }
  // A hairline around it, so a frame that is mostly black still reads as a
  // picture with edges rather than as a hole in the panel.
  if (style.border.a > 0.0) painter.stroke(area, 0.0, style.border, 1.0);
}

}  // namespace cutline::ui
