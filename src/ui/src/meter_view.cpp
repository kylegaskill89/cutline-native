#include "cutline/ui/meter_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace cutline::ui {
namespace {

/// Width given over to the scale's numbers when they are shown. Enough for
/// "-48", measured rather than guessed at paint time — this is only the
/// reservation, and the text is right-aligned into it.
constexpr double kScaleWidth = 22.0;

/// How wide the bars themselves may get, and the least they may be squeezed to.
/// A bar's width carries no information — its height is the reading — so this
/// is only about being visible at one end and not absurd at the other.
constexpr double kMaxBarsWidth = 40.0;
constexpr double kMinBarWidth = 12.0;

/// The gap between two channels' bars. One pixel, so a stereo pair still reads
/// as two bars without the meter looking like a bar chart.
constexpr double kBarGap = 1.0;

/// How tall the peak-hold mark is.
constexpr double kHoldThickness = 2.0;

/// Where the bar changes colour. Green below, amber approaching the top, red
/// once the limiter is in play. The amber point is -6 rather than anything
/// derived: it is the conventional "getting close" mark, and it is about where
/// a mix stops having room for one more thing.
constexpr double kWarnDb = -6.0;
constexpr double kHotDb = 0.0;

constexpr Color kGreen{0.30f, 0.85f, 0.40f, 1.0f};
constexpr Color kAmber{0.95f, 0.75f, 0.20f, 1.0f};
constexpr Color kRed{0.95f, 0.28f, 0.25f, 1.0f};

[[nodiscard]] Color fade(const Color& color, float amount) noexcept {
  return Color{color.r, color.g, color.b, color.a * amount};
}

/// The colour a bar is at a given level. Read from the level rather than the
/// bar's height so it does not change when the scale does.
[[nodiscard]] Color ink_for(double db) noexcept {
  if (db >= kHotDb) return kRed;
  if (db >= kWarnDb) return kAmber;
  return kGreen;
}

}  // namespace

MeterView::MeterView() { set_clips_children(true); }

LayoutItem MeterView::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Horizontal) {
    // Happy to be narrow and happy to be given the width, since the bars
    // themselves are capped below — a meter docked in a thin column has to
    // still fit, and one in a wide panel must not answer with bars the width
    // of a door.
    return LayoutItem::flexible(1.0, kMinBarWidth + (shows_scale_ ? kScaleWidth : 0.0) +
                                         metrics.padding_x * 2.0);
  }
  // The tall direction is the useful one: height is resolution, and a meter
  // four control-heights short cannot be read to a decibel.
  return LayoutItem::flexible(1.0, metrics.control_height * 4.0);
}

void MeterView::layout(const LayoutContext& context) { metrics_ = context.metrics(); }

Rect MeterView::bars_area() const {
  const Rect whole = bounds();
  const Rect box{whole.x + metrics_.padding_x, whole.y + metrics_.padding_y,
                 whole.width - metrics_.padding_x * 2.0,
                 whole.height - metrics_.padding_y * 2.0};
  if (box.empty()) return {};
  const double left = shows_scale_ ? box.x + kScaleWidth : box.x;
  if (left >= box.right()) return {};

  // Capped rather than filling: past a certain width a bar says nothing more,
  // and a meter given half a panel reads as a chart of something.
  const double width = std::min(box.right() - left, kMaxBarsWidth);
  return Rect{left, box.y, width, box.height};
}

double MeterView::y_of(double db) const {
  const Rect area = bars_area();
  // Zero decibels at the top, the floor at the bottom: a meter is read as a
  // level rising towards a ceiling, so the ceiling has to be up.
  const double fraction = audio::meter_fraction(db, kMeterBottomDb, kMeterTopDb);
  return area.bottom() - area.height * fraction;
}

void MeterView::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = bars_area();
  if (area.empty()) return;

  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);

  // A level is read against black, for the same reason a scope is: the bar is
  // the light, and a pale trough would make a quiet mix the loud thing on
  // screen.
  painter.fill(area, 0.0, Fill::solid(Color{0.0f, 0.0f, 0.0f, 1.0f}));

  for (const double mark : kMeterMarks) {
    const double y = y_of(mark);
    painter.line(area.x, y, area.right(), y, fade(style.text, 0.2f), 1.0);
    if (!shows_scale_) continue;

    // Sat on the line rather than between two of them, so the number and the
    // level it names are at the same height.
    const Rect label{bounds().x + metrics_.padding_x, y - metrics_.small_font_size * 0.75,
                     kScaleWidth - 4.0, metrics_.small_font_size * 1.5};
    painter.text(text_run(label, std::format("{:.0f}", mark), style,
                          metrics_.small_font_size, TextAlign::Right, false));
  }

  const int count = std::clamp(levels_.count, 0, static_cast<int>(audio::kMaxMeterChannels));
  if (count == 0) return;

  const double bar_width =
      (area.width - kBarGap * (count - 1)) / static_cast<double>(count);
  if (bar_width <= 0.0) return;

  for (int c = 0; c < count; ++c) {
    const audio::ChannelLevel& level = levels_.channels[static_cast<std::size_t>(c)];
    const double x = area.x + static_cast<double>(c) * (bar_width + kBarGap);

    // The RMS bar is the solid one and the peak a faint column behind it. Both
    // in one lane rather than two, because they are two readings of the same
    // signal and side by side they read as two signals.
    const double peak_y = y_of(level.peak_db);
    if (peak_y < area.bottom()) {
      painter.fill(Rect{x, peak_y, bar_width, area.bottom() - peak_y}, 0.0,
                   Fill::solid(fade(ink_for(level.peak_db), 0.35f)));
    }

    const double rms_y = y_of(level.rms_db);
    if (rms_y < area.bottom()) {
      painter.fill(Rect{x, rms_y, bar_width, area.bottom() - rms_y}, 0.0,
                   Fill::solid(ink_for(level.rms_db)));
    }

    // The hold mark, only once it is clear of the bar it came from — otherwise
    // a steady tone draws a line along the top of its own bar, which reads as
    // an artefact rather than as a mark.
    const double hold_y = y_of(level.hold_db);
    if (level.hold_db > kMeterBottomDb && hold_y < peak_y - kHoldThickness) {
      painter.fill(Rect{x, hold_y, bar_width, kHoldThickness}, 0.0,
                   Fill::solid(ink_for(level.hold_db)));
    }

    if (level.over) {
      // Along the very top, where a bar cannot reach: the mix went past the
      // ceiling at some point and the meter has no other way to say so once
      // the moment has passed.
      painter.fill(Rect{x, area.y, bar_width, kHoldThickness}, 0.0, Fill::solid(kRed));
    }
  }

  painter.stroke(area, 0.0, fade(style.text, 0.3f), 1.0);
}

}  // namespace cutline::ui
