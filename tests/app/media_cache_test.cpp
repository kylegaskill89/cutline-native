/// Keeping derived work between sessions.
///
/// Everything here is regenerable, so the behaviour that matters is not "does
/// it store" but "does it ever hand back the wrong thing": a stale entry that
/// looks valid is a waveform belonging to footage somebody no longer has.

#include "cutline/app/media_cache.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace cutline::app {
namespace {

/// A cache directory and a source file of its own, both removed afterwards.
class Scratch {
 public:
  Scratch() {
    root_ = std::filesystem::temp_directory_path() /
            ("cutline-cache-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(dir());
    write_source("some footage");
  }
  ~Scratch() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  [[nodiscard]] std::filesystem::path dir() const { return root_ / "cache"; }
  [[nodiscard]] std::filesystem::path source() const { return root_ / "clip.mp4"; }

  void write_source(std::string_view contents) const {
    std::ofstream out(source(), std::ios::binary | std::ios::trunc);
    out << contents;
  }

 private:
  std::filesystem::path root_;
};

[[nodiscard]] ui::Waveform a_waveform() {
  ui::Waveform wave;
  wave.buckets_per_second = 100.0;
  wave.minimum = {-0.5f, -0.25f, 0.0f};
  wave.maximum = {0.5f, 0.75f, 0.125f};
  return wave;
}

[[nodiscard]] ui::FilmFrame a_frame() {
  ui::FilmFrame frame;
  frame.t = 4.0;
  frame.width = 2;
  frame.height = 2;
  frame.rgba = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
  return frame;
}

// ------------------------------------------------------------------ keys --

TEST(MediaCacheKey, TheSameFileKeepsTheSameKey) {
  const Scratch scratch;
  EXPECT_EQ(media_cache_key(scratch.source()), media_cache_key(scratch.source()));
  EXPECT_FALSE(media_cache_key(scratch.source()).empty());
}

// The whole point of size and modification time being *in* the key rather than
// checked against a stored copy: footage that has been re-encoded misses, and a
// miss costs a decode where a false hit costs somebody the wrong waveform.
TEST(MediaCacheKey, AChangedFileGetsANewKey) {
  const Scratch scratch;
  const std::string before = media_cache_key(scratch.source());

  // The filesystem's timestamps are coarse on some volumes, so the rewrite is
  // given a moment to land in a different tick.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  scratch.write_source("quite different footage of another length");

  EXPECT_NE(media_cache_key(scratch.source()), before);
}

TEST(MediaCacheKey, AFileThatIsNotThereHasNoKey) {
  EXPECT_TRUE(media_cache_key("d:/nothing/at/all.mp4").empty());
}

TEST(MediaCacheKey, NothingIsCachedWithoutOne) {
  // An empty key reads as "do not cache this", and both halves have to honour
  // it or a missing file would be stored under a name shared by every other
  // missing file.
  const Scratch scratch;
  write_cached_waveform(scratch.dir(), {}, 0, a_waveform());
  EXPECT_FALSE(read_cached_waveform(scratch.dir(), {}, 0).has_value());
  EXPECT_EQ(media_cache_bytes(scratch.dir()), 0u);
}

// ------------------------------------------------------------- waveforms --

TEST(MediaCache, AWaveformComesBackAsItWentIn) {
  const Scratch scratch;
  const std::string key = media_cache_key(scratch.source());
  const ui::Waveform wave = a_waveform();

  write_cached_waveform(scratch.dir(), key, 2, wave);
  const auto read = read_cached_waveform(scratch.dir(), key, 2);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(*read, wave);
}

TEST(MediaCache, EachStreamIsItsOwnEntry) {
  const Scratch scratch;
  const std::string key = media_cache_key(scratch.source());

  ui::Waveform first = a_waveform();
  ui::Waveform second = a_waveform();
  second.maximum[0] = 0.125f;

  write_cached_waveform(scratch.dir(), key, 0, first);
  write_cached_waveform(scratch.dir(), key, 1, second);

  EXPECT_EQ(read_cached_waveform(scratch.dir(), key, 0)->maximum[0], first.maximum[0]);
  EXPECT_EQ(read_cached_waveform(scratch.dir(), key, 1)->maximum[0], second.maximum[0]);
}

TEST(MediaCache, NothingStoredIsNothingFound) {
  const Scratch scratch;
  EXPECT_FALSE(
      read_cached_waveform(scratch.dir(), media_cache_key(scratch.source()), 0).has_value());
}

// ------------------------------------------------------------ filmstrips --

constexpr CachedStrip kStretch{.from = 0.0, .to = 8.0, .count = 6, .height = 72};

TEST(MediaCache, AStretchComesBackAsItWentIn) {
  const Scratch scratch;
  const std::string key = media_cache_key(scratch.source());

  ui::FilmFrame second = a_frame();
  second.t = 6.0;
  second.rgba[0] = 99;
  const std::vector<ui::FilmFrame> frames{a_frame(), second};

  write_cached_strip(scratch.dir(), key, kStretch, frames);
  const auto read = read_cached_strip(scratch.dir(), key, kStretch);
  ASSERT_TRUE(read.has_value());
  ASSERT_EQ(read->size(), frames.size());
  for (std::size_t i = 0; i < frames.size(); ++i) {
    EXPECT_DOUBLE_EQ((*read)[i].t, frames[i].t) << "frame " << i;
    EXPECT_EQ((*read)[i].width, frames[i].width);
    EXPECT_EQ((*read)[i].height, frames[i].height);
    EXPECT_EQ((*read)[i].rgba, frames[i].rgba) << "frame " << i;
  }
}

// Every part of what identifies a stretch has to be part of finding it. Where
// the frames land is a property of the range and the count, and the pixels are
// a property of the height — so a stretch stored for one is not the answer for
// another, and handing it back would draw the wrong part of the footage.
TEST(MediaCache, AStretchIsNotSharedWithADifferentOne) {
  const Scratch scratch;
  const std::string key = media_cache_key(scratch.source());
  write_cached_strip(scratch.dir(), key, kStretch, {a_frame()});

  EXPECT_TRUE(read_cached_strip(scratch.dir(), key, kStretch).has_value());

  for (const CachedStrip other : {CachedStrip{1.0, 8.0, 6, 72}, CachedStrip{0.0, 9.0, 6, 72},
                                  CachedStrip{0.0, 8.0, 7, 72}, CachedStrip{0.0, 8.0, 6, 144}}) {
    EXPECT_FALSE(read_cached_strip(scratch.dir(), key, other).has_value())
        << "from " << other.from << " to " << other.to << " x" << other.count << " at "
        << other.height;
  }
}

TEST(MediaCache, NoFramesAreNotStored) {
  const Scratch scratch;
  const std::string key = media_cache_key(scratch.source());
  write_cached_strip(scratch.dir(), key, kStretch, {});
  EXPECT_FALSE(read_cached_strip(scratch.dir(), key, kStretch).has_value());
  EXPECT_EQ(media_cache_bytes(scratch.dir()), 0u);
}

// ---------------------------------------------------------- housekeeping --

TEST(MediaCache, EmptyingItLeavesNothingButTheFolder) {
  const Scratch scratch;
  const std::string key = media_cache_key(scratch.source());
  write_cached_waveform(scratch.dir(), key, 0, a_waveform());
  write_cached_strip(scratch.dir(), key, kStretch, {a_frame()});
  ASSERT_GT(media_cache_bytes(scratch.dir()), 0u);

  const std::uintmax_t went = clear_media_cache(scratch.dir());
  EXPECT_GT(went, 0u);
  EXPECT_EQ(media_cache_bytes(scratch.dir()), 0u);
  // The folder itself stays, because somebody chose it.
  EXPECT_TRUE(std::filesystem::exists(scratch.dir()));
  EXPECT_FALSE(read_cached_waveform(scratch.dir(), key, 0).has_value());
}

TEST(MediaCache, MeasuringAFolderThatIsNotThereIsNotAnError) {
  EXPECT_EQ(media_cache_bytes("d:/no/such/cache"), 0u);
  EXPECT_EQ(clear_media_cache("d:/no/such/cache"), 0u);
}

// A truncated entry is a miss rather than half an envelope. Half a waveform
// draws a clip that stops in the middle, which reads as damaged footage.
TEST(MediaCache, AHalfWrittenEntryIsAMiss) {
  const Scratch scratch;
  const std::string key = media_cache_key(scratch.source());
  write_cached_waveform(scratch.dir(), key, 0, a_waveform());

  // Find what was written and cut it short.
  std::filesystem::path entry;
  for (const auto& found : std::filesystem::recursive_directory_iterator(scratch.dir())) {
    if (found.is_regular_file()) entry = found.path();
  }
  ASSERT_FALSE(entry.empty());
  const auto was = std::filesystem::file_size(entry);
  std::filesystem::resize_file(entry, was / 2);

  EXPECT_FALSE(read_cached_waveform(scratch.dir(), key, 0).has_value());
}

// Rubbish in the file must not be believed. Every length read here comes
// straight out of it, and trusting one is how a corrupt entry stops being a
// slow session and starts being a crash.
TEST(MediaCache, RubbishIsRefusedRatherThanBelieved) {
  const Scratch scratch;
  const std::string key = media_cache_key(scratch.source());
  write_cached_waveform(scratch.dir(), key, 0, a_waveform());

  std::filesystem::path entry;
  for (const auto& found : std::filesystem::recursive_directory_iterator(scratch.dir())) {
    if (found.is_regular_file()) entry = found.path();
  }
  ASSERT_FALSE(entry.empty());
  {
    std::ofstream out(entry, std::ios::binary | std::ios::trunc);
    const std::string junk(64, '\xa5');
    out << junk;
  }

  EXPECT_FALSE(read_cached_waveform(scratch.dir(), key, 0).has_value());
  EXPECT_FALSE(read_cached_strip(scratch.dir(), key, kStretch).has_value());
}

TEST(MediaCacheDir, TheDefaultIsUsedWhenNobodyHasChosen) {
  EXPECT_EQ(media_cache_dir({}), default_media_cache_dir());
  EXPECT_EQ(media_cache_dir("E:/Fast/Cache"), std::filesystem::path("E:/Fast/Cache"));
}

}  // namespace
}  // namespace cutline::app
