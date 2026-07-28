#pragma once

/// Time along the timeline, in pixels.
///
/// Pure arithmetic, like the rest of layout, and for the same reason: a
/// timeline that puts the playhead a pixel out, or a ruler that labels every
/// 0.4 seconds, is maddening to debug by looking and trivial to state as an
/// assertion.
///
/// Positions here are relative to the left edge of the area time is drawn in,
/// not to the window. The widget adds its own origin, so nothing in this file
/// has to know where the track headers ended.

#include <cstddef>
#include <string>
#include <vector>

namespace cutline::ui {

/// The limits of zoom.
///
/// The lower bound keeps a whole day's footage from collapsing onto one pixel;
/// the upper one is well past a frame filling the window, which is as far in as
/// anyone can use.
inline constexpr double kMinPixelsPerSecond = 0.25;
inline constexpr double kMaxPixelsPerSecond = 20000.0;

/// Maps seconds to pixels and back.
struct TimeScale {
  double pixels_per_second = 100.0;
  /// The time at the left edge. Never negative: there is no timeline before
  /// zero, and allowing it only produces empty space nobody asked to scroll to.
  double start = 0.0;

  [[nodiscard]] double to_x(double seconds) const noexcept;
  [[nodiscard]] double to_time(double x) const noexcept;
  /// How wide a span of time is, independent of where it sits.
  [[nodiscard]] double width_of(double duration) const noexcept;

  /// How much time is visible across `width` pixels.
  [[nodiscard]] double visible_duration(double width) const noexcept;

  /// Multiplies the zoom, keeping the time currently under `x` under `x`.
  ///
  /// This is what a wheel zoom on a timeline has to do. Zooming about the left
  /// edge instead means scrolling back to where you were looking after every
  /// notch.
  void zoom_about(double x, double factor) noexcept;

  /// Sets the zoom so `duration` exactly fills `width`, and scrolls to zero.
  void fit(double duration, double width) noexcept;

  /// Pulls `start` back into range.
  ///
  /// The furthest the view may scroll is the end of the content sitting at the
  /// left edge, which leaves a whole screen of empty space after it. That is
  /// deliberate: dropping a clip at the very end of a project needs somewhere
  /// to drop it.
  void clamp_start(double content_duration) noexcept;

  friend bool operator==(const TimeScale&, const TimeScale&) = default;
};

/// One mark on the ruler.
struct Tick {
  double time = 0.0;
  /// Whether it carries a label. Minor ticks are unlabelled subdivisions.
  bool major = false;

  friend bool operator==(const Tick&, const Tick&) = default;
};

/// The interval a ruler should mark at, in seconds.
///
/// Not a decimal ladder. Time is base sixty above a second and base `fps`
/// below it, so the steps run 1/2/5/10 frames, then 1/2/5/10/15/30 seconds,
/// then the same in minutes, then 1/2/6/12/24 hours. A ruler that stepped in
/// tenths would be arithmetically tidy and unreadable.
///
/// `min_spacing` is how far apart labels have to be to stay legible.
[[nodiscard]] double tick_interval(double pixels_per_second, double fps,
                                   double min_spacing) noexcept;

/// Ticks covering `[from, to]`, labelled at `tick_interval` and subdivided
/// below that where the subdivisions would still be far enough apart to see.
[[nodiscard]] std::vector<Tick> ruler_ticks(const TimeScale& scale, double from, double to,
                                            double fps, double min_label_spacing = 90.0,
                                            double min_tick_spacing = 7.0);

}  // namespace cutline::ui
