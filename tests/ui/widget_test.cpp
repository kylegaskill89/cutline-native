/// Input routing, driven without a window.
///
/// Events are values, so every question about interaction is a function call
/// here: which widget a click lands on, whether a drag keeps tracking after the
/// cursor leaves the control, what Tab does, whether a disabled button swallows
/// a press or lets it through to the panel behind. These are all things that
/// are tedious to check by hand and quietly break when the tree changes.

#include "cutline/ui/widget.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace cutline::ui {
namespace {

/// Records what it was sent, and can be told what to accept.
class Probe : public Widget {
 public:
  explicit Probe(std::string name) { set_name(std::move(name)); }

  bool takes_mouse = false;
  bool takes_wheel = false;
  bool takes_keys = false;

  std::vector<std::string> log;
  int enters = 0;
  int leaves = 0;
  int focus_gained = 0;
  int focus_lost = 0;
  double last_x = 0.0;

  bool on_mouse_down(const MouseEvent& event) override {
    log.emplace_back("down");
    last_x = event.x;
    return takes_mouse;
  }
  bool on_mouse_up(const MouseEvent& event) override {
    log.emplace_back("up");
    last_x = event.x;
    return takes_mouse;
  }
  bool on_mouse_move(const MouseEvent& event) override {
    log.emplace_back("move");
    last_x = event.x;
    return takes_mouse;
  }
  bool on_wheel(const WheelEvent&) override {
    log.emplace_back("wheel");
    return takes_wheel;
  }
  bool on_key_down(const KeyEvent&) override {
    log.emplace_back("key");
    return takes_keys;
  }
  bool on_text(char32_t) override {
    log.emplace_back("text");
    return takes_keys;
  }
  void on_mouse_enter() override { ++enters; }
  void on_mouse_leave() override { ++leaves; }
  void on_focus_changed(bool gained) override { gained ? ++focus_gained : ++focus_lost; }
};

/// A host with a root and two children side by side, which is enough shape for
/// most of what routing has to get right.
struct Fixture {
  Fixture() {
    auto root = std::make_unique<Probe>("root");
    host = std::make_unique<WidgetHost>(std::move(root));
    left = &host->root().emplace<Probe>("left");
    right = &host->root().emplace<Probe>("right");

    host->resize(Rect{0.0, 0.0, 200.0, 100.0});
    left->arrange(Rect{0.0, 0.0, 100.0, 100.0});
    right->arrange(Rect{100.0, 0.0, 100.0, 100.0});
  }

  [[nodiscard]] Probe& root() const { return static_cast<Probe&>(host->root()); }

  std::unique_ptr<WidgetHost> host;
  Probe* left = nullptr;
  Probe* right = nullptr;
};

[[nodiscard]] MouseEvent press(double x, double y) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left};
}

// ------------------------------------------------------------ hit testing --

TEST(WidgetTree, TheDeepestWidgetUnderAPointWins) {
  Fixture fixture;
  auto& inner = fixture.left->emplace<Probe>("inner");
  inner.arrange(Rect{10.0, 10.0, 20.0, 20.0});

  EXPECT_EQ(fixture.host->root().at(15.0, 15.0), &inner);
  EXPECT_EQ(fixture.host->root().at(60.0, 60.0), fixture.left);
  EXPECT_EQ(fixture.host->root().at(150.0, 50.0), fixture.right);
  EXPECT_EQ(fixture.host->root().at(500.0, 50.0), nullptr);
}

TEST(WidgetTree, LaterSiblingsAreOnTop) {
  // Hit testing has to agree with paint order, or overlapping panels answer
  // clicks with whichever one happens to be drawn underneath.
  Fixture fixture;
  fixture.left->arrange(Rect{0.0, 0.0, 200.0, 100.0});
  fixture.right->arrange(Rect{0.0, 0.0, 200.0, 100.0});

  EXPECT_EQ(fixture.host->root().at(50.0, 50.0), fixture.right);
}

TEST(WidgetTree, AHiddenWidgetIsNotThere) {
  Fixture fixture;
  fixture.left->set_visible(false);
  EXPECT_EQ(fixture.host->root().at(50.0, 50.0), &fixture.host->root());
}

TEST(WidgetTree, ChildrenAreOnlyFoundInsideTheirParent) {
  // A child sticking out of its parent is not reachable, which is why menus
  // and tooltips belong at the top of the tree rather than nested where they
  // visually appear.
  Fixture fixture;
  auto& escapee = fixture.left->emplace<Probe>("escapee");
  escapee.arrange(Rect{150.0, 0.0, 40.0, 40.0});

  EXPECT_NE(fixture.host->root().at(160.0, 20.0), &escapee);
}

TEST(WidgetTree, DescentIsReflexive) {
  Fixture fixture;
  EXPECT_TRUE(fixture.left->descends_from(&fixture.host->root()));
  EXPECT_TRUE(fixture.left->descends_from(fixture.left));
  EXPECT_FALSE(fixture.left->descends_from(fixture.right));
}

// ------------------------------------------------------------------- hover --

TEST(Hover, MovingBetweenWidgetsLeavesOneAndEntersTheOther) {
  Fixture fixture;

  fixture.host->mouse_move(press(50.0, 50.0));
  EXPECT_EQ(fixture.host->hovered(), fixture.left);
  EXPECT_EQ(fixture.left->enters, 1);
  EXPECT_TRUE(fixture.left->hovered());

  fixture.host->mouse_move(press(150.0, 50.0));
  EXPECT_EQ(fixture.left->leaves, 1);
  EXPECT_EQ(fixture.right->enters, 1);
  EXPECT_FALSE(fixture.left->hovered());
  EXPECT_TRUE(fixture.right->hovered());
}

TEST(Hover, StayingPutDoesNotReenter) {
  Fixture fixture;
  fixture.host->mouse_move(press(50.0, 50.0));
  fixture.host->mouse_move(press(51.0, 51.0));
  EXPECT_EQ(fixture.left->enters, 1);
  EXPECT_EQ(fixture.left->leaves, 0);
}

TEST(Hover, LeavingTheWindowClearsIt) {
  Fixture fixture;
  fixture.host->mouse_move(press(50.0, 50.0));
  fixture.host->mouse_exit();

  EXPECT_EQ(fixture.host->hovered(), nullptr);
  EXPECT_EQ(fixture.left->leaves, 1);
}

TEST(Hover, ResizingRefreshesWhatIsUnderARestingCursor) {
  // Otherwise a panel that appears under a still pointer stays unlit until it
  // is nudged.
  Fixture fixture;
  fixture.host->mouse_move(press(150.0, 50.0));
  ASSERT_EQ(fixture.host->hovered(), fixture.right);

  fixture.left->arrange(Rect{0.0, 0.0, 200.0, 100.0});
  fixture.right->arrange(Rect{0.0, 0.0, 0.0, 0.0});
  fixture.host->resize(Rect{0.0, 0.0, 200.0, 100.0});

  EXPECT_EQ(fixture.host->hovered(), fixture.left);
}

// ----------------------------------------------------------------- bubbling --

TEST(Bubbling, AnUnhandledEventReachesTheParent) {
  // What makes a click on a label inside a button still press the button.
  Fixture fixture;
  fixture.root().takes_mouse = true;

  EXPECT_TRUE(fixture.host->mouse_down(press(50.0, 50.0)));
  EXPECT_EQ(fixture.left->log.size(), 1u) << "the child should have been offered it first";
  EXPECT_EQ(fixture.root().log.size(), 1u);
}

TEST(Bubbling, AHandledEventStopsThere) {
  Fixture fixture;
  fixture.left->takes_mouse = true;
  fixture.root().takes_mouse = true;

  EXPECT_TRUE(fixture.host->mouse_down(press(50.0, 50.0)));
  EXPECT_TRUE(fixture.root().log.empty()) << "the parent saw an event its child had taken";
}

TEST(Bubbling, ADisabledWidgetSwallowsRatherThanPassingOn) {
  // Clicking a greyed-out button should do nothing at all, not fall through
  // and hit whatever panel is behind it.
  Fixture fixture;
  fixture.left->set_enabled(false);
  fixture.root().takes_mouse = true;

  EXPECT_FALSE(fixture.host->mouse_down(press(50.0, 50.0)));
  EXPECT_TRUE(fixture.left->log.empty());
  EXPECT_TRUE(fixture.root().log.empty()) << "the press leaked past a disabled widget";
}

TEST(Bubbling, WheelReachesAnAncestorThatScrolls) {
  // Scrolling over a clip has to scroll the timeline holding it.
  Fixture fixture;
  fixture.root().takes_wheel = true;

  EXPECT_TRUE(fixture.host->wheel(WheelEvent{.x = 50.0, .y = 50.0, .delta_y = 1.0}));
  EXPECT_EQ(fixture.left->log.size(), 1u);
  EXPECT_EQ(fixture.root().log.size(), 1u);
}

TEST(Bubbling, NothingUnderThePointerIsNotAnError) {
  Fixture fixture;
  EXPECT_FALSE(fixture.host->mouse_down(press(1000.0, 1000.0)));
  EXPECT_FALSE(fixture.host->wheel(WheelEvent{.x = 1000.0, .y = 1000.0}));
}

// ----------------------------------------------------------------- capture --

TEST(Capture, AHandledPressKeepsThePointerUntilRelease) {
  // The property every drag in the application rests on.
  Fixture fixture;
  fixture.left->takes_mouse = true;

  fixture.host->mouse_down(press(50.0, 50.0));
  EXPECT_EQ(fixture.host->captured(), fixture.left);

  // Well outside the widget, and even outside the window.
  fixture.host->mouse_move(press(900.0, 50.0));
  EXPECT_EQ(fixture.left->last_x, 900.0) << "the drag stopped tracking at the edge";
  EXPECT_TRUE(fixture.right->log.empty());

  fixture.host->mouse_up(press(900.0, 50.0));
  EXPECT_EQ(fixture.host->captured(), nullptr);
}

TEST(Capture, AnUnhandledPressCapturesNothing) {
  Fixture fixture;
  fixture.host->mouse_down(press(50.0, 50.0));
  EXPECT_EQ(fixture.host->captured(), nullptr);
}

TEST(Capture, HoverDoesNotWanderDuringADrag) {
  // A slider being dragged should keep its highlight rather than handing it to
  // whatever the cursor passes over.
  Fixture fixture;
  fixture.left->takes_mouse = true;

  fixture.host->mouse_move(press(50.0, 50.0));
  fixture.host->mouse_down(press(50.0, 50.0));
  fixture.host->mouse_move(press(150.0, 50.0));

  EXPECT_EQ(fixture.host->hovered(), fixture.left);
  EXPECT_FALSE(fixture.right->hovered());
}

TEST(Capture, ReleasingCatchesUpOnHover) {
  Fixture fixture;
  fixture.left->takes_mouse = true;

  fixture.host->mouse_move(press(50.0, 50.0));
  fixture.host->mouse_down(press(50.0, 50.0));
  fixture.host->mouse_move(press(150.0, 50.0));
  fixture.host->mouse_up(press(150.0, 50.0));

  EXPECT_EQ(fixture.host->hovered(), fixture.right);
}

TEST(Capture, TheWidgetLooksPressedWhileItIs) {
  Fixture fixture;
  fixture.left->takes_mouse = true;

  fixture.host->mouse_down(press(50.0, 50.0));
  EXPECT_TRUE(fixture.left->pressed());
  EXPECT_EQ(fixture.left->state(), State::Pressed);

  fixture.host->mouse_up(press(50.0, 50.0));
  EXPECT_FALSE(fixture.left->pressed());
}

TEST(Capture, ADragThatLeavesTheWindowIsStillADrag) {
  Fixture fixture;
  fixture.left->takes_mouse = true;
  fixture.host->mouse_down(press(50.0, 50.0));

  fixture.host->mouse_exit();
  EXPECT_EQ(fixture.host->captured(), fixture.left) << "the drag was dropped halfway";
}

TEST(Capture, TheReleaseGoesToTheCapturingWidgetWhereverItHappens) {
  Fixture fixture;
  fixture.left->takes_mouse = true;
  fixture.host->mouse_down(press(50.0, 50.0));

  EXPECT_TRUE(fixture.host->mouse_up(press(150.0, 50.0)));
  EXPECT_EQ(fixture.left->log.back(), "up");
  EXPECT_TRUE(fixture.right->log.empty());
}

// ------------------------------------------------------------------- focus --

TEST(Focus, OnlyFocusableWidgetsTakeIt) {
  Fixture fixture;
  EXPECT_FALSE(fixture.host->set_focus(fixture.left));
  EXPECT_EQ(fixture.host->focused(), nullptr);

  fixture.left->set_focusable(true);
  EXPECT_TRUE(fixture.host->set_focus(fixture.left));
  EXPECT_EQ(fixture.left->focus_gained, 1);
}

TEST(Focus, ADisabledOrHiddenWidgetIsRefused) {
  Fixture fixture;
  fixture.left->set_focusable(true);
  fixture.left->set_enabled(false);
  EXPECT_FALSE(fixture.host->set_focus(fixture.left));

  fixture.left->set_enabled(true);
  fixture.left->set_visible(false);
  EXPECT_FALSE(fixture.host->set_focus(fixture.left));
}

TEST(Focus, AControlInsideAHiddenPanelIsUnreachable) {
  Fixture fixture;
  auto& inner = fixture.left->emplace<Probe>("inner");
  inner.set_focusable(true);
  inner.arrange(Rect{0.0, 0.0, 20.0, 20.0});
  fixture.left->set_visible(false);

  EXPECT_FALSE(fixture.host->set_focus(&inner));
}

TEST(Focus, MovingItNotifiesBothSides) {
  Fixture fixture;
  fixture.left->set_focusable(true);
  fixture.right->set_focusable(true);

  fixture.host->set_focus(fixture.left);
  fixture.host->set_focus(fixture.right);

  EXPECT_EQ(fixture.left->focus_lost, 1);
  EXPECT_EQ(fixture.right->focus_gained, 1);
  EXPECT_FALSE(fixture.left->focused());
}

TEST(Focus, KeysGoToWhatIsFocusedAndBubbleFromThere) {
  Fixture fixture;
  fixture.left->set_focusable(true);
  fixture.host->set_focus(fixture.left);
  fixture.root().takes_keys = true;

  EXPECT_TRUE(fixture.host->key_down(KeyEvent{.key = Key::Space}));
  EXPECT_EQ(fixture.left->log.size(), 1u);
  EXPECT_EQ(fixture.root().log.size(), 1u) << "the shortcut never reached the window";
  EXPECT_TRUE(fixture.right->log.empty());
}

TEST(Focus, KeysGoToTheRootWhenNothingIsFocused) {
  Fixture fixture;
  fixture.root().takes_keys = true;
  EXPECT_TRUE(fixture.host->key_down(KeyEvent{.key = Key::Space}));
  EXPECT_TRUE(fixture.host->text(U'x'));
}

TEST(Focus, TabWalksTreeOrderAndWraps) {
  Fixture fixture;
  fixture.left->set_focusable(true);
  fixture.right->set_focusable(true);

  EXPECT_TRUE(fixture.host->focus_next());
  EXPECT_EQ(fixture.host->focused(), fixture.left);
  EXPECT_TRUE(fixture.host->focus_next());
  EXPECT_EQ(fixture.host->focused(), fixture.right);
  EXPECT_TRUE(fixture.host->focus_next());
  EXPECT_EQ(fixture.host->focused(), fixture.left) << "it should wrap";

  EXPECT_TRUE(fixture.host->focus_next(true));
  EXPECT_EQ(fixture.host->focused(), fixture.right);
}

TEST(Focus, TabSkipsWhatCannotTakeIt) {
  Fixture fixture;
  fixture.left->set_focusable(true);
  fixture.left->set_enabled(false);
  fixture.right->set_focusable(true);

  fixture.host->focus_next();
  EXPECT_EQ(fixture.host->focused(), fixture.right);
}

TEST(Focus, TabWithNowhereToGoDoesNothing) {
  Fixture fixture;
  EXPECT_FALSE(fixture.host->focus_next());
  EXPECT_EQ(fixture.host->focused(), nullptr);
}

TEST(Focus, PressingAFocusableWidgetFocusesIt) {
  Fixture fixture;
  fixture.left->takes_mouse = true;
  fixture.left->set_focusable(true);

  fixture.host->mouse_down(press(50.0, 50.0));
  EXPECT_EQ(fixture.host->focused(), fixture.left);
}

TEST(Focus, PressingSomethingUnfocusableLeavesTheKeyboardWhereItWas) {
  // Clicking a toolbar button must not strand the keyboard, or the transport
  // shortcuts stop working until you click back into the timeline.
  Fixture fixture;
  fixture.left->set_focusable(true);
  fixture.host->set_focus(fixture.left);

  fixture.right->takes_mouse = true;
  fixture.host->mouse_down(press(150.0, 50.0));

  EXPECT_EQ(fixture.host->focused(), fixture.left);
}

TEST(Focus, PressingAChildFocusesItsNearestFocusableAncestor) {
  Fixture fixture;
  fixture.left->set_focusable(true);
  auto& inner = fixture.left->emplace<Probe>("inner");
  inner.takes_mouse = true;
  inner.arrange(Rect{0.0, 0.0, 20.0, 20.0});

  fixture.host->mouse_down(press(10.0, 10.0));
  EXPECT_EQ(fixture.host->focused(), fixture.left);
}

// --------------------------------------------------------------- lifetimes --

TEST(Lifetimes, ClearingASubtreeDropsEveryReferenceToIt) {
  // Rebuilding a panel is routine. A stale hover or capture pointer would
  // survive until the next mouse move and then crash.
  Fixture fixture;
  fixture.left->takes_mouse = true;
  fixture.left->set_focusable(true);

  fixture.host->mouse_move(press(50.0, 50.0));
  fixture.host->mouse_down(press(50.0, 50.0));
  ASSERT_EQ(fixture.host->hovered(), fixture.left);
  ASSERT_EQ(fixture.host->captured(), fixture.left);
  ASSERT_EQ(fixture.host->focused(), fixture.left);

  fixture.host->root().clear_children();

  EXPECT_EQ(fixture.host->hovered(), nullptr);
  EXPECT_EQ(fixture.host->captured(), nullptr);
  EXPECT_EQ(fixture.host->focused(), nullptr);
}

TEST(Lifetimes, ForgettingReachesDescendantsToo) {
  Fixture fixture;
  auto& inner = fixture.left->emplace<Probe>("inner");
  inner.takes_mouse = true;
  inner.arrange(Rect{0.0, 0.0, 20.0, 20.0});

  fixture.host->mouse_down(press(10.0, 10.0));
  ASSERT_EQ(fixture.host->captured(), &inner);

  fixture.host->forget(fixture.left);
  EXPECT_EQ(fixture.host->captured(), nullptr);
}

TEST(Lifetimes, ATreeWithNoRootStillWorks) {
  WidgetHost host(nullptr);
  host.resize(Rect{0.0, 0.0, 100.0, 100.0});
  EXPECT_FALSE(host.mouse_down(press(10.0, 10.0)));
}

// -------------------------------------------------------------- appearance --

/// Paints its surface, so the theme lookup can be observed.
class Surface : public Widget {
 public:
  [[nodiscard]] Part part() const noexcept override { return Part::Button; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }
};

TEST(Appearance, StatePrefersTheMoreSpecificCondition) {
  Fixture fixture;
  EXPECT_EQ(fixture.left->state(), State::Normal);

  fixture.left->set_selected(true);
  EXPECT_EQ(fixture.left->state(), State::Selected);

  fixture.left->set_enabled(false);
  EXPECT_EQ(fixture.left->state(), State::Disabled) << "disabled has to win";
}

TEST(Appearance, AWidgetDrawsItselfAndThenItsChildren) {
  WidgetHost host(std::make_unique<Surface>());
  auto& child = host.root().emplace<Surface>();
  host.resize(Rect{0.0, 0.0, 100.0, 100.0});
  child.arrange(Rect{10.0, 10.0, 40.0, 40.0});

  RecordingPainter painter;
  host.paint(painter, default_theme());

  ASSERT_EQ(painter.count(DrawCall::Kind::Fill), 2u);
  const DrawCall* first = painter.first(DrawCall::Kind::Fill);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->bounds, host.root().bounds()) << "the child was painted under its parent";
}

TEST(Appearance, ClippingChildrenIsBalanced) {
  WidgetHost host(std::make_unique<Surface>());
  host.root().set_clips_children(true);
  host.root().emplace<Surface>().arrange(Rect{0.0, 0.0, 10.0, 10.0});
  host.resize(Rect{0.0, 0.0, 100.0, 100.0});

  RecordingPainter painter;
  host.paint(painter, default_theme());

  EXPECT_TRUE(painter.clips_balanced());
  EXPECT_NE(painter.first(DrawCall::Kind::PushClip), nullptr);
}

TEST(Appearance, AHiddenSubtreeIsNotPainted) {
  WidgetHost host(std::make_unique<Surface>());
  host.root().set_visible(false);
  host.root().emplace<Surface>().arrange(Rect{0.0, 0.0, 10.0, 10.0});
  host.resize(Rect{0.0, 0.0, 100.0, 100.0});

  RecordingPainter painter;
  host.paint(painter, default_theme());
  EXPECT_TRUE(painter.calls().empty());
}

TEST(Appearance, KeysHaveNames) {
  EXPECT_EQ(to_string(Key::Z), "Z");
  EXPECT_EQ(to_string(Key::Space), " ");
  EXPECT_EQ(to_string(Key::PageDown), "PageDown");
  EXPECT_EQ(to_string(Key::F5), "F5");
  EXPECT_EQ(to_string(Key::None), "None");
}

}  // namespace
}  // namespace cutline::ui
