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
  /// The top of the first row. Not zero any more: the column headings take a
  /// row's height off the top, and every coordinate here is measured from
  /// below them.
  [[nodiscard]] double top() const { return view->list_area().y; }

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

  EXPECT_DOUBLE_EQ(first.y, fixture.top());
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
  EXPECT_DOUBLE_EQ(row.y, fixture.top() - fixture.row() * 0.5);
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
  EXPECT_EQ(fixture.view->row_at(20.0, fixture.top() + 1.0), std::optional<std::size_t>{0});
  EXPECT_EQ(fixture.view->row_at(20.0, fixture.top() + fixture.row() + 1.0),
            std::optional<std::size_t>{1});
}

TEST(Browser, PastTheLastRowIsNothing) {
  const Fixture fixture(3);
  EXPECT_FALSE(
      fixture.view->row_at(20.0, fixture.top() + fixture.row() * 3.0 + 4.0).has_value());
}

TEST(Browser, HitTestingFollowsTheScroll) {
  Fixture fixture;
  fixture.view->scroll_by(fixture.row() * 3.0);
  EXPECT_EQ(fixture.view->row_at(20.0, fixture.top() + 1.0), std::optional<std::size_t>{3});
}

TEST(Browser, TheScrollbarGutterIsNotPartOfARow) {
  // Otherwise dragging the scrollbar would also select whatever it passed over.
  const Fixture fixture;
  EXPECT_FALSE(fixture.view->row_at(299.0, fixture.top() + 1.0).has_value());
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

  ASSERT_TRUE(fixture.host->mouse_down(press(20.0, fixture.top() + fixture.row() * 2.0 + 1.0)));

  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{2});
  EXPECT_EQ(reported, std::optional<std::size_t>{2});
  EXPECT_EQ(calls, 1);
}

TEST(Browser, ClickingBelowTheLastRowClearsTheSelection) {
  Fixture fixture(3);
  fixture.view->select(1);

  std::optional<std::size_t> reported{7};
  fixture.view->set_on_select([&](std::optional<std::size_t> index) { reported = index; });

  ASSERT_TRUE(fixture.host->mouse_down(press(20.0, fixture.top() + fixture.row() * 3.0 + 4.0)));

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

  fixture.host->mouse_down(press(20.0, fixture.top() + 1.0, 2));

  EXPECT_EQ(activated, std::optional<std::size_t>{0});
  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{0});
}

TEST(Browser, ASingleClickDoesNotActivate) {
  Fixture fixture;
  int activations = 0;
  fixture.view->set_on_activate([&](std::size_t) { ++activations; });

  fixture.host->mouse_down(press(20.0, fixture.top() + 1.0));
  fixture.host->mouse_up(press(20.0, fixture.top() + 1.0));

  EXPECT_EQ(activations, 0);
}

TEST(Browser, ADoubleClickDoesNotAlsoArmADrag) {
  // An impatient double-click wobbles by a pixel or two. Without this, the
  // second press starts a drag and the release drops a clip on the timeline.
  Fixture fixture;
  std::optional<std::size_t> dropped;
  fixture.view->set_on_drop([&](std::size_t index, double, double) { dropped = index; });

  fixture.host->mouse_down(press(20.0, fixture.top() + 1.0, 2));
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

  fixture.host->mouse_down(press(20.0, fixture.top() + fixture.row() + 1.0));
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

  fixture.host->mouse_down(press(20.0, fixture.top() + 1.0));
  // Inside the threshold: a hand that is not quite still is still a click.
  fixture.host->mouse_move(press(21.0, fixture.top() + 2.0));
  fixture.host->mouse_up(press(21.0, fixture.top() + 2.0));

  EXPECT_EQ(drops, 0);
  EXPECT_FALSE(fixture.view->dragging().has_value());
}

TEST(Browser, ADragKeepsGoingPastTheEdgeOfTheList) {
  // The press captured the pointer, so a drop over the timeline still arrives
  // here. Without that, every drag out of the browser would die at its border.
  Fixture fixture;
  std::optional<std::size_t> dropped;
  fixture.view->set_on_drop([&](std::size_t index, double, double) { dropped = index; });

  fixture.host->mouse_down(press(20.0, fixture.top() + 1.0));
  fixture.host->mouse_move(press(900.0, 900.0));
  fixture.host->mouse_up(press(900.0, 900.0));

  EXPECT_EQ(dropped, std::optional<std::size_t>{0});
}

TEST(Browser, RebuildingTheListCancelsADragInFlight) {
  // The row a drag refers to may not exist afterwards.
  Fixture fixture;
  int drops = 0;
  fixture.view->set_on_drop([&](std::size_t, double, double) { ++drops; });

  fixture.host->mouse_down(press(20.0, fixture.top() + 1.0));
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

TEST(Browser, ARightClickSelectsWhatItIsOver) {
  // A menu that acted on something other than what was clicked would be a trap.
  Fixture fixture(4);
  std::optional<std::size_t> reported;
  double where_x = 0.0;
  fixture.view->set_on_select([&](std::optional<std::size_t> index) { reported = index; });
  fixture.view->set_on_context_menu([&](double x, double) { where_x = x; });

  const MouseEvent right{
      .x = 20.0, .y = fixture.top() + fixture.row() * 2.0 + 1.0, .button = MouseButton::Right, .click_count = 1};
  ASSERT_TRUE(fixture.host->mouse_down(right));

  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{2});
  EXPECT_EQ(reported, std::optional<std::size_t>{2});
  EXPECT_DOUBLE_EQ(where_x, 20.0);
}

TEST(Browser, ARightClickOnEmptySpaceClearsTheSelection) {
  // Found by driving: a right-click below the last row offered Remove, and
  // would have removed a row the pointer was nowhere near.
  Fixture fixture(3);
  fixture.view->select(1);
  bool asked = false;
  fixture.view->set_on_context_menu([&](double, double) { asked = true; });

  const MouseEvent right{
      .x = 20.0, .y = fixture.top() + fixture.row() * 3.0 + 4.0, .button = MouseButton::Right, .click_count = 1};
  ASSERT_TRUE(fixture.host->mouse_down(right));

  EXPECT_FALSE(fixture.view->selection().has_value());
  EXPECT_TRUE(asked) << "the menu should still open, with only what applies to the panel";
}

// --------------------------------------------------------------- columns --

TEST(Browser, TheHeaderTakesARowOffTheTopOfTheList) {
  const Fixture fixture;
  const Rect header = fixture.view->header_rect();

  ASSERT_FALSE(header.empty());
  EXPECT_DOUBLE_EQ(header.y, 0.0);
  EXPECT_DOUBLE_EQ(header.height, fixture.row());
  EXPECT_DOUBLE_EQ(fixture.view->list_area().y, header.bottom());
  // And the header spans the scrollbar's gutter, so there is no notch in it.
  EXPECT_DOUBLE_EQ(header.width, 300.0);
}

TEST(Browser, NoColumnsMeansNoHeader) {
  Fixture fixture;
  fixture.view->set_columns({});
  EXPECT_TRUE(fixture.view->header_rect().empty());
  EXPECT_DOUBLE_EQ(fixture.view->list_area().y, 0.0);
}

TEST(Browser, EveryColumnHasAHeading) {
  // One added to the enumeration without one here would draw a blank heading
  // that still sorts, which is a control nobody can find.
  for (const MediaColumn column : {MediaColumn::Name, MediaColumn::Duration, MediaColumn::Uses,
                                   MediaColumn::Kind}) {
    EXPECT_FALSE(column_heading(column).empty());
  }
}

TEST(Browser, ColumnsAreDrawnInTheOrderTheyWereGiven) {
  const Fixture fixture;
  const std::vector<MediaColumn> shown = fixture.view->visible_columns();
  ASSERT_FALSE(shown.empty());
  EXPECT_EQ(shown.front(), MediaColumn::Name);

  double previous = -1.0;
  for (const MediaColumn column : shown) {
    const Rect heading = fixture.view->heading_rect(column);
    ASSERT_FALSE(heading.empty()) << to_string(MediaKind::Video);
    EXPECT_GT(heading.x, previous);
    previous = heading.x;
  }
}

TEST(Browser, ANarrowPanelDropsColumnsFromTheEndRatherThanSqueezingThem) {
  // A column narrow enough to cut its own heading in half says less than no
  // column at all, and Kind is last because the badge already says it.
  Fixture fixture;
  fixture.host->resize(Rect{0.0, 0.0, 180.0, 260.0}, flat_context());

  const std::vector<MediaColumn> shown = fixture.view->visible_columns();
  EXPECT_LT(shown.size(), default_columns().size());
  EXPECT_EQ(shown.front(), MediaColumn::Name) << "the name is never the one dropped";
  EXPECT_EQ(std::ranges::find(shown, MediaColumn::Kind), shown.end());
}

TEST(Browser, TheNameKeepsAReadableWidthHoweverNarrowThePanelIs) {
  Fixture fixture;
  fixture.host->resize(Rect{0.0, 0.0, 120.0, 260.0}, flat_context());

  const Rect name = fixture.view->heading_rect(MediaColumn::Name);
  ASSERT_FALSE(name.empty()) << "the name column was dropped";
  EXPECT_GE(name.width, kBrowserNameFloor * 0.5)
      << "the name was squeezed rather than a column being dropped";
}

TEST(Browser, ColumnsLineUpWithTheirHeadings) {
  // The thing that goes wrong when the arithmetic is done twice: a column of
  // durations that drifts a pixel out of line with the word above it.
  const Fixture fixture;
  for (const MediaColumn column : fixture.view->visible_columns()) {
    const Rect heading = fixture.view->heading_rect(column);
    const Rect cell = fixture.view->column_rect(0, column);
    ASSERT_FALSE(cell.empty());
    EXPECT_DOUBLE_EQ(cell.x, heading.x);
    EXPECT_DOUBLE_EQ(cell.width, heading.width);
  }
}

TEST(Browser, ClickingAHeadingAsksForThatOrder) {
  Fixture fixture;
  std::optional<MediaColumn> asked;
  fixture.view->set_on_sort([&](MediaColumn column) { asked = column; });

  const Rect heading = fixture.view->heading_rect(MediaColumn::Duration);
  ASSERT_FALSE(heading.empty());
  ASSERT_TRUE(fixture.host->mouse_down(
      press(heading.x + heading.width * 0.5, heading.y + heading.height * 0.5)));

  ASSERT_TRUE(asked.has_value());
  EXPECT_EQ(*asked, MediaColumn::Duration);
}

TEST(Browser, ClickingTheHeaderNeverSelectsARow) {
  // The rows scroll underneath it, so treating a press there as a press on a
  // row would select whatever happened to be beneath the headings.
  Fixture fixture;
  fixture.view->set_on_sort([](MediaColumn) {});
  bool selected = false;
  fixture.view->set_on_select([&](std::optional<std::size_t>) { selected = true; });

  const Rect header = fixture.view->header_rect();
  ASSERT_TRUE(fixture.host->mouse_down(press(20.0, header.y + header.height * 0.5)));

  EXPECT_FALSE(selected);
  EXPECT_FALSE(fixture.view->selection().has_value());
}

TEST(Browser, TheSortedColumnIsMarkedAndTheOthersAreNot) {
  Fixture fixture;
  fixture.view->set_sorted_by(MediaColumn::Duration, false);

  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());
  const std::vector<std::string> texts = drawn_text(painter);

  // The headings are all drawn; the mark beside one of them is two strokes,
  // which is what tells a sorted column from an unsorted one.
  EXPECT_TRUE(contains(texts, "Name"));
  EXPECT_TRUE(contains(texts, "Duration"));

  int lines = 0;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Line) ++lines;
  }
  EXPECT_GE(lines, 2) << "no arrow was drawn on the sorted column";
}

TEST(Browser, ARowShowsSomethingInEveryColumnItHas) {
  Fixture fixture;
  std::vector<MediaItem> items = sample_items(1);
  items[0].uses = 3;
  fixture.view->set_items(items);

  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());
  const std::vector<std::string> texts = drawn_text(painter);

  EXPECT_TRUE(contains(texts, "Clip 0"));
  EXPECT_TRUE(contains(texts, "00:00:04:00"));
  EXPECT_TRUE(contains(texts, "3")) << "the uses column drew nothing";
  EXPECT_TRUE(contains(texts, "video")) << "the kind column drew nothing";
}

TEST(Browser, ABinDoesNotClaimADurationOrACountOfClips) {
  // A folder saying "0" in the used column reads as a fault rather than as a
  // question that does not apply.
  Fixture fixture(0);
  fixture.view->set_items(
      {MediaItem{.id = "bin:b1", .name = "Interviews", .detail = "2 items", .is_bin = true}});

  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());
  const std::vector<std::string> texts = drawn_text(painter);

  EXPECT_TRUE(contains(texts, "Interviews"));
  EXPECT_TRUE(contains(texts, "2 items")) << "a bin should say what is in it";
  EXPECT_FALSE(contains(texts, "0")) << "a bin claimed a clip count";
}

TEST(Browser, ALongNameIsClippedToItsColumn) {
  // Found by driving: text is drawn *from* a rectangle but not bounded by one,
  // so a camera file whose name is half a sentence ran straight over the
  // duration beside it.
  Fixture fixture(0);
  fixture.view->set_items({MediaItem{.id = "m0",
                                     .name = "Replay 07-23-2026 10PM-59-02 second unit.mkv",
                                     .detail = "00:09:58:17",
                                     .duration = 598.0}});

  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());

  const Rect name = fixture.view->heading_rect(MediaColumn::Name);
  ASSERT_FALSE(name.empty());

  // The clip stack, walked alongside the calls: what bounds a piece of text is
  // whatever was pushed last when it was drawn.
  std::vector<Rect> clips;
  bool found = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::PushClip) clips.push_back(call.bounds);
    if (call.kind == DrawCall::Kind::PopClip && !clips.empty()) clips.pop_back();
    if (!call.run.has_value() || call.run->text.find("Replay") == std::string::npos) continue;

    found = true;
    ASSERT_FALSE(clips.empty()) << "the name was drawn with nothing holding it in";
    EXPECT_LE(clips.back().right(), name.right() + 0.001)
        << "the name could run over the column beside it";
  }
  EXPECT_TRUE(found) << "the name was never drawn";
  EXPECT_TRUE(painter.clips_balanced());
}

// ------------------------------------------------------------- icon view --

namespace {

/// A filmstrip whose frames are flat colours, so which one was drawn can be
/// told from the pixels.
[[nodiscard]] std::shared_ptr<const Filmstrip> striped(int count) {
  auto strip = std::make_shared<Filmstrip>();
  for (int i = 0; i < count; ++i) {
    FilmFrame frame;
    frame.t = static_cast<double>(i);
    frame.width = 2;
    frame.height = 2;
    frame.rgba.assign(2 * 2 * 4, static_cast<std::uint8_t>(10 + i * 20));
    strip->frames.push_back(std::move(frame));
  }
  return strip;
}

/// An icon view with `count` entries, each ten seconds long with frames.
[[nodiscard]] Fixture icon_fixture(std::size_t count) {
  Fixture fixture(0);
  std::vector<MediaItem> items = sample_items(count);
  for (MediaItem& item : items) {
    item.duration = 10.0;
    item.filmstrip = striped(10);
  }
  fixture.view->set_items(std::move(items));
  fixture.view->set_view(BrowserView::Icons);
  return fixture;
}

}  // namespace

TEST(Browser, TheIconViewLaysTilesAcrossAndThenDown) {
  const Fixture fixture = icon_fixture(9);
  const int across = fixture.view->tiles_across();
  ASSERT_GT(across, 1) << "the panel should hold more than one tile at 300 wide";

  const Rect first = fixture.view->row_rect(0);
  const Rect second = fixture.view->row_rect(1);
  EXPECT_DOUBLE_EQ(second.y, first.y) << "the second tile started a new row";
  EXPECT_GT(second.x, first.x);

  const Rect wrapped = fixture.view->row_rect(static_cast<std::size_t>(across));
  EXPECT_DOUBLE_EQ(wrapped.x, first.x) << "the row did not wrap back to the left";
  EXPECT_GT(wrapped.y, first.y);
}

TEST(Browser, TheListViewIsOneTileAcross) {
  const Fixture fixture;
  EXPECT_EQ(fixture.view->tiles_across(), 1);
}

TEST(Browser, ATileIsTallerThanARow) {
  // It has to hold a picture and a name under it. Everything else — scrolling,
  // hit testing, selection — is the same arithmetic with a taller row.
  Fixture fixture = icon_fixture(4);
  const double tall = fixture.view->row_height();
  fixture.view->set_view(BrowserView::List);
  EXPECT_GT(tall, fixture.view->row_height());
}

TEST(Browser, ThereAreNoHeadingsOverAGrid) {
  const Fixture fixture = icon_fixture(4);
  EXPECT_TRUE(fixture.view->header_rect().empty());
  EXPECT_DOUBLE_EQ(fixture.view->list_area().y, 0.0);
}

TEST(Browser, APointFindsTheTileItIsOver) {
  const Fixture fixture = icon_fixture(9);
  for (std::size_t i = 0; i < 5; ++i) {
    const Rect tile = fixture.view->row_rect(i);
    if (tile.empty()) continue;
    EXPECT_EQ(fixture.view->row_at(tile.x + tile.width * 0.5, tile.y + tile.height * 0.5),
              std::optional<std::size_t>{i});
  }
}

TEST(Browser, EmptySpaceBesideAPartFilledLastRowIsNothing) {
  // Otherwise a click in the gap after the last tile selects the first tile of
  // a row that does not exist.
  const Fixture fixture = icon_fixture(1);
  const Rect tile = fixture.view->row_rect(0);
  ASSERT_FALSE(tile.empty());
  EXPECT_FALSE(fixture.view->row_at(tile.right() + tile.width * 0.5,
                                    tile.y + tile.height * 0.5)
                   .has_value());
}

TEST(Browser, TheGridScrollsByRowsRatherThanByEntries) {
  // Nine entries three across is three rows, not nine — and a scroll extent
  // that counted entries would let the grid scroll six rows past its end.
  const Fixture fixture = icon_fixture(9);
  const int across = fixture.view->tiles_across();
  const auto rows = static_cast<double>((9 + across - 1) / across);
  EXPECT_DOUBLE_EQ(fixture.view->vertical().content, rows * fixture.view->row_height());
}

TEST(Browser, MovingAcrossATileScrubsIt) {
  // The whole point of looking at a pool as pictures: running the pointer along
  // one walks the source.
  Fixture fixture = icon_fixture(3);
  const Rect tile = fixture.view->row_rect(0);
  ASSERT_FALSE(tile.empty());

  fixture.host->mouse_move(press(tile.x + tile.width * 0.1, tile.y + tile.height * 0.5));
  ASSERT_EQ(fixture.view->hovered_item(), std::optional<std::size_t>{0});
  const double early = fixture.view->hovered_fraction();

  fixture.host->mouse_move(press(tile.x + tile.width * 0.9, tile.y + tile.height * 0.5));
  EXPECT_GT(fixture.view->hovered_fraction(), early);
  EXPECT_LE(fixture.view->hovered_fraction(), 1.0);
  EXPECT_GE(early, 0.0);
}

TEST(Browser, ScrubbingShowsADifferentFrame) {
  // Asserted through the pixels, because "the fraction changed" would pass
  // just as well against a tile that always draws the same picture.
  Fixture fixture = icon_fixture(1);
  const Rect tile = fixture.view->row_rect(0);

  const auto drawn_image = [](const RecordingPainter& painter) -> std::uint8_t {
    for (const DrawCall& call : painter.calls()) {
      if (call.kind == DrawCall::Kind::Image && call.image.pixels != nullptr) {
        return call.image.pixels[0];
      }
    }
    return 0;
  };

  fixture.host->mouse_move(press(tile.x + tile.width * 0.05, tile.y + tile.height * 0.4));
  RecordingPainter early;
  fixture.host->paint(early, default_theme());

  fixture.host->mouse_move(press(tile.x + tile.width * 0.95, tile.y + tile.height * 0.4));
  RecordingPainter late;
  fixture.host->paint(late, default_theme());

  EXPECT_NE(drawn_image(early), 0) << "no frame was drawn at all";
  EXPECT_NE(drawn_image(early), drawn_image(late))
      << "the tile showed the same frame wherever the pointer was";
}

TEST(Browser, LeavingThePanelStopsTheScrub) {
  // Or the last tile crossed keeps showing whichever frame it was left on,
  // which reads as a picture changing on its own.
  Fixture fixture = icon_fixture(3);
  const Rect tile = fixture.view->row_rect(0);
  fixture.host->mouse_move(press(tile.x + tile.width * 0.5, tile.y + tile.height * 0.5));
  ASSERT_TRUE(fixture.view->hovered_item().has_value());

  fixture.host->mouse_exit();
  EXPECT_FALSE(fixture.view->hovered_item().has_value());
}

TEST(Browser, ATileWithNoFramesYetDrawsItsBadgeInstead) {
  // Filmstrips arrive on a worker, so a pool always starts without them. A
  // blank tile is indistinguishable from one that failed to draw.
  Fixture fixture(0);
  std::vector<MediaItem> items = sample_items(1);
  items[0].kind = MediaKind::Audio;
  fixture.view->set_items(std::move(items));
  fixture.view->set_view(BrowserView::Icons);

  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());
  const std::vector<std::string> texts = drawn_text(painter);

  EXPECT_TRUE(contains(texts, "A")) << "a tile with no frames drew nothing in their place";
  EXPECT_TRUE(contains(texts, "Clip 0")) << "the name is under the picture either way";
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(Browser, ALongTileCaptionKeepsItsStartRatherThanItsMiddle) {
  // Found by driving. Clipping a centred name cuts both ends, and the end it
  // must not cut is the front: a camera file is told from its neighbours by how
  // it starts, and "07-23-2026 10PM-59-" says nothing that "Replay 07-23-2026"
  // does not say better.
  const auto alignment_of = [](const RecordingPainter& painter,
                               std::string_view needle) -> std::optional<TextAlign> {
    for (const DrawCall& call : painter.calls()) {
      if (call.run.has_value() && call.run->text.find(needle) != std::string::npos) {
        return call.run->align;
      }
    }
    return std::nullopt;
  };

  Fixture fixture(0);
  fixture.view->set_items({MediaItem{.id = "m0", .name = "A.mp4"},
                           MediaItem{.id = "m1",
                                     .name = "Replay 07-23-2026 10PM-59-02 second unit.mkv"}});
  fixture.view->set_view(BrowserView::Icons);

  RecordingPainter painter;
  fixture.host->paint(painter, default_theme());

  EXPECT_EQ(alignment_of(painter, "A.mp4"), std::optional<TextAlign>{TextAlign::Center})
      << "a name that fits should sit under the middle of its picture";
  EXPECT_EQ(alignment_of(painter, "Replay"), std::optional<TextAlign>{TextAlign::Left})
      << "a name too long to fit lost its beginning";
}

TEST(Browser, SelectingAndActivatingWorkTheSameInEitherView) {
  // The one thing that would make the icon view a second implementation to keep
  // in step. It is the same rows at a different shape.
  Fixture fixture = icon_fixture(6);
  std::optional<std::size_t> activated;
  fixture.view->set_on_activate([&](std::size_t index) { activated = index; });

  const Rect tile = fixture.view->row_rect(2);
  ASSERT_FALSE(tile.empty());
  fixture.host->mouse_down(press(tile.x + tile.width * 0.5, tile.y + tile.height * 0.5, 2));

  EXPECT_EQ(fixture.view->selection(), std::optional<std::size_t>{2});
  EXPECT_EQ(activated, std::optional<std::size_t>{2});
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
