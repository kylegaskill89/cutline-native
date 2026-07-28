#pragma once

/// The timeline.
///
/// What it draws is passed in as plain data rather than read from the project
/// model. That keeps the whole of `cutline::ui` free of the editor's model —
/// the widget can be laid out, hit tested and painted from a handful of
/// structs in a test, with no project, no media and no decoding — and it means
/// the same view can later show something that is not a project at all, like a
/// nested sequence or a range being previewed.
///
/// Building that data from a project belongs to the editor, above both.

#include "cutline/ui/layout.hpp"
#include "cutline/ui/timescale.hpp"
#include "cutline/ui/widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cutline::ui {

/// One clip, as far as drawing is concerned.
struct TimelineBlock {
  double start = 0.0;
  double end = 0.0;
  std::string label;
  bool selected = false;

  [[nodiscard]] double duration() const noexcept { return end - start; }

  friend bool operator==(const TimelineBlock&, const TimelineBlock&) = default;
};

struct TimelineTrack {
  std::string name;
  /// Audio tracks are shorter, because the theme says so.
  bool audio = false;
  bool muted = false;
  std::vector<TimelineBlock> blocks;

  friend bool operator==(const TimelineTrack&, const TimelineTrack&) = default;
};

struct TimelineModel {
  std::vector<TimelineTrack> tracks;
  /// How long the project is, which is what bounds scrolling. Zero means it is
  /// worked out from the blocks.
  double duration = 0.0;
  double fps = 30.0;

  /// The end of the last block, or `duration` where that is longer.
  [[nodiscard]] double content_duration() const noexcept;

  friend bool operator==(const TimelineModel&, const TimelineModel&) = default;
};

/// Which block a point landed on.
struct BlockRef {
  std::size_t track = 0;
  std::size_t block = 0;

  friend bool operator==(const BlockRef&, const BlockRef&) = default;
};

/// What a drag is doing.
enum class DragMode {
  None,
  /// Moving the playhead.
  Scrub,
  /// Sliding a whole clip along its track.
  Move,
  /// Pulling one edge of a clip, leaving the other where it is.
  TrimStart,
  TrimEnd,
};

/// The times a dragged edge should stick to: the start, the playhead, and the
/// edges of every other clip.
///
/// `exclude` is the clip being dragged, which must not snap to itself — its own
/// edges follow the pointer, so leaving them in pins the drag in place.
[[nodiscard]] std::vector<double> snap_points(const TimelineModel& model, double playhead,
                                              std::optional<BlockRef> exclude);

/// The nearest point within `tolerance`, or nothing. Ties go to the earlier
/// one, so a clip dropped exactly between two edges lands somewhere
/// predictable rather than somewhere that depends on iteration order.
[[nodiscard]] std::optional<double> nearest_snap(std::span<const double> points, double time,
                                                 double tolerance);

class TimelineView : public Widget {
 public:
  TimelineView();

  [[nodiscard]] const TimelineModel& model() const noexcept { return model_; }
  void set_model(TimelineModel model);

  [[nodiscard]] const TimeScale& scale() const noexcept { return scale_; }
  void set_scale(const TimeScale& scale);
  /// Zooms so the whole project fills the time area.
  void zoom_to_fit();

  [[nodiscard]] double playhead() const noexcept { return playhead_; }
  /// Snapped to a frame, and never negative.
  void set_playhead(double seconds);

  /// Called while the playhead is dragged, and once when it is clicked.
  void set_on_scrub(std::function<void(double)> on_scrub) { on_scrub_ = std::move(on_scrub); }
  /// Called when the selection changes. Null means everything was deselected.
  void set_on_select(std::function<void(std::optional<BlockRef>)> on_select) {
    on_select_ = std::move(on_select);
  }

  [[nodiscard]] std::optional<BlockRef> selection() const;
  void select(std::optional<BlockRef> block);

  /// Called once, on release, with the clip as it ended up. Not on every mouse
  /// move: the model is already updated live so the drag can be seen, and an
  /// edit that fired continuously would put a hundred entries in the undo
  /// stack for one gesture.
  void set_on_edit(std::function<void(BlockRef, TimelineBlock)> on_edit) {
    on_edit_ = std::move(on_edit);
  }

  [[nodiscard]] bool snapping() const noexcept { return snapping_; }
  void set_snapping(bool snapping) noexcept { snapping_ = snapping; }

  [[nodiscard]] DragMode drag_mode() const noexcept { return mode_; }
  [[nodiscard]] std::optional<BlockRef> dragging() const noexcept { return drag_; }

  /// What a drag starting at this point would do. The outer edges of a clip
  /// trim it and the middle moves it, which is how every editor behaves and
  /// what makes a trim reachable without a modifier.
  [[nodiscard]] DragMode zone_at(double x, double y) const;

  // -------------------------------------------------------------- geometry --
  //
  // Exposed because it is the whole of what the timeline is, and every one of
  // these is something to assert rather than measure off a screenshot.

  /// The column of track headers down the left.
  [[nodiscard]] Rect header_area() const;
  /// Everything to the right of it, where time is drawn.
  [[nodiscard]] Rect time_area() const;
  /// The strip of ticks along the top of the time area.
  [[nodiscard]] Rect ruler_area() const;
  /// Below the ruler, where the tracks are.
  [[nodiscard]] Rect tracks_area() const;

  /// A track's row across the time area, or empty if it is scrolled out of
  /// sight. Includes the part hidden behind the header column, so a block's
  /// position does not depend on how far the view is scrolled.
  [[nodiscard]] Rect track_rect(std::size_t track) const;
  [[nodiscard]] Rect header_rect(std::size_t track) const;
  [[nodiscard]] Rect block_rect(std::size_t track, std::size_t block) const;
  [[nodiscard]] double playhead_x() const;

  [[nodiscard]] std::optional<BlockRef> block_at(double x, double y) const;

  /// How tall the tracks are altogether, and how far they are scrolled.
  [[nodiscard]] const Viewport& vertical() const noexcept { return vertical_; }

  // ------------------------------------------------------------ behaviour --

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_wheel(const WheelEvent& event) override;

 private:
  [[nodiscard]] double track_height(std::size_t track) const noexcept;
  [[nodiscard]] double trim_handle_width(std::size_t track, std::size_t block) const;
  void scrub_to(double x);
  void drag_to(double x);
  void refresh_bounds();

  TimelineModel model_;
  TimeScale scale_;
  Viewport vertical_;

  double playhead_ = 0.0;
  bool snapping_ = true;

  DragMode mode_ = DragMode::None;
  std::optional<BlockRef> drag_;
  /// The clip as it was when the drag began. Every position is computed from
  /// this rather than from the last one, so rounding cannot accumulate over a
  /// long drag and leave the clip a frame off where the pointer is.
  TimelineBlock origin_;
  double press_x_ = 0.0;
  /// Whether the pointer has moved far enough for this to be a drag at all.
  bool moved_ = false;

  /// Taken from the theme at layout, because input arrives without one.
  Metrics metrics_;

  std::function<void(double)> on_scrub_;
  std::function<void(std::optional<BlockRef>)> on_select_;
  std::function<void(BlockRef, TimelineBlock)> on_edit_;
};

}  // namespace cutline::ui
