/// Where the panels are, and what dragging one somewhere else does to that.
///
/// Docking is the part of an interface most likely to go quietly wrong: the
/// result of a bad move still draws, still responds, and is only obviously
/// broken three drags later. So the tests are mostly about the shape the tree
/// is left in rather than about any one operation succeeding — that a split
/// never keeps a single child, that a row inside a row is flattened, that
/// fractions still sum to one, and that no panel is lost or duplicated by a
/// move.

#include "cutline/ui/dock.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace cutline::ui {
namespace {

/// The editor's own arrangement, near enough: a browser down the left, a
/// monitor over a timeline on the right, and an inspector tabbed with the
/// browser.
///
///     +----------+---------------+
///     | project  |    monitor    |
///     | effects  +---------------+
///     |          |   timeline    |
///     +----------+---------------+
[[nodiscard]] DockLayout sample_layout() {
  DockLayout layout;
  layout.root = DockNode::split(
      Axis::Horizontal,
      {DockNode::tabs({"project", "effects"}),
       DockNode::split(Axis::Vertical,
                       {DockNode::tabs({"monitor"}), DockNode::tabs({"timeline"})})});
  normalise(layout);
  return layout;
}

/// Every rule the tree is meant to keep, checked in one place so each test can
/// state what it is about and still assert the invariants.
void expect_canonical(const DockNode& node) {
  if (!node.is_split()) {
    if (node.panels.empty()) {
      EXPECT_EQ(node.active, 0u);
    } else {
      EXPECT_LT(node.active, node.panels.size()) << "the active tab is out of range";
    }
    return;
  }

  EXPECT_GE(node.children.size(), 2u) << "a split kept fewer than two panes";
  EXPECT_EQ(node.fractions.size(), node.children.size());

  double total = 0.0;
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    EXPECT_FALSE(node.children[i].empty()) << "an empty pane was kept";
    EXPECT_FALSE(node.children[i].is_split() && node.children[i].axis == node.axis)
        << "a split of the same axis was left nested";
    EXPECT_GT(node.fractions[i], 0.0);
    total += node.fractions[i];
    expect_canonical(node.children[i]);
  }
  EXPECT_NEAR(total, 1.0, 1e-9);
}

void expect_canonical(const DockLayout& layout) {
  expect_canonical(layout.root);
  for (const FloatingDock& window : layout.floating) {
    EXPECT_FALSE(window.root.empty()) << "an empty floating window was kept";
    expect_canonical(window.root);
  }
}

[[nodiscard]] std::vector<PanelId> sorted_panels(const DockLayout& layout) {
  std::vector<PanelId> panels = panels_in(layout);
  std::ranges::sort(panels);
  return panels;
}

// ------------------------------------------------------------------ reading --

TEST(Dock, ALayoutListsItsPanelsInOrder) {
  const std::vector<PanelId> panels = panels_in(sample_layout());
  EXPECT_EQ(panels,
            (std::vector<PanelId>{"project", "effects", "monitor", "timeline"}));
}

TEST(Dock, APanelIsFoundInTheGroupItIsIn) {
  const DockLayout layout = sample_layout();
  const DockNode* group = group_of(layout.root, "effects");

  ASSERT_NE(group, nullptr);
  EXPECT_FALSE(group->is_split());
  EXPECT_EQ(group->panels, (std::vector<PanelId>{"project", "effects"}));
  EXPECT_EQ(group_of(layout.root, "nowhere"), nullptr);
}

TEST(Dock, TheFirstTabIsTheOneShowing) {
  const DockLayout layout = sample_layout();
  const DockNode* group = group_of(layout.root, "project");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->active_panel(), std::optional<PanelId>{"project"});
}

TEST(Dock, ASplitHasNoActivePanelOfItsOwn) {
  const DockLayout layout = sample_layout();
  EXPECT_FALSE(layout.root.active_panel().has_value());
}

// ------------------------------------------------------------- normalising --

TEST(DockNormalise, ASplitWithOnePaneIsNotASplit) {
  DockNode node = DockNode::split(Axis::Horizontal, {DockNode::tabs({"only"})});
  normalise(node);

  EXPECT_FALSE(node.is_split());
  EXPECT_EQ(node.panels, (std::vector<PanelId>{"only"}));
}

TEST(DockNormalise, AnEmptyPaneIsDropped) {
  DockNode node = DockNode::split(
      Axis::Horizontal, {DockNode::tabs({"a"}), DockNode::tabs({}), DockNode::tabs({"b"})});
  normalise(node);

  ASSERT_TRUE(node.is_split());
  EXPECT_EQ(node.children.size(), 2u);
  expect_canonical(node);
}

TEST(DockNormalise, ARowInsideARowIsOneRow) {
  // Left nested it lays out identically and drags worse: the inner dividers no
  // longer line up with the outer ones, and three drags build a tower.
  DockNode node = DockNode::split(
      Axis::Horizontal,
      {DockNode::tabs({"a"}),
       DockNode::split(Axis::Horizontal, {DockNode::tabs({"b"}), DockNode::tabs({"c"})})});
  normalise(node);

  ASSERT_TRUE(node.is_split());
  EXPECT_EQ(node.children.size(), 3u);
  EXPECT_EQ(panels_in(node), (std::vector<PanelId>{"a", "b", "c"}));
  expect_canonical(node);
}

TEST(DockNormalise, AColumnInsideARowIsLeftAlone) {
  DockNode node = DockNode::split(
      Axis::Horizontal,
      {DockNode::tabs({"a"}),
       DockNode::split(Axis::Vertical, {DockNode::tabs({"b"}), DockNode::tabs({"c"})})});
  normalise(node);

  ASSERT_TRUE(node.is_split());
  EXPECT_EQ(node.children.size(), 2u);
  EXPECT_TRUE(node.children[1].is_split());
}

TEST(DockNormalise, FlatteningKeepsTheProportionsItFound) {
  // The inner split was half the row and its two panes split that in three and
  // one. Flattening has to leave them at three eighths and one eighth, or every
  // divider jumps the moment an unrelated panel is docked.
  DockNode inner =
      DockNode::split(Axis::Horizontal, {DockNode::tabs({"b"}), DockNode::tabs({"c"})});
  inner.fractions = {0.75, 0.25};

  DockNode node = DockNode::split(Axis::Horizontal, {DockNode::tabs({"a"}), std::move(inner)});
  node.fractions = {0.5, 0.5};
  normalise(node);

  ASSERT_EQ(node.fractions.size(), 3u);
  EXPECT_NEAR(node.fractions[0], 0.5, 1e-9);
  EXPECT_NEAR(node.fractions[1], 0.375, 1e-9);
  EXPECT_NEAR(node.fractions[2], 0.125, 1e-9);
}

TEST(DockNormalise, FractionsAreRescaledToSumToOne) {
  DockNode node =
      DockNode::split(Axis::Horizontal, {DockNode::tabs({"a"}), DockNode::tabs({"b"})});
  node.fractions = {3.0, 1.0};
  normalise(node);

  EXPECT_NEAR(node.fractions[0], 0.75, 1e-9);
  EXPECT_NEAR(node.fractions[1], 0.25, 1e-9);
}

TEST(DockNormalise, FractionsThatDoNotMatchThePanesAreEvenedOut) {
  // A mismatch means the caller is out of step with the tree, and guessing
  // which pane the spare fraction belonged to would be worse.
  DockNode node = DockNode::split(
      Axis::Horizontal, {DockNode::tabs({"a"}), DockNode::tabs({"b"}), DockNode::tabs({"c"})});
  node.fractions = {0.9, 0.1};
  normalise(node);

  for (const double share : node.fractions) EXPECT_NEAR(share, 1.0 / 3.0, 1e-9);
}

TEST(DockNormalise, AnActiveTabPastTheEndIsPulledBack) {
  DockNode node = DockNode::tabs({"a", "b"});
  node.active = 7;
  normalise(node);
  EXPECT_EQ(node.active, 1u);
}

TEST(DockNormalise, NormalisingTwiceChangesNothing) {
  // What makes it safe to run over a tree of unknown provenance — one just
  // read out of a file, most obviously.
  DockNode node = DockNode::split(
      Axis::Horizontal,
      {DockNode::tabs({}), DockNode::tabs({"a"}),
       DockNode::split(Axis::Horizontal, {DockNode::tabs({"b"}), DockNode::tabs({"c"})})});
  normalise(node);

  const DockNode once = node;
  normalise(node);
  EXPECT_EQ(node, once);
}

TEST(DockNormalise, ASplitOfNothingBecomesAnEmptyGroup) {
  DockNode node = DockNode::split(Axis::Horizontal, {});
  normalise(node);

  EXPECT_FALSE(node.is_split());
  EXPECT_TRUE(node.empty());
}

// -------------------------------------------------------------- docking --

TEST(Dock, DroppingInTheMiddleAddsATab) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(dock_panel(layout, "timeline", "project", DockSide::Centre));

  const DockNode* group = group_of(layout.root, "timeline");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->panels, (std::vector<PanelId>{"project", "effects", "timeline"}));
  EXPECT_EQ(group->active_panel(), std::optional<PanelId>{"timeline"})
      << "a panel dropped onto a group should be the one showing";
  expect_canonical(layout);
}

TEST(Dock, DroppingOnAnEdgeSplitsTheTarget) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(dock_panel(layout, "effects", "monitor", DockSide::Bottom));

  const DockNode* group = group_of(layout.root, "effects");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->panels, (std::vector<PanelId>{"effects"}));
  expect_canonical(layout);
}

TEST(Dock, TheSideDecidesTheOrderAndTheAxis) {
  {
    DockLayout layout;
    layout.root = DockNode::tabs({"a", "b"});
    ASSERT_TRUE(dock_panel(layout, "b", "a", DockSide::Left));

    ASSERT_TRUE(layout.root.is_split());
    EXPECT_EQ(layout.root.axis, Axis::Horizontal);
    EXPECT_EQ(layout.root.children[0].panels, (std::vector<PanelId>{"b"}));
    EXPECT_EQ(layout.root.children[1].panels, (std::vector<PanelId>{"a"}));
  }
  {
    DockLayout layout;
    layout.root = DockNode::tabs({"a", "b"});
    ASSERT_TRUE(dock_panel(layout, "b", "a", DockSide::Bottom));

    ASSERT_TRUE(layout.root.is_split());
    EXPECT_EQ(layout.root.axis, Axis::Vertical);
    EXPECT_EQ(layout.root.children[0].panels, (std::vector<PanelId>{"a"}));
    EXPECT_EQ(layout.root.children[1].panels, (std::vector<PanelId>{"b"}));
  }
}

TEST(Dock, NoPanelIsLostOrDuplicatedByAMove) {
  DockLayout layout = sample_layout();
  const std::vector<PanelId> before = sorted_panels(layout);

  ASSERT_TRUE(dock_panel(layout, "project", "timeline", DockSide::Right));

  EXPECT_EQ(sorted_panels(layout), before);
  expect_canonical(layout);
}

TEST(Dock, MovingTheLastPanelOutOfAGroupCollapsesIt) {
  // The group it left is empty, and the split holding it now has one child.
  // Both have to go, or the layout keeps a divider with nothing beside it.
  DockLayout layout = sample_layout();
  ASSERT_TRUE(dock_panel(layout, "monitor", "project", DockSide::Centre));

  // The right-hand column had monitor over timeline; only the timeline is left.
  ASSERT_TRUE(layout.root.is_split());
  EXPECT_EQ(layout.root.children.size(), 2u);
  EXPECT_FALSE(layout.root.children[1].is_split());
  EXPECT_EQ(layout.root.children[1].panels, (std::vector<PanelId>{"timeline"}));
  expect_canonical(layout);
}

TEST(Dock, APanelCannotBeDockedOntoItself) {
  DockLayout layout = sample_layout();
  const DockLayout before = layout;

  EXPECT_FALSE(dock_panel(layout, "project", "project", DockSide::Left));
  EXPECT_EQ(layout, before);
}

TEST(Dock, DockingOntoATargetThatIsNotThereDoesNothing) {
  DockLayout layout = sample_layout();
  const DockLayout before = layout;

  EXPECT_FALSE(dock_panel(layout, "project", "nowhere", DockSide::Left));
  EXPECT_FALSE(dock_panel(layout, "nowhere", "project", DockSide::Left));
  EXPECT_EQ(layout, before);
}

TEST(Dock, DroppingAPanelInTheMiddleOfItsOwnGroupJustShowsIt) {
  // Taking it out and putting it back would reorder the tabs behind the user's
  // back, which is not what dropping one on its neighbours looks like it does.
  DockLayout layout = sample_layout();
  ASSERT_TRUE(dock_panel(layout, "effects", "project", DockSide::Centre));

  const DockNode* group = group_of(layout.root, "effects");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->panels, (std::vector<PanelId>{"project", "effects"}))
      << "the tab order changed";
  EXPECT_EQ(group->active_panel(), std::optional<PanelId>{"effects"});
}

TEST(Dock, ADropThatChangesNothingReportsSo) {
  DockLayout layout = sample_layout();
  activate_panel(layout, "effects");
  EXPECT_FALSE(dock_panel(layout, "effects", "project", DockSide::Centre));
}

TEST(Dock, SplittingAPanelOutOfItsOwnGroupWorks) {
  DockLayout layout;
  layout.root = DockNode::tabs({"a", "b"});
  ASSERT_TRUE(dock_panel(layout, "b", "a", DockSide::Right));

  EXPECT_EQ(panels_in(layout), (std::vector<PanelId>{"a", "b"}));
  expect_canonical(layout);
}

// ------------------------------------------------------------ outer edges --

TEST(Dock, DockingAtAnEdgeSpansTheWholeWindow) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(dock_panel_at_edge(layout, "effects", DockSide::Bottom));

  ASSERT_TRUE(layout.root.is_split());
  EXPECT_EQ(layout.root.axis, Axis::Vertical);
  EXPECT_EQ(layout.root.children.back().panels, (std::vector<PanelId>{"effects"}));
  expect_canonical(layout);
}

TEST(Dock, AnEdgeDropOnTheSameAxisJoinsTheTopLevelRatherThanNesting) {
  // The root is already a row. Docking down the left has to make a third
  // sibling of it, not wrap the whole thing in another row.
  DockLayout layout = sample_layout();
  ASSERT_TRUE(dock_panel_at_edge(layout, "timeline", DockSide::Left));

  ASSERT_TRUE(layout.root.is_split());
  EXPECT_EQ(layout.root.axis, Axis::Horizontal);
  EXPECT_EQ(layout.root.children.front().panels, (std::vector<PanelId>{"timeline"}));
  expect_canonical(layout);
}

TEST(Dock, ThereIsNoMiddleOfTheRim) {
  DockLayout layout = sample_layout();
  const DockLayout before = layout;

  EXPECT_FALSE(dock_panel_at_edge(layout, "project", DockSide::Centre));
  EXPECT_EQ(layout, before);
}

// -------------------------------------------------------------- floating --

TEST(Dock, APanelCanBeTakenOutIntoItsOwnWindow) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(float_panel(layout, "effects", Rect{100.0, 80.0, 400.0, 300.0}));

  ASSERT_EQ(layout.floating.size(), 1u);
  EXPECT_EQ(layout.floating[0].root.panels, (std::vector<PanelId>{"effects"}));
  EXPECT_EQ(layout.floating[0].bounds, (Rect{100.0, 80.0, 400.0, 300.0}));
  EXPECT_TRUE(is_floating(layout, "effects"));
  EXPECT_FALSE(contains_panel(layout.root, "effects"));
  expect_canonical(layout);
}

TEST(Dock, FloatingAPanelThatIsAlreadyOutOnItsOwnJustMovesIt) {
  // Otherwise dragging a floating window around destroys and remakes the very
  // thing being dragged, once per mouse move.
  DockLayout layout = sample_layout();
  ASSERT_TRUE(float_panel(layout, "effects", Rect{0.0, 0.0, 400.0, 300.0}));

  ASSERT_TRUE(float_panel(layout, "effects", Rect{50.0, 60.0, 400.0, 300.0}));
  EXPECT_EQ(layout.floating.size(), 1u);
  EXPECT_EQ(layout.floating[0].bounds, (Rect{50.0, 60.0, 400.0, 300.0}));

  EXPECT_FALSE(float_panel(layout, "effects", Rect{50.0, 60.0, 400.0, 300.0}))
      << "moving it nowhere should report nothing changed";
}

TEST(Dock, AFloatingPanelCanBeDockedBackIn) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(float_panel(layout, "effects", Rect{0.0, 0.0, 400.0, 300.0}));
  ASSERT_TRUE(dock_panel(layout, "effects", "timeline", DockSide::Centre));

  EXPECT_TRUE(layout.floating.empty()) << "the window it left should have gone with it";
  EXPECT_TRUE(contains_panel(layout.root, "effects"));
  expect_canonical(layout);
}

TEST(Dock, AWindowWithMorePanelsSurvivesLosingOne) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(float_panel(layout, "effects", Rect{0.0, 0.0, 400.0, 300.0}));
  ASSERT_TRUE(dock_panel(layout, "project", "effects", DockSide::Centre));
  ASSERT_EQ(layout.floating.size(), 1u);

  ASSERT_TRUE(dock_panel(layout, "project", "timeline", DockSide::Centre));

  ASSERT_EQ(layout.floating.size(), 1u);
  EXPECT_EQ(layout.floating[0].root.panels, (std::vector<PanelId>{"effects"}));
  expect_canonical(layout);
}

TEST(Dock, TwoPanelsCanShareAFloatingWindow) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(float_panel(layout, "effects", Rect{0.0, 0.0, 400.0, 300.0}));
  ASSERT_TRUE(dock_panel(layout, "monitor", "effects", DockSide::Bottom));

  ASSERT_EQ(layout.floating.size(), 1u);
  EXPECT_TRUE(layout.floating[0].root.is_split());
  EXPECT_EQ(panels_in(layout.floating[0].root), (std::vector<PanelId>{"effects", "monitor"}));
  expect_canonical(layout);
}

TEST(Dock, FloatingSomethingThatIsNotThereDoesNothing) {
  DockLayout layout = sample_layout();
  const DockLayout before = layout;

  EXPECT_FALSE(float_panel(layout, "nowhere", Rect{0.0, 0.0, 100.0, 100.0}));
  EXPECT_EQ(layout, before);
}

// ------------------------------------------------ opening, closing, showing --

TEST(Dock, ClosingAPanelTakesItOutAndTidiesUp) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(close_panel(layout, "monitor"));

  EXPECT_FALSE(contains_panel(layout.root, "monitor"));
  EXPECT_EQ(panels_in(layout), (std::vector<PanelId>{"project", "effects", "timeline"}));
  expect_canonical(layout);
}

TEST(Dock, ClosingSomethingThatIsNotThereDoesNothing) {
  DockLayout layout = sample_layout();
  const DockLayout before = layout;

  EXPECT_FALSE(close_panel(layout, "nowhere"));
  EXPECT_EQ(layout, before);
}

TEST(Dock, ClosingTheLastPanelLeavesAnEmptyLayoutRatherThanARuin) {
  DockLayout layout;
  layout.root = DockNode::tabs({"only"});
  ASSERT_TRUE(close_panel(layout, "only"));

  EXPECT_TRUE(layout.root.empty());
  EXPECT_FALSE(layout.root.is_split());
  expect_canonical(layout);
}

TEST(Dock, ClosingATabBehindTheOneShowingKeepsTheRightOneShowing) {
  DockLayout layout;
  layout.root = DockNode::tabs({"a", "b", "c"});
  layout.root.active = 2;

  ASSERT_TRUE(close_panel(layout, "a"));
  EXPECT_EQ(layout.root.active_panel(), std::optional<PanelId>{"c"});
}

TEST(Dock, ClosingTheTabThatIsShowingFallsBackToANeighbour) {
  DockLayout layout;
  layout.root = DockNode::tabs({"a", "b", "c"});
  layout.root.active = 2;

  ASSERT_TRUE(close_panel(layout, "c"));
  EXPECT_EQ(layout.root.active_panel(), std::optional<PanelId>{"b"});
}

TEST(Dock, ReopeningAPanelPutsItBesideWhatWasAskedFor) {
  DockLayout layout = sample_layout();
  ASSERT_TRUE(close_panel(layout, "effects"));

  ASSERT_TRUE(open_panel(layout, "effects", "timeline"));
  const DockNode* group = group_of(layout.root, "effects");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->panels, (std::vector<PanelId>{"timeline", "effects"}));
  EXPECT_EQ(group->active_panel(), std::optional<PanelId>{"effects"});
}

TEST(Dock, ReopeningWithNowhereToGoLandsInTheMainWindow) {
  // Not in whatever floating window was made last: a panel that reappears in a
  // torn-out window the user has since moved off screen is one they cannot
  // find.
  DockLayout layout = sample_layout();
  ASSERT_TRUE(float_panel(layout, "monitor", Rect{0.0, 0.0, 400.0, 300.0}));
  ASSERT_TRUE(close_panel(layout, "effects"));

  ASSERT_TRUE(open_panel(layout, "effects"));
  EXPECT_TRUE(contains_panel(layout.root, "effects"));
  EXPECT_FALSE(is_floating(layout, "effects"));
}

TEST(Dock, OpeningAPanelThatIsAlreadyThereDoesNothing) {
  DockLayout layout = sample_layout();
  const DockLayout before = layout;

  EXPECT_FALSE(open_panel(layout, "project"));
  EXPECT_EQ(layout, before);
}

TEST(Dock, OpeningIntoAnEmptyLayoutWorks) {
  DockLayout layout;
  ASSERT_TRUE(open_panel(layout, "project"));

  EXPECT_EQ(layout.root.panels, (std::vector<PanelId>{"project"}));
  expect_canonical(layout);
}

TEST(Dock, ActivatingShowsTheTabAndSaysWhetherItHadTo) {
  DockLayout layout = sample_layout();

  EXPECT_TRUE(activate_panel(layout, "effects"));
  EXPECT_EQ(group_of(layout.root, "effects")->active_panel(), std::optional<PanelId>{"effects"});

  EXPECT_FALSE(activate_panel(layout, "effects")) << "it was already showing";
  EXPECT_FALSE(activate_panel(layout, "nowhere"));
}

// ------------------------------------------------------- a run of drags --

TEST(Dock, ALongSequenceOfDragsLeavesACanonicalTree) {
  // The point of the invariants: no single move is hard, and it is the tenth
  // one that finds the split nobody collapsed.
  DockLayout layout = sample_layout();
  const std::vector<PanelId> everything = sorted_panels(layout);

  dock_panel(layout, "effects", "monitor", DockSide::Bottom);
  dock_panel(layout, "project", "timeline", DockSide::Right);
  dock_panel_at_edge(layout, "monitor", DockSide::Top);
  float_panel(layout, "effects", Rect{10.0, 10.0, 300.0, 200.0});
  dock_panel(layout, "timeline", "effects", DockSide::Centre);
  dock_panel(layout, "project", "monitor", DockSide::Left);
  dock_panel(layout, "effects", "project", DockSide::Bottom);
  dock_panel_at_edge(layout, "timeline", DockSide::Bottom);

  EXPECT_EQ(sorted_panels(layout), everything) << "a panel was lost or duplicated";
  expect_canonical(layout);
}

}  // namespace
}  // namespace cutline::ui
