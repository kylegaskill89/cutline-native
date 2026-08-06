/// The project browser, driven from a list of structs.
///
/// It takes what it draws as plain data, which is what makes all of this
/// possible without a project, a decoder or a window: where a row lands once the
/// list is scrolled, what a click at a point selects, that a drag out of the
/// list is told apart from a click, and that rebuilding the list keeps hold of
/// the entry that was selected rather than the row it happened to be in.

#include "cutline/ui/browser.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() { return LayoutContext{default_theme(), measurer()}; }

[[nodiscard]] MouseEvent press(double x, double y, int clicks = 1) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left, .click_count = clicks};
}

[[nodiscard]] KeyEvent key(Key which) { return KeyEvent{.key = which}; }

/// Every string the painter was asked to draw, in order.
[[nodiscard]] std::vector<std::string> drawn_text(const RecordingPainter& painter) {
  std::vector<std::string> out;
  for (const DrawCall& call : painter.calls()) {
    if (call.run.has_value()) out.push_back(call.run->text);
  }
  return out;
}

[[nodiscard]] bool contains(const std::vector<std::string>& texts, std::string_view wanted) {
  return std::ranges::find(texts, wanted) != texts.end();
}

[[nodiscard]] std::vector<MediaItem> sample_items(std::size_t count) {
  std::vector<MediaItem> items;
  for (std::size_t i = 0; i < count; ++i) {
    items.push_back(MediaItem{.id = "m" + std::to_string(i),
                              .name = "Clip " + std::to_string(i),
                              .detail = "00:00:04:00",
                              .duration = 4.0});
  }
  return items;
}

/// A browser in a host, laid out at a known size.
///
/// Sized so the arithmetic is easy to check by hand: the default theme's rows
/// are 26 tall, and 260 of height is exactly ten of them.
struct Fixture {
  explicit Fixture(std::size_t count = 24, double height = 260.0) {
    host = std::make_unique<WidgetHost>(std::make_unique<MediaBrowser>());
    view = static_cast<MediaBrowser*>(&host->root());
    view->set_items(sample_items(count));
    host->resize(Rect{0.0, 0.0, 300.0, height}, flat_context());
  }

  [[nodiscard]] double row() const { return view->row_height(); }

  std::unique_ptr<WidgetHost> host;
  MediaBrowser* view = nullptr;
};

// -------------------------------------------------------------- geometry --

TEST(Browser, RowsAreTheHeightTheThemeSays) {
  const Fixture fixture;
  EXPECT_DOUBLE_EQ(fixture.view->row_height(), default_theme().metrics.list_row_height);
}

TEST(Browser, RowsStackFromTheTop) {
  const Fixture fixture;
  const Rect first = fixture.view->row_rect(0);

  EXPECT_DOUBLE_EQ(first.y, 0.0);
  EXPECT_DOUBLE_EQ(first.height, fixture.row());
  EXPECT_DOUBLE_EQ(fixture.view->row_rect(1).y, first.bottom());
}

TEST(Browser, ARowScrolledOutOfSightHasNoRectangle) {
  // Painting and hit testing both skip an empty rectangle, so this is what
  // stops either of them going near a row that is not on screen.
  const Fixture fixture;
  EXPECT_TRUE(fixture.view->row_rect(23).empty());
  EXPECT_TRUE(fixture.view->row_rect(999).empty());
}

TEST(Browser, ScrollingMovesTheRowsUnderTheWindow) {
  Fixture fixture;
  const double before = fixture.view->row_rect(2).y;

  fixture.view->scroll_by(fixture.row() * 2.0);

  EXPECT_DOUBLE_EQ(fixture.view->row_rect(2).y, before - fixture.row() * 2.0);
  // And row 0 has gone entirely, rather than reporting a rectangle above the
  // top edge that painting would then have to remember to skip.
  EXPECT_TRUE(fixture.view->row_rect(0).empty());
}

TEST(Browser, ARowHalfWayOffTheTopIsStillARectangle) {
  // Only *fully* hidden rows disappear: the one being scrolled past has to keep
  // drawing, or the list flickers a blank strip along its top edge.
  Fixture fixture;
  fixture.view->scroll_by(fixture.row() * 0.5);

  const Rect row = fixture.view->row_rect(0);
  ASSERT_FALSE(row.empty());
  EXPECT_DOUBLE_EQ(row.y, -fixture.row() * 0.5);
}

TEST(Browser, TheScrollbarTakesItsGutterOutOfTheRows) {
  const Fixture fixture;  // 24 rows in room for 10
  ASSERT_FALSE(fixture.view->scrollbar().empty());

  EXPECT_DOUBLE_EQ(fixture.view->row_rect(0).width,
                   300.0 - default_theme().metrics.scrollbar_width);
  EXPECT_DOUBLE_EQ(fixture.view->scrollbar().right(), 300.0);
}

TEST(Browser, AListThatFitsHasNoScrollbarAndNoGutter) {
  const Fixture fixture(4);
  EXPECT_TRUE(fixture.view->scrollbar().empty());
  EXPECT_DOUBLE_EQ(fixture.view->row_rect(0).width, 300.0);
}

TEST(Browser, TheBadgeIsASquareInsideTheRow) {
  const Fixture fixture;
  const Rect badge = fixture.view->badge_rect(0);
  const Rect row = fixture.view->row_rect(0);

  EXPECT_DOUBLE_EQ(badge.width, badge.height);
  EXPECT_GT(badge.x, row.x);
  EXPECT_LT(badge.bottom(), row.bottom() + 0.001);
}

// ------------------------------------------------------------- hit testing --

TEST(Browser, APointFindsTheRowItIsOver) {
  const Fixture fixture;
  EXPECT_EQ(fixture.view->row_at(20.0, 1.0), std::optional<std::size_t>{0});
  EXPECT_EQ(fixture.view->row_at(20.0, fixture.row() + 1.0), std::optional<std::size_t>{1});
}

TEST(Browser, PastTheLastRowIsNothing) {
  const Fixture fixture(3);
  EXPECT_FALSE(fixture.view->row_at(20.0, fixture.row() * 3.0 + 4.0).has_value());
}

TEST(Browser, HitTestingFollowsTheScroll) {
  Fixture fixture;
  fixture.view->scroll_by(fixture.row() * 3.0);
  EXPECT_EQ(fixture.view->row_at(20.0, 1.0), std::optional<std::size_t>{3});
}

TEST(Browser, TheScrollbarGutterIsNotPartOfARow) {
  // Otherwise dragging the scrollbar would also select whatever it passed over.
  const Fixture fixture;
  EXPECT_FALSE(fixture.view->row_at(299.0, 1.0).has_value());
}

// --------------------------------------------------------------- selection --

TEST(Browser, AClickSelectsAndReports) {
  Fixture fixture;
  std::optional<std::size_t> reported;
  int calls = 0;
  fixture.view->set_on_select([&](std::optional<std::size_t> index) {
    reported = index;
    ++calls;
  });

  ASSERT_TRUE(fixture.host->mouse_down(press(20.0, fixture.row() * 2.0 + 1.0)));

  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{2});
  EXPECT_EQ(reported, std::optional<std::size_t>{2});
  EXPECT_EQ(calls, 1);
}

TEST(Browser, ClickingBelowTheLastRowClearsTheSelection) {
  Fixture fixture(3);
  fixture.view->select(1);

  std::optional<std::size_t> reported{7};
  fixture.view->set_on_select([&](std::optional<std::size_t> index) { reported = index; });

  ASSERT_TRUE(fixture.host->mouse_down(press(20.0, fixture.row() * 3.0 + 4.0)));

  EXPECT_FALSE(fixture.view->selection().has_value());
  EXPECT_FALSE(reported.has_value());
}

TEST(Browser, SettingTheSelectionDoesNotCallBack) {
  // Otherwise selecting from inside `on_select` loops.
  Fixture fixture;
  int calls = 0;
  fixture.view->set_on_select([&](std::optional<std::size_t>) { ++calls; });

  fixture.view->select(3);

  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{3});
  EXPECT_EQ(calls, 0);
}

TEST(Browser, SelectingOutOfRangeClearsRatherThanCrashing) {
  Fixture fixture(3);
  fixture.view->select(9);
  EXPECT_FALSE(fixture.view->selection().has_value());
  EXPECT_EQ(fixture.view->selected(), nullptr);
}

TEST(Browser, SelectingScrollsTheRowIntoView) {
  Fixture fixture;
  fixture.view->select(20);

  const Rect row = fixture.view->row_rect(20);
  ASSERT_FALSE(row.empty()) << "the selected row is still off screen";
  EXPECT_GE(row.y, -0.001);
  EXPECT_LE(row.bottom(), 260.001);
}

TEST(Browser, SelectionIsKeptByEntryRatherThanByRow) {
  // The whole reason it is kept by id: after a sort the entry that was selected
  // has moved, and holding the index would silently select its neighbour.
  Fixture fixture(3);
  fixture.view->select(2);
  ASSERT_EQ(fixture.view->selected()->id, "m2");

  std::vector<MediaItem> reordered = sample_items(3);
  std::swap(reordered[0], reordered[2]);
  fixture.view->set_items(reordered);

  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{0});
  EXPECT_EQ(fixture.view->selected()->id, "m2");
}

TEST(Browser, ASelectedEntryThatIsFilteredOutLeavesNothingSelected) {
  Fixture fixture(3);
  fixture.view->select(2);

  fixture.view->set_items(sample_items(2));

  EXPECT_FALSE(fixture.view->selection().has_value());
}

TEST(Browser, SelectingByIdReportsWhetherItWasThere) {
  Fixture fixture(3);
  EXPECT_TRUE(fixture.view->select_id("m1"));
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{1});
  EXPECT_FALSE(fixture.view->select_id("nowhere"));
}

// -------------------------------------------------------------- activation --

TEST(Browser, ADoubleClickActivatesTheRow) {
  Fixture fixture;
  std::optional<std::size_t> activated;
  fixture.view->set_on_activate([&](std::size_t index) { activated = index; });

  fixture.host->mouse_down(press(20.0, 1.0, 2));

  EXPECT_EQ(activated, std::optional<std::size_t>{0});
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{0});
}

TEST(Browser, ASingleClickDoesNotActivate) {
  Fixture fixture;
  int activations = 0;
  fixture.view->set_on_activate([&](std::size_t) { ++activations; });

  fixture.host->mouse_down(press(20.0, 1.0));
  fixture.host->mouse_up(press(20.0, 1.0));

  EXPECT_EQ(activations, 0);
}

TEST(Browser, ADoubleClickDoesNotAlsoArmADrag) {
  // An impatient double-click wobbles by a pixel or two. Without this, the
  // second press starts a drag and the release drops a clip on the timeline.
  Fixture fixture;
  std::optional<std::size_t> dropped;
  fixture.view->set_on_drop([&](std::size_t index, double, double) { dropped = index; });

  fixture.host->mouse_down(press(20.0, 1.0, 2));
  fixture.host->mouse_move(press(60.0, 40.0));
  fixture.host->mouse_up(press(60.0, 40.0));

  EXPECT_FALSE(dropped.has_value());
}

// ------------------------------------------------------------------- drags --

TEST(Browser, DraggingARowOutReportsWhereItWasDropped) {
  Fixture fixture;
  std::optional<std::size_t> dropped;
  double at_x = 0.0;
  double at_y = 0.0;
  fixture.view->set_on_drop([&](std::size_t index, double x, double y) {
    dropped = index;
    at_x = x;
    at_y = y;
  });

  fixture.host->mouse_down(press(20.0, fixture.row() + 1.0));
  fixture.host->mouse_move(press(400.0, 500.0));
  EXPECT_EQ(fixture.view->dragging(), std::optional<std::size_t>{1});

  fixture.host->mouse_up(press(420.0, 520.0));

  EXPECT_EQ(dropped, std::optional<std::size_t>{1});
  EXPECT_DOUBLE_EQ(at_x, 420.0);
  EXPECT_DOUBLE_EQ(at_y, 520.0);
  EXPECT_FALSE(fixture.view->dragging().has_value());
}

TEST(Browser, AClickIsNotADrop) {
  Fixture fixture;
  int drops = 0;
  fixture.view->set_on_drop([&](std::size_t, double, double) { ++drops; });

  fixture.host->mouse_down(press(20.0, 1.0));
  // Inside the threshold: a hand that is not quite still is still a click.
  fixture.host->mouse_move(press(21.0, 2.0));
  fixture.host->mouse_up(press(21.0, 2.0));

  EXPECT_EQ(drops, 0);
  EXPECT_FALSE(fixture.view->dragging().has_value());
}

TEST(Browser, ADragKeepsGoingPastTheEdgeOfTheList) {
  // The press captured the pointer, so a drop over the timeline still arrives
  // here. Without that, every drag out of the browser would die at its border.
  Fixture fixture;
  std::optional<std::size_t> dropped;
  fixture.view->set_on_drop([&](std::size_t index, double, double) { dropped = index; });

  fixture.host->mouse_down(press(20.0, 1.0));
  fixture.host->mouse_move(press(900.0, 900.0));
  fixture.host->mouse_up(press(900.0, 900.0));

  EXPECT_EQ(dropped, std::optional<std::size_t>{0});
}

TEST(Browser, RebuildingTheListCancelsADragInFlight) {
  // The row a drag refers to may not exist afterwards.
  Fixture fixture;
  int drops = 0;
  fixture.view->set_on_drop([&](std::size_t, double, double) { ++drops; });

  fixture.host->mouse_down(press(20.0, 1.0));
  fixture.host->mouse_move(press(400.0, 400.0));
  fixture.view->set_items(sample_items(2));
  fixture.host->mouse_up(press(400.0, 400.0));

  EXPECT_EQ(drops, 0);
}

// -------------------------------------------------------------- the wheel --

TEST(Browser, TheWheelScrolls) {
  Fixture fixture;
  ASSERT_TRUE(fixture.host->wheel(WheelEvent{.x = 20.0, .y = 20.0, .delta_y = 1.0}));
  EXPECT_GT(fixture.view->vertical().offset, 0.0);
}

TEST(Browser, TheWheelIsUnhandledAtTheEnd) {
  // So it carries on to whatever is outside rather than dying against a list
  // that cannot move any further.
  Fixture fixture;
  EXPECT_FALSE(fixture.host->wheel(WheelEvent{.x = 20.0, .y = 20.0, .delta_y = -1.0}));
}

TEST(Browser, AListThatFitsIgnoresTheWheelEntirely) {
  Fixture fixture(4);
  EXPECT_FALSE(fixture.host->wheel(WheelEvent{.x = 20.0, .y = 20.0, .delta_y = 1.0}));
}

// ---------------------------------------------------------------- the keys --

TEST(Browser, ArrowsMoveTheSelection) {
  Fixture fixture;
  fixture.host->set_focus(fixture.view);
  fixture.view->select(2);

  ASSERT_TRUE(fixture.host->key_down(key(Key::Down)));
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{3});

  ASSERT_TRUE(fixture.host->key_down(key(Key::Up)));
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{2});
}

TEST(Browser, ArrowsStopAtTheEnds) {
  Fixture fixture(3);
  fixture.host->set_focus(fixture.view);
  fixture.view->select(0);

  EXPECT_FALSE(fixture.host->key_down(key(Key::Up)));
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{0});
}

TEST(Browser, ADownArrowWithNothingSelectedTakesTheFirstRow) {
  Fixture fixture;
  fixture.host->set_focus(fixture.view);

  ASSERT_TRUE(fixture.host->key_down(key(Key::Down)));
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{0});
}

TEST(Browser, AnUpArrowWithNothingSelectedTakesTheLastRow) {
  Fixture fixture(6);
  fixture.host->set_focus(fixture.view);

  ASSERT_TRUE(fixture.host->key_down(key(Key::Up)));
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{5});
}

TEST(Browser, HomeAndEndGoToTheEnds) {
  Fixture fixture;
  fixture.host->set_focus(fixture.view);
  fixture.view->select(5);

  ASSERT_TRUE(fixture.host->key_down(key(Key::End)));
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{23});

  ASSERT_TRUE(fixture.host->key_down(key(Key::Home)));
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{0});
}

TEST(Browser, EnterActivatesTheSelection) {
  Fixture fixture;
  fixture.host->set_focus(fixture.view);
  fixture.view->select(4);

  std::optional<std::size_t> activated;
  fixture.view->set_on_activate([&](std::size_t index) { activated = index; });

  ASSERT_TRUE(fixture.host->key_down(key(Key::Enter)));
  EXPECT_EQ(activated, std::optional<std::size_t>{4});
}

TEST(Browser, EnterWithNothingSelectedDoesNothing) {
  Fixture fixture;
  fixture.host->set_focus(fixture.view);
  EXPECT_FALSE(fixture.host->key_down(key(Key::Enter)));
}

TEST(Browser, KeysWithAModifierAreLeftAlone) {
  // Ctrl+Home belongs to whoever binds it — go to the start of the sequence,
  // most likely — and a list that swallowed it would break that binding
  // whenever the browser happened to have focus.
  Fixture fixture;
  fixture.host->set_focus(fixture.view);
  EXPECT_FALSE(
      fixture.host->key_down(KeyEvent{.key = Key::Home, .modifiers = {.control = true}}));
}

// ------------------------------------------------------------------ paint --

TEST(Browser, AnEmptyListSaysSoRatherThanDrawingNothing) {
  Fixture fixture(0);
  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());

  bool said = false;
  for (const std::string& text : drawn_text(painter)) {
    if (text.find("No media") != std::string::npos) said = true;
  }
  EXPECT_TRUE(said);
}

TEST(Browser, OnlyTheVisibleRowsAreDrawn) {
  // Twenty-four entries in room for ten. Drawing all of them would be the
  // difference between a pool that scales and one that does not.
  Fixture fixture;
  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());

  int names = 0;
  for (const std::string& text : drawn_text(painter)) {
    if (text.rfind("Clip ", 0) == 0) ++names;
  }
  EXPECT_GT(names, 0);
  EXPECT_LE(names, 11) << "rows off the bottom of the list were drawn too";
}

TEST(Browser, TheRowClipIsAlwaysPutBack) {
  // A leaked clip swallows everything drawn after it, which looks exactly like
  // whatever came next having gone missing.
  Fixture fixture;
  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(Browser, AnOfflineEntrySaysSoInPlaceOfItsDuration) {
  Fixture fixture(1);
  std::vector<MediaItem> items = sample_items(1);
  items[0].offline = true;
  fixture.view->set_items(items);

  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());
  const std::vector<std::string> texts = drawn_text(painter);

  EXPECT_TRUE(contains(texts, "offline"));
  EXPECT_FALSE(contains(texts, "00:00:04:00"));
}

// ------------------------------------------------------------------ bins --

namespace {

/// A bin with two entries inside it and one outside, already flattened the way
/// the binding hands it over.
[[nodiscard]] std::vector<MediaItem> tree_items() {
  return {
      MediaItem{.id = "bin:b1", .name = "Interviews", .detail = "2 items", .is_bin = true,
                .expanded = true},
      MediaItem{.id = "m0", .name = "Clip 0", .detail = "00:00:04:00", .depth = 1},
      MediaItem{.id = "m1", .name = "Clip 1", .detail = "00:00:04:00", .depth = 1},
      MediaItem{.id = "m2", .name = "Clip 2", .detail = "00:00:04:00"},
  };
}

[[nodiscard]] Fixture tree_fixture() {
  Fixture fixture(0);
  fixture.view->set_items(tree_items());
  return fixture;
}

}  // namespace

TEST(Browser, ABinHasAChevronWhereMediaHasABadge) {
  const Fixture fixture = tree_fixture();
  EXPECT_FALSE(fixture.view->twist_rect(0).empty()) << "the bin drew no chevron";
  EXPECT_TRUE(fixture.view->badge_rect(0).empty()) << "a bin should not also carry a badge";

  EXPECT_TRUE(fixture.view->twist_rect(1).empty()) << "media drew a chevron";
  EXPECT_FALSE(fixture.view->badge_rect(1).empty());
}

TEST(Browser, DepthMovesTheBadgeAcross) {
  // The whole visible difference between a tree and a list. Without it a nested
  // entry starts where its folder does and the panel reads as flat.
  const Fixture fixture = tree_fixture();
  EXPECT_GT(fixture.view->badge_rect(1).x, fixture.view->badge_rect(3).x)
      << "a row inside a bin was not indented past one outside it";
}

TEST(Browser, ClickingTheChevronOpensWithoutSelecting) {
  // Looking inside a folder must not move what a menu item would act on.
  Fixture fixture = tree_fixture();
  fixture.view->select(3);

  std::optional<std::size_t> toggled;
  fixture.view->set_on_toggle([&](std::size_t index) { toggled = index; });

  const Rect twist = fixture.view->twist_rect(0);
  ASSERT_FALSE(twist.empty());
  fixture.host->mouse_down(press(twist.x + twist.width * 0.5, twist.y + twist.height * 0.5));

  ASSERT_TRUE(toggled.has_value());
  EXPECT_EQ(*toggled, 0u);
  ASSERT_TRUE(fixture.view->selection().has_value());
  EXPECT_EQ(*fixture.view->selection(), 3u) << "opening a bin moved the selection";
}

TEST(Browser, DoubleClickingABinOpensItRatherThanPlacingIt) {
  Fixture fixture = tree_fixture();
  bool activated = false;
  std::optional<std::size_t> toggled;
  fixture.view->set_on_activate([&](std::size_t) { activated = true; });
  fixture.view->set_on_toggle([&](std::size_t index) { toggled = index; });

  const Rect row = fixture.view->row_rect(0);
  // Past the chevron, so this is the row itself rather than the chevron again.
  fixture.host->mouse_down(press(row.right() - 40.0, row.y + row.height * 0.5, 2));

  EXPECT_FALSE(activated) << "a bin was put in the sequence";
  ASSERT_TRUE(toggled.has_value());
  EXPECT_EQ(*toggled, 0u);
}

TEST(Browser, EnterOnABinOpensItToo) {
  Fixture fixture = tree_fixture();
  bool activated = false;
  bool toggled = false;
  fixture.view->set_on_activate([&](std::size_t) { activated = true; });
  fixture.view->set_on_toggle([&](std::size_t) { toggled = true; });

  fixture.view->select(0);
  fixture.host->key_down(key(Key::Enter));

  EXPECT_TRUE(toggled);
  EXPECT_FALSE(activated);
}

TEST(Browser, DraggingOntoABinFilesItRatherThanPlacingIt) {
  // A gesture that did both would put a clip on the timeline every time
  // somebody tidied the pool.
  Fixture fixture = tree_fixture();
  std::optional<std::pair<std::size_t, std::size_t>> filed;
  bool dropped = false;
  fixture.view->set_on_file([&](std::size_t from, std::size_t onto) { filed = {from, onto}; });
  fixture.view->set_on_drop([&](std::size_t, double, double) { dropped = true; });

  const Rect from = fixture.view->row_rect(3);
  const Rect onto = fixture.view->row_rect(0);
  fixture.host->mouse_down(press(from.x + 100.0, from.y + from.height * 0.5));
  fixture.host->mouse_move(press(onto.x + 100.0, onto.y + onto.height * 0.5));
  fixture.host->mouse_up(press(onto.x + 100.0, onto.y + onto.height * 0.5));

  ASSERT_TRUE(filed.has_value());
  EXPECT_EQ(filed->first, 3u);
  EXPECT_EQ(filed->second, 0u);
  EXPECT_FALSE(dropped) << "the same gesture also placed a clip";
}

TEST(Browser, DraggingOntoMediaFilesNothing) {
  // There is nowhere for it to go: the pool's order is the order it was
  // imported in, so there is no position to insert at.
  Fixture fixture = tree_fixture();
  bool filed = false;
  bool dropped = false;
  fixture.view->set_on_file([&](std::size_t, std::size_t) { filed = true; });
  fixture.view->set_on_drop([&](std::size_t, double, double) { dropped = true; });

  const Rect from = fixture.view->row_rect(3);
  const Rect onto = fixture.view->row_rect(1);
  fixture.host->mouse_down(press(from.x + 100.0, from.y + from.height * 0.5));
  fixture.host->mouse_move(press(onto.x + 100.0, onto.y + onto.height * 0.5));
  fixture.host->mouse_up(press(onto.x + 100.0, onto.y + onto.height * 0.5));

  EXPECT_FALSE(filed);
  EXPECT_TRUE(dropped) << "a release inside the list should still be a placement";
}

TEST(Browser, ABinCannotBeDraggedIntoItself) {
  // Knowable from the flattened list alone: what is inside a row is the run of
  // rows after it with a greater depth. Refused here so the highlight never
  // offers something that would then quietly not happen.
  Fixture fixture(0);
  fixture.view->set_items({
      MediaItem{.id = "bin:outer", .name = "Day 1", .is_bin = true, .expanded = true},
      MediaItem{.id = "bin:inner", .name = "Cameras", .depth = 1, .is_bin = true},
      MediaItem{.id = "bin:other", .name = "Elsewhere", .is_bin = true},
  });

  const Rect from = fixture.view->row_rect(0);
  const Rect onto = fixture.view->row_rect(1);
  fixture.host->mouse_down(press(from.right() - 20.0, from.y + from.height * 0.5));
  fixture.host->mouse_move(press(onto.right() - 20.0, onto.y + onto.height * 0.5));
  EXPECT_FALSE(fixture.view->file_target().has_value()) << "a bin was offered its own child";

  // And into an unrelated bin, which is allowed.
  const Rect other = fixture.view->row_rect(2);
  fixture.host->mouse_move(press(other.right() - 20.0, other.y + other.height * 0.5));
  ASSERT_TRUE(fixture.view->file_target().has_value());
  EXPECT_EQ(*fixture.view->file_target(), 2u);
}

TEST(Browser, ARowIsNotFiledIntoItself) {
  Fixture fixture = tree_fixture();
  const Rect row = fixture.view->row_rect(0);
  fixture.host->mouse_down(press(row.right() - 20.0, row.y + row.height * 0.5));
  fixture.host->mouse_move(press(row.right() - 30.0, row.y + row.height * 0.5));
  EXPECT_FALSE(fixture.view->file_target().has_value());
}

TEST(Browser, EveryKindHasABadge) {
  // A kind added to the model without one here would draw an empty square.
  for (const MediaKind kind :
       {MediaKind::Video, MediaKind::Audio, MediaKind::Image, MediaKind::Title, MediaKind::Color,
        MediaKind::Adjustment}) {
    EXPECT_FALSE(badge_text(kind).empty()) << to_string(kind);
    EXPECT_LE(badge_text(kind).size(), 2u) << to_string(kind);
    EXPECT_FALSE(to_string(kind).empty());
  }
}

}  // namespace
}  // namespace cutline::ui
