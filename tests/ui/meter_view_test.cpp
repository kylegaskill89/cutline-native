/// The master meter, as drawn.
///
/// The ballistics are tested next door in `audio/meter_test.cpp`; what is left
/// here is the mapping — that a louder level really is drawn higher, that the
/// scale and the bars agree about where a decibel is, and that the widget says
/// nothing at all when it has nothing to say.

#include "cutline/ui/meter_view.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <algorithm>
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

[[nodiscard]] audio::MeterReading stereo(double left_db, double right_db,
                                         bool over = false) {
  audio::MeterReading reading;
  reading.count = 2;
  reading.channels[0] = {.peak_db = left_db, .rms_db = left_db, .hold_db = left_db,
                         .over = over};
  reading.channels[1] = {.peak_db = right_db, .rms_db = right_db, .hold_db = right_db,
                         .over = over};
  return reading;
}

struct Fixture {
  Fixture() {
    host = std::make_unique<WidgetHost>(std::make_unique<MeterView>());
    view = static_cast<MeterView*>(&host->root());
    host->resize(Rect{0.0, 0.0, 100.0, 240.0}, flat_context());
  }

  [[nodiscard]] std::vector<DrawCall> painted() {
    RecordingPainter painter;
    view->paint(painter, default_theme());
    return painter.calls();
  }

  std::unique_ptr<WidgetHost> host;
  MeterView* view = nullptr;
};

TEST(MeterView, PutsTheCeilingAtTheTopAndTheFloorAtTheBottom) {
  const Fixture fixture;
  const Rect area = fixture.view->bars_area();

  EXPECT_NEAR(fixture.view->y_of(kMeterTopDb), area.y, 0.001);
  EXPECT_NEAR(fixture.view->y_of(kMeterBottomDb), area.bottom(), 0.001);
}

TEST(MeterView, DrawsALouderLevelHigher) {
  const Fixture fixture;
  EXPECT_LT(fixture.view->y_of(-6.0), fixture.view->y_of(-24.0));
}

TEST(MeterView, ClampsALevelBelowTheFloorToTheFoot) {
  const Fixture fixture;
  EXPECT_DOUBLE_EQ(fixture.view->y_of(-200.0), fixture.view->y_of(kMeterBottomDb));
}

TEST(MeterView, TheBarsSitInsideThePanelsPadding) {
  const Fixture fixture;
  const Rect area = fixture.view->bars_area();
  const Rect box = fixture.view->bounds();

  EXPECT_FALSE(area.empty());
  EXPECT_GT(area.x, box.x);
  EXPECT_LT(area.right(), box.right());
}

TEST(MeterView, WithoutTheScaleTheBarsStartWhereTheNumbersWere) {
  Fixture fixture;
  const double left = fixture.view->bars_area().x;
  fixture.view->set_shows_scale(false);
  EXPECT_LT(fixture.view->bars_area().x, left);
}

// A meter given a whole panel is still a meter: a bar's width says nothing, so
// past a point the extra room is simply not taken.
TEST(MeterView, TheBarsAreCappedRatherThanFillingAWidePanel) {
  Fixture fixture;
  const double narrow = fixture.view->bars_area().width;
  fixture.host->resize(Rect{0.0, 0.0, 600.0, 240.0}, flat_context());
  EXPECT_LE(fixture.view->bars_area().width, narrow + 1.0);
}

TEST(MeterView, APanelTooSmallForBarsDrawsNone) {
  Fixture fixture;
  fixture.host->resize(Rect{0.0, 0.0, 4.0, 4.0}, flat_context());
  EXPECT_TRUE(fixture.view->bars_area().empty());
}

// The bar's foot is the bottom of the scale and its head is the level, so a
// loud channel's bar is the taller of the two.
TEST(MeterView, ALoudChannelsBarIsTallerThanAQuietOnes) {
  Fixture fixture;
  fixture.view->set_levels(stereo(-3.0, -40.0));

  const Rect area = fixture.view->bars_area();
  double left_top = area.bottom();
  double right_top = area.bottom();
  for (const DrawCall& call : fixture.painted()) {
    if (call.kind != DrawCall::Kind::Fill) continue;
    if (call.bounds.width >= area.width) continue;  // the trough, not a bar
    const double middle = call.bounds.x + call.bounds.width * 0.5;
    double& top = middle < area.x + area.width * 0.5 ? left_top : right_top;
    top = std::min(top, call.bounds.y);
  }

  EXPECT_LT(left_top, right_top);
  EXPECT_NEAR(left_top, fixture.view->y_of(-3.0), 1.0);
}

TEST(MeterView, ASilentMixDrawsNoBarAtAll) {
  Fixture fixture;
  fixture.view->set_levels(stereo(audio::kMeterFloorDb, audio::kMeterFloorDb));

  const Rect area = fixture.view->bars_area();
  for (const DrawCall& call : fixture.painted()) {
    if (call.kind != DrawCall::Kind::Fill) continue;
    if (call.bounds.width >= area.width) continue;
    ADD_FAILURE() << "a bar was drawn at y " << call.bounds.y;
  }
}

TEST(MeterView, MarksAnOverAcrossTheTopWhereNoBarCanReach) {
  Fixture quiet;
  quiet.view->set_levels(stereo(-40.0, -40.0, false));
  const std::size_t without = quiet.painted().size();

  Fixture tripped;
  tripped.view->set_levels(stereo(-40.0, -40.0, true));
  const auto calls = tripped.painted();
  EXPECT_GT(calls.size(), without);

  const Rect area = tripped.view->bars_area();
  const bool at_the_top = std::ranges::any_of(calls, [&](const DrawCall& call) {
    return call.kind == DrawCall::Kind::Fill && call.bounds.width < area.width &&
           std::abs(call.bounds.y - area.y) < 0.001;
  });
  EXPECT_TRUE(at_the_top);
}

TEST(MeterView, TheScaleIsMarkedWhereItSaysItIs) {
  Fixture fixture;
  const auto calls = fixture.painted();

  // Every mark is drawn as a line, and each one sits where `y_of` puts it.
  for (const double mark : kMeterMarks) {
    const double y = fixture.view->y_of(mark);
    const bool ruled = std::ranges::any_of(calls, [&](const DrawCall& call) {
      return call.kind == DrawCall::Kind::Line && std::abs(call.bounds.y - y) < 0.001;
    });
    EXPECT_TRUE(ruled) << mark;
  }
}

TEST(MeterView, MoreChannelsMeansNarrowerBarsRatherThanAWiderMeter) {
  Fixture two;
  two.view->set_levels(stereo(-3.0, -3.0));
  const Rect area = two.view->bars_area();

  audio::MeterReading six;
  six.count = 6;
  for (int c = 0; c < 6; ++c) six.channels[static_cast<std::size_t>(c)].rms_db = -3.0;

  Fixture many;
  many.view->set_levels(six);
  EXPECT_EQ(many.view->bars_area(), area);
}

}  // namespace
}  // namespace cutline::ui
