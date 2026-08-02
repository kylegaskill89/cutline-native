/// The effects library.
///
/// Most of what matters here is the tree: which rows exist after a filter, and
/// which folders are open. A search that appears to find nothing because its
/// results are behind a closed folder is the failure this is guarding against.

#include "cutline/ui/effects_browser.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() {
  return LayoutContext{default_theme(), measurer()};
}

[[nodiscard]] std::vector<EffectEntry> catalogue() {
  return {EffectEntry{.id = "video:brightness", .name = "Brightness", .folder = "Video · Colour"},
          EffectEntry{.id = "video:hue", .name = "Hue", .folder = "Video · Colour"},
          EffectEntry{.id = "video:blur", .name = "Gaussian Blur",
                      .folder = "Video · Blur & Sharpen"},
          EffectEntry{.id = "audio:lowpass", .name = "Low Pass", .folder = "Audio"},
          EffectEntry{.id = "transition:dissolve", .name = "Cross Dissolve",
                      .folder = "Video Transitions"}};
}

/// A browser in a host. Every folder is opened unless a test says otherwise,
/// because most of these are about rows rather than about folders — and the
/// library itself starts collapsed, so a fixture that did not say which it
/// wanted would be testing the default by accident.
struct Listed {
  explicit Listed(bool open_all = true) {
    host = std::make_unique<WidgetHost>(std::make_unique<Widget>());
    browser = &host->root().emplace<EffectsBrowser>();
    browser->set_items(catalogue());
    if (open_all) {
      for (const EffectEntry& entry : catalogue()) browser->set_open(entry.folder, true);
    }
    host->resize(Rect{0.0, 0.0, 300.0, 600.0}, flat_context());
    browser->arrange(Rect{0.0, 0.0, 260.0, 500.0}, flat_context());
  }

  /// The row index for an entry, or past the end.
  [[nodiscard]] std::size_t row_of(std::string_view id) const {
    const auto& rows = browser->rows();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].id == id) return i;
    }
    return rows.size();
  }

  [[nodiscard]] std::size_t folders() const {
    std::size_t count = 0;
    for (const EffectsBrowser::Row& row : browser->rows()) {
      if (row.is_folder) ++count;
    }
    return count;
  }

  std::unique_ptr<WidgetHost> host;
  EffectsBrowser* browser = nullptr;
};

[[nodiscard]] MouseEvent press(double y, int clicks = 1) {
  return MouseEvent{.x = 40.0, .y = y, .button = MouseButton::Left, .click_count = clicks};
}

TEST(EffectsBrowser, GroupsEntriesUnderTheirFolders) {
  const Listed test;
  EXPECT_EQ(test.folders(), 4u);
  // Four folders and five entries.
  EXPECT_EQ(test.browser->rows().size(), 9u);
}

TEST(EffectsBrowser, CollapsedItIsJustTheHeadings) {
  const Listed test(false);
  EXPECT_EQ(test.folders(), 4u);
  EXPECT_EQ(test.browser->rows().size(), 4u);
}

TEST(EffectsBrowser, KeepsTheCataloguesOrderRatherThanTheAlphabets) {
  // Where a folder sits is the registry's decision. Sorted, Audio would come
  // first and the video effects nobody sorted would look shuffled.
  const Listed test;
  EXPECT_EQ(test.browser->rows()[0].name, "Video · Colour");
}

TEST(EffectsBrowser, ClosingAFolderHidesItsContentsAndNotItself) {
  Listed test;
  ASSERT_LT(test.row_of("video:brightness"), test.browser->rows().size());

  test.browser->set_open("Video · Colour", false);

  EXPECT_FALSE(test.browser->is_open("Video · Colour"));
  EXPECT_EQ(test.folders(), 4u) << "the heading went too";
  EXPECT_EQ(test.row_of("video:brightness"), test.browser->rows().size());
  EXPECT_LT(test.row_of("audio:lowpass"), test.browser->rows().size())
      << "another folder's contents went with it";
}

TEST(EffectsBrowser, EverythingStartsCollapsed) {
  // The whole catalogue laid out at once is forty names in a narrow column,
  // which is a list rather than a library. Remembering which folders were
  // *opened* also means a category added later arrives collapsed like the rest
  // rather than spilling open.
  const Listed test(false);
  EXPECT_FALSE(test.browser->is_open("Audio"));
  EXPECT_FALSE(test.browser->is_open("A folder nobody has heard of"));
}

TEST(EffectsBrowser, ClickingAHeadingAnywhereOpensAndClosesIt) {
  // A folder's name is a much larger target than a six-pixel chevron, and both
  // mean the same thing.
  Listed test(false);
  const Rect heading = test.browser->row_rect(0);
  test.host->mouse_down(press(heading.y + heading.height / 2));
  EXPECT_TRUE(test.browser->is_open("Video · Colour"));

  test.host->mouse_down(press(heading.y + heading.height / 2));
  EXPECT_FALSE(test.browser->is_open("Video · Colour"));
}

// ---------------------------------------------------------------- search --

TEST(EffectsBrowser, FilteringNarrowsToWhatMatches) {
  Listed test;
  test.browser->set_filter("hue");
  EXPECT_EQ(test.browser->rows().size(), 2u) << "one folder and one entry";
  EXPECT_LT(test.row_of("video:hue"), test.browser->rows().size());
}

TEST(EffectsBrowser, FilteringIgnoresCase) {
  Listed test;
  test.browser->set_filter("BLUR");
  EXPECT_LT(test.row_of("video:blur"), test.browser->rows().size());
}

TEST(EffectsBrowser, AFolderWhoseNameMatchesKeepsItsContents) {
  // Searching for "audio" should find the Audio folder as readily as an effect
  // with Audio in its name.
  Listed test;
  test.browser->set_filter("audio");
  EXPECT_LT(test.row_of("audio:lowpass"), test.browser->rows().size());
}

TEST(EffectsBrowser, AFolderWithNothingLeftInItIsNotDrawnAtAll) {
  // A column of empty headings is the least useful possible answer to a search.
  Listed test;
  test.browser->set_filter("hue");
  EXPECT_EQ(test.folders(), 1u);
}

TEST(EffectsBrowser, ASearchOpensEveryFolderWhateverWasClosed) {
  // Everything starts collapsed, so without this a search would appear to find
  // nothing at all — which is the whole reason searching has to override it.
  Listed test(false);
  ASSERT_EQ(test.row_of("video:hue"), test.browser->rows().size());

  test.browser->set_filter("hue");
  EXPECT_LT(test.row_of("video:hue"), test.browser->rows().size());

  // And what was closed is closed again once the search is cleared.
  test.browser->set_filter("");
  EXPECT_FALSE(test.browser->is_open("Video · Colour"));
}

TEST(EffectsBrowser, NothingMatchingSaysSoRatherThanDrawingAnEmptyPanel) {
  Listed test;
  test.browser->set_filter("xyzzy");
  ASSERT_TRUE(test.browser->rows().empty());

  RecordingPainter painter;
  test.browser->paint(painter, default_theme());
  bool said = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.run.has_value() && call.run->text.find("xyzzy") != std::string::npos) said = true;
  }
  EXPECT_TRUE(said);
}

TEST(EffectsBrowser, ASearchGoesBackToTheTop) {
  // Results beginning above the scroll position look like no results at all.
  Listed test;
  // Short enough that there is somewhere to scroll to at all.
  test.browser->arrange(Rect{0.0, 0.0, 260.0, 60.0}, flat_context());
  test.browser->set_scroll(40.0);
  ASSERT_GT(test.browser->scroll(), 0.0);
  test.browser->set_filter("a");
  EXPECT_DOUBLE_EQ(test.browser->scroll(), 0.0);
}

// ------------------------------------------------------------- selection --

TEST(EffectsBrowser, ASingleClickPicksAndADoubleClickApplies) {
  // So the catalogue can be read without changing anything.
  Listed test;
  std::string chosen;
  int chooses = 0;
  test.browser->set_on_choose([&](const std::string& id) {
    ++chooses;
    chosen = id;
  });

  const std::size_t row = test.row_of("video:hue");
  const Rect where = test.browser->row_rect(row);
  test.host->mouse_down(press(where.y + where.height / 2));
  EXPECT_EQ(test.browser->selected(), "video:hue");
  EXPECT_EQ(chooses, 0);

  test.host->mouse_down(press(where.y + where.height / 2, 2));
  EXPECT_EQ(chooses, 1);
  EXPECT_EQ(chosen, "video:hue");
}

TEST(EffectsBrowser, AFolderOpenedByHandStaysOpenAcrossASearch) {
  Listed test(false);
  test.browser->set_open("Audio", true);
  test.browser->set_filter("hue");
  test.browser->set_filter("");
  EXPECT_TRUE(test.browser->is_open("Audio"));
}

TEST(EffectsBrowser, ClickingAFolderDoesNotSelectAnything) {
  Listed test;
  const Rect heading = test.browser->row_rect(0);
  test.host->mouse_down(press(heading.y + heading.height / 2));
  EXPECT_TRUE(test.browser->selected().empty());
}

TEST(EffectsBrowser, TheSelectionSurvivesASearchThatKeepsIt) {
  // Held by id rather than by row, so narrowing the list does not silently
  // select whatever has moved into the old position.
  Listed test;
  test.browser->select("video:hue");
  test.browser->set_filter("hue");
  EXPECT_EQ(test.browser->selected(), "video:hue");
}

TEST(EffectsBrowser, ArrowKeysWalkPastTheFolderHeadings) {
  Listed test;
  test.host->set_focus(test.browser);
  test.host->key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.browser->selected(), "video:brightness");

  test.host->key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.browser->selected(), "video:hue");

  // The next one down is in the folder after this one, and the heading between
  // them is not something the keyboard should land on.
  test.host->key_down(KeyEvent{.key = Key::Down});
  EXPECT_EQ(test.browser->selected(), "video:blur");
}

TEST(EffectsBrowser, EnterApplliesWhatIsSelected) {
  Listed test;
  std::string chosen;
  test.browser->set_on_choose([&](const std::string& id) { chosen = id; });
  test.browser->select("audio:lowpass");
  test.host->set_focus(test.browser);

  test.host->key_down(KeyEvent{.key = Key::Enter});
  EXPECT_EQ(chosen, "audio:lowpass");
}

// --------------------------------------------------------------- dragging --

TEST(EffectsBrowser, APressThatDoesNotMoveIsAClickRatherThanADrag) {
  Listed test;
  int drags = 0;
  int drops = 0;
  test.browser->set_on_drag([&](const std::string&, double, double) { ++drags; });
  test.browser->set_on_drop([&](const std::string&, double, double) { ++drops; });

  const Rect where = test.browser->row_rect(test.row_of("video:hue"));
  const double y = where.y + where.height / 2;
  test.host->mouse_down(press(y));
  test.host->mouse_move(MouseEvent{.x = 40.0, .y = y});
  test.host->mouse_up(press(y));

  EXPECT_EQ(drags, 0);
  EXPECT_EQ(drops, 0);
  EXPECT_EQ(test.browser->selected(), "video:hue") << "the click did not even pick";
}

TEST(EffectsBrowser, MovingPastTheThresholdStartsCarryingTheEntry) {
  Listed test;
  std::string carried;
  test.browser->set_on_drag([&](const std::string& id, double, double) { carried = id; });

  const Rect where = test.browser->row_rect(test.row_of("video:blur"));
  const double y = where.y + where.height / 2;
  test.host->mouse_down(press(y));
  test.host->mouse_move(MouseEvent{.x = 40.0, .y = y + EffectsBrowser::kDragThreshold - 1.0});
  EXPECT_TRUE(carried.empty()) << "a wobble started a drag";

  test.host->mouse_move(MouseEvent{.x = 40.0, .y = y + 40.0});
  EXPECT_EQ(carried, "video:blur");
  EXPECT_EQ(test.browser->dragging(), "video:blur");
}

TEST(EffectsBrowser, TheDragKeepsArrivingOutsideTheBrowser) {
  // The whole reason a drag can cross into another panel: a handled press
  // captures the pointer, so the moves keep coming however far away it goes.
  Listed test;
  double last_x = 0.0;
  test.browser->set_on_drag([&](const std::string&, double x, double) { last_x = x; });

  const Rect where = test.browser->row_rect(test.row_of("video:blur"));
  const double y = where.y + where.height / 2;
  test.host->mouse_down(press(y));
  ASSERT_EQ(test.host->captured(), test.browser);

  test.host->mouse_move(MouseEvent{.x = 900.0, .y = y + 200.0});
  EXPECT_DOUBLE_EQ(last_x, 900.0);
}

TEST(EffectsBrowser, TheDropReportsWhereTheButtonCameUp) {
  Listed test;
  std::string dropped;
  double where_x = 0.0;
  test.browser->set_on_drop([&](const std::string& id, double x, double) {
    dropped = id;
    where_x = x;
  });

  const Rect row = test.browser->row_rect(test.row_of("audio:lowpass"));
  const double y = row.y + row.height / 2;
  test.host->mouse_down(press(y));
  test.host->mouse_move(MouseEvent{.x = 600.0, .y = y + 100.0});
  test.host->mouse_up(MouseEvent{.x = 600.0, .y = y + 100.0, .button = MouseButton::Left});

  EXPECT_EQ(dropped, "audio:lowpass");
  EXPECT_DOUBLE_EQ(where_x, 600.0);
  EXPECT_TRUE(test.browser->dragging().empty()) << "still carrying it after the release";
}

TEST(EffectsBrowser, AFolderCannotBeDragged) {
  // There is nothing to apply, and a heading that could be picked up would be a
  // gesture with no possible ending.
  Listed test;
  int drags = 0;
  test.browser->set_on_drag([&](const std::string&, double, double) { ++drags; });

  const Rect heading = test.browser->row_rect(0);
  test.host->mouse_down(press(heading.y + heading.height / 2));
  test.host->mouse_move(MouseEvent{.x = 40.0, .y = heading.y + 80.0});
  EXPECT_EQ(drags, 0);
}

// -------------------------------------------------------------- scrolling --

TEST(EffectsBrowser, ScrollingStopsAtBothEnds) {
  Listed test;
  test.browser->arrange(Rect{0.0, 0.0, 260.0, 40.0}, flat_context());

  test.browser->set_scroll(-50.0);
  EXPECT_DOUBLE_EQ(test.browser->scroll(), 0.0);

  test.browser->set_scroll(9000.0);
  EXPECT_DOUBLE_EQ(test.browser->scroll(), test.browser->content_height() - 40.0);
}

TEST(EffectsBrowser, AListThatCannotScrollDoesNotSwallowTheWheel) {
  // Otherwise the panel behind it stops scrolling for no visible reason.
  Listed test;
  EXPECT_FALSE(test.host->wheel(WheelEvent{.x = 40.0, .y = 10.0, .delta_y = 1.0}));
}

TEST(EffectsBrowser, PaintsOnlyTheRowsOnScreen) {
  Listed test;
  test.browser->arrange(Rect{0.0, 0.0, 260.0, 30.0}, flat_context());

  RecordingPainter painter;
  test.browser->paint(painter, default_theme());
  EXPECT_TRUE(painter.clips_balanced());

  std::size_t texts = 0;
  for (const DrawCall& call : painter.calls()) {
    if (call.run.has_value()) ++texts;
  }
  EXPECT_LT(texts, test.browser->rows().size());
}


// ------------------------------------------------------------------ nesting --
//
// A folder is a path, and the tree is made out of the paths rather than
// declared anywhere. Twenty effects in five categories under one flat heading
// each is a list nobody can scan.

[[nodiscard]] std::vector<EffectEntry> nested_items() {
  return {
      EffectEntry{.id = "v:bright", .name = "Brightness", .folder = "Video Effects/Colour"},
      EffectEntry{.id = "v:blur", .name = "Blur", .folder = "Video Effects/Blur"},
      EffectEntry{.id = "a:eq", .name = "EQ", .folder = "Audio Effects"},
  };
}

TEST(EffectsBrowserNesting, AParentIsDrawnOnceForAllItsChildren) {
  EffectsBrowser browser;
  browser.set_items(nested_items());

  // Closed, the tree is its top level and nothing else.
  ASSERT_EQ(browser.rows().size(), 2u);
  EXPECT_EQ(browser.rows()[0].name, "Video Effects");
  EXPECT_EQ(browser.rows()[0].depth, 0u);
  EXPECT_EQ(browser.rows()[1].name, "Audio Effects");
}

TEST(EffectsBrowserNesting, OpeningAParentShowsItsFoldersRatherThanItsEffects) {
  EffectsBrowser browser;
  browser.set_items(nested_items());
  browser.set_open("Video Effects", true);

  ASSERT_EQ(browser.rows().size(), 4u);
  EXPECT_EQ(browser.rows()[1].name, "Colour");
  EXPECT_EQ(browser.rows()[1].depth, 1u);
  EXPECT_TRUE(browser.rows()[1].is_folder);
  EXPECT_EQ(browser.rows()[2].name, "Blur");
}

TEST(EffectsBrowserNesting, OpeningAChildShowsWhatIsInIt) {
  EffectsBrowser browser;
  browser.set_items(nested_items());
  browser.set_open("Video Effects", true);
  browser.set_open("Video Effects/Colour", true);

  const std::vector<EffectsBrowser::Row>& rows = browser.rows();
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[2].id, "v:bright");
  EXPECT_EQ(rows[2].depth, 2u) << "indented under its own folder";
}

TEST(EffectsBrowserNesting, AClosedParentHidesEverythingBelowIt) {
  // Even a child that has been opened. Closed at any level means closed, which
  // is what a tree is.
  EffectsBrowser browser;
  browser.set_items(nested_items());
  browser.set_open("Video Effects/Colour", true);

  EXPECT_EQ(browser.rows().size(), 2u);
}

TEST(EffectsBrowserNesting, TwoFoldersOfTheSameNameUnderDifferentParentsStayApart) {
  EffectsBrowser browser;
  browser.set_items({
      EffectEntry{.id = "v:one", .name = "One", .folder = "Video Effects/Colour"},
      EffectEntry{.id = "a:two", .name = "Two", .folder = "Audio Effects/Colour"},
  });
  browser.set_open("Video Effects", true);
  browser.set_open("Video Effects/Colour", true);

  const std::vector<EffectsBrowser::Row>& rows = browser.rows();
  // The video Colour is open and the audio one is not, which could not be true
  // if the two were keyed by name.
  ASSERT_GE(rows.size(), 4u);
  EXPECT_EQ(rows[2].id, "v:one");
  EXPECT_EQ(rows[3].name, "Audio Effects");
}

TEST(EffectsBrowserNesting, ASearchOpensTheWholeTree) {
  EffectsBrowser browser;
  browser.set_items(nested_items());
  browser.set_filter("bright");

  const std::vector<EffectsBrowser::Row>& rows = browser.rows();
  ASSERT_EQ(rows.size(), 3u) << "the two folders above it, and the effect";
  EXPECT_EQ(rows.back().id, "v:bright");
}

TEST(EffectsBrowserNesting, AStraySeparatorDoesNotMakeANamelessFolder) {
  EffectsBrowser browser;
  browser.set_items({EffectEntry{.id = "x", .name = "X", .folder = "/Video Effects//Colour/"}});
  browser.set_open("Video Effects", true);

  const std::vector<EffectsBrowser::Row>& rows = browser.rows();
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].name, "Video Effects");
  EXPECT_EQ(rows[1].name, "Colour");
}

}  // namespace
}  // namespace cutline::ui
