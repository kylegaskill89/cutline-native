#pragma once

/// The program monitor: a picture, letterboxed, with what is around it.
///
/// It takes pixels rather than a renderer, for the same reason the timeline
/// takes structs rather than a project: the widget layer stays free of the
/// media stack, so all of this is testable with no decoder, no GPU and no
/// window, and the same view can later show a source clip, a nested sequence,
/// or a frame that came from somewhere else entirely.
///
/// Connecting it to the compositor belongs to the application, above both.

#include "cutline/ui/layout.hpp"
#include "cutline/ui/widget.hpp"

#include <optional>
#include <string>
#include <utility>

namespace cutline::ui {

class MonitorView : public Widget {
 public:
  MonitorView();

  /// The frame to show. Borrowed: the caller keeps the pixels alive until the
  /// next call or the next paint, whichever comes first.
  void set_frame(const ImageView& frame);

  /// The frame to show, when it is already on the graphics card and there is
  /// no reason to bring it down. Borrowed on the same terms.
  ///
  /// The two are alternatives, not layers: setting either forgets the other.
  /// A monitor holding both would have to decide which one wins on every
  /// paint, and the answer would be whichever was written most recently —
  /// which is this, said once, where it can be tested.
  void set_texture(const TextureView& frame);

  /// Forgets both.
  void clear_frame();

  [[nodiscard]] const ImageView& frame() const noexcept { return frame_; }
  [[nodiscard]] const TextureView& texture() const noexcept { return texture_; }

  /// Whether there is anything to show, from either source.
  [[nodiscard]] bool has_picture() const noexcept {
    return !frame_.empty() || !texture_.empty();
  }

  /// The shape of the sequence, used to letterbox when there is no frame yet.
  /// Without it an empty monitor would be the shape of its panel, and the
  /// picture would jump into a different rectangle the moment one arrived.
  void set_canvas_aspect(double aspect) noexcept;
  [[nodiscard]] double canvas_aspect() const noexcept { return canvas_aspect_; }

  /// What is shown under the picture when there is none.
  void set_placeholder(std::string text) { placeholder_ = std::move(text); }

  /// Where the picture goes: the largest rectangle of the right shape that
  /// fits, centred. Empty only when there is no room at all.
  [[nodiscard]] Rect picture() const;

  /// Turns a point in the window into a position within the picture, from
  /// (0,0) at its top left to (1,1) at its bottom right. Outside the picture
  /// the values run past those, which is what a drag that leaves the frame
  /// needs. Nothing is returned when there is no picture to be inside.
  [[nodiscard]] std::optional<std::pair<double, double>> to_picture(double x,
                                                                    double y) const;

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  ImageView frame_;
  TextureView texture_;
  double canvas_aspect_ = 16.0 / 9.0;
  std::string placeholder_ = "No preview";
};

}  // namespace cutline::ui
