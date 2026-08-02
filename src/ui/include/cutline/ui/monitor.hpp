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

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cutline::ui {

/// Where a layer sits on the canvas, in canvas fractions: the centre at
/// (0.5, 0.5), a width of 1 spanning the frame.
///
/// Fractions rather than pixels because that is what the model stores, and
/// because it makes the overlay independent of what resolution the preview
/// happens to be rendered at. What it is *not* is a clip's transform: scale
/// there is relative to the aspect-fit size of the media, and only the caller
/// knows what that is. Converting between the two is one multiplication, and it
/// belongs where the media is known.
struct MonitorBox {
  double x = 0.5;
  double y = 0.5;
  double width = 1.0;
  double height = 1.0;
  double rotation = 0.0;  ///< degrees, clockwise

  friend bool operator==(const MonitorBox&, const MonitorBox&) = default;
};

/// One effect's mask, drawn over the picture.
///
/// In canvas fractions like everything else here, because a shape on the frame
/// is a place on the frame — the mask is stored in fractions of the *layer*,
/// and turning one into the other needs the clip's transform, which is the
/// editor's business rather than this widget's.
struct MaskOverlay {
  /// 1 for an ellipse, 2 for a rectangle, matching `core::MaskShape`.
  int shape = 1;
  double x = 0.5;
  double y = 0.5;
  /// Half-extents, as fractions of the canvas.
  double width = 0.25;
  double height = 0.25;
  double rotation = 0.0;  ///< degrees, clockwise

  friend bool operator==(const MaskOverlay&, const MaskOverlay&) = default;
};

/// What a press on the overlay took hold of. The order is the one the hit test
/// walks, which is why the corners come before the edges: a corner handle sits
/// inside both edges it belongs to, and testing an edge first would make the
/// corners unreachable.
enum class TransformHandle {
  None,
  TopLeft,
  TopRight,
  BottomRight,
  BottomLeft,
  Top,
  Right,
  Bottom,
  Left,
  Rotate,
  Move,
};

/// A snap line the overlay is currently held against, in canvas fractions.
/// Drawn while a drag is snapped and gone as soon as it is not, which is the
/// only thing that tells the difference between "it lined up" and "it was put
/// there".
struct SnapGuide {
  bool vertical = true;  ///< a vertical line at `at` along x; otherwise along y
  double at = 0.0;

  friend bool operator==(const SnapGuide&, const SnapGuide&) = default;
};

/// The border kept between the panel and the picture.
///
/// Not decoration: a layer filling the frame has its transform handles *on* the
/// frame's edge, and half of each one would fall outside the widget. A press
/// outside a widget's bounds goes to whatever is behind it, so without this the
/// corner handles of a full-frame layer could not be grabbed at all — which is
/// exactly how it behaved the first time it was tried on screen.
inline constexpr double kMonitorInset = 20.0;

/// The smallest a mask may be dragged to, as a fraction of the canvas.
///
/// Not zero: a shape with no size has no grip to take hold of, so it could be
/// shrunk away and never brought back — the same trap the transform box's own
/// minimum extent exists for.
inline constexpr double kMinMaskExtent = 0.01;

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

  /// Whether something dragged from elsewhere would land here.
  ///
  /// Drawn as an outline round the picture while it is true. The monitor has no
  /// idea what is being dragged or what landing would mean — the panel that
  /// owns the drag decides both, and this is only how it says so.
  [[nodiscard]] bool drop_lit() const noexcept { return drop_lit_; }
  void set_drop_lit(bool lit) noexcept;

  /// Where the picture goes: the largest rectangle of the right shape that
  /// fits, centred. Empty only when there is no room at all.
  [[nodiscard]] Rect picture() const;

  /// Turns a point in the window into a position within the picture, from
  /// (0,0) at its top left to (1,1) at its bottom right. Outside the picture
  /// the values run past those, which is what a drag that leaves the frame
  /// needs. Nothing is returned when there is no picture to be inside.
  [[nodiscard]] std::optional<std::pair<double, double>> to_picture(double x,
                                                                    double y) const;

  // ------------------------------------------------------ transform handles --

  /// The layer being transformed, or nothing when there is no selection to
  /// show handles for.
  void set_transform(std::optional<MonitorBox> box);
  [[nodiscard]] const std::optional<MonitorBox>& transform() const noexcept { return box_; }

  /// Every pixel of a drag, so the preview can follow it, and once on release
  /// so the edit is one entry in the undo stack. The same split every other
  /// dragged control here uses.
  void set_on_transform_change(std::function<void(const MonitorBox&)> on_change) {
    on_change_ = std::move(on_change);
  }
  void set_on_transform_commit(std::function<void(const MonitorBox&)> on_commit) {
    on_commit_ = std::move(on_commit);
  }

  // --------------------------------------------------------------- masks --

  /// The masks to draw, in the order the effect stack lists them.
  ///
  /// Drawn under the transform handles, which are what a press finds first: a
  /// mask sitting where a corner handle is would otherwise make the layer
  /// impossible to resize.
  void set_masks(std::vector<MaskOverlay> masks);
  [[nodiscard]] const std::vector<MaskOverlay>& masks() const noexcept { return masks_; }

  /// Every pixel of a mask drag, and once on release. The index is into
  /// `masks`, which is the caller's own order.
  void set_on_mask_change(std::function<void(std::size_t, const MaskOverlay&)> on_change) {
    on_mask_change_ = std::move(on_change);
  }
  void set_on_mask_commit(std::function<void(std::size_t, const MaskOverlay&)> on_commit) {
    on_mask_commit_ = std::move(on_commit);
  }

  /// Which mask a press at this point would take hold of, and whether by its
  /// body or by a corner. Nothing when the press is on no mask.
  [[nodiscard]] std::optional<std::size_t> mask_at(double x, double y) const;
  /// Where a mask's resize grip is, in widget pixels. Empty when there is no
  /// such mask or no picture to draw it over.
  [[nodiscard]] Rect mask_grip(std::size_t index) const;

  /// What a press at this point would take hold of.
  [[nodiscard]] TransformHandle handle_at(double x, double y) const;

  /// Where a handle's grab square is, in widget pixels. Empty when there is no
  /// box, or for `None`.
  [[nodiscard]] Rect handle_rect(TransformHandle handle) const;

  /// The lines the drag is currently snapped to. Empty when nothing is being
  /// dragged, or when it is not lined up with anything.
  [[nodiscard]] const std::vector<SnapGuide>& guides() const noexcept { return guides_; }

  /// Whether snapping applies at all. Off with a modifier held, which is the
  /// universal way out of a snap that is fighting you.
  [[nodiscard]] bool snapping() const noexcept { return snapping_; }
  void set_snapping(bool snapping) noexcept { snapping_ = snapping; }

  /// Whether a corner drag keeps the layer's shape without being asked.
  ///
  /// Shift means "keep the shape" either way; this makes it the default rather
  /// than adding a second, opposite meaning for the same key. A modifier that
  /// reverses itself depending on a setting elsewhere is one nobody can
  /// predict.
  [[nodiscard]] bool aspect_locked() const noexcept { return aspect_locked_; }
  void set_aspect_locked(bool locked) noexcept { aspect_locked_ = locked; }

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

 private:
  /// The box's four corners in widget pixels, clockwise from the top left,
  /// with the rotation applied. Everything about the overlay — drawing, hit
  /// testing, dragging — is expressed against these, because rotation is only
  /// a rotation in *pixel* space: canvas fractions are anisotropic, and a
  /// square turned 45 degrees in them comes out as a rhombus.
  [[nodiscard]] std::array<std::pair<double, double>, 4> corners() const;
  [[nodiscard]] std::pair<double, double> centre_px() const;

  void drag_to(double x, double y, const Modifiers& modifiers);
  /// Moves or resizes the mask the drag has hold of.
  void drag_mask(double x, double y);
  void move_to(double x, double y, const Modifiers& modifiers);
  void resize_to(double x, double y, const Modifiers& modifiers);
  void rotate_to(double x, double y, const Modifiers& modifiers);

  void paint_overlay(Painter& painter, const Theme& theme) const;
  void paint_masks(Painter& painter, const Theme& theme) const;

  ImageView frame_;
  TextureView texture_;
  double canvas_aspect_ = 16.0 / 9.0;
  std::string placeholder_ = "No preview";

  std::optional<MonitorBox> box_;
  std::function<void(const MonitorBox&)> on_change_;
  std::function<void(const MonitorBox&)> on_commit_;
  std::function<void(std::size_t, const MaskOverlay&)> on_mask_change_;
  std::function<void(std::size_t, const MaskOverlay&)> on_mask_commit_;

  bool drop_lit_ = false;
  std::vector<MaskOverlay> masks_;
  /// The mask drag in flight: which one, whether by a corner, and where it
  /// started, so every frame is computed from the press rather than
  /// accumulated.
  std::optional<std::size_t> mask_dragging_;
  bool mask_resizing_ = false;
  MaskOverlay mask_origin_;

  bool snapping_ = true;
  bool aspect_locked_ = false;
  std::vector<SnapGuide> guides_;

  /// The gesture in flight. `origin_` is the box as it was at the press, so
  /// every frame of the drag is computed from where it started rather than
  /// accumulated — accumulating turns a snap into a place the box can never
  /// get out of, because each frame starts from the snapped value.
  TransformHandle dragging_ = TransformHandle::None;
  MonitorBox origin_;
  double press_x_ = 0.0;
  double press_y_ = 0.0;
};

}  // namespace cutline::ui
