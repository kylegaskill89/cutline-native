/// The two containers a docked editor is made of.
///
/// Both are thin wrappers over arithmetic already proved in `layout_test`, so
/// what these check is the wiring: that a divider drag reaches the panes, that
/// scrolling actually moves the content and hit testing follows it, and that
/// both take their sizes from the theme rather than from constants.

#include "cutline/ui/widgets.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext context_for(const Theme& theme) {
  return LayoutContext{theme, measurer()};
}

[[nodiscard]] LayoutContext flat_context() { return context_for(default_theme()); }

[[nodiscard]] MouseEvent press(double x, double y) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left};
}

// ---------------------------------------------------------------- splitter --

/// A two-pane splitter inside a host, so drags go through real routing.
struct Split {
  explicit Split(Axis axis = Axis::Horizontal) {
    host = std::make_unique<WidgetHost>(std::make_unique<Splitter>(axis));
    splitter = static_cast<Splitter*>(&host->root());
    left = &splitter->emplace<Panel>();
    right = &splitter->emplace<Panel>();
    host->resize(Rect{0.0, 0.0, 800.0, 400.0}, flat_context());
  }

  [[nodiscard]] double divider_centre() const {
    const double width = default_theme().metrics.splitter_width;
    return left->bounds().right() + width / 2.0;
  }

  /// A mouse move plus the layout pass the frame loop would run afterwards.
  /// Splitting the two is the point: an input handler has no theme to lay out
  /// with, so it marks and the frame resolves.
  void drag_to(double x, double y) {
    host->mouse_move(press(x, y));
    host->update_layout(flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  Splitter* splitter = nullptr;
  Panel* left = nullptr;
  Panel* right = nullptr;
};

TEST(Splitter, PanesFillTheWidthWithADividerBetween) {
  const Split split;
  const double width = default_theme().metrics.splitter_width;

  EXPECT_DOUBLE_EQ(split.left->bounds().x, 0.0);
  EXPECT_DOUBLE_EQ(split.right->bounds().right(), 800.0);
  EXPECT_DOUBLE_EQ(split.right->bounds().x - split.left->bounds().right(), width);
}

TEST(Splitter, TheDividerTakesItsWidthFromTheTheme) {
  // XP's splitters were raised bars; flat chrome wants a hairline. Neither is
  // something a widget should be deciding.
  const Theme& flat = default_theme();
  const Theme& xp = *built_in_theme("xp");
  ASSERT_NE(flat.metrics.splitter_width, xp.metrics.splitter_width);

  Split split;
  const double flat_gap = split.right->bounds().x - split.left->bounds().right();

  split.host->resize(Rect{0.0, 0.0, 800.0, 400.0}, context_for(xp));
  const double xp_gap = split.right->bounds().x - split.left->bounds().right();

  EXPECT_DOUBLE_EQ(flat_gap, flat.metrics.splitter_width);
  EXPECT_DOUBLE_EQ(xp_gap, xp.metrics.splitter_width);
}

TEST(Splitter, DraggingTheDividerResizesBothPanes) {
  Split split;
  const double start = split.divider_centre();

  split.host->mouse_down(press(start, 200.0));
  ASSERT_EQ(split.host->captured(), split.splitter) << "the drag was not captured";
  split.drag_to(250.0, 200.0);

  EXPECT_NEAR(split.left->bounds().width, 250.0 - default_theme().metrics.splitter_width / 2.0,
              0.5);
  EXPECT_DOUBLE_EQ(split.right->bounds().right(), 800.0);

  split.host->mouse_up(press(250.0, 200.0));
  EXPECT_EQ(split.host->captured(), nullptr);
  EXPECT_EQ(split.splitter->dragging(), SplitLayout::kNoDivider);
}

TEST(Splitter, ADragThatRunsOffTheWindowKeepsWorking) {
  // The reason the press captures: the pointer outruns the divider constantly.
  Split split;
  split.host->mouse_down(press(split.divider_centre(), 200.0));
  split.drag_to(-4000.0, 200.0);

  EXPECT_NEAR(split.left->bounds().width, default_theme().metrics.min_pane, 0.5)
      << "the pane was dragged away past its minimum";
}

TEST(Splitter, ADragAsksForLayoutRatherThanDoingIt) {
  // The contract that lets input handlers change geometry at all: they mark,
  // and the frame loop resolves it once with a theme in hand — however many
  // mouse moves arrived in between.
  Split split;
  ASSERT_FALSE(split.host->needs_layout());

  split.host->mouse_down(press(split.divider_centre(), 200.0));
  split.host->mouse_move(press(300.0, 200.0));
  split.host->mouse_move(press(250.0, 200.0));
  EXPECT_TRUE(split.host->needs_layout());

  EXPECT_TRUE(split.host->update_layout(flat_context()));
  EXPECT_FALSE(split.host->needs_layout());
  EXPECT_FALSE(split.host->update_layout(flat_context())) << "it laid out twice for one change";
}

TEST(Splitter, PressingAPaneIsNotADrag) {
  Split split;
  split.host->mouse_down(press(100.0, 200.0));
  EXPECT_EQ(split.splitter->dragging(), SplitLayout::kNoDivider);
}

TEST(Splitter, ADragCarriesThePaneContentsWithIt) {
  // A drag changes how wide a pane is, so its contents have to be given their
  // share of the new width. A child left at the old position is the bug this
  // catches, and the reason a drag cannot simply move the panes.
  Split split;
  auto& button = split.right->emplace<Button>("Deep");
  split.host->resize(Rect{0.0, 0.0, 800.0, 400.0}, flat_context());

  const double offset = button.bounds().x - split.right->bounds().x;

  split.host->mouse_down(press(split.divider_centre(), 200.0));
  split.drag_to(200.0, 200.0);

  EXPECT_DOUBLE_EQ(button.bounds().x - split.right->bounds().x, offset);
  EXPECT_EQ(split.host->root().at(button.bounds().x + 2.0, button.bounds().y + 2.0), &button)
      << "hit testing did not follow the pane";
}

TEST(Splitter, ProportionsSurviveAResize) {
  Split split;
  split.host->mouse_down(press(split.divider_centre(), 200.0));
  split.drag_to(200.0, 200.0);
  split.host->mouse_up(press(200.0, 200.0));

  // Compared as a ratio between the panes rather than as a share of the
  // window: the divider is a fixed width and does not grow with it, so the
  // panes split what is left over rather than the whole thing.
  const double ratio = split.left->bounds().width / split.right->bounds().width;
  split.host->resize(Rect{0.0, 0.0, 1600.0, 400.0}, flat_context());

  EXPECT_NEAR(split.left->bounds().width / split.right->bounds().width, ratio, 1e-6);
}

TEST(Splitter, AVerticalSplitterStacksItsPanes) {
  const Split split(Axis::Vertical);
  EXPECT_DOUBLE_EQ(split.left->bounds().width, 800.0);
  EXPECT_LT(split.left->bounds().bottom(), split.right->bounds().y);
}

TEST(Splitter, DividersArePaintedOverThePanes) {
  const Split split;
  RecordingPainter painter;
  split.host->paint(painter, default_theme());

  // The last fills belong to the divider, not to a pane.
  ASSERT_FALSE(painter.calls().empty());
  const Rect divider = painter.calls().back().bounds;
  EXPECT_DOUBLE_EQ(divider.width, default_theme().metrics.splitter_width);
}

TEST(Splitter, AMismatchedSetOfFractionsFallsBackToAnEvenSplit) {
  Split split;
  split.splitter->set_fractions({0.9, 0.05, 0.05});
  split.host->resize(Rect{0.0, 0.0, 800.0, 400.0}, flat_context());

  EXPECT_NEAR(split.left->bounds().width, split.right->bounds().width, 1.0);
}

TEST(Splitter, WithNoPanesItIsHarmless) {
  WidgetHost host(std::make_unique<Splitter>());
  host.resize(Rect{0.0, 0.0, 400.0, 200.0}, flat_context());
  EXPECT_FALSE(host.mouse_down(press(200.0, 100.0)));
}

// ------------------------------------------------------------- scroll view --

/// A scroll view over a column three times its own height.
struct Scrolled {
  Scrolled() {
    host = std::make_unique<WidgetHost>(std::make_unique<ScrollView>(Axis::Vertical));
    view = static_cast<ScrollView*>(&host->root());

    auto column = std::make_unique<Box>(Axis::Vertical);
    for (int i = 0; i < 12; ++i) column->emplace<Button>("Row");
    content = static_cast<Box*>(&view->set_content(std::move(column)));

    host->resize(Rect{0.0, 0.0, 300.0, 120.0}, flat_context());
  }

  std::unique_ptr<WidgetHost> host;
  ScrollView* view = nullptr;
  Box* content = nullptr;
};

TEST(ScrollView, KnowsHowMuchContentThereIs) {
  const Scrolled test;
  EXPECT_TRUE(test.view->viewport().scrollable());
  EXPECT_DOUBLE_EQ(test.view->viewport().visible, 120.0);
  EXPECT_GT(test.view->viewport().content, 120.0);
}

TEST(ScrollView, ContentShorterThanTheViewDoesNotScroll) {
  WidgetHost host(std::make_unique<ScrollView>(Axis::Vertical));
  auto& view = static_cast<ScrollView&>(host.root());
  view.set_content(std::make_unique<Button>("Only"));
  host.resize(Rect{0.0, 0.0, 300.0, 400.0}, flat_context());

  EXPECT_FALSE(view.viewport().scrollable());
  EXPECT_TRUE(view.track().empty()) << "a scrollbar appeared with nothing to scroll";
}

TEST(ScrollView, TheWheelMovesTheContentAndHitTestingFollows) {
  Scrolled test;
  const Widget* first = test.content->children().front().get();
  const double before = first->bounds().y;

  ASSERT_TRUE(test.host->wheel(WheelEvent{.x = 100.0, .y = 60.0, .delta_y = 1.0}));

  EXPECT_LT(first->bounds().y, before) << "the content did not move";
  EXPECT_DOUBLE_EQ(before - first->bounds().y, test.view->viewport().offset);
}

TEST(ScrollView, ScrollingStopsAtBothEnds) {
  Scrolled test;
  test.view->scroll_by(-500.0);
  EXPECT_DOUBLE_EQ(test.view->viewport().offset, 0.0);

  test.view->scroll_by(100000.0);
  EXPECT_DOUBLE_EQ(test.view->viewport().offset, test.view->viewport().max_offset());
}

TEST(ScrollView, TheWheelBubblesOnceItCannotMoveFurther) {
  // Otherwise a nested list swallows the wheel at its end and the panel around
  // it never scrolls.
  Scrolled test;
  test.view->scroll_to(test.view->viewport().max_offset());
  EXPECT_FALSE(test.host->wheel(WheelEvent{.x = 100.0, .y = 60.0, .delta_y = 1.0}));
}

TEST(ScrollView, TheContentKeepsItsLayoutWhenItMoves) {
  Scrolled test;
  const Widget* first = test.content->children().front().get();
  const Widget* second = test.content->children()[1].get();
  const double gap = second->bounds().y - first->bounds().y;

  test.view->scroll_by(40.0);
  EXPECT_DOUBLE_EQ(second->bounds().y - first->bounds().y, gap);
}

TEST(ScrollView, TheScrollbarIsTheWidthTheThemeSays) {
  Scrolled test;
  EXPECT_DOUBLE_EQ(test.view->track().width, default_theme().metrics.scrollbar_width);
  EXPECT_DOUBLE_EQ(test.view->track().right(), 300.0);

  // And the content gets the rest, rather than sitting under the bar.
  EXPECT_DOUBLE_EQ(test.content->bounds().width, 300.0 - default_theme().metrics.scrollbar_width);
}

TEST(ScrollView, TheThumbReachesTheBottomOfItsTrack) {
  Scrolled test;
  test.view->scroll_to(test.view->viewport().max_offset());
  EXPECT_NEAR(test.view->thumb().bottom(), test.view->track().bottom(), 1e-6);
}

TEST(ScrollView, DraggingTheThumbScrolls) {
  Scrolled test;
  const Rect grip = test.view->thumb();

  test.host->mouse_down(press(grip.x + 4.0, grip.y + 4.0));
  ASSERT_EQ(test.host->captured(), test.view);

  test.host->mouse_move(press(grip.x + 4.0, grip.y + 40.0));
  EXPECT_GT(test.view->viewport().offset, 0.0);

  test.host->mouse_up(press(grip.x + 4.0, grip.y + 40.0));
  EXPECT_EQ(test.host->captured(), nullptr);
}

TEST(ScrollView, TheThumbDoesNotJumpUnderTheCursor) {
  // Grabbing the thumb near its bottom and not moving must not scroll at all.
  Scrolled test;
  const Rect grip = test.view->thumb();

  const double grab_y = grip.bottom() - 2.0;
  test.host->mouse_down(press(grip.x + 4.0, grab_y));
  test.host->mouse_move(press(grip.x + 4.0, grab_y));

  EXPECT_DOUBLE_EQ(test.view->viewport().offset, 0.0);
}

TEST(ScrollView, ClickingTheEmptyTrackPages) {
  Scrolled test;
  const Rect bar = test.view->track();

  test.host->mouse_down(press(bar.x + 2.0, bar.bottom() - 2.0));
  EXPECT_DOUBLE_EQ(test.view->viewport().offset, test.view->viewport().visible);
}

TEST(ScrollView, ItPaintsATrackAndAThumbOverItsContents) {
  Scrolled test;
  RecordingPainter painter;
  test.host->paint(painter, default_theme());

  EXPECT_TRUE(painter.clips_balanced());
  // The scrollbar is drawn after the clip around the content is popped, or it
  // would be cut off along with whatever it is scrolling.
  const auto kinds = painter.kinds();
  const auto last_pop = std::find(kinds.rbegin(), kinds.rend(), DrawCall::Kind::PopClip);
  ASSERT_NE(last_pop, kinds.rend());
  EXPECT_GT(std::distance(last_pop.base(), kinds.end()), 0)
      << "nothing was drawn after the content's clip was popped";
}

TEST(ScrollView, AHorizontalViewScrollsSideways) {
  WidgetHost host(std::make_unique<ScrollView>(Axis::Horizontal));
  auto& view = static_cast<ScrollView&>(host.root());

  auto row = std::make_unique<Box>(Axis::Horizontal);
  for (int i = 0; i < 20; ++i) row->emplace<Button>("Clip");
  const Widget* first = &row->children().front().operator*();
  view.set_content(std::move(row));
  host.resize(Rect{0.0, 0.0, 200.0, 60.0}, flat_context());

  ASSERT_TRUE(view.viewport().scrollable());
  const double before = first->bounds().x;
  view.scroll_by(50.0);

  EXPECT_DOUBLE_EQ(before - first->bounds().x, 50.0);
  EXPECT_DOUBLE_EQ(view.track().height, default_theme().metrics.scrollbar_width);
  EXPECT_DOUBLE_EQ(view.track().bottom(), 60.0);
}

TEST(ScrollView, WithNoContentItIsHarmless) {
  WidgetHost host(std::make_unique<ScrollView>());
  host.resize(Rect{0.0, 0.0, 200.0, 100.0}, flat_context());

  EXPECT_FALSE(host.wheel(WheelEvent{.x = 50.0, .y = 50.0, .delta_y = 1.0}));
  EXPECT_TRUE(host.root().bounds().contains(50.0, 50.0));
}

}  // namespace
}  // namespace cutline::ui
