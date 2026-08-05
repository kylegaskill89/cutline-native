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

// ------------------------------------------------- without any media at all --

TEST(ThumbnailCache, AskingForNothingIsNotAsking) {
  ThumbnailCache cache;
  cache.request("matte", "", 10.0);
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
  cache.request("gone", "P:/no/such/file.mp4", 10.0);
  std::this_thread::sleep_for(400ms);
  EXPECT_EQ(cache.find("gone"), nullptr);
  EXPECT_EQ(cache.size(), 0u);
}

TEST(ThumbnailCache, ClosingWhileWorkIsOutstandingIsClean) {
  ThumbnailCache cache;
  cache.request("gone", "P:/no/such/file.mp4", 10.0);
  cache.request("gone2", "P:/no/such/other.mp4", 10.0);
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
  cache.request("boiler", path_, 8.0);
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
  cache.request("boiler", path_, 8.0);
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
  cache.request("boiler", path_, 8.0);
  const std::shared_ptr<const ui::Filmstrip> strip = wait_for(cache, "boiler");
  ASSERT_NE(strip, nullptr);
  ASSERT_GE(strip->frames.size(), 2u);

  EXPECT_GT(strip->frames.back().t, strip->frames.front().t)
      << "every frame was taken from the same instant";
}

TEST_F(WithVideoFootage, TheSameSourceIsExtractedOnceHoweverOftenItIsAskedFor) {
  ThumbnailCache cache;
  for (int i = 0; i < 20; ++i) cache.request("boiler", path_, 8.0);

  const std::shared_ptr<const ui::Filmstrip> strip = wait_for(cache, "boiler");
  ASSERT_NE(strip, nullptr);
  EXPECT_EQ(cache.size(), 1u);
  EXPECT_EQ(cache.find("boiler"), strip) << "every clip of a source shares one strip";
}

TEST_F(WithVideoFootage, WhatIsStillToComeIsCountedAndComesBackDown) {
  ThumbnailCache cache;
  EXPECT_EQ(cache.pending(), 0u);

  cache.request("boiler", path_, 6.0);
  ASSERT_NE(wait_for(cache, "boiler"), nullptr);
  EXPECT_EQ(cache.pending(), 0u);
}

TEST(Thumbnails, ASourceThatWillNotDecodeStopsBeingOutstanding) {
  // Three ways out of the worker's loop — extracted, refused, and yielding no
  // usable frames — and the count has to come down on all of them.
  ThumbnailCache cache;
  cache.request("nowhere", "Z:/definitely/not/here.mp4", 10.0);

  for (int i = 0; i < 200 && cache.pending() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(cache.pending(), 0u);
}

TEST_F(WithVideoFootage, TheWakeUpFires) {
  ThumbnailCache cache;
  std::atomic<int> woken{0};
  cache.set_on_arrival([&woken] { woken.fetch_add(1); });

  cache.request("boiler", path_, 8.0);
  ASSERT_NE(wait_for(cache, "boiler"), nullptr);
  for (int i = 0; i < 100 && woken.load() == 0; ++i) std::this_thread::sleep_for(10ms);

  EXPECT_GE(woken.load(), 1) << "nothing would have told a waiting message loop";
}

TEST_F(WithVideoFootage, AFilmstripCostsPixelsAndTheCacheKnowsHowMany) {
  ThumbnailCache cache;
  cache.request("boiler", path_, 8.0);
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
    cache.request(id, path_, 8.0);
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
    probe.request("probe", path_, 8.0);
    ASSERT_NE(wait_for(probe, "probe"), nullptr);
    one_strip = probe.bytes();
  }
  ASSERT_GT(one_strip, 0u);

  ThumbnailCache cache(one_strip * 2 + one_strip / 2);
  cache.request("one", path_, 8.0);
  ASSERT_NE(wait_for(cache, "one"), nullptr);
  cache.request("two", path_, 8.0);
  ASSERT_NE(wait_for(cache, "two"), nullptr);
  ASSERT_EQ(cache.size(), 2u) << "the budget was not big enough to hold two";

  // "one" has gone longest without being asked for; touching it makes "two" the
  // stale one instead. This is the assertion that fails if eviction picks by
  // anything other than when an entry was last wanted.
  ASSERT_NE(cache.find("one"), nullptr);

  cache.request("three", path_, 8.0);
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
  cache.request("one", path_, 8.0);
  ASSERT_NE(wait_for(cache, "one"), nullptr);
  cache.request("two", path_, 8.0);
  ASSERT_NE(wait_for(cache, "two"), nullptr);
  ASSERT_EQ(cache.find("one"), nullptr) << "the first was not evicted, so this proves nothing";

  cache.request("one", path_, 8.0);
  EXPECT_NE(wait_for(cache, "one"), nullptr) << "it was remembered as done and never came back";
}

}  // namespace
}  // namespace cutline::app
