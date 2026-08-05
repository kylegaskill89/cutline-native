#include "cutline/ui/waveform_view.hpp"

#include "cutline/ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cutline::ui {
namespace {

[[nodiscard]] Color fade(const Color& color, float amount) noexcept {
  return Color{color.r, color.g, color.b, color.a * amount};
}

}  // namespace

WaveformView::WaveformView() { set_clips_children(true); }

void WaveformView::set_waveform(std::shared_ptr<const Waveform> wave) {
  wave_ = std::move(wave);
}

void WaveformView::set_duration(double seconds) noexcept {
  duration_ = std::max(0.0, seconds);
  playhead_ = std::clamp(playhead_, 0.0, duration_);
}

void WaveformView::set_playhead(double seconds) noexcept {
  playhead_ = std::clamp(seconds, 0.0, duration_);
}

LayoutItem WaveformView::sizing(Axis axis, const LayoutContext& context) const {
  if (axis == Axis::Horizontal) return LayoutItem::flexible(1.0, context.metrics().control_height);
  // Takes what it is given, like the picture it stands in for.
  return LayoutItem::flexible(1.0, context.metrics().control_height * 2.0);
}

void WaveformView::layout(const LayoutContext& context) { metrics_ = context.metrics(); }

Rect WaveformView::plot_area() const {
  const Rect whole = bounds();
  const Rect box{whole.x + metrics_.padding_x, whole.y + metrics_.padding_y,
                 whole.width - metrics_.padding_x * 2.0,
                 whole.height - metrics_.padding_y * 2.0};
  return box.empty() ? Rect{} : box;
}

void WaveformView::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = plot_area();
  if (area.empty()) return;

  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);

  // Against black, like the meter and the scopes: the envelope is the light,
  // and a pale trough would make a quiet take the loud thing on screen.
  painter.fill(area, 0.0, Fill::solid(Color{0.0f, 0.0f, 0.0f, 1.0f}));

  const double middle = area.y + area.height * 0.5;
  painter.line(area.x, middle, area.right(), middle, fade(style.text, 0.25f), 1.0);

  if (wave_ != nullptr && !wave_->empty() && duration_ > 0.0) {
    const SurfaceStyle& clip = theme.style(Part::Clip, State::Selected);
    const double reach = area.height * 0.5;
    const std::size_t buckets = wave_->size();

    // One column of the panel at a time, taking the loudest bucket that falls
    // in it. Sampling one bucket per column instead would drop peaks between
    // them, which is how an envelope comes out looking quieter than the take.
    for (double x = area.x; x < area.right(); x += 1.0) {
      const double from = (x - area.x) / area.width * duration_;
      const double to = (x + 1.0 - area.x) / area.width * duration_;

      const auto first = static_cast<std::size_t>(std::max(0.0, from * wave_->buckets_per_second));
      const auto last = static_cast<std::size_t>(std::max(0.0, to * wave_->buckets_per_second));
      if (first >= buckets) break;

      double low = 0.0;
      double high = 0.0;
      for (std::size_t i = first; i <= std::min(last, buckets - 1); ++i) {
        low = std::min(low, static_cast<double>(wave_->minimum[i]));
        high = std::max(high, static_cast<double>(wave_->maximum[i]));
      }

      const double top = middle - std::clamp(high, -1.0, 1.0) * reach;
      const double bottom = middle - std::clamp(low, -1.0, 1.0) * reach;
      // A hairline at silence, so a quiet passage is still a line rather than
      // a hole in the drawing.
      painter.fill(Rect{x, top, 1.0, std::max(1.0, bottom - top)}, 0.0,
                   Fill::solid(clip.fill.color));
    }
  }

  if (duration_ > 0.0) {
    const double x = area.x + area.width * std::clamp(playhead_ / duration_, 0.0, 1.0);
    painter.line(x, area.y, x, area.bottom(),
                 theme.style(Part::Playhead, State::Normal).fill.color, 1.5);
  }

  painter.stroke(area, 0.0, fade(style.text, 0.3f), 1.0);
}

}  // namespace cutline::ui
