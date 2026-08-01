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
#include <string>
#include <vector>

namespace cutline::ui {

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

  // ------------------------------------------------------------- geometry --

  /// The strip along the top holding the times.
  [[nodiscard]] Rect ruler() const;
  /// The whole of one lane's row, gutter included.
  [[nodiscard]] Rect lane_rect(std::size_t lane) const;
  /// The part of it keyframes are drawn in.
  [[nodiscard]] Rect track_rect(std::size_t lane) const;
  /// Where a keyframe's diamond sits.
  [[nodiscard]] Rect keyframe_rect(std::size_t lane, std::size_t index) const;

  /// Where the time axis begins, and how wide it is. Inset from the track by
  /// `kEdgeInset` at each end — see the note there.
  [[nodiscard]] double axis_x() const;
  [[nodiscard]] double axis_width() const;

  [[nodiscard]] double x_of(double t) const;
  [[nodiscard]] double time_at(double x) const;

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
  /// Replaces the selection with everything the rubber band covers.
  void select_within(const Rect& band, bool add);
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

  double row_height_ = 22.0;
  double ruler_height_ = 20.0;
  double font_size_ = 12.0;

  std::function<void(std::size_t, std::size_t, double)> on_move_;
  std::function<void(std::size_t, std::size_t, double, double)> on_move_commit_;
  std::function<void()> on_select_;
  std::function<void(double)> on_scrub_;
  std::function<void(double, double)> on_context_menu_;
  std::function<void()> on_delete_;
};

}  // namespace cutline::ui
