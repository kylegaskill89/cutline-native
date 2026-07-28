/// Layout, which is arithmetic and therefore ought to be provable.
///
/// These are the questions that are miserable to answer by dragging a window
/// around: whether a divider drag disturbs the far side of the layout, whether
/// a scrollbar thumb can actually reach the end of its content, whether zooming
/// keeps the frame under the cursor under the cursor.

#include "cutline/ui/layout.hpp"

#include "cutline/ui/theme.hpp"

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

namespace cutline::ui {
namespace {

constexpr double kTolerance = 1e-6;

[[nodiscard]] double total(const std::vector<double>& sizes) {
  return std::accumulate(sizes.begin(), sizes.end(), 0.0);
}

// ------------------------------------------------------------------- edges --

TEST(Edges, InsetShrinksOnEverySide) {
  const Rect out = inset(Rect{10.0, 20.0, 100.0, 50.0}, Edges{2.0, 4.0, 6.0, 8.0});
  EXPECT_DOUBLE_EQ(out.x, 12.0);
  EXPECT_DOUBLE_EQ(out.y, 24.0);
  EXPECT_DOUBLE_EQ(out.width, 92.0);
  EXPECT_DOUBLE_EQ(out.height, 38.0);
}

TEST(Edges, InsettingPastNothingStopsAtNothing) {
  // A panel dragged narrower than its own padding should end up empty, not
  // inside out. A negative width propagates into every child.
  const Rect out = inset(Rect{0.0, 0.0, 10.0, 10.0}, Edges::all(20.0));
  EXPECT_DOUBLE_EQ(out.width, 0.0);
  EXPECT_DOUBLE_EQ(out.height, 0.0);
  EXPECT_TRUE(out.empty());
}

TEST(Edges, PaddingComesFromTheThemeMetrics) {
  // The point of routing this through metrics: bevelled chrome is roomier than
  // flat chrome, and it has to be one lookup rather than a number per widget.
  const Metrics flat = default_theme().metrics;
  const Metrics xp = built_in_theme("xp")->metrics;

  EXPECT_DOUBLE_EQ(control_padding(flat).left, flat.padding_x);
  EXPECT_DOUBLE_EQ(control_padding(flat).top, flat.padding_y);
  EXPECT_DOUBLE_EQ(panel_padding(xp).left, xp.panel_padding);
}

// -------------------------------------------------------------- distribute --

TEST(Distribute, FixedChildrenKeepTheirSize) {
  const std::vector<LayoutItem> items{LayoutItem::fixed(30.0), LayoutItem::fixed(50.0)};
  const std::vector<double> sizes = distribute(items, 400.0);

  EXPECT_DOUBLE_EQ(sizes[0], 30.0);
  EXPECT_DOUBLE_EQ(sizes[1], 50.0);
}

TEST(Distribute, SpacingIsPaidForOutOfTheAvailableSpace) {
  const std::vector<LayoutItem> items{LayoutItem::flexible(), LayoutItem::flexible(),
                                      LayoutItem::flexible()};
  const std::vector<double> sizes = distribute(items, 100.0, 10.0);

  // Three children, two gaps of ten, eighty left to share.
  EXPECT_NEAR(total(sizes), 80.0, kTolerance);
  for (double size : sizes) EXPECT_NEAR(size, 80.0 / 3.0, kTolerance);
}

TEST(Distribute, SurplusIsSharedInProportionToGrow) {
  const std::vector<LayoutItem> items{LayoutItem::flexible(1.0), LayoutItem::flexible(3.0)};
  const std::vector<double> sizes = distribute(items, 200.0);

  EXPECT_NEAR(sizes[0], 50.0, kTolerance);
  EXPECT_NEAR(sizes[1], 150.0, kTolerance);
}

TEST(Distribute, AChildThatHitsItsMaximumDoesNotEatTheRest) {
  // The reason for the freeze-and-repeat pass. A single proportional share
  // would clamp the capped child and quietly lose the space it could not use,
  // leaving a gap on the right of every toolbar with a capped control in it.
  const std::vector<LayoutItem> items{
      LayoutItem{.basis = 0.0, .grow = 1.0, .shrink = 1.0, .min = 0.0, .max = 40.0},
      LayoutItem::flexible(1.0)};
  const std::vector<double> sizes = distribute(items, 200.0);

  EXPECT_NEAR(sizes[0], 40.0, kTolerance);
  EXPECT_NEAR(sizes[1], 160.0, kTolerance) << "the capped child's leftover went nowhere";
  EXPECT_NEAR(total(sizes), 200.0, kTolerance);
}

TEST(Distribute, ShortfallTakesMoreFromTheWiderChild) {
  const std::vector<LayoutItem> items{
      LayoutItem{.basis = 100.0, .grow = 0.0, .shrink = 1.0},
      LayoutItem{.basis = 300.0, .grow = 0.0, .shrink = 1.0}};
  const std::vector<double> sizes = distribute(items, 200.0);

  EXPECT_NEAR(total(sizes), 200.0, kTolerance);
  EXPECT_GT(sizes[1], sizes[0]);
  // Scaled by size, so the 3:1 ratio survives the squeeze.
  EXPECT_NEAR(sizes[1] / sizes[0], 3.0, 1e-3);
}

TEST(Distribute, MinimumsAreHonouredWhenThereIsNotEnoughRoom) {
  const std::vector<LayoutItem> items{
      LayoutItem{.basis = 100.0, .grow = 0.0, .shrink = 1.0, .min = 80.0},
      LayoutItem{.basis = 100.0, .grow = 0.0, .shrink = 1.0, .min = 0.0}};
  const std::vector<double> sizes = distribute(items, 100.0);

  EXPECT_GE(sizes[0], 80.0 - kTolerance);
  EXPECT_NEAR(total(sizes), 100.0, kTolerance);
}

TEST(Distribute, OverflowIsReportedRatherThanHidden) {
  // Everything is already at its minimum, so the honest answer is that it does
  // not fit. Squashing to zero would hide a panel that should be scrolling.
  const std::vector<LayoutItem> items{LayoutItem::fixed(100.0), LayoutItem::fixed(100.0)};
  const std::vector<double> sizes = distribute(items, 50.0);

  EXPECT_NEAR(total(sizes), 200.0, kTolerance);
  for (double size : sizes) EXPECT_GE(size, 0.0);
}

TEST(Distribute, NoChildrenIsNotAnError) {
  EXPECT_TRUE(distribute({}, 100.0, 4.0).empty());
}

TEST(Distribute, InflexibleChildrenIgnoreSurplus) {
  const std::vector<LayoutItem> items{
      LayoutItem{.basis = 40.0, .grow = 0.0, .shrink = 0.0},
      LayoutItem{.basis = 40.0, .grow = 0.0, .shrink = 0.0}};
  const std::vector<double> sizes = distribute(items, 500.0);

  EXPECT_DOUBLE_EQ(sizes[0], 40.0);
  EXPECT_DOUBLE_EQ(sizes[1], 40.0);
}

// -------------------------------------------------------------- box layout --

TEST(BoxLayout, ARowPlacesChildrenLeftToRightInsideThePadding) {
  const std::vector<BoxChild> children{{.main = LayoutItem::fixed(50.0)},
                                       {.main = LayoutItem::fixed(50.0)}};
  const std::vector<Rect> laid =
      layout_box(Rect{0.0, 0.0, 200.0, 40.0},
                 BoxLayout{.axis = Axis::Horizontal, .spacing = 8.0, .padding = Edges::all(10.0)},
                 children);

  ASSERT_EQ(laid.size(), 2u);
  EXPECT_DOUBLE_EQ(laid[0].x, 10.0);
  EXPECT_DOUBLE_EQ(laid[0].width, 50.0);
  EXPECT_DOUBLE_EQ(laid[1].x, 68.0) << "the gap between them is missing or doubled";
  // Stretch is the default across the axis.
  EXPECT_DOUBLE_EQ(laid[0].y, 10.0);
  EXPECT_DOUBLE_EQ(laid[0].height, 20.0);
}

TEST(BoxLayout, AColumnRunsDownwards) {
  const std::vector<BoxChild> children{{.main = LayoutItem::fixed(30.0)},
                                       {.main = LayoutItem::fixed(30.0)}};
  const std::vector<Rect> laid = layout_box(
      Rect{0.0, 0.0, 100.0, 200.0}, BoxLayout{.axis = Axis::Vertical, .spacing = 4.0}, children);

  ASSERT_EQ(laid.size(), 2u);
  EXPECT_DOUBLE_EQ(laid[0].y, 0.0);
  EXPECT_DOUBLE_EQ(laid[1].y, 34.0);
  EXPECT_DOUBLE_EQ(laid[0].width, 100.0);
}

TEST(BoxLayout, CrossAlignmentCentresAShortChild) {
  const std::vector<BoxChild> children{{.main = LayoutItem::fixed(50.0), .cross_size = 20.0}};
  const std::vector<Rect> laid =
      layout_box(Rect{0.0, 0.0, 200.0, 100.0},
                 BoxLayout{.axis = Axis::Horizontal, .cross = Align::Center}, children);

  ASSERT_EQ(laid.size(), 1u);
  EXPECT_DOUBLE_EQ(laid[0].height, 20.0);
  EXPECT_DOUBLE_EQ(laid[0].y, 40.0);
}

TEST(BoxLayout, MainAlignmentPushesInflexibleChildrenToTheEnd) {
  // How the buttons on the right of a toolbar get there without a spacer.
  const std::vector<BoxChild> children{{.main = LayoutItem::fixed(40.0)},
                                       {.main = LayoutItem::fixed(40.0)}};
  const std::vector<Rect> laid =
      layout_box(Rect{0.0, 0.0, 200.0, 30.0},
                 BoxLayout{.axis = Axis::Horizontal, .spacing = 10.0, .main = Align::End},
                 children);

  ASSERT_EQ(laid.size(), 2u);
  EXPECT_DOUBLE_EQ(laid[1].right(), 200.0);
  EXPECT_DOUBLE_EQ(laid[0].x, 110.0);
}

TEST(BoxLayout, OverflowStartsAtTheNearEdgeWhateverTheAlignment) {
  // Centring content that does not fit would push the first child off the left,
  // where it can never be scrolled back to.
  const std::vector<BoxChild> children{{.main = LayoutItem::fixed(300.0)}};
  const std::vector<Rect> laid = layout_box(
      Rect{0.0, 0.0, 100.0, 30.0},
      BoxLayout{.axis = Axis::Horizontal, .main = Align::Center}, children);

  ASSERT_EQ(laid.size(), 1u);
  EXPECT_DOUBLE_EQ(laid[0].x, 0.0);
}

TEST(BoxLayout, AFlexibleChildFillsWhatTheFixedOnesLeave) {
  // The shape of nearly every panel: a header, a body that takes the rest.
  const std::vector<BoxChild> children{{.main = LayoutItem::fixed(26.0)},
                                       {.main = LayoutItem::flexible()},
                                       {.main = LayoutItem::fixed(12.0)}};
  const std::vector<Rect> laid =
      layout_box(Rect{0.0, 0.0, 300.0, 400.0}, BoxLayout{.axis = Axis::Vertical}, children);

  ASSERT_EQ(laid.size(), 3u);
  EXPECT_DOUBLE_EQ(laid[1].height, 362.0);
  EXPECT_DOUBLE_EQ(laid[2].bottom(), 400.0) << "the children do not fill their parent";
}

// --------------------------------------------------------------- splitters --

TEST(SplitLayout, FractionsAreNormalised) {
  const SplitLayout split(Axis::Horizontal, {2.0, 2.0}, 0.0);
  EXPECT_NEAR(split.fractions()[0], 0.5, kTolerance);
  EXPECT_NEAR(split.fractions()[1], 0.5, kTolerance);
}

TEST(SplitLayout, NonsenseFractionsBecomeAnEvenSplit) {
  // A theme or project file with zeroes in it should open, not collapse.
  const SplitLayout split(Axis::Horizontal, {0.0, 0.0, 0.0}, 0.0);
  for (double f : split.fractions()) EXPECT_NEAR(f, 1.0 / 3.0, kTolerance);
}

TEST(SplitLayout, PanesAndDividersExactlyFillTheBounds) {
  // Rounding left over anywhere shows up as a sliver of unpainted background
  // down the edge of a panel.
  const SplitLayout split(Axis::Horizontal, {0.3, 0.3, 0.4}, 6.0);
  const Rect bounds{0.0, 0.0, 1001.0, 500.0};
  const std::vector<Rect> panes = split.panes(bounds);

  ASSERT_EQ(panes.size(), 3u);
  EXPECT_DOUBLE_EQ(panes[0].x, 0.0);
  EXPECT_DOUBLE_EQ(panes.back().right(), bounds.width);
  for (std::size_t i = 0; i + 1 < panes.size(); ++i) {
    EXPECT_NEAR(panes[i + 1].x - panes[i].right(), 6.0, kTolerance) << "gap " << i;
  }
}

TEST(SplitLayout, DividersSitBetweenThePanes) {
  const SplitLayout split(Axis::Vertical, {0.5, 0.5}, 8.0);
  const Rect bounds{0.0, 0.0, 400.0, 208.0};

  const Rect strip = split.divider(bounds, 0);
  EXPECT_DOUBLE_EQ(strip.y, 100.0);
  EXPECT_DOUBLE_EQ(strip.height, 8.0);
  EXPECT_DOUBLE_EQ(strip.width, 400.0);
  EXPECT_TRUE(split.divider(bounds, 1).empty()) << "there is no second divider";
}

TEST(SplitLayout, TheGrabAreaIsWiderThanTheDividerButNoTaller) {
  const SplitLayout split(Axis::Horizontal, {0.5, 0.5}, 4.0);
  const Rect bounds{0.0, 0.0, 204.0, 100.0};

  // Two pixels outside the strip still counts, so the divider is catchable.
  EXPECT_EQ(split.divider_at(bounds, 98.0, 50.0, 3.0), 0u);
  EXPECT_EQ(split.divider_at(bounds, 101.0, 50.0, 3.0), 0u);
  // Well away from it does not.
  EXPECT_EQ(split.divider_at(bounds, 40.0, 50.0, 3.0), SplitLayout::kNoDivider);
}

TEST(SplitLayout, DraggingADividerMovesOnlyItsTwoNeighbours) {
  // The property that makes a docked layout feel solid: pulling one edge must
  // not quietly reshuffle the far side of the window.
  SplitLayout split(Axis::Horizontal, {0.25, 0.25, 0.5}, 0.0);
  const Rect bounds{0.0, 0.0, 400.0, 300.0};

  const double before = split.panes(bounds)[2].width;
  ASSERT_TRUE(split.drag(bounds, 0, 150.0));

  const std::vector<Rect> after = split.panes(bounds);
  EXPECT_NEAR(after[0].width, 150.0, kTolerance);
  EXPECT_NEAR(after[1].width, 50.0, kTolerance);
  EXPECT_NEAR(after[2].width, before, kTolerance) << "the far pane moved";
}

TEST(SplitLayout, ADividerCannotBeDraggedThroughItsNeighbour) {
  SplitLayout split(Axis::Horizontal, {0.5, 0.5}, 0.0, 40.0);
  const Rect bounds{0.0, 0.0, 400.0, 300.0};

  split.drag(bounds, 0, -500.0);
  EXPECT_NEAR(split.panes(bounds)[0].width, 40.0, kTolerance);

  split.drag(bounds, 0, 5000.0);
  EXPECT_NEAR(split.panes(bounds)[1].width, 40.0, kTolerance);
}

TEST(SplitLayout, DraggingReportsWhetherAnythingMoved) {
  // So a drag that changes nothing does not schedule a repaint every frame.
  SplitLayout split(Axis::Horizontal, {0.5, 0.5}, 0.0, 40.0);
  const Rect bounds{0.0, 0.0, 400.0, 300.0};

  EXPECT_TRUE(split.drag(bounds, 0, 120.0));
  EXPECT_FALSE(split.drag(bounds, 0, 120.0));
  EXPECT_FALSE(split.drag(bounds, 5, 120.0)) << "there is no divider five";
}

TEST(SplitLayout, ProportionsSurviveAResize) {
  // Why fractions rather than pixels: maximising a window should not hand the
  // whole gain to one pane.
  SplitLayout split(Axis::Horizontal, {0.25, 0.75}, 0.0);
  const std::vector<Rect> small = split.panes(Rect{0.0, 0.0, 400.0, 100.0});
  const std::vector<Rect> large = split.panes(Rect{0.0, 0.0, 1600.0, 100.0});

  EXPECT_NEAR(small[0].width / 400.0, large[0].width / 1600.0, kTolerance);
}

TEST(SplitLayout, ASinglePaneHasNoDividers) {
  const SplitLayout split(Axis::Horizontal, {1.0});
  EXPECT_EQ(split.divider_count(), 0u);
  EXPECT_EQ(split.panes(Rect{0.0, 0.0, 100.0, 100.0}).size(), 1u);
}

// ---------------------------------------------------------------- viewport --

TEST(Viewport, ContentThatFitsDoesNotScroll) {
  Viewport view{.content = 50.0, .visible = 100.0};
  EXPECT_FALSE(view.scrollable());
  view.scroll_by(30.0);
  EXPECT_DOUBLE_EQ(view.offset, 0.0);
}

TEST(Viewport, ScrollingStopsAtBothEnds) {
  Viewport view{.content = 500.0, .visible = 100.0};

  view.scroll_by(-50.0);
  EXPECT_DOUBLE_EQ(view.offset, 0.0);

  view.scroll_by(10000.0);
  EXPECT_DOUBLE_EQ(view.offset, 400.0);
}

TEST(Viewport, RevealScrollsTheLeastItCan) {
  Viewport view{.content = 1000.0, .visible = 100.0, .offset = 0.0};

  view.reveal(300.0, 340.0);
  EXPECT_DOUBLE_EQ(view.offset, 240.0) << "it should scroll just far enough";

  // Already visible: nothing should move.
  const double settled = view.offset;
  view.reveal(300.0, 340.0);
  EXPECT_DOUBLE_EQ(view.offset, settled);

  view.reveal(50.0, 60.0);
  EXPECT_DOUBLE_EQ(view.offset, 50.0);
}

TEST(Viewport, RevealingSomethingTooBigShowsItsStart) {
  Viewport view{.content = 1000.0, .visible = 100.0};
  view.reveal(200.0, 600.0);
  EXPECT_DOUBLE_EQ(view.offset, 200.0);
}

TEST(Viewport, TheThumbIsProportionalToWhatIsVisible) {
  const Viewport view{.content = 400.0, .visible = 100.0};
  EXPECT_NEAR(view.thumb_size(200.0), 50.0, kTolerance);
}

TEST(Viewport, TheThumbStaysGrabbableOnVeryLongContent) {
  const Viewport view{.content = 1000000.0, .visible = 100.0};
  EXPECT_NEAR(view.thumb_size(200.0, 20.0), 20.0, kTolerance);
}

TEST(Viewport, TheThumbFillsTheTrackWhenThereIsNothingToScroll) {
  const Viewport view{.content = 50.0, .visible = 100.0};
  EXPECT_NEAR(view.thumb_size(200.0), 200.0, kTolerance);
  EXPECT_DOUBLE_EQ(view.thumb_offset(200.0), 0.0);
}

TEST(Viewport, TheThumbReachesTheEndOfItsTrack) {
  // Thumb travel is the track minus the thumb. Mapping against the whole track
  // is the classic bug where the last screen of content is unreachable.
  Viewport view{.content = 400.0, .visible = 100.0};
  view.scroll_to(view.max_offset());

  const double track = 200.0;
  const double thumb = view.thumb_size(track);
  EXPECT_NEAR(view.thumb_offset(track) + thumb, track, kTolerance);
}

TEST(Viewport, DraggingTheThumbToTheEndScrollsToTheEnd) {
  Viewport view{.content = 400.0, .visible = 100.0};
  const double track = 200.0;

  view.drag_thumb(track, track);  // past the end of the travel
  EXPECT_NEAR(view.offset, view.max_offset(), kTolerance);

  view.drag_thumb(track, 0.0);
  EXPECT_DOUBLE_EQ(view.offset, 0.0);
}

TEST(Viewport, ThumbDraggingAndThumbDrawingAgree) {
  // Round trip: put the thumb somewhere, ask where it is, and land back.
  Viewport view{.content = 900.0, .visible = 300.0};
  const double track = 240.0;

  for (const double position : {0.0, 20.0, 77.5, 160.0}) {
    view.drag_thumb(track, position);
    EXPECT_NEAR(view.thumb_offset(track), position, kTolerance) << "at " << position;
  }
}

// ------------------------------------------------------------------- zoom --

TEST(ZoomAbout, TheContentUnderTheCursorStaysUnderTheCursor) {
  // Zooming a timeline anywhere other than under the pointer means scrolling
  // back to where you were looking after every wheel notch.
  const double offset = 500.0;
  const double anchor = 120.0;
  const double at = (offset + anchor) / 100.0;  // seconds, at 100 px/s

  const double zoomed = zoom_about(offset, anchor, 100.0, 250.0);
  EXPECT_NEAR(at * 250.0 - zoomed, anchor, kTolerance);
}

TEST(ZoomAbout, ZoomingOutNearTheStartDoesNotGoNegative) {
  EXPECT_DOUBLE_EQ(zoom_about(10.0, 5.0, 100.0, 10.0), 0.0);
}

TEST(ZoomAbout, ADegenerateScaleLeavesTheOffsetAlone) {
  EXPECT_DOUBLE_EQ(zoom_about(42.0, 10.0, 0.0, 100.0), 42.0);
  EXPECT_DOUBLE_EQ(zoom_about(42.0, 10.0, 100.0, 0.0), 42.0);
}

TEST(ZoomAbout, NotZoomingChangesNothing) {
  EXPECT_NEAR(zoom_about(317.0, 88.0, 120.0, 120.0), 317.0, kTolerance);
}

}  // namespace
}  // namespace cutline::ui
