#pragma once

/// The keyframe timeline: a clip's animation laid out in time.
///
/// The right-hand half of Premiere's Effect Controls panel, and the thing a
/// parameter row cannot be. A row says a keyframe exists at the playhead; this
/// says *where all of them are*, which is what shaping an animation actually
/// needs — a hold in the middle, a point nudged half a second later, two that
/// should have been one.
///
/// Draws its own rows rather than building a widget per keyframe, the same way
/// the timeline and the browser do. A clip with six animated properties and
/// twenty points each is a hundred and twenty widgets to lay out, style and
/// route input through, for a hundred and twenty diamonds.
///
/// Times are **clip-local seconds** throughout, matching the model. What that
/// is in timeline time is the caller's business, and the caller is the only
/// thing that knows where the clip starts.

#include "cutline/ui/layout.hpp"
#include "cutline/ui/widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::ui {

/// Which of a keyframe's two handles a point is on.
enum class KeyframeHandle {
  None,
  /// Toward the next keyframe.
  Out,
  /// Back toward the previous one.
  In,
};

/// One keyframe's bezier handles, in normalised segment space: x a fraction of
/// the segment's duration, y a fraction of its value change, both measured from
/// this keyframe's own end. The view never interprets them beyond drawing —
/// what a handle *means* is the model's business.
struct KeyframeHandles {
  double out_x = 1.0 / 3.0;
  double out_y = 1.0 / 3.0;
  double in_x = 1.0 / 3.0;
  double in_y = 1.0 / 3.0;
  /// The segment *leaving* this keyframe is shaped by handles rather than by a
  /// fixed curve. Drawn differently, because a handle on a segment that is not
  /// listening to it would be a control that does nothing.
  bool bezier = false;

  friend bool operator==(const KeyframeHandles&, const KeyframeHandles&) = default;
};

/// Where in the view a point lands.
struct KeyframeHit {
  std::size_t lane = 0;
  std::size_t index = 0;
  bool found = false;

  friend bool operator==(const KeyframeHit&, const KeyframeHit&) = default;
};

class KeyframeView : public Widget {
 public:
  /// One animated property.
  struct Lane {
    std::string name;
    /// Clip-local seconds, sorted.
    std::vector<double> times;

    /// The property's value sampled evenly from the start of the clip to its
    /// end, for the graph. In model units, and never read as a number: the
    /// graph is drawn against the curve's own highest and lowest, so what the
    /// units are does not matter and nothing here has to know them.
    ///
    /// Supplied rather than worked out, because interpolating keyframes is the
    /// core's job and a widget that did it would be a widget that had opinions
    /// about easing.
    std::vector<double> curve;

    /// Each keyframe's own value, one per entry in `times`, in the same units
    /// as `curve`. Empty when the caller has nothing to say, and then no
    /// handles are drawn — which is the honest answer, since a handle has to be
    /// placed against the two values it sits between.
    std::vector<double> values;

    /// Each keyframe's handles, one per entry in `times`. Empty for the same
    /// reason and with the same result.
    std::vector<KeyframeHandles> handles;

    friend bool operator==(const Lane&, const Lane&) = default;
  };

  struct Model {
    double duration = 0.0;
    std::vector<Lane> lanes;

    friend bool operator==(const Model&, const Model&) = default;
  };

  /// Width of the gutter the property names are written in.
  static constexpr double kNameWidth = 96.0;
  /// How far from a diamond's centre a press still counts as being on it.
  static constexpr double kGrabReach = 7.0;
  /// Half-width of a diamond.
  static constexpr double kDiamond = 4.0;
  /// How far in from each end the time axis starts.
  ///
  /// A keyframe at the very first or very last instant of a clip is common —
  /// switching the stopwatch on at the head of a clip makes one — and without
  /// this its diamond is drawn half outside the widget, where a press does not
  /// reach it at all: a point outside a widget's bounds routes somewhere else
  /// entirely. Wide enough that the whole grab reach is inside.
  static constexpr double kEdgeInset = kGrabReach;
  /// A press only becomes a drag once it has travelled this far, so a click
  /// that wobbles selects rather than nudging a keyframe off its time.
  static constexpr double kDragThreshold = 3.0;
  /// How tall an expanded lane's graph is.
  static constexpr double kGraphHeight = 52.0;
  /// The disclosure chevron's square, at the left of the gutter.
  static constexpr double kRevealWidth = 16.0;

  KeyframeView();

  void set_model(Model model);
  [[nodiscard]] const Model& model() const noexcept { return model_; }

  /// Moves one keyframe's time in place, without re-sorting.
  ///
  /// What a drag uses, so the diamond follows the pointer rather than jumping
  /// when the button comes up. Deliberately *not* sorted: the drag is holding
  /// an index, and re-ordering underneath it would leave it dragging whichever
  /// keyframe had taken that position. The document's own list is sorted when
  /// the move is committed, and the model is rebuilt from it afterwards.
  void nudge(std::size_t lane, std::size_t index, double t);

  /// Clip-local, and drawn as the line running down every lane.
  void set_playhead(double t) noexcept;
  [[nodiscard]] double playhead() const noexcept { return playhead_; }

  // ----------------------------------------------------------------- zoom --

  /// The furthest in the view goes, as a fraction of the clip. A hundredth is
  /// a frame or two of an ordinary clip filling the panel, which is as far in
  /// as anybody can place a keyframe by eye.
  static constexpr double kMinSpan = 0.01;

  /// The stretch of the clip the axis covers: where it starts, and how long it
  /// is. The whole clip by default, which is what Premiere's shows before
  /// anybody touches the zoom.
  [[nodiscard]] double view_start() const noexcept { return view_start_; }
  [[nodiscard]] double view_span() const noexcept;
  /// Clamped so the view never runs past either end of the clip, and never
  /// shrinks below `kMinSpan` of it.
  void set_view(double start, double span);
  /// Back to the whole clip.
  void reset_view() noexcept;

  /// Multiplies the zoom, keeping the time under `x` under `x`.
  ///
  /// What a wheel has to do. Zooming about the left edge instead means
  /// scrolling back to what you were looking at after every notch.
  void zoom_about(double x, double factor);

  /// Which keyframes are picked out. Cleared whenever the model changes, since
  /// indices into the old one mean nothing in the new.
  [[nodiscard]] const std::vector<KeyframeHit>& selection() const noexcept {
    return selection_;
  }
  void set_selection(std::vector<KeyframeHit> selection);
  [[nodiscard]] bool is_selected(std::size_t lane, std::size_t index) const noexcept;

  /// A keyframe was dragged to a new time. Called on every move of the drag,
  /// for a preview; `on_move_commit` is the one that should be written down.
  void set_on_move(std::function<void(std::size_t lane, std::size_t index, double to)> on_move) {
    on_move_ = std::move(on_move);
  }
  void set_on_move_commit(
      std::function<void(std::size_t lane, std::size_t index, double from, double to)> on_commit) {
    on_move_commit_ = std::move(on_commit);
  }

  /// The selection changed, by a click, a shift-click or a marquee.
  void set_on_select(std::function<void()> on_select) { on_select_ = std::move(on_select); }

  /// A press in the ruler, which is a scrub.
  void set_on_scrub(std::function<void(double)> on_scrub) { on_scrub_ = std::move(on_scrub); }

  /// A right-click with something selected, at the point it happened, for
  /// whoever owns the menu. The view has no opinion about what is on it: the
  /// operations belong to the editor, and a widget that knew about
  /// interpolation modes would be a widget that knew about the model.
  void set_on_context_menu(std::function<void(double x, double y)> on_menu) {
    on_context_menu_ = std::move(on_menu);
  }

  /// Delete, with a selection. What is on the other end has to decide what
  /// removing the last keyframe of a property means.
  void set_on_delete(std::function<void()> on_delete) { on_delete_ = std::move(on_delete); }

  /// Ctrl+C with a selection, and Ctrl+V. The clipboard is not the view's:
  /// keyframes copied here go back onto a *clip*, and the view has never known
  /// which clip it is showing.
  void set_on_copy(std::function<void()> on_copy) { on_copy_ = std::move(on_copy); }
  void set_on_paste(std::function<void()> on_paste) { on_paste_ = std::move(on_paste); }

  /// A handle was dragged. `x` and `y` are in normalised segment space, ready
  /// for the model. Called on every move for a preview; the commit is the one
  /// that should be written down, and carries where the handle started so an
  /// undo can put it back.
  void set_on_handle(
      std::function<void(std::size_t lane, std::size_t index, KeyframeHandle side, double x,
                         double y)>
          on_handle) {
    on_handle_ = std::move(on_handle);
  }
  void set_on_handle_commit(
      std::function<void(std::size_t lane, std::size_t index, KeyframeHandle side, double x,
                         double y)>
          on_commit) {
    on_handle_commit_ = std::move(on_commit);
  }

  /// A graph was opened or closed, by name.
  ///
  /// Reported rather than merely remembered here, because the panel this lives
  /// in is rebuilt from nothing on every edit — and a view that kept its own
  /// answer would close every graph the moment anything was changed. Which is
  /// exactly what happened, and is the same lesson the inspector's disclosure
  /// triangles learned.
  void set_on_expand(std::function<void(const std::string&, bool)> on_expand) {
    on_expand_ = std::move(on_expand);
  }

  // ------------------------------------------------------------- geometry --

  /// The strip along the top holding the times.
  [[nodiscard]] Rect ruler() const;
  /// The whole of one lane's row, gutter included — taller when expanded.
  [[nodiscard]] Rect lane_rect(std::size_t lane) const;
  /// The part of it keyframes are drawn in. One row high whether or not the
  /// lane is expanded: the diamonds stay put and the graph appears under them.
  [[nodiscard]] Rect track_rect(std::size_t lane) const;
  /// The graph under an expanded lane. Empty when it is not expanded, or when
  /// the lane has no curve to draw.
  [[nodiscard]] Rect graph_rect(std::size_t lane) const;
  /// The chevron that opens the graph.
  [[nodiscard]] Rect reveal_rect(std::size_t lane) const;

  /// Whether a lane is showing its graph.
  ///
  /// Remembered by the property's *name* rather than by its position, because
  /// the model is rebuilt on every edit and adding an effect renumbers every
  /// lane below it.
  [[nodiscard]] bool is_expanded(std::size_t lane) const;
  void set_expanded(std::string_view name, bool expanded);
  /// Where a keyframe's diamond sits.
  [[nodiscard]] Rect keyframe_rect(std::size_t lane, std::size_t index) const;

  /// Where the time axis begins, and how wide it is. Inset from the track by
  /// `kEdgeInset` at each end — see the note there.
  [[nodiscard]] double axis_x() const;
  [[nodiscard]] double axis_width() const;

  [[nodiscard]] double x_of(double t) const;
  [[nodiscard]] double time_at(double x) const;

  /// Half the side of a handle's square, and how far from its centre a press
  /// still counts as being on it.
  static constexpr double kHandleSize = 3.0;
  static constexpr double kHandleReach = 6.0;

  /// Where a handle sits, in widget pixels. Empty when the lane is not
  /// expanded, when there is no segment on that side, or when the caller
  /// supplied no values to place it against.
  ///
  /// Only for keyframes that are **selected**. Every handle of every keyframe
  /// on every lane at once is a thicket nobody can aim at, and Premiere shows
  /// them for the selection too.
  [[nodiscard]] Rect handle_rect(std::size_t lane, std::size_t index,
                                 KeyframeHandle side) const;

  /// The handle at a point, if a press there would grab one. `lane` and `index`
  /// name which keyframe it belongs to.
  [[nodiscard]] KeyframeHandle handle_at(double x, double y, std::size_t& lane,
                                         std::size_t& index) const;

  /// Where a value sits inside a lane's graph, and back again. The graph is
  /// scaled to the curve's own highest and lowest, so this needs the lane.
  /// `value_at` returns nothing when the curve is flat, where there is no
  /// scale to invert.
  [[nodiscard]] double graph_y_of(std::size_t lane, double value) const;
  [[nodiscard]] std::optional<double> graph_value_at(std::size_t lane, double y) const;

  /// The keyframe at a point, if a press there would grab one.
  [[nodiscard]] KeyframeHit keyframe_at(double x, double y) const;
  /// Which lane a y falls in. Past the end when none.
  [[nodiscard]] std::size_t lane_at(double y) const;

  /// The rubber band being dragged, if one is. Empty otherwise.
  [[nodiscard]] Rect marquee() const noexcept { return marquee_; }

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;
  bool on_wheel(const WheelEvent& event) override;

 private:
  void select_only(const KeyframeHit& hit);
  void toggle(const KeyframeHit& hit);
  /// How tall one lane is: a row, plus its graph when it is open.
  [[nodiscard]] double lane_height(std::size_t lane) const;
  /// The value and speed curves under an expanded lane.
  void paint_graph(Painter& painter, const Theme& theme, std::size_t lane) const;
  /// Replaces the selection with everything the rubber band covers.
  void select_within(const Rect& band, bool add);
  /// Turns a pointer position into the handle it describes, in normalised
  /// segment space. Falls back to whatever the handle already was on either
  /// axis it cannot answer for — a flat curve has no vertical scale to invert,
  /// and a segment of no duration has no horizontal one.
  [[nodiscard]] std::pair<double, double> handle_from(double x, double y) const;
  /// Asks for a fresh frame without asking for a fresh layout.
  void repaint();

  Model model_;
  double playhead_ = 0.0;
  double view_start_ = 0.0;
  /// Zero means the whole clip, so a model arriving with a new duration is
  /// shown whole rather than at whatever span the last one happened to have.
  double view_span_ = 0.0;
  std::vector<KeyframeHit> selection_;

  /// The drag in progress, if there is one.
  KeyframeHit dragging_;
  bool pressed_ = false;
  bool moved_ = false;
  double press_x_ = 0.0;
  double press_y_ = 0.0;
  /// The rubber band, and whether the drag that is running is one. Kept apart
  /// from `dragging_`: a press either takes hold of a keyframe or sweeps for
  /// them, and never both.
  bool banding_ = false;
  bool band_adds_ = false;
  Rect marquee_;
  /// What was selected when the band started, so dragging it with shift held
  /// adds rather than replacing — and so shrinking the band back releases what
  /// it no longer covers.
  std::vector<KeyframeHit> before_band_;
  /// Where the dragged keyframe started, so a commit can name both ends and a
  /// drag that goes nowhere reports nothing.
  double drag_from_ = 0.0;

  /// The handle drag in progress. Kept apart from `dragging_` for the same
  /// reason the band is: a press takes hold of exactly one thing.
  KeyframeHandle handle_side_ = KeyframeHandle::None;
  std::size_t handle_lane_ = 0;
  std::size_t handle_index_ = 0;
  double handle_x_ = 0.0;
  double handle_y_ = 0.0;

  double row_height_ = 22.0;
  double ruler_height_ = 20.0;
  double font_size_ = 12.0;

  /// By name, so a rebuild does not close every graph. See `is_expanded`.
  std::set<std::string, std::less<>> expanded_;

  std::function<void(std::size_t, std::size_t, double)> on_move_;
  std::function<void(std::size_t, std::size_t, double, double)> on_move_commit_;
  std::function<void()> on_select_;
  std::function<void(double)> on_scrub_;
  std::function<void(double, double)> on_context_menu_;
  std::function<void()> on_delete_;
  std::function<void()> on_copy_;
  std::function<void()> on_paste_;
  std::function<void(const std::string&, bool)> on_expand_;
  std::function<void(std::size_t, std::size_t, KeyframeHandle, double, double)> on_handle_;
  std::function<void(std::size_t, std::size_t, KeyframeHandle, double, double)>
      on_handle_commit_;
};

}  // namespace cutline::ui
