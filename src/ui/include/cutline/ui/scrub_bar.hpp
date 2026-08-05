#pragma once

/// A time strip under a picture: where the playhead is, how long the thing is,
/// and which part of it has been marked.
///
/// The timeline's ruler does all of this and a great deal else — tracks,
/// markers, zoom, scrolling, five drag modes — and none of that belongs under a
/// monitor, which shows one thing from beginning to end at a fixed scale. So
/// this is the small one, and both monitors want it: the source monitor to mark
/// a span, the program monitor to scrub the sequence.
///
/// Time runs across the whole width. There is no zoom and no scroll, which is
/// the property that makes it readable at a glance — the left edge is the start
/// and the right edge is the end, always, so how far along the playhead sits is
/// the answer to "how far through are we" without anything else to check.

#include "cutline/ui/widget.hpp"

#include <functional>
#include <optional>

namespace cutline::ui {

class ScrubBar : public Widget {
 public:
  ScrubBar();

  /// How long the thing being scrubbed is. Zero means there is nothing to
  /// scrub, which draws an empty trough rather than nothing at all — a monitor
  /// with no source still has the strip, so the panel does not change shape
  /// when one arrives.
  void set_duration(double seconds) noexcept;
  [[nodiscard]] double duration() const noexcept { return duration_; }

  /// Where the playhead is, clamped into the duration.
  void set_playhead(double seconds) noexcept;
  [[nodiscard]] double playhead() const noexcept { return playhead_; }

  /// The marked span. Either end may be absent, and an absent end means the
  /// start or the end of the source — the same rule the marks themselves keep.
  void set_marks(std::optional<double> in, std::optional<double> out) noexcept;
  [[nodiscard]] std::optional<double> in_point() const noexcept { return in_; }
  [[nodiscard]] std::optional<double> out_point() const noexcept { return out_; }

  /// Called with a time whenever the playhead is dragged to it, including the
  /// press that starts the drag.
  ///
  /// The bar does not move its own playhead in response to a drag. Whatever
  /// owns it decides where the playhead really is — snapped to a frame, held at
  /// the end of a source — and sets it back. A bar that moved itself would show
  /// a position nothing else agreed with for as long as the drag lasted.
  std::function<void(double)> on_scrub;

  // ------------------------------------------------------------- geometry --

  /// The trough, inside the padding. Empty when there is no room.
  [[nodiscard]] Rect track_area() const;

  /// Where a time sits, in window coordinates. Times outside the duration are
  /// clamped, so nothing is ever drawn off the end.
  [[nodiscard]] double x_of(double seconds) const;

  /// The time under a window x, clamped into `[0, duration]`.
  [[nodiscard]] double time_at(double x) const;

  /// The marked span as a rectangle, empty when nothing is marked or when the
  /// span has no width.
  [[nodiscard]] Rect marked_area() const;

  /// The playhead's own rectangle — a thin upright, wide enough to see and to
  /// grab.
  [[nodiscard]] Rect playhead_area() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return false; }

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

  [[nodiscard]] bool scrubbing() const noexcept { return scrubbing_; }

 private:
  /// Reports the time under `x`, if there is anything to report it to.
  void scrub_to(double x);

  double duration_ = 0.0;
  double playhead_ = 0.0;
  std::optional<double> in_;
  std::optional<double> out_;
  bool scrubbing_ = false;
  Metrics metrics_;
};

}  // namespace cutline::ui
