#include "cutline/ui/timescale.hpp"

#include "cutline/core/time.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace cutline::ui {
namespace {

/// The steps a ruler is allowed to mark at, above one second. Base sixty, not
/// base ten, because that is what the clock on the wall does.
constexpr std::array<double, 16> kCoarseSteps{{
    1.0, 2.0, 5.0, 10.0, 15.0, 30.0,                        // seconds
    60.0, 120.0, 300.0, 600.0, 900.0, 1800.0,               // minutes
    3600.0, 7200.0, 21600.0, 43200.0,                       // hours
}};

/// And below one second, in frames rather than in decimals of a second: an
/// editor counts in frames, and a mark between two of them means nothing.
constexpr std::array<double, 4> kFrameSteps{{1.0, 2.0, 5.0, 10.0}};

}  // namespace

double TimeScale::to_x(double seconds) const noexcept {
  return (seconds - start) * pixels_per_second;
}

double TimeScale::to_time(double x) const noexcept {
  if (pixels_per_second <= 0.0) return start;
  return start + x / pixels_per_second;
}

double TimeScale::width_of(double duration) const noexcept {
  return duration * pixels_per_second;
}

double TimeScale::visible_duration(double width) const noexcept {
  if (pixels_per_second <= 0.0) return 0.0;
  return std::max(0.0, width) / pixels_per_second;
}

void TimeScale::zoom_about(double x, double factor) noexcept {
  if (factor <= 0.0 || pixels_per_second <= 0.0) return;

  // The time under the cursor, held fixed across the change of scale.
  const double anchor = to_time(x);
  pixels_per_second =
      std::clamp(pixels_per_second * factor, kMinPixelsPerSecond, kMaxPixelsPerSecond);
  start = std::max(0.0, anchor - x / pixels_per_second);
}

void TimeScale::fit(double duration, double width) noexcept {
  start = 0.0;
  if (duration <= 0.0 || width <= 0.0) return;
  pixels_per_second =
      std::clamp(width / duration, kMinPixelsPerSecond, kMaxPixelsPerSecond);
}

void TimeScale::clamp_start(double content_duration) noexcept {
  start = std::clamp(start, 0.0, std::max(0.0, content_duration));
}

double tick_interval(double pixels_per_second, double fps, double min_spacing) noexcept {
  if (pixels_per_second <= 0.0) return 1.0;
  const double wanted = std::max(1.0, min_spacing);

  // Frames first, so that zoomed all the way in the ruler counts in the unit
  // the footage is actually cut in.
  const double frame = core::frame_duration(fps);
  for (const double step : kFrameSteps) {
    const double candidate = step * frame;
    // Never offer a sub-second step that is really a second; at 10 fps, ten
    // frames is one second and the coarse ladder should own it.
    if (candidate >= 1.0) break;
    if (candidate * pixels_per_second >= wanted) return candidate;
  }

  for (const double step : kCoarseSteps) {
    if (step * pixels_per_second >= wanted) return step;
  }

  // Past a day, keep doubling rather than inventing more names for it.
  double step = kCoarseSteps.back();
  while (step * pixels_per_second < wanted && step < 1e9) step *= 2.0;
  return step;
}

std::vector<Tick> ruler_ticks(const TimeScale& scale, double from, double to, double fps,
                              double min_label_spacing, double min_tick_spacing) {
  std::vector<Tick> ticks;
  if (to <= from || scale.pixels_per_second <= 0.0) return ticks;

  const double major = tick_interval(scale.pixels_per_second, fps, min_label_spacing);
  if (major <= 0.0) return ticks;

  // The subdivision is the largest step below the major one that is still far
  // enough apart to see. Below that there are simply no minor ticks, which is
  // better than a grey smear.
  double minor = major;
  const double smaller = tick_interval(scale.pixels_per_second, fps, min_tick_spacing);
  if (smaller < major && smaller > 0.0) {
    // Only if it divides the major step, or the minors drift out of alignment
    // and the ruler looks broken.
    const double ratio = major / smaller;
    const double rounded = std::round(ratio);
    if (rounded >= 2.0 && std::abs(ratio - rounded) < 1e-6) minor = smaller;
  }

  const double first = std::floor(from / minor) * minor;
  // Guarded rather than trusted: a pathological scale could otherwise ask for
  // millions of ticks and stall the paint.
  constexpr std::size_t kMaxTicks = 4096;

  for (std::size_t i = 0; i < kMaxTicks; ++i) {
    const double time = first + static_cast<double>(i) * minor;
    if (time > to) break;
    if (time < from - minor / 2.0) continue;

    const double steps = time / major;
    const bool is_major = std::abs(steps - std::round(steps)) < 1e-6;
    ticks.push_back(Tick{.time = time, .major = is_major});
  }
  return ticks;
}

}  // namespace cutline::ui
