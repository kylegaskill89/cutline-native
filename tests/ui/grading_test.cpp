/// The grading controls: wheels, curves and range sliders, driven as a mouse
/// would drive them.

#include "cutline/ui/grading.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext context() { return LayoutContext{default_theme(), measurer()}; }

[[nodiscard]] MouseEvent press(double x, double y, int clicks = 1, Modifiers modifiers = {}) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left, .modifiers = modifiers,
                    .click_count = clicks};
}

[[nodiscard]] MouseEvent at(double x, double y) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left};
}

void arrange(Widget& widget, double width, double height) {
  widget.arrange(Rect{0.0, 0.0, width, height}, context());
}

[[nodiscard]] Color grey(double, double) { return Color{0.5f, 0.5f, 0.5f, 1.0f}; }

TEST(ColorWheel, APositionIsHeldToTheDisc) {
  ColorWheel wheel;
  wheel.set_position(3.0, 4.0);
  EXPECT_NEAR(wheel.x(), 0.6, 1e-9);
  EXPECT_NEAR(wheel.y(), 0.8, 1e-9);
}

TEST(ColorWheel, APressPutsThePuckThereWithUpPositive) {
  ColorWheel wheel(100.0);
  wheel.set_shade(grey);
  arrange(wheel, 104.0, 104.0);
  const Rect disc = wheel.disc();
  double x = 0.0;
  double y = 0.0;
  int committed = 0;
  wheel.set_on_change([&](double nx, double ny) {
    x = nx;
    y = ny;
  });
  wheel.set_on_commit([&](double, double) { ++committed; });

  // A quarter of the way up from the centre.
  EXPECT_TRUE(wheel.on_mouse_down(press(disc.x + disc.width / 2.0, disc.y + disc.height / 4.0)));
  EXPECT_NEAR(x, 0.0, 0.02);
  EXPECT_NEAR(y, 0.5, 0.02);
  EXPECT_EQ(committed, 0);
  wheel.on_mouse_move(at(disc.x + disc.width * 0.75, disc.y + disc.height / 2.0));
  EXPECT_NEAR(x, 0.5, 0.02);
  EXPECT_NEAR(y, 0.0, 0.02);
  wheel.on_mouse_up(at(disc.x + disc.width * 0.75, disc.y + disc.height / 2.0));
  EXPECT_EQ(committed, 1);
}

TEST(ColorWheel, ADoubleClickPutsThePuckBackInTheMiddle) {
  ColorWheel wheel(100.0);
  arrange(wheel, 104.0, 104.0);
  wheel.set_position(0.4, -0.3);
  int committed = 0;
  wheel.set_on_commit([&](double x, double y) {
    EXPECT_EQ(x, 0.0);
    EXPECT_EQ(y, 0.0);
    ++committed;
  });
  const Rect disc = wheel.disc();
  wheel.on_mouse_down(press(disc.x + 20.0, disc.y + disc.height / 2.0, 2));
  EXPECT_EQ(committed, 1);
  EXPECT_EQ(wheel.x(), 0.0);
}

TEST(ColorWheel, APressOutsideTheDiscIsNotTaken) {
  ColorWheel wheel(100.0);
  arrange(wheel, 300.0, 104.0);
  EXPECT_FALSE(wheel.on_mouse_down(press(5.0, 5.0)));
}

class CurveGraphTest : public ::testing::Test {
 protected:
  void SetUp() override { arrange(graph_, 212.0, 112.0); }

  /// A point of the plot by its curve coordinates.
  [[nodiscard]] std::pair<double, double> where(double x, double y) const {
    const Rect plot = graph_.plot();
    return {plot.x + x * plot.width, plot.y + (1.0 - y) * plot.height};
  }

  CurveGraph graph_{CurveGraph::Backdrop::Tone, Color{1.0f, 1.0f, 1.0f, 1.0f}, true, false, 112.0};
};

TEST_F(CurveGraphTest, APressAddsAPointThereAndADragMovesIt) {
  std::vector<CurvePointView> committed;
  graph_.set_on_commit([&](const std::vector<CurvePointView>& points) { committed = points; });
  const auto [x, y] = where(0.5, 0.5);
  graph_.on_mouse_down(press(x, y));
  ASSERT_EQ(graph_.points().size(), 1u);
  const auto [x2, y2] = where(0.5, 0.75);
  graph_.on_mouse_move(at(x2, y2));
  graph_.on_mouse_up(at(x2, y2));
  ASSERT_EQ(committed.size(), 1u);
  EXPECT_NEAR(committed[0].x, 0.5, 0.01);
  EXPECT_NEAR(committed[0].y, 0.75, 0.01);
}

TEST_F(CurveGraphTest, ControlClickTakesAPointAway) {
  graph_.set_points({{.x = 0.3, .y = 0.6}, {.x = 0.7, .y = 0.8}});
  std::vector<CurvePointView> committed{{}, {}, {}};
  graph_.set_on_commit([&](const std::vector<CurvePointView>& points) { committed = points; });
  const auto [x, y] = where(0.3, 0.6);
  graph_.on_mouse_down(press(x, y, 1, Modifiers{.control = true}));
  ASSERT_EQ(committed.size(), 1u);
  EXPECT_DOUBLE_EQ(committed[0].x, 0.7);
}

TEST_F(CurveGraphTest, ADoubleClickOffThePointsStraightensTheCurve) {
  graph_.set_points({{.x = 0.3, .y = 0.6}});
  const auto [x, y] = where(0.8, 0.2);
  graph_.on_mouse_down(press(x, y, 2));
  EXPECT_TRUE(graph_.points().empty());
}

TEST_F(CurveGraphTest, TheEndOfAStraightCurveCanBeTakenHoldOf) {
  const auto [x, y] = where(1.0, 1.0);
  graph_.on_mouse_down(press(x, y));
  const auto [x2, y2] = where(1.0, 0.8);
  graph_.on_mouse_move(at(x2, y2));
  ASSERT_EQ(graph_.points().size(), 1u);
  EXPECT_DOUBLE_EQ(graph_.points()[0].x, 1.0);
  EXPECT_NEAR(graph_.points()[0].y, 0.8, 0.01);
}

TEST(CurveGraph, APressOnAHueCurveHoldsTheRestOfItWithAPointEitherSide) {
  CurveGraph graph(CurveGraph::Backdrop::Hue, Color{1.0f, 1.0f, 1.0f, 1.0f}, false, true, 112.0);
  arrange(graph, 212.0, 112.0);
  const Rect plot = graph.plot();
  graph.on_mouse_down(press(plot.x + plot.width * 0.5, plot.y + plot.height * 0.5));
  ASSERT_EQ(graph.points().size(), 3u);
  for (const CurvePointView& point : graph.points()) EXPECT_DOUBLE_EQ(point.y, 0.5);
  // Near an end, the point either side goes round to the other.
  CurveGraph edge(CurveGraph::Backdrop::Hue, Color{1.0f, 1.0f, 1.0f, 1.0f}, false, true, 112.0);
  arrange(edge, 212.0, 112.0);
  edge.on_mouse_down(press(plot.x + plot.width * 0.02, plot.y + plot.height * 0.5));
  ASSERT_EQ(edge.points().size(), 3u);
  EXPECT_GT(edge.points().back().x, 0.9);
}

TEST(RangeSlider, DraggingTheTopHandlesMovesTheEnds) {
  RangeSlider slider([](double) { return Color{0.5f, 0.5f, 0.5f, 1.0f}; }, false);
  arrange(slider, 216.0, 30.0);
  slider.set_range(0.2, 0.8, 0.0);
  const Rect track = slider.track();
  double low = -1.0;
  slider.set_on_commit([&](double l, double, double) { low = l; });
  slider.on_mouse_down(press(track.x + track.width * 0.2, track.y - 2.0));
  slider.on_mouse_move(at(track.x + track.width * 0.4, track.y - 2.0));
  slider.on_mouse_up(at(track.x + track.width * 0.4, track.y - 2.0));
  EXPECT_NEAR(low, 0.4, 0.01);
  EXPECT_NEAR(slider.high(), 0.8, 1e-9);
}

TEST(RangeSlider, TheBottomHandlesSetTheSoftness) {
  RangeSlider slider([](double) { return Color{0.5f, 0.5f, 0.5f, 1.0f}; }, false);
  arrange(slider, 216.0, 30.0);
  slider.set_range(0.3, 0.6, 0.0);
  const Rect track = slider.track();
  const double below = track.y + track.height + 3.0;
  slider.on_mouse_down(press(track.x + track.width * 0.6, below));
  slider.on_mouse_move(at(track.x + track.width * 0.7, below));
  slider.on_mouse_up(at(track.x + track.width * 0.7, below));
  EXPECT_NEAR(slider.soft(), 0.1, 0.01);
}

TEST(RangeSlider, AHueRangeSlidesRoundTheEnd) {
  RangeSlider slider([](double) { return Color{0.5f, 0.5f, 0.5f, 1.0f}; }, true);
  arrange(slider, 216.0, 30.0);
  slider.set_range(0.8, 0.9, 0.0);
  const Rect track = slider.track();
  const double y = track.y + track.height * 0.5;
  slider.on_mouse_down(press(track.x + track.width * 0.85, y));
  slider.on_mouse_move(at(track.x + track.width * 1.0, y));
  slider.on_mouse_up(at(track.x + track.width * 1.0, y));
  // Moved on by 0.15: the low end past 0.9, the high end round past nought.
  EXPECT_NEAR(slider.low(), 0.95, 0.01);
  EXPECT_NEAR(slider.high(), 0.05, 0.01);
}

TEST(RangeSlider, ADoubleClickTakesEverythingAgain) {
  RangeSlider slider([](double) { return Color{0.5f, 0.5f, 0.5f, 1.0f}; }, true);
  arrange(slider, 216.0, 30.0);
  slider.set_range(0.2, 0.3, 0.1);
  slider.on_mouse_down(press(50.0, 15.0, 2));
  EXPECT_EQ(slider.low(), 0.0);
  EXPECT_EQ(slider.high(), 1.0);
  EXPECT_EQ(slider.soft(), 0.0);
}

}  // namespace
}  // namespace cutline::ui
