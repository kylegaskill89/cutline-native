/// The audio envelopes the timeline draws, and the worker that fills them in.
///
/// The interesting half is the threading, and it is only interesting against a
/// real file: what has to hold is that asking costs nothing on the calling
/// thread, that the answer turns up afterwards, and that a source asked for
/// twice is decoded once. So these skip unless `CUTLINE_TEST_MEDIA_DIR` points
/// at the reference footage.

#include "cutline/app/waveforms.hpp"

#include <gtest/gtest.h>

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

/// Waits for an envelope, up to a bound. Decoding a stream is seconds of work
/// and how many depends on the machine, so this cannot be a fixed sleep.
[[nodiscard]] std::shared_ptr<const ui::Waveform> wait_for(const WaveformCache& cache,
                                                           std::string_view media_id,
                                                           int stream,
                                                           std::chrono::seconds bound = 60s) {
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto found = cache.find(media_id, stream)) return found;
    std::this_thread::sleep_for(20ms);
  }
  return nullptr;
}

// ------------------------------------------------- without any media at all --

TEST(WaveformCache, AskingForNothingIsNotAsking) {
  // Generated media -- titles, colour mattes, adjustment layers -- have no file
  // behind them. Queueing one would be an error reported once a frame for as
  // long as the clip is on screen.
  WaveformCache cache;
  cache.request("title", "", 0);
  EXPECT_EQ(cache.size(), 0u);
  EXPECT_FALSE(cache.take_arrival());
}

TEST(WaveformCache, AnEnvelopeNobodyHasYetIsSimplyAbsent) {
  const WaveformCache cache;
  EXPECT_EQ(cache.find("anything", 0), nullptr);
}

TEST(WaveformCache, AFileThatIsNotThereLeavesNoEnvelopeAndNoCrash) {
  WaveformCache cache;
  cache.request("gone", "P:/no/such/file.mp4", 0);
  // Long enough for the worker to have tried and failed.
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(cache.find("gone", 0), nullptr);
  EXPECT_EQ(cache.size(), 0u);
}

// A cache torn down while a job is still queued must not hang or fault. This is
// what happens every time the application is closed with a project open.
TEST(WaveformCache, ClosingWhileWorkIsOutstandingIsClean) {
  WaveformCache cache;
  cache.request("gone", "P:/no/such/file.mp4", 0);
  cache.request("gone2", "P:/no/such/other.mp4", 0);
  SUCCEED() << "the destructor runs at the end of this scope";
}

// ---------------------------------------------------------- with real audio --

class WithAudioFootage : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = reference_clip();
    if (path_.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";
  }
  std::string path_;
};

TEST_F(WithAudioFootage, AnEnvelopeArrivesWithoutBlockingTheAsker) {
  WaveformCache cache;

  // The whole point: requesting returns at once, whatever the file costs to
  // decode. A lookup on the paint thread that waited for a decoder would be the
  // problem this class exists to avoid.
  const auto before = std::chrono::steady_clock::now();
  cache.request("boiler", path_, 0);
  const auto asked_for = std::chrono::steady_clock::now() - before;
  EXPECT_LT(asked_for, 250ms) << "requesting an envelope blocked the caller";

  const std::shared_ptr<const ui::Waveform> wave = wait_for(cache, "boiler", 0);
  ASSERT_NE(wave, nullptr) << "no envelope arrived";

  EXPECT_FALSE(wave->empty());
  EXPECT_DOUBLE_EQ(wave->buckets_per_second, kWaveformBucketsPerSecond);
  // Roughly the length of the clip, which is what says the whole stream was
  // read rather than the first packet of it.
  EXPECT_GT(wave->duration(), 1.0);

  // And it is an envelope rather than a flat line: the minimum below zero, the
  // maximum above, and neither beyond the sample range.
  float lowest = 0.0f;
  float highest = 0.0f;
  for (std::size_t i = 0; i < wave->size(); ++i) {
    lowest = std::min(lowest, wave->minimum[i]);
    highest = std::max(highest, wave->maximum[i]);
  }
  EXPECT_LT(lowest, 0.0f);
  EXPECT_GT(highest, 0.0f);
  EXPECT_GE(lowest, -1.01f);
  EXPECT_LE(highest, 1.01f);
}

TEST_F(WithAudioFootage, AnArrivalIsReportedOnceAndThenCleared) {
  WaveformCache cache;
  cache.request("boiler", path_, 0);
  ASSERT_NE(wait_for(cache, "boiler", 0), nullptr);

  EXPECT_TRUE(cache.take_arrival()) << "the frame loop was never told";
  EXPECT_FALSE(cache.take_arrival()) << "and told twice for one envelope";
}

TEST_F(WithAudioFootage, TheSameSourceIsDecodedOnceHoweverOftenItIsAskedFor) {
  WaveformCache cache;
  // A source cut into twenty clips is one file. Without the guard the timeline
  // queues every audio clip again on every rebuild, which is after every
  // gesture.
  for (int i = 0; i < 20; ++i) cache.request("boiler", path_, 0);

  const std::shared_ptr<const ui::Waveform> wave = wait_for(cache, "boiler", 0);
  ASSERT_NE(wave, nullptr);
  EXPECT_EQ(cache.size(), 1u);

  // And the same object, so every clip of the source shares one envelope rather
  // than holding a copy of half a megabyte each.
  EXPECT_EQ(cache.find("boiler", 0), wave);
}

TEST_F(WithAudioFootage, TheWakeUpFires) {
  WaveformCache cache;
  std::atomic<int> woken{0};
  cache.set_on_arrival([&woken] { woken.fetch_add(1); });

  cache.request("boiler", path_, 0);
  ASSERT_NE(wait_for(cache, "boiler", 0), nullptr);
  // The callback runs just after the map is written, so it may not have
  // happened at the instant the lookup succeeded.
  for (int i = 0; i < 100 && woken.load() == 0; ++i) std::this_thread::sleep_for(10ms);

  EXPECT_GE(woken.load(), 1) << "nothing would have told a waiting message loop";
}

}  // namespace
}  // namespace cutline::app
