/// The media pool, turned into rows.
///
/// The seam is where the mistakes are: what kind of thing an entry is, what its
/// right-hand column should say, and — the one with teeth — that removing an
/// entry takes the clips that used it with it, because a sequence naming media
/// that is gone cannot be repaired.

#include "cutline/editor/browser_binding.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/model.hpp"
#include "cutline/core/pool.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/time.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace cutline::editor {
namespace {

[[nodiscard]] core::Media clip_media(std::string id, std::string name, double duration = 8.0) {
  core::Media media;
  media.id = std::move(id);
  media.name = std::move(name);
  media.path = "D:/footage/" + media.name;
  media.duration = duration;
  media.has_video = true;
  media.audio_stream_count = 1;
  return media;
}

/// Two video clips and a still in the pool, with the first one placed twice.
[[nodiscard]] core::Project sample_project() {
  core::Project project = core::empty_project(1, 1);
  project.fps = 30.0;

  project.media.push_back(clip_media("m1", "Boiler.mp4", 12.0));
  project.media.push_back(clip_media("m2", "Beach.mov", 4.0));

  core::Media still;
  still.id = "m3";
  still.name = "Logo.png";
  still.path = "D:/art/Logo.png";
  still.duration = 5.0;
  still.has_video = true;
  still.is_image = true;
  project.media.push_back(still);

  project = core::place_media(std::move(project), "m1", 0.0);
  project = core::place_media(std::move(project), "m1", 20.0);
  return project;
}

// -------------------------------------------------------------------- kind --

TEST(BrowserBinding, VideoAndAudioAreToldApartByWhetherThereIsAPicture) {
  core::Media video = clip_media("a", "a.mp4");
  EXPECT_EQ(media_kind(video), ui::MediaKind::Video);

  core::Media audio = clip_media("b", "b.wav");
  audio.has_video = false;
  EXPECT_EQ(media_kind(audio), ui::MediaKind::Audio);
}

TEST(BrowserBinding, GeneratedMediaBeatsTheStillFlag) {
  // A title and a colour matte are both stills as far as the model is
  // concerned. Testing the flags in the other order would call them images.
  core::Media title;
  title.is_image = true;
  title.is_text = true;
  EXPECT_EQ(media_kind(title), ui::MediaKind::Title);

  core::Media matte;
  matte.is_image = true;
  matte.is_color = true;
  EXPECT_EQ(media_kind(matte), ui::MediaKind::Color);

  core::Media adjustment;
  adjustment.is_adjustment = true;
  EXPECT_EQ(media_kind(adjustment), ui::MediaKind::Adjustment);
}

// ------------------------------------------------------------------ detail --

TEST(BrowserBinding, ADurationIsShownAsTimecode) {
  const core::Media media = clip_media("a", "a.mp4", 12.0);
  EXPECT_EQ(media_detail(media, 30.0), core::seconds_to_timecode(12.0, 30.0));
}

TEST(BrowserBinding, AStillSaysSoRatherThanShowingAMadeUpLength) {
  // Its duration is the length it would be placed at, not a running time, and
  // showing that as timecode would claim the file is five seconds long.
  core::Media still;
  still.is_image = true;
  still.duration = 5.0;
  EXPECT_EQ(media_detail(still, 30.0), "still");
}

TEST(BrowserBinding, AnAnimatedStillHasARealRunningTime) {
  core::Media gif;
  gif.is_image = true;
  gif.is_animated = true;
  gif.duration = 2.0;
  EXPECT_EQ(media_detail(gif, 30.0), core::seconds_to_timecode(2.0, 30.0));
}

TEST(BrowserBinding, MediaWithNoDurationAtAllSaysNothing) {
  const core::Media media;
  EXPECT_TRUE(media_detail(media, 30.0).empty());
}

// ------------------------------------------------------------------- uses --

TEST(BrowserBinding, UsesCountsEveryClipOfTheMedia) {
  const core::Project project = sample_project();
  // Placed twice, and each placement made a video clip and an audio clip.
  EXPECT_EQ(media_uses(project, "m1"), 4);
  EXPECT_EQ(media_uses(project, "m2"), 0);
  EXPECT_EQ(media_uses(project, "nowhere"), 0);
  EXPECT_EQ(media_uses(project, ""), 0);
}

// ------------------------------------------------------------------- rows --

TEST(BrowserBinding, EveryEntryBecomesARowInPoolOrder) {
  const std::vector<ui::MediaItem> items = browser_items(sample_project());

  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0].id, "m1");
  EXPECT_EQ(items[0].name, "Boiler.mp4");
  EXPECT_EQ(items[1].id, "m2");
  EXPECT_EQ(items[2].kind, ui::MediaKind::Image);
  EXPECT_EQ(items[0].uses, 4);
  EXPECT_DOUBLE_EQ(items[0].duration, 12.0);
}

TEST(BrowserBinding, ANamelessEntryFallsBackToItsPath) {
  // A row with no text at all looks like a fault, and a path is enough to
  // import a file with.
  core::Project project = core::empty_project(1, 1);
  core::Media media;
  media.id = "m";
  media.path = "D:/footage/unnamed.mp4";
  project.media.push_back(media);

  const std::vector<ui::MediaItem> items = browser_items(project);
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].name, "D:/footage/unnamed.mp4");
}

TEST(BrowserBinding, MissingPathsComeBackMarkedOffline) {
  const std::vector<std::string> missing{"D:/footage/Beach.mov"};
  const BrowserOptions options{.offline = missing};

  const std::vector<ui::MediaItem> items = browser_items(sample_project(), options);

  EXPECT_FALSE(items[0].offline);
  EXPECT_TRUE(items[1].offline);
}

TEST(BrowserBinding, GeneratedMediaIsNeverOffline) {
  // It has no path, so an empty entry in the missing list must not match it.
  core::Project project = core::empty_project(1, 1);
  core::Media title;
  title.id = "t";
  title.name = "Opening";
  title.is_text = true;
  project.media.push_back(title);

  const std::vector<std::string> missing{""};
  const std::vector<ui::MediaItem> items = browser_items(project, {.offline = missing});

  ASSERT_EQ(items.size(), 1u);
  EXPECT_FALSE(items[0].offline);
}

// ---------------------------------------------------------------- searching --

TEST(BrowserBinding, SearchIsCaseInsensitiveAndMatchesTheName) {
  const std::vector<ui::MediaItem> items = browser_items(sample_project(), {.search = "boiler"});
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].id, "m1");
}

TEST(BrowserBinding, EveryTermHasToMatchAndTheirOrderDoesNot) {
  const core::Project project = sample_project();
  EXPECT_EQ(browser_items(project, {.search = "boiler mp4"}).size(), 1u);
  EXPECT_EQ(browser_items(project, {.search = "mp4 boiler"}).size(), 1u);
  EXPECT_EQ(browser_items(project, {.search = "boiler png"}).size(), 0u);
}

TEST(BrowserBinding, SearchLooksAtThePathAsWellAsTheName) {
  const std::vector<ui::MediaItem> items = browser_items(sample_project(), {.search = "art"});
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].id, "m3");
}

TEST(BrowserBinding, AnEmptyOrBlankSearchKeepsEverything) {
  const core::Project project = sample_project();
  EXPECT_EQ(browser_items(project, {.search = ""}).size(), 3u);
  EXPECT_EQ(browser_items(project, {.search = "   "}).size(), 3u);
}

// ----------------------------------------------------------------- sorting --

TEST(BrowserBinding, SortingByNameIgnoresCase) {
  // Otherwise every capitalised name sorts before every lower-case one, which
  // is not how anyone reads a list.
  core::Project project = core::empty_project(1, 1);
  project.media.push_back(clip_media("a", "apple.mp4"));
  project.media.push_back(clip_media("b", "Banana.mp4"));
  project.media.push_back(clip_media("c", "cherry.mp4"));

  const std::vector<ui::MediaItem> items = browser_items(project, {.sort = BrowserSort::Name});

  EXPECT_EQ(items[0].name, "apple.mp4");
  EXPECT_EQ(items[1].name, "Banana.mp4");
  EXPECT_EQ(items[2].name, "cherry.mp4");
}

TEST(BrowserBinding, SortingByDurationPutsTheShortestFirst) {
  const std::vector<ui::MediaItem> items =
      browser_items(sample_project(), {.sort = BrowserSort::Duration});
  EXPECT_EQ(items[0].id, "m2");  // 4s
  EXPECT_EQ(items[1].id, "m3");  // 5s
  EXPECT_EQ(items[2].id, "m1");  // 12s
}

TEST(BrowserBinding, SortingByUsesFindsWhatIsUnused) {
  const std::vector<ui::MediaItem> items =
      browser_items(sample_project(), {.sort = BrowserSort::Uses});
  EXPECT_EQ(items[0].uses, 0);
  EXPECT_EQ(items.back().id, "m1");
}

TEST(BrowserBinding, DescendingReversesWhateverTheOrderWas) {
  const std::vector<ui::MediaItem> items =
      browser_items(sample_project(), {.sort = BrowserSort::Pool, .descending = true});
  EXPECT_EQ(items[0].id, "m3");
  EXPECT_EQ(items[2].id, "m1");
}

TEST(BrowserBinding, EntriesThatCompareEqualKeepTheirPoolOrder) {
  // Otherwise the list reshuffles itself every time it is rebuilt, which is
  // once per keystroke while searching.
  core::Project project = core::empty_project(1, 1);
  project.media.push_back(clip_media("first", "a.mp4", 3.0));
  project.media.push_back(clip_media("second", "b.mp4", 3.0));
  project.media.push_back(clip_media("third", "c.mp4", 3.0));

  const std::vector<ui::MediaItem> items = browser_items(project, {.sort = BrowserSort::Duration});

  EXPECT_EQ(items[0].id, "first");
  EXPECT_EQ(items[1].id, "second");
  EXPECT_EQ(items[2].id, "third");
}

// ---------------------------------------------------------------- removing --

TEST(BrowserBinding, RemovingAnEntryTakesItsClipsWithIt) {
  const core::Project before = sample_project();
  ASSERT_EQ(media_uses(before, "m1"), 4);

  const core::Project after = remove_media(before, "m1");

  EXPECT_EQ(media_uses(after, "m1"), 0);
  EXPECT_EQ(after.media.size(), 2u);
  for (const core::Media& media : after.media) EXPECT_NE(media.id, "m1");
}

TEST(BrowserBinding, RemovingLeavesEveryOtherClipAlone) {
  core::Project project = sample_project();
  project = core::place_media(std::move(project), "m2", 40.0);
  const int others = media_uses(project, "m2");
  ASSERT_GT(others, 0);

  const core::Project after = remove_media(project, "m1");
  EXPECT_EQ(media_uses(after, "m2"), others);
}

TEST(BrowserBinding, RemovingSomethingThatIsNotThereChangesNothing) {
  const core::Project before = sample_project();
  EXPECT_EQ(remove_media(before, "nowhere"), before);
  EXPECT_EQ(remove_media(before, ""), before);
}

// ---------------------------------------------------------------- renaming --

TEST(BrowserBinding, RenamingChangesTheNameAndNotThePath) {
  const core::Project after = rename_media(sample_project(), "m1", "Opening shot");

  ASSERT_FALSE(after.media.empty());
  EXPECT_EQ(after.media[0].name, "Opening shot");
  EXPECT_EQ(after.media[0].path, "D:/footage/Boiler.mp4");
}

TEST(BrowserBinding, AnEmptyNameIsRefused) {
  // A nameless entry would show its path instead, which reads as the rename
  // having gone wrong rather than as having been ignored.
  const core::Project before = sample_project();
  EXPECT_EQ(rename_media(before, "m1", ""), before);
}

TEST(BrowserBinding, RenamingSomethingThatIsNotThereChangesNothing) {
  const core::Project before = sample_project();
  EXPECT_EQ(rename_media(before, "nowhere", "Whatever"), before);
}

// ------------------------------------------------------------------ bins --

namespace {

/// Every row's name, which is what a tree is easiest to assert against.
[[nodiscard]] std::vector<std::string> names_of(const std::vector<ui::MediaItem>& rows) {
  std::vector<std::string> out;
  for (const ui::MediaItem& row : rows) out.push_back(row.name);
  return out;
}

/// The sample project with `m1` filed in a bin called Interviews.
[[nodiscard]] core::Project filed_project(std::string& bin_out) {
  core::Project project = core::create_bin(sample_project(), "Interviews");
  bin_out = project.bins.back().id;
  return core::file_media(std::move(project), "m1", bin_out);
}

}  // namespace

TEST(BrowserBinding, AClosedBinShowsNothingInside) {
  std::string bin;
  const core::Project project = filed_project(bin);

  const std::vector<ui::MediaItem> rows = browser_items(project);
  const std::vector<std::string> names = names_of(rows);
  EXPECT_NE(std::ranges::find(names, "Interviews"), names.end());
  EXPECT_EQ(std::ranges::find(names, "Boiler.mp4"), names.end())
      << "a closed bin showed what was inside it";

  ASSERT_FALSE(rows.empty());
  EXPECT_TRUE(rows.front().is_bin);
  EXPECT_FALSE(rows.front().expanded);
  EXPECT_EQ(rows.front().detail, "1 item");
}

TEST(BrowserBinding, AnOpenBinShowsItsContentsIndentedBeneathIt) {
  std::string bin;
  const core::Project project = filed_project(bin);

  const std::string open[] = {bin};
  const std::vector<ui::MediaItem> rows = browser_items(project, {.expanded = open});

  ASSERT_GE(rows.size(), 2u);
  EXPECT_TRUE(rows[0].is_bin);
  EXPECT_TRUE(rows[0].expanded);
  EXPECT_EQ(rows[1].name, "Boiler.mp4");
  EXPECT_EQ(rows[1].depth, 1) << "what is in a bin was not indented under it";
  EXPECT_EQ(rows[0].depth, 0);
}

TEST(BrowserBinding, BinsComeBeforeMediaAtEveryLevel) {
  // Where Premiere puts them, and what stops the folders of a large pool being
  // scattered down a list of files.
  std::string bin;
  const core::Project project = filed_project(bin);

  const std::vector<ui::MediaItem> rows = browser_items(project);
  ASSERT_FALSE(rows.empty());
  EXPECT_TRUE(rows.front().is_bin);
  for (std::size_t i = 1; i < rows.size(); ++i) {
    EXPECT_FALSE(rows[i].is_bin) << "a bin appeared after media at row " << i;
  }
}

TEST(BrowserBinding, ABinRowIsToldApartFromMediaByItsId) {
  // Both kinds share one list, so their ids share one namespace. Without a
  // prefix "the selected row" would be ambiguous the moment a bin and a media
  // entry were given the same id by two different id counters.
  std::string bin;
  const core::Project project = filed_project(bin);

  const std::vector<ui::MediaItem> rows = browser_items(project);
  ASSERT_FALSE(rows.empty());
  EXPECT_EQ(bin_of_row(rows.front().id), bin);
  EXPECT_TRUE(bin_of_row("m1").empty()) << "a media id read as a bin";
  EXPECT_EQ(bin_of_row(bin_row_id("bin_7")), "bin_7");
}

TEST(BrowserBinding, SearchingFlattensTheTree) {
  // A match hidden inside a closed bin is the one thing a search must not do.
  std::string bin;
  const core::Project project = filed_project(bin);

  const std::vector<ui::MediaItem> rows = browser_items(project, {.search = "boiler"});
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().name, "Boiler.mp4");
  EXPECT_EQ(rows.front().depth, 0) << "a search result was indented under a bin it did not show";
  EXPECT_FALSE(rows.front().is_bin);
}

TEST(BrowserBinding, SortingHappensInsideABinRatherThanAcrossThePool) {
  // Sorting across bins would take entries out of the folders they were put in,
  // which is the one thing filing them was for.
  std::string bin;
  core::Project project = filed_project(bin);
  project = core::file_media(std::move(project), "m2", bin);

  const std::string open[] = {bin};
  const std::vector<ui::MediaItem> rows =
      browser_items(project, {.sort = BrowserSort::Name, .expanded = open});

  const std::vector<std::string> names = names_of(rows);
  ASSERT_GE(names.size(), 3u);
  EXPECT_EQ(names[0], "Interviews");
  EXPECT_EQ(names[1], "Beach.mov") << "the bin's contents were not ordered by name";
  EXPECT_EQ(names[2], "Boiler.mp4");
  // And the still, which is outside the bin, is still after everything in it.
  EXPECT_EQ(names.back(), "Logo.png");
}

TEST(BrowserBinding, MediaNamingABinThatHasGoneStillShows) {
  // The reason bins hold nothing: a folder that vanished cannot take footage
  // out of the panel with it.
  std::string bin;
  core::Project project = filed_project(bin);
  project = core::remove_bins(std::move(project), bin);

  const std::vector<std::string> names = names_of(browser_items(project));
  EXPECT_NE(std::ranges::find(names, "Boiler.mp4"), names.end())
      << "media was lost with the bin that held it";
}

TEST(BrowserBinding, DeletingABinTakesItsMediaAndTheClipsThatUsedIt) {
  // The destructive one, and Premiere's behaviour. Both halves or neither: a
  // sequence naming media that is gone cannot be repaired.
  std::string bin;
  core::Project project = filed_project(bin);
  ASSERT_GT(media_uses(project, "m1"), 0) << "the fixture stopped placing m1";

  const core::Project after = remove_bin(std::move(project), bin);
  EXPECT_TRUE(after.bins.empty());
  EXPECT_EQ(std::ranges::find(after.media, "m1", &core::Media::id), after.media.end());
  EXPECT_EQ(media_uses(after, "m1"), 0) << "clips were left naming media that is gone";
  // And nothing outside the bin went with it.
  EXPECT_NE(std::ranges::find(after.media, "m2", &core::Media::id), after.media.end());
}

TEST(BrowserBinding, DeletingABinReachesTheBinsInsideIt) {
  std::string bin;
  core::Project project = filed_project(bin);
  project = core::create_bin(std::move(project), "Cameras", bin);
  const std::string inner = project.bins.back().id;
  project = core::file_media(std::move(project), "m2", inner);

  const core::Project after = remove_bin(std::move(project), bin);
  EXPECT_TRUE(after.bins.empty());
  EXPECT_EQ(std::ranges::find(after.media, "m2", &core::Media::id), after.media.end())
      << "media inside a nested bin survived its grandparent being deleted";
}

}  // namespace
}  // namespace cutline::editor
