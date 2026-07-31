#include "cutline/ui/scopes_view.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cutline::ui {
namespace {

/// How bright a cell is drawn, given how full it is against the fullest.
///
/// A square root rather than a straight ratio. One very full cell — the sky, a
/// black bar — is often hundreds of times fuller than everything else, and
/// scaling linearly against it leaves the rest of the picture invisible. This
/// is what every scope does, and it is why a waveform shows faint detail at all.
///
/// Then a gain on top, because the root alone is not enough here. A scatter is
/// spread over 65,536 cells and a waveform over 256 levels a column, so a cell
/// holding one pixel against a peak of fifty comes out at fourteen per cent —
/// which on screen is not there at all. The gain lifts the single-pixel cells
/// to something visible and lets the dense ones saturate, which is what the
/// intensity control on a real scope does.
constexpr float kScopeGain = 3.0f;

/// The rooted share, which is what a *height* is drawn from — a histogram bar
/// says how many, so it must not saturate.
[[nodiscard]] float share_of(std::uint32_t count, std::uint32_t peak) noexcept {
  if (count == 0 || peak == 0) return 0.0f;
  return std::sqrt(static_cast<float>(count) / static_cast<float>(peak));
}

/// The same, with the gain, for anything drawn as *ink*.
[[nodiscard]] float brightness_of(std::uint32_t count, std::uint32_t peak) noexcept {
  return std::min(1.0f, share_of(count, peak) * kScopeGain);
}

[[nodiscard]] Color fade(const Color& color, float amount) noexcept {
  return Color{color.r, color.g, color.b, color.a * amount};
}

/// The eighths a graticule is ruled at. Enough to read a level against and few
/// enough not to be a grid.
constexpr int kGraticule = 4;

}  // namespace

std::string_view to_string(ScopeKind kind) noexcept {
  switch (kind) {
    case ScopeKind::Histogram: return "Histogram";
    case ScopeKind::Waveform: return "Waveform";
    case ScopeKind::Parade: return "Parade";
    case ScopeKind::Vectorscope: return "Vectorscope";
  }
  return "Histogram";
}

ScopesView::ScopesView() { set_clips_children(true); }

void ScopesView::set_readings(std::shared_ptr<const ScopeReadings> readings) {
  readings_ = std::move(readings);
}

void ScopesView::layout(const LayoutContext& context) { metrics_ = context.metrics(); }

Rect ScopesView::graph_area() const {
  const Rect box = bounds();
  const double pad = metrics_.padding_x;
  if (box.width <= pad * 2.0 || box.height <= pad * 2.0) return {};
  return box.inset(pad);
}

void ScopesView::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = graph_area();
  if (area.empty()) return;

  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);

  // A scope is read against black. Every one of these is a count of light, and
  // a light background would make the brightest parts the least visible.
  painter.fill(area, 0.0, Fill::solid(Color{0.0f, 0.0f, 0.0f, 1.0f}));

  if (readings_ == nullptr || !readings_->measured) {
    painter.text(text_run(area, "No frame to measure", style, metrics_.small_font_size,
                          TextAlign::Center, false));
    return;
  }

  switch (kind_) {
    case ScopeKind::Histogram: paint_histogram(painter, theme, area); break;
    case ScopeKind::Waveform:
      paint_waveform(painter, theme, area, readings_->waveform, style.text);
      break;
    case ScopeKind::Parade: paint_parade(painter, theme, area); break;
    case ScopeKind::Vectorscope: paint_vectorscope(painter, theme, area); break;
  }

  painter.stroke(area, 0.0, fade(style.text, 0.3f), 1.0);
}

void ScopesView::paint_histogram(Painter& painter, const Theme& theme, const Rect& area) const {
  const render::Histogram& histogram = readings_->histogram;
  const std::uint32_t peak = histogram.peak();
  if (peak == 0) return;

  // Levels along the bottom, so a reading can be placed without counting.
  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);
  for (int i = 1; i < kGraticule; ++i) {
    const double x = area.x + area.width * i / kGraticule;
    painter.line(x, area.y, x, area.bottom(), fade(style.text, 0.15f), 1.0);
  }

  // Additive, as a histogram is: where all three channels agree the trace comes
  // out white, which is what says a picture is neutral there.
  const std::array<std::pair<const std::array<std::uint32_t, 256>*, Color>, 3> traces{{
      {&histogram.red, Color{1.0f, 0.25f, 0.25f, 0.75f}},
      {&histogram.green, Color{0.25f, 1.0f, 0.35f, 0.75f}},
      {&histogram.blue, Color{0.35f, 0.5f, 1.0f, 0.75f}},
  }};

  const double bar = area.width / static_cast<double>(render::Histogram::kBins);
  for (const auto& [bins, colour] : traces) {
    for (std::size_t i = 0; i < render::Histogram::kBins; ++i) {
      const std::uint32_t count = (*bins)[i];
      if (count == 0) continue;
      const double height = area.height * share_of(count, peak);
      const double x = area.x + static_cast<double>(i) * bar;
      painter.fill(Rect{x, area.bottom() - height, std::max(1.0, bar), height}, 0.0,
                   Fill::solid(colour));
    }
  }
}

void ScopesView::paint_waveform(Painter& painter, const Theme& theme, const Rect& area,
                                const render::Waveform& wave, const Color& ink) const {
  if (wave.empty()) return;
  const std::uint32_t peak = wave.peak();
  if (peak == 0) return;

  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);
  for (int i = 1; i < kGraticule; ++i) {
    const double y = area.y + area.height * i / kGraticule;
    painter.line(area.x, y, area.right(), y, fade(style.text, 0.15f), 1.0);
  }

  // A cell per pixel of the widget rather than per column of the frame: the
  // frame is measured at whatever width it was scaled to, and the graph is
  // whatever width the panel is. Walking the widget means every column of the
  // graph is drawn once, however the two compare.
  const auto columns = static_cast<int>(std::floor(area.width));
  for (int column = 0; column < columns; ++column) {
    const int source = wave.columns == 0
                           ? 0
                           : std::min(wave.columns - 1,
                                      static_cast<int>(static_cast<double>(column) /
                                                       area.width * wave.columns));
    for (std::size_t level = 0; level < render::Waveform::kLevels; ++level) {
      const std::uint32_t count = wave.at(source, level);
      if (count == 0) continue;

      // Black at the bottom, white at the top: a waveform is read as a picture
      // of brightness, so brightness has to go up.
      const double y = area.bottom() - area.height * (static_cast<double>(level) /
                                                      (render::Waveform::kLevels - 1));
      const double cell = std::max(1.0, area.height / render::Waveform::kLevels);
      painter.fill(Rect{area.x + column, y, 1.0, cell}, 0.0,
                   Fill::solid(fade(ink, brightness_of(count, peak))));
    }
  }
}

void ScopesView::paint_parade(Painter& painter, const Theme& theme, const Rect& area) const {
  const std::array<std::pair<const render::Waveform*, Color>, 3> bands{{
      {&readings_->parade.red, Color{1.0f, 0.3f, 0.3f, 1.0f}},
      {&readings_->parade.green, Color{0.3f, 1.0f, 0.4f, 1.0f}},
      {&readings_->parade.blue, Color{0.4f, 0.55f, 1.0f, 1.0f}},
  }};

  // Three across, with a hair between them so the bands read as three graphs
  // rather than one wide one.
  constexpr double kGap = 2.0;
  const double each = (area.width - kGap * 2.0) / 3.0;
  if (each <= 1.0) return;

  for (std::size_t i = 0; i < bands.size(); ++i) {
    const Rect band{area.x + static_cast<double>(i) * (each + kGap), area.y, each, area.height};
    paint_waveform(painter, theme, band, *bands[i].first, bands[i].second);
  }
}

void ScopesView::paint_vectorscope(Painter& painter, const Theme& theme,
                                   const Rect& area) const {
  const render::Vectorscope& scope = readings_->vectorscope;
  const std::uint32_t peak = scope.peak();
  if (peak == 0) return;

  // Square and centred: chroma has no aspect ratio, and stretching it to the
  // panel would move every colour off its target.
  const double side = std::min(area.width, area.height);
  const Rect square{area.x + (area.width - side) * 0.5, area.y + (area.height - side) * 0.5,
                    side, side};

  const SurfaceStyle& style = theme.style(Part::Panel, State::Normal);
  const Color guide = fade(style.text, 0.25f);

  // The rim, the centre, and the six targets — which come from the same matrix
  // the scatter does, so a colour lands on its box rather than near it.
  painter.stroke(square, side * 0.5, guide, 1.0);
  painter.line(square.x + side * 0.5, square.y, square.x + side * 0.5, square.bottom(),
               fade(style.text, 0.12f), 1.0);
  painter.line(square.x, square.y + side * 0.5, square.right(), square.y + side * 0.5,
               fade(style.text, 0.12f), 1.0);

  constexpr double kTarget = 4.0;
  for (const render::VectorTarget& target : render::vector_targets()) {
    const double x = square.x + target.x * side;
    const double y = square.y + target.y * side;
    painter.stroke(Rect{x - kTarget, y - kTarget, kTarget * 2.0, kTarget * 2.0}, 1.0, guide,
                   1.0);
  }

  const double cell = side / render::Vectorscope::kSize;
  const Color ink{0.4f, 1.0f, 0.5f, 1.0f};
  for (int y = 0; y < render::Vectorscope::kSize; ++y) {
    for (int x = 0; x < render::Vectorscope::kSize; ++x) {
      const std::uint32_t count = scope.at(x, y);
      if (count == 0) continue;
      painter.fill(Rect{square.x + x * cell, square.y + y * cell, std::max(1.0, cell),
                        std::max(1.0, cell)},
                   0.0, Fill::solid(fade(ink, brightness_of(count, peak))));
    }
  }
}

}  // namespace cutline::ui
