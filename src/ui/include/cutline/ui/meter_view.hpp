#pragma once

/// The master meter, drawn.
///
/// Handed levels rather than samples, the same split the scopes use: the
/// ballistics live in `audio::Meter` where they can be asserted against a
/// buffer, and this decides only where a bar reaches and what colour it is.
///
/// The scale is decibels, non-linear in the way every meter is: the top of the
/// range gets most of the height, because the difference between -3 and 0 is
/// the one anybody is watching for and the difference between -55 and -50 is
/// not. That falls out of plotting decibels directly — the linear amplitudes
/// they stand for are already logarithmically spaced.

#include "cutline/audio/meter.hpp"
#include "cutline/ui/widget.hpp"

#include <array>

namespace cutline::ui {

/// The span of the scale. -60 is quiet enough to be silence for editing
/// purposes, and the few decibels above zero are there so an over reads as
/// over-the-top rather than as merely full.
inline constexpr double kMeterTopDb = 6.0;
inline constexpr double kMeterBottomDb = -60.0;

/// Where the scale is marked. Every twelve decibels below zero would be even
/// and unmemorable; these are the numbers an editor actually thinks in.
inline constexpr std::array<double, 6> kMeterMarks{0.0, -6.0, -12.0, -24.0, -36.0, -48.0};

class MeterView : public Widget {
 public:
  MeterView();

  void set_levels(const audio::MeterReading& levels) noexcept { levels_ = levels; }
  [[nodiscard]] const audio::MeterReading& levels() const noexcept { return levels_; }

  /// Whether the scale's numbers are drawn beside the bars. Off in a narrow
  /// dock, where there is no width to spare and the graticule alone still says
  /// where the top is.
  void set_shows_scale(bool shows) noexcept { shows_scale_ = shows; }
  [[nodiscard]] bool shows_scale() const noexcept { return shows_scale_; }

  /// The bars' area, inside the padding and to the right of the scale.
  [[nodiscard]] Rect bars_area() const;

  /// Where a level sits inside `bars_area`, in pixels from the top. Public so
  /// the graticule and the bars cannot disagree about the mapping.
  [[nodiscard]] double y_of(double db) const;

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  audio::MeterReading levels_;
  bool shows_scale_ = true;
  Metrics metrics_;
};

}  // namespace cutline::ui
