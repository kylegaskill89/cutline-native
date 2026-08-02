/// User bins: folders you make yourself, holding ids rather than copies.
///
/// The tests that matter here are the ones about what a bin *is not*: it does
/// not own what is in it, so a stale id is a display question rather than a data
/// loss, and an empty one still has to be visible.

#include "cutline/editor/bins.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::editor {
namespace {

[[nodiscard]] std::vector<LibraryEntry> catalogue() {
  return {LibraryEntry{.id = "video:blur", .name = "Blur", .folder = "Video Effects/Blur"},
          LibraryEntry{.id = "audio:lowpass", .name = "Low Pass", .folder = "Audio Effects"},
          LibraryEntry{.id = "preset:Warm", .name = "Warm", .folder = "Presets"}};
}

[[nodiscard]] Bins one_bin() {
  Bins bins;
  create_bin(bins, "Favourites");
  return bins;
}

// -------------------------------------------------------------- the bins --

TEST(Bins, CreatingRefusesWhatCannotBeShown) {
  Bins bins;
  EXPECT_TRUE(create_bin(bins, "Favourites"));

  EXPECT_FALSE(create_bin(bins, ""));
  // Two by one name are indistinguishable once the panel has drawn them, and
  // the second would look like the first had lost its contents.
  EXPECT_FALSE(create_bin(bins, "Favourites"));
  // A separator would split into two folders, and nothing could delete the
  // outer one because it would belong to no bin.
  EXPECT_FALSE(create_bin(bins, "Mine/Colour"));

  EXPECT_EQ(bins.named.size(), 1u);
}

TEST(Bins, RemovingSaysWhetherItWasThere) {
  Bins bins = one_bin();
  EXPECT_TRUE(remove_bin(bins, "Favourites"));
  EXPECT_FALSE(remove_bin(bins, "Favourites"));
  EXPECT_TRUE(bins.named.empty());
}

TEST(Bins, RenamingKeepsItsPlaceAndItsContents) {
  Bins bins;
  ASSERT_TRUE(create_bin(bins, "First"));
  ASSERT_TRUE(create_bin(bins, "Second"));
  ASSERT_TRUE(add_to_bin(bins, "First", "video:blur"));

  ASSERT_TRUE(rename_bin(bins, "First", "Colour"));
  EXPECT_EQ(bins.named[0].name, "Colour");
  EXPECT_EQ(bins.named[0].ids, std::vector<std::string>{"video:blur"});

  EXPECT_FALSE(rename_bin(bins, "Colour", "Second"));
  EXPECT_FALSE(rename_bin(bins, "Nothing", "Anything"));
  // A dialog dismissed with the text untouched is not an error.
  EXPECT_TRUE(rename_bin(bins, "Colour", "Colour"));
}

// ----------------------------------------------------------- the contents --

TEST(Bins, GatheringRefusesTheSameThingTwice) {
  Bins bins = one_bin();
  EXPECT_TRUE(add_to_bin(bins, "Favourites", "video:blur"));
  EXPECT_FALSE(add_to_bin(bins, "Favourites", "video:blur"));
  EXPECT_FALSE(add_to_bin(bins, "Nothing", "video:blur"));
  EXPECT_EQ(bins.named[0].ids.size(), 1u);
}

TEST(Bins, TheSameThingCanBeInTwoBins) {
  // Expected, rather than merely allowed: a blur belongs in Favourites and in
  // Titles at once, and that is what gathering by hand is for.
  Bins bins;
  ASSERT_TRUE(create_bin(bins, "Favourites"));
  ASSERT_TRUE(create_bin(bins, "Titles"));
  EXPECT_TRUE(add_to_bin(bins, "Favourites", "video:blur"));
  EXPECT_TRUE(add_to_bin(bins, "Titles", "video:blur"));
}

TEST(Bins, TakingOneOutLeavesTheRest) {
  Bins bins = one_bin();
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "video:blur"));
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "preset:Warm"));

  EXPECT_TRUE(remove_from_bin(bins, "Favourites", "video:blur"));
  EXPECT_FALSE(remove_from_bin(bins, "Favourites", "video:blur"));
  EXPECT_EQ(bins.named[0].ids, std::vector<std::string>{"preset:Warm"});
}

TEST(Bins, ReorderingLandsWhereItWasDropped) {
  Bins bins = one_bin();
  for (const char* id : {"a", "b", "c"}) ASSERT_TRUE(add_to_bin(bins, "Favourites", id));

  ASSERT_TRUE(move_in_bin(bins, "Favourites", 0, 2));
  EXPECT_EQ(bins.named[0].ids, (std::vector<std::string>{"b", "c", "a"}));

  ASSERT_TRUE(move_in_bin(bins, "Favourites", 2, 0));
  EXPECT_EQ(bins.named[0].ids, (std::vector<std::string>{"a", "b", "c"}));

  // Past the end lands at the end rather than doing nothing.
  ASSERT_TRUE(move_in_bin(bins, "Favourites", 0, 99));
  EXPECT_EQ(bins.named[0].ids, (std::vector<std::string>{"b", "c", "a"}));

  EXPECT_FALSE(move_in_bin(bins, "Favourites", 1, 1));
  EXPECT_FALSE(move_in_bin(bins, "Favourites", 9, 0));
  EXPECT_FALSE(move_in_bin(bins, "Nothing", 0, 1));
}

// -------------------------------------------------------------- the rows --

TEST(Bins, EntriesTakeTheirNamesFromTheCatalogue) {
  // So a renamed preset is renamed in the bin too, which is what holding an id
  // rather than a copy buys.
  Bins bins = one_bin();
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "preset:Warm"));

  const std::vector<LibraryEntry> rows = bin_entries(bins, catalogue());
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].id, "preset:Warm");
  EXPECT_EQ(rows[0].name, "Warm");
  EXPECT_EQ(rows[0].folder, "Bins/Favourites");
}

TEST(Bins, EntriesKeepTheOrderTheyWereGatheredIn) {
  Bins bins = one_bin();
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "preset:Warm"));
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "video:blur"));

  const std::vector<LibraryEntry> rows = bin_entries(bins, catalogue());
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].id, "preset:Warm");
  EXPECT_EQ(rows[1].id, "video:blur");
}

TEST(Bins, AStaleIdIsLeftOutOfTheListAndLeftInTheBin) {
  // A deleted preset, or an effect gone from a later build. Dropping it from the
  // data instead would mean a build that temporarily lacked an effect silently
  // emptied somebody's bins.
  Bins bins = one_bin();
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "video:nonesuch"));
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "video:blur"));

  const std::vector<LibraryEntry> rows = bin_entries(bins, catalogue());
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].id, "video:blur");
  EXPECT_EQ(bins.named[0].ids.size(), 2u);
}

TEST(Bins, AnEmptyBinStillHasAFolder) {
  // A tree built out of paths cannot show a folder with nothing under it, and a
  // bin you have just made and cannot see looks like one that failed to be made.
  const Bins bins = one_bin();
  EXPECT_TRUE(bin_entries(bins, catalogue()).empty());
  EXPECT_EQ(bin_folders(bins), std::vector<std::string>{"Bins/Favourites"});
}

TEST(Bins, AFolderPathNamesItsBinBackAgain) {
  EXPECT_EQ(bin_of_folder("Bins/Favourites"), "Favourites");
  EXPECT_TRUE(bin_of_folder("Bins").empty());
  EXPECT_TRUE(bin_of_folder("Video Effects/Blur").empty());
  // One level down only: anything deeper is inside something else.
  EXPECT_TRUE(bin_of_folder("Bins/Favourites/More").empty());
}

// ------------------------------------------------------------ persistence --

TEST(Bins, RoundTripThroughItsFile) {
  Bins bins;
  ASSERT_TRUE(create_bin(bins, "Favourites"));
  ASSERT_TRUE(create_bin(bins, "Empty"));
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "video:blur"));
  ASSERT_TRUE(add_to_bin(bins, "Favourites", "preset:Warm"));

  const auto read = bins_from_json(to_json(bins));
  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_EQ(*read, bins);
}

TEST(Bins, AMissingOrEmptyFileIsNotAnError) {
  const auto read = bins_from_json(R"({"version": 1})");
  ASSERT_TRUE(read.has_value()) << read.error();
  EXPECT_TRUE(read->named.empty());
}

TEST(Bins, ANewerFileIsRefusedRatherThanHalfRead) {
  const auto read = bins_from_json(R"({"version": 9999, "bins": []})");
  ASSERT_FALSE(read.has_value());
  EXPECT_NE(read.error().find("newer version"), std::string::npos);
}

TEST(Bins, RubbishIsRefused) {
  EXPECT_FALSE(bins_from_json("not json").has_value());
  EXPECT_FALSE(bins_from_json("[1, 2, 3]").has_value());
}

TEST(Bins, WhatCannotBeShownIsDroppedOnReading) {
  const auto read = bins_from_json(R"({
    "version": 1,
    "bins": [
      {"name": "", "ids": ["video:blur"]},
      {"name": "Bad/Name", "ids": []},
      {"name": "Good", "ids": ["video:blur", "video:blur", "", 7]},
      {"name": "Good", "ids": ["audio:lowpass"]}
    ]
  })");

  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_EQ(read->named.size(), 1u);
  EXPECT_EQ(read->named[0].name, "Good");
  // Duplicates and rubbish out; the first "Good" wins, so a second one cannot
  // hide it.
  EXPECT_EQ(read->named[0].ids, std::vector<std::string>{"video:blur"});
}

TEST(Bins, AnEmptyBinSurvivesTheFile) {
  // Unlike an empty preset, which can do nothing: this is a folder somebody made
  // and has not filled yet, and losing it on the next start is forgetting.
  const auto read = bins_from_json(R"({"version": 1, "bins": [{"name": "Later"}]})");
  ASSERT_TRUE(read.has_value()) << read.error();
  ASSERT_EQ(read->named.size(), 1u);
  EXPECT_TRUE(read->named[0].ids.empty());
}

TEST(Bins, FindsOneByName) {
  const Bins bins = one_bin();
  ASSERT_NE(find_bin(bins, "Favourites"), nullptr);
  EXPECT_EQ(find_bin(bins, "Nothing"), nullptr);
}

}  // namespace
}  // namespace cutline::editor
