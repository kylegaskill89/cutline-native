/// A dock layout, realised as widgets and dragged about.
///
/// Two things here are worth more than the rest. A drop has to mean what it
/// looks like it means — the middle of a panel joins its tabs, the edge splits
/// it, the rim of the window splits everything — and that is entirely geometry,
/// so it is asserted rather than eyeballed. And panel content has to survive
/// being rearranged: a browser that lost its selection every time a neighbour
/// was dragged would be unusable, and nothing about that is visible until it
/// happens.

#include "cutline/ui/dock_view.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() { return LayoutContext{default_theme(), measurer()}; }

[[nodiscard]] MouseEvent press(double x, double y) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left};
}

/// A panel whose content can be told apart from every other panel's, and which
/// remembers something so that surviving a rearrangement can be checked.
class Marker : public Widget {
 public:
  explicit Marker(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  int state = 0;

  /// Whether a press on this content is handled, which is what makes the host
  /// hold on to it as pressed and captured. Off by default so the tests about
  /// dragging tabs are unaffected by content that would swallow a press.
  bool takes_presses = false;

  bool on_mouse_down(const MouseEvent&) override { return takes_presses; }

 private:
  std::string name_;
};

/// A browser down the left, a monitor over a timeline on the right, with an
/// inspector tabbed behind the browser.
[[nodiscard]] DockNode sample_node() {
  DockNode node = DockNode::split(
      Axis::Horizontal,
      {DockNode::tabs({"project", "effects"}),
       DockNode::split(Axis::Vertical,
                       {DockNode::tabs({"monitor"}), DockNode::tabs({"timeline"})})});
  normalise(node);
  return node;
}

struct Fixture {
  Fixture() {
    host = std::make_unique<WidgetHost>(std::make_unique<DockView>());
    view = static_cast<DockView*>(&host->root());

    view->set_content_factory([this](const PanelId& panel) -> std::unique_ptr<Widget> {
      ++made[panel];
      return std::make_unique<Marker>(panel);
    });
    view->set_titles([](const PanelId& panel) { return "The " + panel; });

    view->set_on_dock([this](PanelId panel, DropTarget target) {
      docked = std::pair{std::move(panel), target};
    });
    view->set_on_tear_out([this](PanelId panel, double x, double y) {
      torn_out = std::tuple{std::move(panel), x, y};
    });
    view->set_on_close([this](PanelId panel) { closed = std::move(panel); });
    view->set_on_activate([this](PanelId panel) { activated = std::move(panel); });

    view->set_node(sample_node());
    host->resize(Rect{0.0, 0.0, 1000.0, 600.0}, flat_context());
  }

  void relayout() { host->resize(Rect{0.0, 0.0, 1000.0, 600.0}, flat_context()); }

  /// The content widget of whichever group is showing a panel.
  [[nodiscard]] Marker* content_for(std::string_view panel) const {
    DockGroup* group = view->group_showing(panel);
    if (group == nullptr) return nullptr;
    for (const std::unique_ptr<Widget>& child : group->children()) {
      if (auto* marker = dynamic_cast<Marker*>(child.get()); marker != nullptr) return marker;
    }
    return nullptr;
  }

  std::unique_ptr<WidgetHost> host;
  DockView* view = nullptr;

  std::map<PanelId, int> made;
  std::optional<std::pair<PanelId, DropTarget>> docked;
  std::optional<std::tuple<PanelId, double, double>> torn_out;
  std::optional<PanelId> closed;
  std::optional<PanelId> activated;
};

// ------------------------------------------------------------------ building --

TEST(DockView, EveryGroupInTheTreeIsBuilt) {
  const Fixture fixture;
  EXPECT_EQ(fixture.view->groups().size(), 3u);
}

TEST(DockView, OnlyTheActiveTabsContentIsMade) {
  // Four panels, three groups: the inspector is behind the browser and nothing
  // has asked to see it yet.
  const Fixture fixture;
  EXPECT_EQ(fixture.made.at("project"), 1);
  EXPECT_EQ(fixture.made.count("effects"), 0u) << "a hidden tab's content was built anyway";
}

TEST(DockView, TabsAreTitledThroughTheLookup) {
  const Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);

  ASSERT_EQ(group->strip().tabs().size(), 2u);
  EXPECT_EQ(group->strip().tabs()[0].title, "The project");
  EXPECT_EQ(group->strip().tabs()[1].title, "The effects");
}

TEST(DockView, APanelWithNoTitleFallsBackToItsName) {
  Fixture fixture;
  fixture.view->set_titles([](const PanelId&) { return std::string{}; });
  fixture.view->set_node(sample_node());

  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->strip().tabs()[0].title, "project");
}

TEST(DockView, TheTabsSitAboveTheContent) {
  const Fixture fixture;
  DockGroup* group = fixture.view->group_showing("monitor");
  ASSERT_NE(group, nullptr);

  const Marker* content = fixture.content_for("monitor");
  ASSERT_NE(content, nullptr);
  EXPECT_DOUBLE_EQ(group->strip().bounds().y, group->bounds().y);
  EXPECT_DOUBLE_EQ(content->bounds().y, group->strip().bounds().bottom());
  EXPECT_DOUBLE_EQ(content->bounds().bottom(), group->bounds().bottom());
}

TEST(DockView, SplitsBecomeSplittersWithTheirFractions) {
  Fixture fixture;
  DockNode node = sample_node();
  node.fractions = {0.3, 0.7};
  fixture.view->set_node(node);
  fixture.relayout();

  DockGroup* left = fixture.view->group_showing("project");
  ASSERT_NE(left, nullptr);
  // 30% of a thousand, less its share of the divider between the two.
  EXPECT_NEAR(left->bounds().width, 300.0, default_theme().metrics.splitter_width);
}

TEST(DockView, AnEmptyLayoutBuildsNothingRatherThanCrashing) {
  Fixture fixture;
  fixture.view->set_node(DockNode{});
  fixture.relayout();

  EXPECT_TRUE(fixture.view->groups().empty());
  EXPECT_TRUE(fixture.view->children().empty());
}

// ------------------------------------------------------- content survives --

TEST(DockView, ContentIsKeptAcrossARearrangement) {
  // The whole reason panels are moved rather than remade. A browser that lost
  // its scroll position every time a neighbour was dragged would be unusable.
  Fixture fixture;
  Marker* before = fixture.content_for("project");
  ASSERT_NE(before, nullptr);
  before->state = 42;

  DockLayout layout{.root = sample_node()};
  ASSERT_TRUE(dock_panel(layout, "project", "timeline", DockSide::Bottom));
  fixture.view->set_node(layout.root);
  fixture.relayout();

  Marker* after = fixture.content_for("project");
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after, before) << "the panel was rebuilt rather than moved";
  EXPECT_EQ(after->state, 42);
  EXPECT_EQ(fixture.made.at("project"), 1) << "the factory was asked for it twice";
}

TEST(DockView, AHiddenPanelIsKeptRatherThanDestroyed) {
  // Switching to a tab and back has to give the same panel, not a fresh one.
  Fixture fixture;
  DockNode node = sample_node();
  node.children[0].active = 1;  // show the inspector
  fixture.view->set_node(node);
  fixture.relayout();

  Marker* effects = fixture.content_for("effects");
  ASSERT_NE(effects, nullptr);
  effects->state = 7;

  node.children[0].active = 0;  // back to the browser
  fixture.view->set_node(node);
  node.children[0].active = 1;  // and back again
  fixture.view->set_node(node);
  fixture.relayout();

  Marker* again = fixture.content_for("effects");
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(again, effects);
  EXPECT_EQ(again->state, 7);
  EXPECT_EQ(fixture.made.at("effects"), 1);
}

TEST(DockView, FractionsCanBeReadBackOutOfTheBuiltSplitters) {
  // A divider the user dragged has to get back into the layout, or the next
  // rearrangement undoes it.
  Fixture fixture;
  DockNode node = sample_node();
  node.fractions = {0.25, 0.75};
  fixture.view->set_node(node);
  fixture.relayout();

  DockNode read = sample_node();
  fixture.view->read_fractions_into(read);

  ASSERT_EQ(read.fractions.size(), 2u);
  EXPECT_NEAR(read.fractions[0], 0.25, 1e-9);
}

// ---------------------------------------------------------------- dropping --

TEST(DockView, TheMiddleOfAPanelJoinsItsTabs) {
  const Fixture fixture;
  DockGroup* group = fixture.view->group_showing("monitor");
  ASSERT_NE(group, nullptr);

  const Rect area = group->content_area();
  const auto target =
      fixture.view->drop_target(area.x + area.width / 2.0, area.y + area.height / 2.0);

  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->onto, "monitor");
  EXPECT_EQ(target->side, DockSide::Centre);
  EXPECT_FALSE(target->at_edge);
}

TEST(DockView, TheEdgeOfAPanelSplitsIt) {
  const Fixture fixture;
  DockGroup* group = fixture.view->group_showing("monitor");
  ASSERT_NE(group, nullptr);
  const Rect area = group->content_area();

  const auto left =
      fixture.view->drop_target(area.x + area.width * 0.05, area.y + area.height / 2.0);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->side, DockSide::Left);

  const auto bottom =
      fixture.view->drop_target(area.x + area.width / 2.0, area.bottom() - area.height * 0.05);
  ASSERT_TRUE(bottom.has_value());
  EXPECT_EQ(bottom->side, DockSide::Bottom);
}

TEST(DockView, DroppingOnTheTabsIsAlwaysJoiningThem) {
  // Whatever the geometry of the rest of the group would say. It is what the
  // strip looks like it means.
  const Fixture fixture;
  DockGroup* group = fixture.view->group_showing("monitor");
  ASSERT_NE(group, nullptr);

  const Rect strip = group->strip().bounds();
  const auto target = fixture.view->drop_target(strip.x + 4.0, strip.y + strip.height / 2.0);

  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->side, DockSide::Centre);
}

TEST(DockView, TheRimOfTheWindowDocksAgainstEverything) {
  const Fixture fixture;
  const auto target = fixture.view->drop_target(2.0, 300.0);

  ASSERT_TRUE(target.has_value());
  EXPECT_TRUE(target->at_edge);
  EXPECT_EQ(target->side, DockSide::Left);
  EXPECT_TRUE(target->onto.empty()) << "an edge drop is not onto any one panel";
}

TEST(DockView, TheRimWinsOverThePanelUnderIt) {
  // The left rim runs over the browser. A drop there means "down the whole
  // side", which is a different thing from splitting the browser.
  const Fixture fixture;
  const auto target = fixture.view->drop_target(2.0, 300.0);
  ASSERT_TRUE(target.has_value());
  EXPECT_TRUE(target->at_edge);
}

TEST(DockView, TheSideRimWinsOverAStripAtTheSameHeight) {
  // A strip runs the whole width of its group, so the left rim crosses one for
  // an inch. Letting the strip win there meant that aiming a panel at the very
  // edge of the window joined a tab group instead — the drop did something, but
  // not the thing being aimed at, which reads as docking having failed.
  const Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect strip = group->strip().bounds();

  const auto target = fixture.view->drop_target(2.0, strip.y + strip.height / 2.0);
  ASSERT_TRUE(target.has_value());
  EXPECT_TRUE(target->at_edge);
  EXPECT_EQ(target->side, DockSide::Left);

  // And the strip still wins everywhere else along itself, which is most of it.
  const auto joined = fixture.view->drop_target(strip.x + strip.width / 2.0,
                                                strip.y + strip.height / 2.0);
  ASSERT_TRUE(joined.has_value());
  EXPECT_EQ(joined->side, DockSide::Centre);
}

TEST(DockView, TheTopRimStillYieldsToTheStripUnderIt) {
  // The topmost group's tabs run the whole length of the top rim, so this is the
  // one place the rim has to give way: otherwise the top panel would be the one
  // group in the window that nothing could ever be tabbed into.
  const Fixture fixture;
  DockGroup* group = fixture.view->group_showing("monitor");
  ASSERT_NE(group, nullptr);
  const Rect strip = group->strip().bounds();
  ASSERT_LT(strip.y, kDockRimWidth) << "this strip is not under the top rim";

  const auto target =
      fixture.view->drop_target(strip.x + strip.width / 2.0, strip.y + 2.0);
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->side, DockSide::Centre);
  EXPECT_FALSE(target->at_edge);
}

TEST(DockView, OutsideTheViewIsNotADropAtAll) {
  // Which is how a panel is torn out into a window of its own.
  const Fixture fixture;
  EXPECT_FALSE(fixture.view->drop_target(-20.0, 300.0).has_value());
  EXPECT_FALSE(fixture.view->drop_target(500.0, 900.0).has_value());
}

TEST(DockView, TheMiddleIsABiggerTargetThanAnyEdge) {
  // Deliberately: joining a group is the common intent, and a scheme where
  // every drop near the centre is a coin toss between four splits makes
  // docking feel unpredictable.
  const Fixture fixture;
  DockGroup* group = fixture.view->group_showing("monitor");
  ASSERT_NE(group, nullptr);
  const Rect area = group->content_area();

  std::map<DockSide, int> hits;
  for (double fx = 0.025; fx < 1.0; fx += 0.05) {
    for (double fy = 0.025; fy < 1.0; fy += 0.05) {
      const auto target =
          fixture.view->drop_target(area.x + area.width * fx, area.y + area.height * fy);
      if (!target.has_value() || target->at_edge) continue;
      ++hits[target->side];
    }
  }

  ASSERT_GT(hits[DockSide::Centre], 0);
  for (const DockSide side :
       {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom}) {
    EXPECT_GT(hits[DockSide::Centre], hits[side])
        << "the " << to_string(side) << " edge is a bigger target than the middle";
  }
}

// ------------------------------------------------------------- dragging --

TEST(DockView, DraggingATabAndDroppingItReportsWhereItWent) {
  Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect tab = group->strip().tab_rect(0);

  DockGroup* onto = fixture.view->group_showing("timeline");
  ASSERT_NE(onto, nullptr);
  const Rect area = onto->content_area();

  fixture.host->mouse_down(press(tab.x + 5.0, tab.y + tab.height / 2.0));
  fixture.host->mouse_move(press(area.x + area.width / 2.0, area.y + area.height / 2.0));
  ASSERT_TRUE(fixture.view->dragging().has_value());
  EXPECT_EQ(*fixture.view->dragging(), "project");

  fixture.host->mouse_up(press(area.x + area.width / 2.0, area.y + area.height / 2.0));

  ASSERT_TRUE(fixture.docked.has_value());
  EXPECT_EQ(fixture.docked->first, "project");
  EXPECT_EQ(fixture.docked->second.onto, "timeline");
  EXPECT_EQ(fixture.docked->second.side, DockSide::Centre);
  EXPECT_FALSE(fixture.view->dragging().has_value());
}

TEST(DockView, ATabDroppedOutsideWantsToBeAWindow) {
  Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect tab = group->strip().tab_rect(0);

  fixture.host->mouse_down(press(tab.x + 5.0, tab.y + tab.height / 2.0));
  fixture.host->mouse_move(press(1400.0, 400.0));
  fixture.host->mouse_up(press(1400.0, 400.0));

  ASSERT_TRUE(fixture.torn_out.has_value());
  EXPECT_EQ(std::get<0>(*fixture.torn_out), "project");
  EXPECT_DOUBLE_EQ(std::get<1>(*fixture.torn_out), 1400.0);
  EXPECT_FALSE(fixture.docked.has_value());
}

TEST(DockView, ATabThatIsNotShowingCanStillBeDragged) {
  // Pressing it asks for it to be shown, and showing it rebuilds the tree —
  // which destroys the strip the press landed on. With the gesture living in
  // the strip, the host dropped the capture along with it and the drag was over
  // before it started: most of the tabs in a group of several could not be
  // moved at all.
  Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect tab = group->strip().tab_rect(1);
  ASSERT_FALSE(tab.empty());

  fixture.host->mouse_down(press(tab.x + 5.0, tab.y + tab.height / 2.0));
  ASSERT_TRUE(fixture.activated.has_value()) << "pressing it did not ask for it to be shown";
  EXPECT_EQ(*fixture.activated, "effects");

  // The rebuild that showing it causes, in the middle of the gesture.
  fixture.view->set_node(sample_node());
  fixture.relayout();

  DockGroup* onto = fixture.view->group_showing("monitor");
  ASSERT_NE(onto, nullptr);
  const Rect area = onto->content_area();
  fixture.host->mouse_move(press(area.x + area.width / 2.0, area.y + area.height / 2.0));
  ASSERT_TRUE(fixture.view->carrying().has_value()) << "the rebuild swallowed the drag";

  fixture.host->mouse_up(press(area.x + area.width / 2.0, area.y + area.height / 2.0));
  ASSERT_TRUE(fixture.docked.has_value());
  EXPECT_EQ(fixture.docked->first, "effects");
  EXPECT_EQ(fixture.docked->second.onto, "monitor");
}

TEST(DockView, ADragWithNowhereToLandStillEndsInATearOut) {
  // While a tab is dragged the application tells every window where the drop
  // zone should be, and a window the pointer is not over is told there is none.
  // The window the drag *started* in gets that message too the moment the
  // pointer leaves it — and when the highlight and the gesture were one field,
  // being told "nothing to show" also meant forgetting what was being carried.
  // Releasing then did nothing, so a panel could not be torn out into a window.
  Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect tab = group->strip().tab_rect(0);

  fixture.host->mouse_down(press(tab.x + 5.0, tab.y + tab.height / 2.0));
  fixture.host->mouse_move(press(1400.0, 400.0));

  fixture.view->set_drag(std::nullopt, 0.0, 0.0);
  EXPECT_FALSE(fixture.view->dragging().has_value()) << "the zone should be gone";
  ASSERT_TRUE(fixture.view->carrying().has_value()) << "but not the drag itself";

  fixture.host->mouse_up(press(1400.0, 400.0));
  ASSERT_TRUE(fixture.torn_out.has_value());
  EXPECT_EQ(std::get<0>(*fixture.torn_out), "project");
}

TEST(DockView, AClickOnATabIsNotADrag) {
  Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect tab = group->strip().tab_rect(1);

  fixture.host->mouse_down(press(tab.x + 5.0, tab.y + tab.height / 2.0));
  // Inside the threshold: a hand that is not quite still is still a click.
  fixture.host->mouse_move(press(tab.x + 6.0, tab.y + tab.height / 2.0 + 1.0));
  fixture.host->mouse_up(press(tab.x + 6.0, tab.y + tab.height / 2.0 + 1.0));

  EXPECT_FALSE(fixture.docked.has_value());
  EXPECT_FALSE(fixture.torn_out.has_value());
  EXPECT_FALSE(fixture.view->dragging().has_value());
}

TEST(DockView, ClickingATabAsksForItToBeShown) {
  Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect tab = group->strip().tab_rect(1);

  fixture.host->mouse_down(press(tab.x + 5.0, tab.y + tab.height / 2.0));

  ASSERT_TRUE(fixture.activated.has_value());
  EXPECT_EQ(*fixture.activated, "effects");
}

TEST(DockView, ClickingTheTabThatIsAlreadyShowingAsksForNothing) {
  Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect tab = group->strip().tab_rect(0);

  fixture.host->mouse_down(press(tab.x + 5.0, tab.y + tab.height / 2.0));
  EXPECT_FALSE(fixture.activated.has_value());
}

TEST(DockView, TheCrossClosesTheTabRatherThanSelectingIt) {
  Fixture fixture;
  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);

  const Rect closer = group->strip().close_rect(1);
  ASSERT_FALSE(closer.empty());

  fixture.host->mouse_down(press(closer.x + closer.width / 2.0, closer.y + closer.height / 2.0));

  ASSERT_TRUE(fixture.closed.has_value());
  EXPECT_EQ(*fixture.closed, "effects");
  EXPECT_FALSE(fixture.activated.has_value());
}

TEST(DockView, RebuildingFromInsideADropDoesNotUseAFreedStrip) {
  // The drop is what rearranges the layout, and rearranging destroys the strip
  // the drag started in. It is resolved by the view instead, which outlives it
  // — this is the test that would crash if that ever stopped being true.
  Fixture fixture;
  DockLayout layout{.root = sample_node()};

  fixture.view->set_on_dock([&](PanelId panel, DropTarget target) {
    if (target.at_edge) {
      dock_panel_at_edge(layout, panel, target.side);
    } else {
      dock_panel(layout, panel, target.onto, target.side);
    }
    fixture.view->set_node(layout.root);
  });

  DockGroup* group = fixture.view->group_showing("project");
  ASSERT_NE(group, nullptr);
  const Rect tab = group->strip().tab_rect(0);

  DockGroup* onto = fixture.view->group_showing("timeline");
  ASSERT_NE(onto, nullptr);
  const Rect area = onto->content_area();

  fixture.host->mouse_down(press(tab.x + 5.0, tab.y + tab.height / 2.0));
  fixture.host->mouse_move(press(area.x + area.width / 2.0, area.y + area.height / 2.0));
  fixture.host->mouse_up(press(area.x + area.width / 2.0, area.y + area.height / 2.0));
  fixture.relayout();

  DockGroup* moved = fixture.view->group_showing("project");
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(moved, fixture.view->group_showing("timeline"))
      << "the browser should have joined the timeline's tabs";
}

// ----------------------------------------------------------------- painting --

TEST(DockView, ADragInFlightDrawsWhereItWouldLand) {
  Fixture fixture;
  DockGroup* onto = fixture.view->group_showing("monitor");
  ASSERT_NE(onto, nullptr);
  const Rect area = onto->content_area();

  RecordingPainter without;
  fixture.host->paint(without, default_theme());

  fixture.view->set_drag(PanelId{"project"}, area.x + area.width / 2.0,
                         area.y + area.height / 2.0);

  RecordingPainter with;
  fixture.host->paint(with, default_theme());

  EXPECT_GT(with.count(DrawCall::Kind::Stroke), without.count(DrawCall::Kind::Stroke))
      << "nothing was drawn to say where the panel would go";
}

TEST(DockView, ADragOffTheEdgeDrawsNothing) {
  // There is nowhere for it to land, and a highlight left over from the last
  // place it passed would say otherwise.
  Fixture fixture;
  RecordingPainter without;
  fixture.host->paint(without, default_theme());

  fixture.view->set_drag(PanelId{"project"}, -50.0, 300.0);

  RecordingPainter with;
  fixture.host->paint(with, default_theme());
  EXPECT_EQ(with.count(DrawCall::Kind::Stroke), without.count(DrawCall::Kind::Stroke));
}

TEST(DockView, TheClipsAroundTheTabsAreAlwaysPutBack) {
  Fixture fixture;
  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
}

// ------------------------------------------------------------- the strip --

TEST(TabStrip, TabsAreAsWideAsTheirTitles) {
  auto host = std::make_unique<WidgetHost>(std::make_unique<TabStrip>());
  auto* strip = static_cast<TabStrip*>(&host->root());
  strip->set_tabs({{.id = "a", .title = "A"}, {.id = "b", .title = "A much longer name"}}, 0);
  host->resize(Rect{0.0, 0.0, 900.0, 30.0}, flat_context());

  EXPECT_LT(strip->tab_rect(0).width, strip->tab_rect(1).width);
  EXPECT_DOUBLE_EQ(strip->tab_rect(1).x, strip->tab_rect(0).right());
}

TEST(TabStrip, TooManyTabsAreSqueezedEqually) {
  // Proportionally would leave a long title readable and a short one a sliver,
  // and it is the sliver that then cannot be clicked.
  auto host = std::make_unique<WidgetHost>(std::make_unique<TabStrip>());
  auto* strip = static_cast<TabStrip*>(&host->root());

  std::vector<TabStrip::Tab> tabs;
  for (int i = 0; i < 8; ++i) {
    tabs.push_back(TabStrip::Tab{.id = std::to_string(i), .title = "Panel " + std::to_string(i)});
  }
  strip->set_tabs(tabs, 0);
  host->resize(Rect{0.0, 0.0, 200.0, 30.0}, flat_context());

  EXPECT_NEAR(strip->tab_rect(0).width, strip->tab_rect(7).width, 0.001);
  EXPECT_LE(strip->tab_rect(7).right(), 200.001) << "the last tab ran off the end";
}

TEST(TabStrip, ASqueezedTabDropsItsCrossRatherThanItsTitle) {
  // A cross that has eaten the whole tab is worse than none: the title is gone
  // and every click closes something.
  auto host = std::make_unique<WidgetHost>(std::make_unique<TabStrip>());
  auto* strip = static_cast<TabStrip*>(&host->root());

  std::vector<TabStrip::Tab> tabs;
  for (int i = 0; i < 12; ++i) {
    tabs.push_back(TabStrip::Tab{.id = std::to_string(i), .title = "Panel"});
  }
  strip->set_tabs(tabs, 0);
  host->resize(Rect{0.0, 0.0, 200.0, 30.0}, flat_context());

  EXPECT_TRUE(strip->close_rect(0).empty());
}

TEST(TabStrip, AnUnclosableTabHasNoCross) {
  auto host = std::make_unique<WidgetHost>(std::make_unique<TabStrip>());
  auto* strip = static_cast<TabStrip*>(&host->root());
  strip->set_tabs({{.id = "a", .title = "Fixed", .closable = false}}, 0);
  host->resize(Rect{0.0, 0.0, 400.0, 30.0}, flat_context());

  EXPECT_TRUE(strip->close_rect(0).empty());
  EXPECT_FALSE(strip->tab_rect(0).empty());
}

TEST(TabStrip, TheActiveTabIsClampedToWhatIsThere) {
  auto host = std::make_unique<WidgetHost>(std::make_unique<TabStrip>());
  auto* strip = static_cast<TabStrip*>(&host->root());
  strip->set_tabs({{.id = "a", .title = "A"}}, 9);
  EXPECT_EQ(strip->active(), 0u);
}

TEST(TabStrip, APointFindsTheTabUnderIt) {
  auto host = std::make_unique<WidgetHost>(std::make_unique<TabStrip>());
  auto* strip = static_cast<TabStrip*>(&host->root());
  strip->set_tabs({{.id = "a", .title = "A"}, {.id = "b", .title = "B"}}, 0);
  host->resize(Rect{0.0, 0.0, 400.0, 30.0}, flat_context());

  const Rect second = strip->tab_rect(1);
  EXPECT_EQ(strip->tab_at(second.x + 2.0, 15.0), std::optional<std::size_t>{1});
  EXPECT_FALSE(strip->tab_at(390.0, 15.0).has_value()) << "the empty end of the strip";
}


TEST(DockView, ReclaimingAPanelTellsTheHostToLetGoOfWhatIsInIt) {
  // The fault this was written for, and it arrives long after its cause: an
  // access violation in `WidgetHost::set_pressed` on the *next* press, because
  // the host was still pointing at a widget that had been freed with the panel
  // holding it.
  //
  // Rearranging takes a panel's content out of the tree and keeps it aside so
  // it survives with its scroll position and selection. `clear_children` then
  // forgets what it destroys — but it can only recognise what still descends
  // from the tree it is clearing, and the content's parent has just been set to
  // nothing. So the pointer stayed, out of the tree and unreachable, until the
  // panel was replaced or the window closed.
  Fixture fixture;

  Marker* inside = fixture.content_for("project");
  ASSERT_NE(inside, nullptr);
  inside->takes_presses = true;
  inside->set_focusable(true);

  // Pressed the way a hand would press it, so the host holds it as pressed,
  // captured, focused and hovered all at once.
  const Rect area = inside->bounds();
  fixture.host->mouse_move(press(area.x + 5.0, area.y + 5.0));
  ASSERT_TRUE(fixture.host->mouse_down(press(area.x + 5.0, area.y + 5.0)));
  ASSERT_EQ(fixture.host->pressed(), inside);

  // A rearrangement that leaves this panel out of the visible tree: its group
  // shows the other tab, so the content goes into the spare pile.
  DockNode node = sample_node();
  DockNode* group = group_of(node, "project");
  ASSERT_NE(group, nullptr);
  group->active = 1;  // "effects" is showing; "project" is reclaimed
  fixture.view->set_node(node);
  fixture.relayout();

  EXPECT_EQ(fixture.host->pressed(), nullptr) << "the next press would write through this";
  EXPECT_EQ(fixture.host->captured(), nullptr);
  EXPECT_EQ(fixture.host->focused(), nullptr);
  // Hover is the one that may legitimately be something: the pointer has not
  // moved, and whatever took this panel's place is now under it. What it must
  // not be is the panel that was put aside.
  EXPECT_NE(fixture.host->hovered(), inside);
}

TEST(DockView, APanelPutAsideStillComesBackWithWhatItHeld) {
  // The other half of the same change: forgetting the host's pointers must not
  // cost the panel its state, which is the entire reason content is reclaimed
  // rather than rebuilt.
  Fixture fixture;

  Marker* inside = fixture.content_for("project");
  ASSERT_NE(inside, nullptr);
  inside->state = 42;
  inside->takes_presses = true;
  const Rect area = inside->bounds();
  ASSERT_TRUE(fixture.host->mouse_down(press(area.x + 5.0, area.y + 5.0)));

  DockNode node = sample_node();
  DockNode* group = group_of(node, "project");
  ASSERT_NE(group, nullptr);
  group->active = 1;
  fixture.view->set_node(node);
  fixture.relayout();

  // And back again.
  fixture.view->set_node(sample_node());
  fixture.relayout();

  Marker* returned = fixture.content_for("project");
  ASSERT_NE(returned, nullptr);
  EXPECT_EQ(returned->state, 42) << "it was rebuilt rather than put back";
  EXPECT_EQ(fixture.made["project"], 1) << "the factory ran a second time";
}

}  // namespace
}  // namespace cutline::ui
