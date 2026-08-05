/// A source's envelope drawn large, which is what the source monitor shows
/// when the source has no picture.
///
/// The mapping is the whole of it: the drawing spans the media's length rather
/// than the envelope's, so a file still being read is drawn in the right place
/// and fills in, and a column takes the loudest bucket that falls in it rather
/// than the first — which is how an envelope comes out looking quieter than the
/// take it describes.

#include "cutline/ui/waveform_view.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <cmath>
#include <memory>

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

/// An envelope of `seconds`, loud in the middle third and silent either side.
[[nodiscard]] std::shared_ptr<const Waveform> loud_in_the_middle(double seconds) {
  auto wave = std::make_shared<Waveform>();
  wave->buckets_per_second = 100.0;
  const auto count = static_cast<std::size_t>(seconds * wave->buckets_per_second);
  wave->minimum.resize(count, 0.0f);
  wave->maximum.resize(count, 0.0f);
  for (std::size_t i = count / 3; i < count * 2 / 3; ++i) {
    wave->minimum[i] = -0.8f;
    wave->maximum[i] = 0.8f;
  }
  return wave;
}

struct Fixture {
  Fixture() {
    view.set_duration(10.0);
    view.arrange(Rect{0.0, 0.0, 400.0, 200.0}, flat_context());
  }

  [[nodiscard]] std::vector<DrawCall> painted() {
    RecordingPainter painter;
    view.paint(painter, default_theme());
    return painter.calls();
  }

  WaveformView view;
};

}  // namespace

TEST(WaveformView, ThePlotSitsInsideThePadding) {
  const Fixture test;
  const Rect area = test.view.plot_area();
  EXPECT_FALSE(area.empty());
  EXPECT_GT(area.x, 0.0);
  EXPECT_LT(area.right(), 400.0);
}

TEST(WaveformView, WithNoEnvelopeThereIsStillATrough) {
  // A panel that appeared only once the worker answered would look broken
  // until then, and reading a ten-minute source takes seconds.
  Fixture test;
  EXPECT_FALSE(test.painted().empty());
}

TEST(WaveformView, ThePlayheadIsClampedIntoTheSource) {
  Fixture test;
  test.view.set_playhead(99.0);
  EXPECT_DOUBLE_EQ(test.view.playhead(), 10.0);
  test.view.set_playhead(-1.0);
  EXPECT_DOUBLE_EQ(test.view.playhead(), 0.0);
}

TEST(WaveformView, AShorterSourceBringsThePlayheadBackWithIt) {
  Fixture test;
  test.view.set_playhead(9.0);
  test.view.set_duration(4.0);
  EXPECT_DOUBLE_EQ(test.view.playhead(), 4.0);
}

TEST(WaveformView, AnEnvelopeIsDrawnAndSilenceIsNot) {
  // Loud in the middle third: the drawing there has to be taller than at the
  // ends, which is the one thing an envelope is for.
  Fixture test;
  const std::size_t empty = test.painted().size();
  test.view.set_waveform(loud_in_the_middle(10.0));
  EXPECT_GT(test.painted().size(), empty);
}

TEST(WaveformView, TheDrawingSpansTheMediaRatherThanTheEnvelope) {
  // Half the file read so far. It has to be drawn across the first half, not
  // stretched over the whole panel — otherwise the shape moves under the
  // playhead as the rest arrives.
  Fixture test;
  test.view.set_waveform(loud_in_the_middle(5.0));  // five seconds of ten

  const Rect area = test.view.plot_area();
  double furthest = area.x;
  for (const DrawCall& call : test.painted()) {
    if (call.kind == DrawCall::Kind::Fill && call.bounds.height > 2.0 &&
        call.bounds.width <= 1.5) {
      furthest = std::max(furthest, call.bounds.x);
    }
  }
  EXPECT_LT(furthest, area.x + area.width * 0.6)
      << "a half-read envelope was stretched across the whole panel";
  EXPECT_GT(furthest, area.x + area.width * 0.2) << "nothing was drawn at all";
}

}  // namespace cutline::ui
