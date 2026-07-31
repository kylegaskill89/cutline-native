/// The scopes, as drawn.
///
/// The arithmetic is tested next door in `render/scopes_test.cpp`; what is left
/// here is whether the drawing says the same thing — that a graph appears at
/// all, that it is scaled to the widget rather than to the frame it was
/// measured from, and that an empty one says so rather than drawing four
/// convincing empty graphs.

#include "cutline/ui/scopes_view.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() {
  return LayoutContext{default_theme(), measurer()};
}

/// A frame of one colour, measured.
[[nodiscard]] std::shared_ptr<const ScopeReadings> readings_of(std::uint8_t r, std::uint8_t g,
                                                               std::uint8_t b, int w = 32,
                                                               int h = 16) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = r;
    pixels[i + 1] = g;
    pixels[i + 2] = b;
    pixels[i + 3] = 255;
  }
  const render::ScopeImage image{.pixels = pixels.data(), .width = w, .height = h};

  auto out = std::make_shared<ScopeReadings>();
  out->histogram = render::compute_histogram(image);
  out->waveform = render::compute_waveform(image, render::ScopeChannel::Luma);
  out->parade = render::compute_parade(image);
  out->vectorscope = render::compute_vectorscope(image);
  out->measured = true;
  return out;
}

struct Fixture {
  Fixture() {
    host = std::make_unique<WidgetHost>(std::make_unique<ScopesView>());
    view = static_cast<ScopesView*>(&host->root());
    host->resize(Rect{0.0, 0.0, 300.0, 200.0}, flat_context());
  }

  [[nodiscard]] std::size_t fills() {
    RecordingPainter painter;
    view->paint(painter, default_theme());
    return painter.count(DrawCall::Kind::Fill);
  }

  std::unique_ptr<WidgetHost> host;
  ScopesView* view = nullptr;
};

TEST(ScopesView, EveryKindHasAName) {
  for (const ScopeKind kind : {ScopeKind::Histogram, ScopeKind::Waveform, ScopeKind::Parade,
                               ScopeKind::Vectorscope}) {
    EXPECT_FALSE(to_string(kind).empty());
  }
}

TEST(ScopesView, WithNothingMeasuredItSaysSo) {
  Fixture fixture;
  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  // Rather than four convincing empty graphs.
  EXPECT_GT(painter.count(DrawCall::Kind::Text), 0u);
}

TEST(ScopesView, TheGraphSitsInsideThePanelsPadding) {
  const Fixture fixture;
  const Rect area = fixture.view->graph_area();
  const Rect box = fixture.view->bounds();

  EXPECT_FALSE(area.empty());
  EXPECT_GT(area.x, box.x);
  EXPECT_LT(area.right(), box.right());
}

TEST(ScopesView, APanelTooSmallForAGraphDrawsNone) {
  Fixture fixture;
  fixture.host->resize(Rect{0.0, 0.0, 4.0, 4.0}, flat_context());
  EXPECT_TRUE(fixture.view->graph_area().empty());
}

TEST(ScopesView, EachKindDrawsSomething) {
  for (const ScopeKind kind : {ScopeKind::Histogram, ScopeKind::Waveform, ScopeKind::Parade,
                               ScopeKind::Vectorscope}) {
    Fixture fixture;
    fixture.view->set_kind(kind);
    fixture.view->set_readings(readings_of(180, 90, 40));
    EXPECT_GT(fixture.fills(), 1u) << to_string(kind);
  }
}

// A histogram of one flat colour has three spikes and nothing else, so the
// drawing has far fewer fills than a picture with a spread of levels.
TEST(ScopesView, AFlatColourDrawsLessThanAVariedOne) {
  Fixture flat;
  flat.view->set_kind(ScopeKind::Histogram);
  flat.view->set_readings(readings_of(128, 128, 128));
  const std::size_t few = flat.fills();

  // A gradient across the frame: every level occupied.
  std::vector<std::uint8_t> pixels(256ull * 4 * 4);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 256; ++x) {
      const std::size_t at = (static_cast<std::size_t>(y) * 256 + x) * 4;
      pixels[at] = pixels[at + 1] = pixels[at + 2] = static_cast<std::uint8_t>(x);
      pixels[at + 3] = 255;
    }
  }
  const render::ScopeImage image{.pixels = pixels.data(), .width = 256, .height = 4};
  auto spread = std::make_shared<ScopeReadings>();
  spread->histogram = render::compute_histogram(image);
  spread->measured = true;

  Fixture varied;
  varied.view->set_kind(ScopeKind::Histogram);
  varied.view->set_readings(spread);

  EXPECT_GT(varied.fills(), few);
}

// The graph is the width of the panel, not of the frame it was measured from —
// the frame is scaled down before measuring and the two have no reason to
// match.
TEST(ScopesView, AWaveformIsDrawnAcrossThePanelWhateverTheFrameWas) {
  Fixture fixture;
  fixture.view->set_kind(ScopeKind::Waveform);
  fixture.view->set_readings(readings_of(200, 200, 200, /*w=*/8, /*h=*/4));

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  const Rect area = fixture.view->graph_area();
  double leftmost = area.right();
  double rightmost = area.x;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind != DrawCall::Kind::Fill || call.bounds.width > 2.0) continue;
    leftmost = std::min(leftmost, call.bounds.x);
    rightmost = std::max(rightmost, call.bounds.x);
  }
  // Eight columns of frame, spread over three hundred pixels of panel.
  EXPECT_GT(rightmost - leftmost, area.width * 0.8);
}

// Chroma has no aspect ratio, so stretching the scatter to a wide panel would
// move every colour off the target it is supposed to land on.
TEST(ScopesView, TheVectorscopeStaysSquare) {
  Fixture fixture;
  fixture.host->resize(Rect{0.0, 0.0, 400.0, 120.0}, flat_context());
  fixture.view->set_kind(ScopeKind::Vectorscope);
  fixture.view->set_readings(readings_of(255, 0, 0));

  RecordingPainter painter;
  fixture.view->paint(painter, default_theme());

  // The rim is the one stroke with equal sides.
  const bool round_rim = std::ranges::any_of(painter.calls(), [](const DrawCall& call) {
    return call.kind == DrawCall::Kind::Stroke &&
           std::abs(call.bounds.width - call.bounds.height) < 0.01 && call.bounds.width > 1.0;
  });
  EXPECT_TRUE(round_rim);
}

TEST(ScopesView, ChangingKindChangesWhatIsDrawn) {
  Fixture fixture;
  fixture.view->set_readings(readings_of(200, 40, 40));

  fixture.view->set_kind(ScopeKind::Histogram);
  RecordingPainter first;
  fixture.view->paint(first, default_theme());

  fixture.view->set_kind(ScopeKind::Vectorscope);
  RecordingPainter second;
  fixture.view->paint(second, default_theme());

  EXPECT_NE(first.kinds(), second.kinds());
}

TEST(ScopesView, ReadingsAreSharedRatherThanCopied) {
  // A full set is a couple of hundred kilobytes and the panel repaints far more
  // often than the frame changes.
  Fixture fixture;
  const std::shared_ptr<const ScopeReadings> readings = readings_of(10, 20, 30);
  fixture.view->set_readings(readings);
  EXPECT_EQ(fixture.view->readings(), readings.get());
}

}  // namespace
}  // namespace cutline::ui
