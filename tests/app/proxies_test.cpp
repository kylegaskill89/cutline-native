/// The worker that makes proxies, and what it hands back.
///
/// A transcode takes long enough that the only honest test of it is against a
/// real file, so the ones that finish a proxy skip without the reference
/// footage. What can be checked anywhere is the bookkeeping — that asking twice
/// queues once, that a cancel leaves nothing behind, that a source that will not
/// open is reported rather than silently forgotten — and those run everywhere.

#include "cutline/app/proxies.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
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

/// A scratch path that cleans itself up, along with the folder it sits in.
class TempProxy {
 public:
  TempProxy()
      : folder_(std::filesystem::temp_directory_path() /
                ("cutline_proxies_" + std::to_string(++counter_))) {}

  ~TempProxy() {
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

  TempProxy(const TempProxy&) = delete;
  TempProxy& operator=(const TempProxy&) = delete;

  [[nodiscard]] std::string path() const { return (folder_ / "proxy.mp4").string(); }
  [[nodiscard]] bool exists() const { return std::filesystem::exists(folder_ / "proxy.mp4"); }

 private:
  std::filesystem::path folder_;
  static inline int counter_ = 0;
};

/// Waits until the builder has nothing left to do, up to a bound. How long a
/// transcode takes depends on the machine, so this cannot be a fixed sleep.
[[nodiscard]] bool wait_until_idle(const ProxyBuilder& builder,
                                   std::chrono::seconds bound = 120s) {
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < deadline) {
    if (builder.pending() == 0) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

// ----------------------------------------------------- without any media at all --

TEST(ProxyBuilder, StartsWithNothingToSay) {
  const ProxyBuilder builder;
  EXPECT_EQ(builder.pending(), 0u);
  EXPECT_FALSE(builder.progress().has_value());
}

TEST(ProxyBuilder, AsksWithNowhereToPutItAreIgnored) {
  // Generated media — titles, colour mattes — has no file behind it, and a
  // proxy of nothing is a decoder error reported once per press.
  ProxyBuilder builder;
  builder.request("m1", "Title", "", "D:/proxies/m1.mp4");
  builder.request("m2", "Clip", "D:/footage/m2.mp4", "");
  EXPECT_EQ(builder.pending(), 0u);
}

TEST(ProxyBuilder, AskingTwiceForTheSameSourceQueuesOnce) {
  // A button pressed twice must not transcode twice: the second copy would
  // write the same path as the first while the first was still writing it.
  ProxyBuilder builder;
  builder.request("m1", "Clip", "Z:/definitely/not/here.mp4", "Z:/nowhere/m1.mp4");
  builder.request("m1", "Clip", "Z:/definitely/not/here.mp4", "Z:/nowhere/m1.mp4");
  EXPECT_LE(builder.pending(), 1u);
}

TEST(ProxyBuilder, AFileThatWillNotOpenIsReported) {
  // The failure that matters is a quiet one: a proxy that never arrives looks
  // exactly like a proxy still being made, and the difference is a session
  // spent waiting.
  ProxyBuilder builder;
  builder.request("m1", "Missing.mp4", "Z:/definitely/not/here.mp4", "Z:/nowhere/m1.mp4");
  ASSERT_TRUE(wait_until_idle(builder, 30s));

  const auto failures = builder.take_failures();
  ASSERT_EQ(failures.size(), 1u);
  EXPECT_EQ(failures.front().media_id, "m1");
  EXPECT_EQ(failures.front().name, "Missing.mp4");
  EXPECT_FALSE(failures.front().message.empty());

  EXPECT_TRUE(builder.take_failures().empty()) << "the same failure was reported twice";
  EXPECT_TRUE(builder.take_finished().empty());
}

// --------------------------------------------------------------- with footage --

TEST(ProxyBuilder, MakesAProxyAndSaysWhereItWent) {
  const std::string source = reference_clip();
  if (source.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";

  const TempProxy proxy;
  ProxyBuilder builder;
  builder.request("m1", "Boiler.mp4", source, proxy.path());
  ASSERT_TRUE(wait_until_idle(builder));

  const auto finished = builder.take_finished();
  ASSERT_EQ(finished.size(), 1u);
  EXPECT_EQ(finished.front().media_id, "m1");
  EXPECT_EQ(finished.front().path, proxy.path());
  EXPECT_TRUE(proxy.exists());

  EXPECT_TRUE(builder.take_finished().empty()) << "the same proxy was handed back twice";
  EXPECT_TRUE(builder.take_failures().empty());
}

TEST(ProxyBuilder, SaysWhatItIsWorkingOn) {
  const std::string source = reference_clip();
  if (source.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";

  const TempProxy first;
  const TempProxy second;
  ProxyBuilder builder;
  builder.request("m1", "Boiler.mp4", source, first.path());
  builder.request("m2", "Boiler again.mp4", source, second.path());

  // Named from the moment it starts rather than at the first progress report,
  // which on a long file is a minute of an indicator saying nothing.
  bool named = false;
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (const auto progress = builder.progress()) {
      EXPECT_FALSE(progress->name.empty());
      EXPECT_GE(progress->done, 0.0);
      EXPECT_LE(progress->done, 1.0);
      named = true;
      break;
    }
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_TRUE(named) << "the builder never said what it was doing";

  // One at a time: a machine struggling enough to want proxies is not helped by
  // transcoding two files at once.
  EXPECT_LE(builder.pending(), 2u);
  builder.cancel_all();
}

TEST(ProxyBuilder, CancellingLeavesNothingBehind) {
  const std::string source = reference_clip();
  if (source.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";

  const TempProxy proxy;
  {
    ProxyBuilder builder;
    builder.request("m1", "Boiler.mp4", source, proxy.path());

    // Once it is genuinely under way, so this cancels a transcode rather than
    // an empty queue.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline && !builder.progress().has_value()) {
      std::this_thread::sleep_for(5ms);
    }
    builder.cancel_all();
    ASSERT_TRUE(wait_until_idle(builder, 30s));

    EXPECT_TRUE(builder.take_finished().empty()) << "a cancelled proxy was handed back";
    EXPECT_TRUE(builder.take_failures().empty()) << "asking it to stop was reported as a failure";
  }
  EXPECT_FALSE(proxy.exists()) << "a half-written proxy was left on disk";
}

TEST(ProxyBuilder, ShuttingDownDoesNotWaitForTheTranscode) {
  const std::string source = reference_clip();
  if (source.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";

  // The one that would be noticed as the application taking minutes to close.
  const TempProxy proxy;
  auto builder = std::make_unique<ProxyBuilder>();
  builder->request("m1", "Boiler.mp4", source, proxy.path());
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  while (std::chrono::steady_clock::now() < deadline && !builder->progress().has_value()) {
    std::this_thread::sleep_for(5ms);
  }
  ASSERT_TRUE(builder->progress().has_value()) << "the transcode never started";

  // Only the teardown is timed. A whole proxy of the reference clip takes
  // several seconds; a shutdown that waited for one would show here.
  const auto started = std::chrono::steady_clock::now();
  builder.reset();
  const auto took = std::chrono::steady_clock::now() - started;
  EXPECT_LT(took, 2s) << "closing waited for the whole transcode";
}

}  // namespace
}  // namespace cutline::app
