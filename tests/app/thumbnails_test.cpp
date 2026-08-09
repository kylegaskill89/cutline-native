/// The filmstrips the timeline draws, and the worker that extracts them.
///
/// The same shape as the waveform cache's tests, plus the one thing that is
/// genuinely different: these are pixels, so the cache is bounded and has to
/// drop what has gone longest without being wanted.

#include "cutline/app/thumbnails.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <process.h>

#include <filesystem>
#include <string>
#include <thread>

namespace cutline::app {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string reference_clip() {
  const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
  if (dir == nullptr) return {};
  const std::filesystem::path path = std::filesystem::path(dir) / "Boiler.mp4";
  return std::filesystem::exists(path) ? path.string() : std::string{};
}

[[nodiscard]] std::shared_ptr<const ui::Filmstrip> wait_for(const ThumbnailCache& cache,
                                                            std::string_view media_id,
                                                            std::chrono::seconds bound = 90s) {
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto found = cache.find(media_id)) return found;
    std::this_thread::sleep_for(20ms);
  }
  return nullptr;
}

// ------------------------------------------------- how many frames are worth --

TEST(ThumbnailCache, ALongerSourceIsWorthMoreFrames) {
  EXPECT_LT(ThumbnailCache::frames_for(10.0), ThumbnailCache::frames_for(120.0));
}

// The cap is what stops a ten-minute source costing minutes of seeking and tens
// of megabytes; the floor is what stops a two-second clip getting one frame and
// a strip that says nothing.
TEST(ThumbnailCache, TheFrameCountIsBoundedAtBothEnds) {
  EXPECT_EQ(ThumbnailCache::frames_for(0.5), kMinThumbnails);
  EXPECT_EQ(ThumbnailCache::frames_for(0.0), kMinThumbnails);
  EXPECT_EQ(ThumbnailCache::frames_for(-5.0), kMinThumbnails);
  EXPECT_EQ(ThumbnailCache::frames_for(60.0 * 60.0), kMaxThumbnails);
}

// ---------------------------------------------- which stretch is still wanted --

// Pure arithmetic, and the whole reason a scroll is cheap. Getting this wrong
// means either re-extracting the file on every rebuild or never filling in what
// was scrolled to.
TEST(ThumbnailSpans, NothingHeldMeansTheWholeStretchIsWanted) {
  const auto want = ThumbnailCache::missing({}, {10.0, 20.0});
  EXPECT_DOUBLE_EQ(want.from, 10.0);
  EXPECT_DOUBLE_EQ(want.to, 20.0);
}

TEST(ThumbnailSpans, AStretchAlreadyHeldIsNotAskedForAgain) {
  EXPECT_TRUE(ThumbnailCache::missing({{0.0, 30.0}}, {10.0, 20.0}).empty());
  EXPECT_TRUE(ThumbnailCache::missing({{10.0, 20.0}}, {10.0, 20.0}).empty());
}

TEST(ThumbnailSpans, ScrollingForwardAsksOnlyForTheNewPart) {
  // The case that matters: the view moves on by two seconds and the cache asks
  // for two seconds, not for the ten minutes it is looking at part of.
  const auto want = ThumbnailCache::missing({{10.0, 20.0}}, {12.0, 22.0});
  EXPECT_DOUBLE_EQ(want.from, 20.0);
  EXPECT_DOUBLE_EQ(want.to, 22.0);
}

TEST(ThumbnailSpans, ScrollingBackAsksOnlyForTheNewPart) {
  const auto want = ThumbnailCache::missing({{10.0, 20.0}}, {8.0, 18.0});
  EXPECT_DOUBLE_EQ(want.from, 8.0);
  EXPECT_DOUBLE_EQ(want.to, 10.0);
}

TEST(ThumbnailSpans, AJumpSomewhereElseAsksForAllOfIt) {
  const auto want = ThumbnailCache::missing({{0.0, 20.0}}, {300.0, 310.0});
  EXPECT_DOUBLE_EQ(want.from, 300.0);
  EXPECT_DOUBLE_EQ(want.to, 310.0);
}

// One stretch and not a set. A hole in the middle is re-extracted along with
// its edges, which costs a few frames already held and saves a second seek.
TEST(ThumbnailSpans, AHoleInTheMiddleIsCoveredInOnePiece) {
  const auto want = ThumbnailCache::missing({{0.0, 5.0}, {15.0, 20.0}}, {0.0, 20.0});
  EXPECT_DOUBLE_EQ(want.from, 5.0);
  EXPECT_DOUBLE_EQ(want.to, 15.0);
}

// ------------------------------------------------------------- how finely --

// The bug this exists to prevent, and it was a real one: the pool's tiles ask
// for a dozen frames across a whole file, which used to record the file's whole
// length as covered. Every clip of that source on the timeline was then stuck
// with one frame every twelve seconds, for the rest of the session, because
// nothing would ask again.
TEST(ThumbnailFineness, ACoarseStretchDoesNotAnswerAFineOne) {
  const ThumbnailCache::Span pool{0.0, 600.0, 50.0};  // a dozen across ten minutes
  const auto want = ThumbnailCache::missing({pool}, {100.0, 120.0, kThumbnailSeconds});

  EXPECT_FALSE(want.empty()) << "the timeline can never fill this in";
  EXPECT_DOUBLE_EQ(want.from, 100.0);
  EXPECT_DOUBLE_EQ(want.to, 120.0);
  EXPECT_DOUBLE_EQ(want.seconds_per_frame, kThumbnailSeconds);
}

// The other way round is fine: what was taken finely answers a coarse question,
// because the frames asked for are all there and more besides.
TEST(ThumbnailFineness, AFineStretchAnswersACoarseOne) {
  const ThumbnailCache::Span timeline{0.0, 600.0, kThumbnailSeconds};
  EXPECT_TRUE(ThumbnailCache::missing({timeline}, {100.0, 120.0, 50.0}).empty());
}

TEST(ThumbnailFineness, TheSameFinenessBehavesAsItAlwaysDid) {
  const ThumbnailCache::Span had{0.0, 30.0, kThumbnailSeconds};
  EXPECT_TRUE(ThumbnailCache::missing({had}, {10.0, 20.0, kThumbnailSeconds}).empty());
}

// A coarse pass and a fine pass over the same seconds are two different facts
// about them, and neither may be merged into the other's claim.
TEST(ThumbnailFineness, TwoFinenessesAreBothRemembered) {
  const ThumbnailCache::Span coarse{0.0, 600.0, 50.0};
  const ThumbnailCache::Span fine{0.0, 20.0, kThumbnailSeconds};

  EXPECT_TRUE(ThumbnailCache::missing({coarse, fine}, {0.0, 20.0, kThumbnailSeconds}).empty())
      << "the fine pass covers this";
  EXPECT_TRUE(ThumbnailCache::missing({coarse, fine}, {0.0, 600.0, 50.0}).empty())
      << "the coarse pass covers this";

  const auto want = ThumbnailCache::missing({coarse, fine}, {100.0, 120.0, kThumbnailSeconds});
  EXPECT_FALSE(want.empty()) << "and neither covers finely past where the fine pass reached";
}

// How many frames a stretch is worth follows the fineness asked for, which is
// what makes a pool tile cost a dozen seeks and not forty-eight.
TEST(ThumbnailFineness, TheFrameCountFollowsTheFineness) {
  EXPECT_EQ(ThumbnailCache::frames_for(600.0, kThumbnailSeconds), kMaxThumbnails)
      << "ten minutes at the timeline's grid is capped";
  EXPECT_EQ(ThumbnailCache::frames_for(600.0, 50.0), 12);
  EXPECT_EQ(ThumbnailCache::frames_for(600.0, 0.0), kMaxThumbnails)
      << "nonsense spacing falls back to the grid rather than dividing by zero";
}

// ------------------------------------------------- without any media at all --

TEST(ThumbnailCache, AskingForNothingIsNotAsking) {
  ThumbnailCache cache;
  cache.request("matte", "", 0.0, 10.0);
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_FALSE(cache.take_arrival());
}

TEST(ThumbnailCache, AFilmstripNobodyHasYetIsSimplyAbsent) {
  const ThumbnailCache cache;
  EXPECT_EQ(cache.find("anything"), nullptr);
  EXPECT_EQ(cache.bytes(), 0u);
}

TEST(ThumbnailCache, AFileThatIsNotThereLeavesNothingAndNoCrash) {
  ThumbnailCache cache;
  cache.request("gone", "P:/no/such/file.mp4", 0.0, 10.0);
  std::this_thread::sleep_for(400ms);
  EXPECT_EQ(cache.find("gone"), nullptr);
  EXPECT_EQ(cache.size(), 0u);
}

TEST(ThumbnailCache, ClosingWhileWorkIsOutstandingIsClean) {
  ThumbnailCache cache;
  cache.request("gone", "P:/no/such/file.mp4", 0.0, 10.0);
  cache.request("gone2", "P:/no/such/other.mp4", 0.0, 10.0);
  SUCCEED() << "the destructor runs at the end of this scope";
}

// ---------------------------------------------------------- with real video --

class WithVideoFootage : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = reference_clip();
    if (path_.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";
  }
  std::string path_;
};

TEST_F(WithVideoFootage, AFilmstripArrivesWithoutBlockingTheAsker) {
  ThumbnailCache cache;

  const auto before = std::chrono::steady_clock::now();
  cache.request("boiler", path_, 0.0, 8.0);
  const auto asked_for = std::chrono::steady_clock::now() - before;
  EXPECT_LT(asked_for, 250ms) << "requesting a filmstrip blocked the caller";

  const std::shared_ptr<const ui::Filmstrip> strip = wait_for(cache, "boiler");
  ASSERT_NE(strip, nullptr) << "no filmstrip arrived";

  EXPECT_FALSE(strip->empty());
  EXPECT_GE(strip->frames.size(), 2u);
  for (const ui::FilmFrame& frame : strip->frames) {
    EXPECT_FALSE(frame.empty());
    EXPECT_EQ(frame.height, kThumbnailHeight) << "sized by height, width follows the aspect";
    EXPECT_GT(frame.width, 0);
    // Tightly packed RGBA, which is what the painter is handed.
    EXPECT_EQ(frame.rgba.size(),
              static_cast<std::size_t>(frame.width) * frame.height * 4u);
  }
}

// The drawing asks for the frame nearest a source time, and a caller may
// reasonably read the strip left to right.
TEST_F(WithVideoFootage, TheFramesComeOutInTimeOrder) {
  ThumbnailCache cache;
  cache.request("boiler", path_, 0.0, 8.0);
  const std::shared_ptr<const ui::Filmstrip> strip = wait_for(cache, "boiler");
  ASSERT_NE(strip, nullptr);

  for (std::size_t i = 1; i < strip->frames.size(); ++i) {
    EXPECT_GE(strip->frames[i].t, strip->frames[i - 1].t);
  }
}

// Different moments of a real clip, so the strip says something about the
// footage rather than being the same frame repeated.
TEST_F(WithVideoFootage, TheFramesAreTakenFromAcrossTheSource) {
  ThumbnailCache cache;
  cache.request("boiler", path_, 0.0, 8.0);
  const std::shared_ptr<const ui::Filmstrip> strip = wait_for(cache, "boiler");
  ASSERT_NE(strip, nullptr);
  ASSERT_GE(strip->frames.size(), 2u);

  EXPECT_GT(strip->frames.back().t, strip->frames.front().t)
      << "every frame was taken from the same instant";
}

TEST_F(WithVideoFootage, TheSameSourceIsExtractedOnceHoweverOftenItIsAskedFor) {
  ThumbnailCache cache;
  for (int i = 0; i < 20; ++i) cache.request("boiler", path_, 0.0, 8.0);

  const std::shared_ptr<const ui::Filmstrip> strip = wait_for(cache, "boiler");
  ASSERT_NE(strip, nullptr);
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_EQ(cache.find("boiler"), strip) << "every clip of a source shares one strip";
}

TEST_F(WithVideoFootage, WhatIsStillToComeIsCountedAndComesBackDown) {
  ThumbnailCache cache;
  EXPECT_EQ(cache.pending(), 0u);

  cache.request("boiler", path_, 0.0, 6.0);
  ASSERT_NE(wait_for(cache, "boiler"), nullptr);
  EXPECT_EQ(cache.pending(), 0u);
}

TEST_F(WithVideoFootage, AsecondStretchIsAddedToTheFirstRatherThanReplacingIt) {
  // Scrolling has to build the strip up. Replacing would mean the part just
  // scrolled away is extracted again the moment it comes back.
  ThumbnailCache cache;
  cache.request("boiler", path_, 0.0, 2.0);
  const auto first = wait_for(cache, "boiler");
  ASSERT_NE(first, nullptr);
  const std::size_t was = first->frames.size();

  cache.request("boiler", path_, 4.0, 6.0);
  for (int i = 0; i < 400 && cache.pending() > 0; ++i) {
    std::this_thread::sleep_for(20ms);
  }
  const auto second = cache.find("boiler");
  ASSERT_NE(second, nullptr);
  EXPECT_GT(second->frames.size(), was) << "the later stretch replaced the earlier one";
  EXPECT_LT(second->frames.front().t, 2.5) << "the first stretch was lost";
  EXPECT_GT(second->frames.back().t, 3.5) << "the second stretch never arrived";
}

TEST_F(WithVideoFootage, AskingForAStretchAlreadyHeldCostsNothing) {
  ThumbnailCache cache;
  cache.request("boiler", path_, 0.0, 6.0);
  ASSERT_NE(wait_for(cache, "boiler"), nullptr);
  ASSERT_EQ(cache.pending(), 0u);

  // Inside what has already been extracted, so there is nothing to do.
  cache.request("boiler", path_, 1.0, 5.0);
  EXPECT_EQ(cache.pending(), 0u);
}

TEST(Thumbnails, ASourceThatWillNotDecodeStopsBeingOutstanding) {
  // Three ways out of the worker's loop — extracted, refused, and yielding no
  // usable frames — and the count has to come down on all of them.
  ThumbnailCache cache;
  cache.request("nowhere", "Z:/definitely/not/here.mp4", 0.0, 10.0);

  for (int i = 0; i < 200 && cache.pending() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(cache.pending(), 0u);
}

TEST_F(WithVideoFootage, TheWakeUpFires) {
  ThumbnailCache cache;
  std::atomic<int> woken{0};
  cache.set_on_arrival([&woken] { woken.fetch_add(1); });

  cache.request("boiler", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "boiler"), nullptr);
  for (int i = 0; i < 100 && woken.load() == 0; ++i) std::this_thread::sleep_for(10ms);

  EXPECT_GE(woken.load(), 1) << "nothing would have told a waiting message loop";
}

TEST_F(WithVideoFootage, AFilmstripCostsPixelsAndTheCacheKnowsHowMany) {
  ThumbnailCache cache;
  cache.request("boiler", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "boiler"), nullptr);

  EXPECT_GT(cache.bytes(), 0u);
  EXPECT_LE(cache.bytes(), kThumbnailBudget)
      << "one source alone should not exceed the whole budget";
}

TEST_F(WithVideoFootage, NothingIsDroppedWhileTheBudgetHolds) {
  // The normal case, and what stops a session re-extracting what it is looking
  // at every time it looks away.
  ThumbnailCache cache;
  for (const char* id : {"one", "two", "three"}) {
    cache.request(id, path_, 0.0, 8.0);
    ASSERT_NE(wait_for(cache, id), nullptr) << id;
  }

  EXPECT_EQ(cache.size(), 3u);
  EXPECT_LE(cache.bytes(), kThumbnailBudget);
}

// What makes this cache different from the waveform one: filmstrips are pixels,
// so there is a budget, and the entry dropped must be the one that has gone
// longest without being wanted rather than whichever the map ordered first.
TEST_F(WithVideoFootage, TheLeastRecentlyWantedIsTheOneDropped) {
  // What one filmstrip of this source costs, so the budget can be set to hold
  // two of them and not three. A fixed number would either never evict or
  // evict down to one, and neither says which entry was chosen.
  std::size_t one_strip = 0;
  {
    ThumbnailCache probe;
    probe.request("probe", path_, 0.0, 8.0);
    ASSERT_NE(wait_for(probe, "probe"), nullptr);
    one_strip = probe.bytes();
  }
  ASSERT_GT(one_strip, 0u);

  ThumbnailCache cache(one_strip * 2 + one_strip / 2);
  cache.request("one", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "one"), nullptr);
  cache.request("two", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "two"), nullptr);
  ASSERT_EQ(cache.size(), 2u) << "the budget was not big enough to hold two";

  // "one" has gone longest without being asked for; touching it makes "two" the
  // stale one instead. This is the assertion that fails if eviction picks by
  // anything other than when an entry was last wanted.
  ASSERT_NE(cache.find("one"), nullptr);

  cache.request("three", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "three"), nullptr);

  EXPECT_EQ(cache.size(), 2u);
  EXPECT_EQ(cache.find("two"), nullptr) << "the stale one survived";
  EXPECT_NE(cache.find("one"), nullptr) << "the one just touched was dropped";
  EXPECT_NE(cache.find("three"), nullptr) << "the newest one was dropped";
}

// A source dropped under the budget must be extractable again, or it would be
// remembered as done and never drawn.
TEST_F(WithVideoFootage, AnEvictedSourceCanBeAskedForAgain) {
  ThumbnailCache cache(1u);
  cache.request("one", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "one"), nullptr);
  cache.request("two", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "two"), nullptr);
  ASSERT_EQ(cache.find("one"), nullptr) << "the first was not evicted, so this proves nothing";

  cache.request("one", path_, 0.0, 8.0);
  EXPECT_NE(wait_for(cache, "one"), nullptr) << "it was remembered as done and never came back";
}

// A coarse pass must not rob a later fine one, all the way through the worker
// rather than only in the arithmetic. This is the shape of what the pool and
// the timeline do to the same source.
TEST_F(WithVideoFootage, ACoarsePassDoesNotStopAFinerOne) {
  ThumbnailCache cache;
  cache.request("boiler", path_, 0.0, 8.0, 4.0);  // as a pool tile asks
  const auto coarse = wait_for(cache, "boiler");
  ASSERT_NE(coarse, nullptr);
  const std::size_t after_coarse = coarse->frames.size();

  cache.request("boiler", path_, 0.0, 8.0, 1.0);  // as the timeline asks
  // The strip is replaced rather than added to, so waiting means waiting for
  // the count to grow rather than for something to appear.
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  std::shared_ptr<const ui::Filmstrip> fine;
  while (std::chrono::steady_clock::now() < deadline) {
    fine = cache.find("boiler");
    if (fine != nullptr && fine->frames.size() > after_coarse) break;
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_NE(fine, nullptr);
  EXPECT_GT(fine->frames.size(), after_coarse)
      << "the coarse pass claimed these seconds and the fine one never ran";
}

// ------------------------------------------------------- kept between runs --

/// A cache directory of its own, removed afterwards.
class Scratch {
 public:
  Scratch() {
    // With the process id as well as the address: `ctest -j` runs these in
    // separate processes, where the same heap address can come up twice.
    dir_ = std::filesystem::temp_directory_path() /
           ("cutline-strip-cache-" + std::to_string(_getpid()) + "-" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
  }
  ~Scratch() {
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }

 private:
  std::filesystem::path dir_;
};

[[nodiscard]] std::size_t files_in(const std::filesystem::path& dir) {
  std::error_code error;
  if (!std::filesystem::exists(dir, error)) return 0;
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, error)) {
    std::error_code ignored;
    if (entry.is_regular_file(ignored)) ++count;
  }
  return count;
}

TEST_F(WithVideoFootage, FramesAreWrittenWhereTheyCanBeFoundAgain) {
  const Scratch scratch;
  ThumbnailCache cache;
  cache.set_cache_dir(scratch.dir());

  cache.request("boiler", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "boiler"), nullptr);

  EXPECT_GT(files_in(scratch.dir()), 0u) << "nothing was kept for the next session";
}

// The half of this that is worth having: a filmstrip costs a seek and a decode
// per frame — measured at about half a second each on a 4K capture — and a
// second session must not pay it again. Checked by the pixels being the same
// rather than by timing, since a test that asserts "faster" fails on a busy
// machine.
TEST_F(WithVideoFootage, ASecondSessionReadsTheFramesTheFirstExtracted) {
  const Scratch scratch;

  std::vector<ui::FilmFrame> first;
  {
    ThumbnailCache cache;
    cache.set_cache_dir(scratch.dir());
    cache.request("boiler", path_, 0.0, 8.0);
    const auto strip = wait_for(cache, "boiler");
    ASSERT_NE(strip, nullptr);
    first = strip->frames;
  }
  ASSERT_FALSE(first.empty());

  // A fresh cache with nothing in memory, pointed at the same directory.
  ThumbnailCache second;
  second.set_cache_dir(scratch.dir());
  second.request("boiler", path_, 0.0, 8.0);
  const auto again = wait_for(second, "boiler");
  ASSERT_NE(again, nullptr);

  ASSERT_EQ(again->frames.size(), first.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_DOUBLE_EQ(again->frames[i].t, first[i].t) << "frame " << i;
    EXPECT_EQ(again->frames[i].rgba, first[i].rgba) << "frame " << i << " is not the same picture";
  }
}

TEST_F(WithVideoFootage, WithNowhereToKeepThemNothingIsKept) {
  const Scratch scratch;
  ThumbnailCache cache;  // no cache dir set

  cache.request("boiler", path_, 0.0, 8.0);
  ASSERT_NE(wait_for(cache, "boiler"), nullptr);
  EXPECT_EQ(files_in(scratch.dir()), 0u);
}

}  // namespace
}  // namespace cutline::app
