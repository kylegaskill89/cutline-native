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

/// One point of a free-drawn mask, with the handle either side of it.
///
/// The same shape as `core::MaskPoint` and deliberately not that type: this
/// header describes what a widget draws, in canvas fractions, and knows nothing
/// about a project. The conversion is one line in the editor either way.
struct MaskVertex {
  double x = 0.0;
  double y = 0.0;
  /// Towards the previous point, and towards the next. Both zero is a corner.
  double in_x = 0.0;
  double in_y = 0.0;
  double out_x = 0.0;
  double out_y = 0.0;

  friend bool operator==(const MaskVertex&, const MaskVertex&) = default;
};

/// One effect's mask, drawn over the picture.
///
/// In canvas fractions like everything else here, because a shape on the frame
/// is a place on the frame — the mask is stored in fractions of the *layer*,
/// and turning one into the other needs the clip's transform, which is the
/// editor's business rather than this widget's.
struct MaskOverlay {
  /// 1 for an ellipse, 2 for a rectangle, 3 for a free-drawn path, matching
  /// `core::MaskShape`.
  int shape = 1;
  double x = 0.5;
  double y = 0.5;
  /// Half-extents, as fractions of the canvas. Meaningless for a path, which
  /// is described by its corners instead.
  double width = 0.25;
  double height = 0.25;
  double rotation = 0.0;  ///< degrees, clockwise

  /// A path's points, each an offset from the centre above in fractions of the
  /// canvas — the same units everything else here is in. Empty for the other
  /// two shapes.
  ///
  /// These are the points somebody placed, handles and all, rather than the
  /// outline they describe. The view flattens them to draw, through the same
  /// `core::flatten_mask_path` the renderer fills with.
  std::vector<MaskVertex> points;

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

  /// Starts placing a fresh path on the mask at `index`, discarding whatever
  /// shape it had.
  ///
  /// While this is on, a press on the picture puts a point down rather than
  /// picking anything up, and holding and dragging as you place pulls the
  /// handles out of it — which is how a curve gets drawn in one gesture rather
  /// than placed and then bent. Pressing the first point again closes the path
  /// and ends the mode, as does `finish_mask_drawing`.
  void begin_mask_drawing(std::size_t index);

  /// Leaves placing mode, keeping whatever was placed.
  void finish_mask_drawing();

  /// Which mask is being drawn, if any.
  [[nodiscard]] std::optional<std::size_t> drawing_mask() const noexcept {
    return mask_drawing_;
  }

  // ---------------------------------------------------------- dragging out --

  /// A drag that began on the picture and was released, at a point in the same
  /// coordinates as widget bounds — so whoever wired it up can ask the timeline
  /// what time that was.
  ///
  /// The source monitor's way onto the timeline, and Premiere's: what is in the
  /// monitor is what is about to be placed, so dragging it there is the shortest
  /// statement of that. Offered last of the gestures on this widget — a layer's
  /// handles and a mask both come first — so it can only start where nothing
  /// else wanted the press.
  ///
  /// The monitor does not know what it is showing or what dropping it would
  /// mean. Both are questions about a sequence, and this layer does not know
  /// sequences exist.
  void set_on_drag_out(std::function<void(double x, double y)> on_drag_out) {
    on_drag_out_ = std::move(on_drag_out);
  }
  /// Whether a drag out of the picture is in the air, and where it has got to.
  [[nodiscard]] bool dragging_out() const noexcept { return dragging_out_; }
  [[nodiscard]] double drag_x() const noexcept { return drag_x_; }
  [[nodiscard]] double drag_y() const noexcept { return drag_y_; }

  /// Which mask a press at this point would take hold of, and whether by its
  /// body or by a corner. Nothing when the press is on no mask.
  [[nodiscard]] std::optional<std::size_t> mask_at(double x, double y) const;
  /// Where a mask's resize grip is, in widget pixels. Empty when there is no
  /// such mask or no picture to draw it over.
  [[nodiscard]] Rect mask_grip(std::size_t index) const;
  /// The handle on one corner of a free-drawn path. Empty when the mask is not
  /// one, or the corner is not there.
  [[nodiscard]] Rect mask_corner_grip(std::size_t index, std::size_t corner) const;

  /// Which side of a point a bezier handle belongs to.
  enum class MaskHandle { In, Out };

  /// The grab square for one of a point's two bezier handles. Empty unless that
  /// point is the selected one — handles are shown for the point being worked
  /// on and nowhere else, or a path of any size becomes a thicket of squares.
  [[nodiscard]] Rect mask_handle_grip(std::size_t index, std::size_t corner,
                                      MaskHandle side) const;

  /// The point whose handles are showing, if any.
  [[nodiscard]] std::optional<std::size_t> selected_mask_point() const noexcept {
    return mask_selected_;
  }

  /// Gives a sharp point handles that follow the run of the path through it, so
  /// double-clicking a corner rounds it off rather than doing nothing visible.
  ///
  /// Along the line between its neighbours, a third of the way to each, which
  /// is the arrangement that leaves no crease and is what every drawing program
  /// hands you when it smooths a point.
  void curve_mask_point(std::size_t index, std::size_t corner);

  /// Puts a point on the outline nearest a place on the widget, splitting the
  /// curve there so the shape is unchanged. Nothing when the place is not near
  /// enough to the path to have meant it.
  std::optional<std::size_t> add_mask_point(std::size_t index, double x, double y);

  /// One press while the pen is out: another point, or the close that ends it.
  bool place_mask_point(const MouseEvent& event);

  /// A place on the widget as an offset from a mask's centre, in the fractions
  /// its points are kept in — the mask's own frame, so a turned mask takes
  /// points where the pointer is rather than where the rotation sends them.
  [[nodiscard]] std::pair<double, double> mask_local(std::size_t index, double x,
                                                     double y) const;

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

  /// The drag out of the picture. `pressed_out_` is a press that could become
  /// one; `dragging_out_` is one that has travelled far enough to be a drag
  /// rather than a click, which is the same threshold the media pool uses.
  std::function<void(double, double)> on_drag_out_;
  bool pressed_out_ = false;
  bool dragging_out_ = false;
  double drag_x_ = 0.0;
  double drag_y_ = 0.0;

  bool drop_lit_ = false;
  std::vector<MaskOverlay> masks_;
  /// The mask drag in flight: which one, whether by a corner, and where it
  /// started, so every frame is computed from the press rather than
  /// accumulated.
  std::optional<std::size_t> mask_dragging_;
  bool mask_resizing_ = false;
  /// Which corner of a free-drawn path is being dragged, when one is. A path
  /// has no size handle: every corner is a handle, and moving one is the only
  /// way its shape ever changes.
  std::optional<std::size_t> mask_corner_;
  /// The bezier handle being pulled, when it is one of those rather than the
  /// point it belongs to.
  std::optional<MaskHandle> mask_handle_;
  /// Whether the pair is being broken as it is pulled — the modifier held at
  /// the press, so letting go of it halfway does not change what the drag is.
  bool mask_handle_broken_ = false;
  /// Whether this gesture began by putting a point down.
  ///
  /// The release commits when the shape differs from what it was at the press,
  /// and `mask_origin_` has to carry the new point for the handle drag to have
  /// a base to measure from — so the two agree and nothing would be written.
  /// Placing says so directly instead.
  bool mask_placed_ = false;
  /// The point whose handles are on show. Set by pressing one, cleared by
  /// pressing anywhere that is not part of this path.
  std::optional<std::size_t> mask_selected_;
  /// The mask being placed point by point, when one is.
  std::optional<std::size_t> mask_drawing_;
  /// Where the pointer was last seen while placing, for the line that trails
  /// from the last point to it.
  double draw_x_ = 0.0;
  double draw_y_ = 0.0;
  bool draw_pointer_known_ = false;
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
