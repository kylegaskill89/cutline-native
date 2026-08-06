/// Bins: folders in the project panel, and what filing something means.
///
/// The tests worth having here are about what a bin *is not*. It does not own
/// what is in it, so a bin that goes away cannot take media with it; and it is a
/// tree only by agreement between parents, so the interesting failures are the
/// ones where that agreement makes a ring instead.

#include "cutline/core/pool.hpp"

#include "cutline/core/id.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

[[nodiscard]] Project pool_of(int count) {
  Project p;
  for (int i = 0; i < count; ++i) {
    Media m;
    m.id = "f" + std::to_string(i + 1);
    m.name = "Clip " + std::to_string(i + 1);
    m.duration = 10.0;
    p.media.push_back(m);
  }
  return p;
}

/// The id of the bin most recently made, which `create_bin` appends.
[[nodiscard]] std::string last_bin(const Project& p) {
  return p.bins.empty() ? std::string{} : p.bins.back().id;
}

// ---------------------------------------------------------------- the tree --

TEST(MediaBins, AMadeBinCanBeFoundAtOnce) {
  // Appended rather than inserted anywhere clever, because whatever made it
  // usually wants to select it or rename it next.
  Project p = create_bin(Project{}, "Interviews");
  ASSERT_EQ(p.bins.size(), 1u);
  EXPECT_EQ(p.bins.back().name, "Interviews");
  EXPECT_TRUE(p.bins.back().parent.empty());
  EXPECT_EQ(find_bin(p, last_bin(p)), &p.bins.back());
}

TEST(MediaBins, TheTopLevelIsNotABin) {
  // Asking for it by an empty id has to answer "there is no such bin" rather
  // than finding whichever one happens to have an empty id.
  const Project p = create_bin(Project{}, "Interviews");
  EXPECT_EQ(find_bin(p, ""), nullptr);
}

TEST(MediaBins, NestingIsRecordedOnTheChild) {
  Project p = create_bin(Project{}, "Day 1");
  const std::string outer = last_bin(p);
  p = create_bin(std::move(p), "Cameras", outer);
  const std::string inner = last_bin(p);

  EXPECT_EQ(find_bin(p, inner)->parent, outer);
  EXPECT_EQ(bin_depth(p, outer), 0);
  EXPECT_EQ(bin_depth(p, inner), 1);
}

TEST(MediaBins, AParentThatIsNotThereLeavesItAtTheTop) {
  // Rather than refusing to make it. The tree is a way of arranging things, and
  // a button that silently does nothing is worse than one that does the plain
  // thing.
  const Project p = create_bin(Project{}, "Cameras", "bin_gone");
  ASSERT_EQ(p.bins.size(), 1u);
  EXPECT_TRUE(p.bins.back().parent.empty());
}

TEST(MediaBins, ABinCannotBeMovedInsideItself) {
  Project p = create_bin(Project{}, "Day 1");
  const std::string self = last_bin(p);
  p = move_bin(std::move(p), self, self);
  EXPECT_TRUE(find_bin(p, self)->parent.empty()) << "a bin became its own parent";
}

TEST(MediaBins, ABinCannotBeMovedInsideOneOfItsOwnChildren) {
  // The case that makes a ring rather than a bad-looking tree, and a walk that
  // does not end.
  Project p = create_bin(Project{}, "Day 1");
  const std::string outer = last_bin(p);
  p = create_bin(std::move(p), "Cameras", outer);
  const std::string inner = last_bin(p);

  p = move_bin(std::move(p), outer, inner);
  EXPECT_TRUE(find_bin(p, outer)->parent.empty()) << "the tree closed into a ring";
  EXPECT_EQ(find_bin(p, inner)->parent, outer);
}

TEST(MediaBins, AnEmptyParentMovesABinBackToTheTop) {
  Project p = create_bin(Project{}, "Day 1");
  const std::string outer = last_bin(p);
  p = create_bin(std::move(p), "Cameras", outer);
  const std::string inner = last_bin(p);

  p = move_bin(std::move(p), inner, "");
  EXPECT_TRUE(find_bin(p, inner)->parent.empty());
  EXPECT_EQ(bin_depth(p, inner), 0);
}

TEST(MediaBins, ContainmentReachesAllTheWayDown) {
  Project p = create_bin(Project{}, "A");
  const std::string a = last_bin(p);
  p = create_bin(std::move(p), "B", a);
  const std::string b = last_bin(p);
  p = create_bin(std::move(p), "C", b);
  const std::string c = last_bin(p);

  EXPECT_TRUE(bin_contains(p, a, c)) << "a grandchild was not found inside";
  EXPECT_TRUE(bin_contains(p, a, a)) << "a bin has to contain itself, or moves are allowed";
  EXPECT_FALSE(bin_contains(p, c, a));
  EXPECT_FALSE(bin_contains(p, "", a)) << "the top level is not a bin and contains nothing";
}

TEST(MediaBins, ARingInAFileDoesNotHangTheWalk) {
  // Not reachable through these operations, and entirely reachable by editing a
  // project file by hand. The guard is a bound rather than a check, because
  // there is nothing useful to answer — only something not to do for ever.
  Project p;
  p.bins = {Bin{.id = "a", .name = "A", .parent = "b"},
            Bin{.id = "b", .name = "B", .parent = "a"}};
  EXPECT_FALSE(bin_contains(p, "c", "a"));
  EXPECT_LE(bin_depth(p, "a"), static_cast<int>(p.bins.size()) + 1);
}

// ------------------------------------------------------------- what is in --

TEST(MediaBins, FilingMediaIsOneFieldOnTheEntry) {
  Project p = pool_of(2);
  p = create_bin(std::move(p), "Interviews");
  const std::string bin = last_bin(p);

  p = file_media(std::move(p), "f1", bin);
  EXPECT_EQ(p.media[0].bin, bin);
  EXPECT_TRUE(p.media[1].bin.empty());

  EXPECT_EQ(media_in_bin(p, bin), std::vector<std::string>{"f1"});
  EXPECT_EQ(media_in_bin(p, ""), std::vector<std::string>{"f2"});
}

TEST(MediaBins, FilingIntoABinThatIsNotThereMeansTheTopLevel) {
  Project p = pool_of(1);
  p = file_media(std::move(p), "f1", "bin_gone");
  EXPECT_TRUE(p.media[0].bin.empty());
}

TEST(MediaBins, RemovingABinLeavesItsMediaInThePool) {
  // The whole point of a bin holding nothing: losing the folder cannot lose the
  // footage, and cannot take clips out of a sequence as a side effect.
  Project p = pool_of(1);
  p = create_bin(std::move(p), "Interviews");
  const std::string bin = last_bin(p);
  p = file_media(std::move(p), "f1", bin);

  p = remove_bins(std::move(p), bin);
  EXPECT_TRUE(p.bins.empty());
  ASSERT_EQ(p.media.size(), 1u) << "removing a folder removed the footage";
  EXPECT_EQ(media_in_bin(p, ""), std::vector<std::string>{"f1"})
      << "media naming a bin that has gone should read as top level";
}

TEST(MediaBins, RemovingABinTakesTheBinsInsideIt) {
  Project p = create_bin(Project{}, "Day 1");
  const std::string outer = last_bin(p);
  p = create_bin(std::move(p), "Cameras", outer);
  p = create_bin(std::move(p), "Sound", outer);
  p = create_bin(std::move(p), "Elsewhere");
  const std::string elsewhere = last_bin(p);

  p = remove_bins(std::move(p), outer);
  ASSERT_EQ(p.bins.size(), 1u);
  EXPECT_EQ(p.bins.front().id, elsewhere) << "a bin outside the one removed went with it";
}

TEST(MediaBins, WhatIsInsideCountsBinsAsWellAsMedia) {
  Project p = pool_of(1);
  p = create_bin(std::move(p), "Day 1");
  const std::string outer = last_bin(p);
  EXPECT_TRUE(bin_is_empty(p, outer));

  p = create_bin(std::move(p), "Cameras", outer);
  EXPECT_FALSE(bin_is_empty(p, outer)) << "a bin inside it is something in it";

  Project other = pool_of(1);
  other = create_bin(std::move(other), "Interviews");
  const std::string bin = last_bin(other);
  other = file_media(std::move(other), "f1", bin);
  EXPECT_FALSE(bin_is_empty(other, bin));
}

TEST(MediaBins, WhatIsWithinListsTheBinItselfFirst) {
  // Which is what lets a caller delete a whole branch in one pass without
  // having to remember the root separately.
  Project p = create_bin(Project{}, "A");
  const std::string a = last_bin(p);
  p = create_bin(std::move(p), "B", a);

  const std::vector<std::string> within = bins_within(p, a);
  ASSERT_EQ(within.size(), 2u);
  EXPECT_EQ(within.front(), a);
  EXPECT_TRUE(bins_within(p, "bin_gone").empty());
}

TEST(MediaBins, MediaDirectlyInABinDoesNotIncludeWhatIsBeneathIt) {
  // Because the browser draws one level at a time, and a bin listing its
  // grandchildren alongside its children would draw them twice.
  Project p = pool_of(2);
  p = create_bin(std::move(p), "Day 1");
  const std::string outer = last_bin(p);
  p = create_bin(std::move(p), "Cameras", outer);
  const std::string inner = last_bin(p);

  p = file_media(std::move(p), "f1", outer);
  p = file_media(std::move(p), "f2", inner);

  EXPECT_EQ(media_in_bin(p, outer), std::vector<std::string>{"f1"});
  EXPECT_EQ(media_in_bin(p, inner), std::vector<std::string>{"f2"});
}

TEST(MediaBins, IdsAreNotHandedOutTwice) {
  reset_ids();
  Project p = create_bin(Project{}, "A");
  p = create_bin(std::move(p), "B");
  EXPECT_NE(p.bins[0].id, p.bins[1].id);
}

}  // namespace
}  // namespace cutline::core
