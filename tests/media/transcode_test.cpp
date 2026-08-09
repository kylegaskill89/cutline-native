/// Proxies: a smaller file that is still the same footage.
///
/// The size is the easy half and the timing is the half that matters. A proxy
/// that runs at the wrong speed moves every cut made against it, and it looks
/// perfectly fine until somebody switches back to the original — so what these
/// check is mostly length and frame count rather than pixels.

#include "cutline/media/transcode.hpp"

#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <process.h>

#include <optional>
#include <string>
#include <vector>

namespace cutline::media {
namespace {

/// A scratch path that cleans itself up.
class TempFile {
 public:
  /// With the process id, because `ctest -j` runs these as separate processes
  /// and a counter starting at one in each of them names the same file twice.
  explicit TempFile(std::string suffix)
      : path_(std::filesystem::temp_directory_path() /
              ("cutline_proxy_test_" + std::to_string(_getpid()) + "_" +
               std::to_string(++counter_) + suffix)) {}

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] std::string string() const { return path_.string(); }
  [[nodiscard]] bool exists() const { return std::filesystem::exists(path_); }

 private:
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

/// Writes a plain file to make a proxy of: `count` frames at `fps`, each a flat
/// grey that says which one it is.
[[nodiscard]] bool write_source(const std::string& path, int width, int height, int count,
                                double fps) {
  auto writer = MediaWriter::create(path, {.width = width,
                                           .height = height,
                                           .fps = fps,
                                           .preference = EncoderPreference::Software});
  if (!writer) {
    ADD_FAILURE() << writer.error();
    return false;
  }
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4, 255);
  for (int i = 0; i < count; ++i) {
    const auto value = static_cast<std::uint8_t>(32 + (i * 4) % 192);
    for (std::size_t p = 0; p < rgba.size(); p += 4) {
      rgba[p] = value;
      rgba[p + 1] = value;
      rgba[p + 2] = value;
    }
    if (auto ok = (*writer)->write_frame(rgba); !ok) {
      ADD_FAILURE() << ok.error();
      return false;
    }
  }
  auto finished = (*writer)->finish();
  EXPECT_TRUE(finished.has_value()) << (finished ? "" : finished.error());
  return finished.has_value();
}

/// The video stream of a file, or nothing when it has none.
[[nodiscard]] std::optional<VideoStreamInfo> video_stream(const std::string& path) {
  const auto info = probe(path);
  if (!info.has_value()) {
    ADD_FAILURE() << info.error();
    return std::nullopt;
  }
  if (info->primary_video() == nullptr) {
    ADD_FAILURE() << path << " came out with no video stream";
    return std::nullopt;
  }
  return *info->primary_video();
}

[[nodiscard]] int count_frames(const std::string& path) {
  auto decoder = VideoDecoder::open(path, {.preferred = Acceleration::Software});
  if (!decoder) return -1;
  int frames = 0;
  while (true) {
    const auto got = (*decoder)->next_frame();
    if (!got || !*got) break;
    ++frames;
  }
  return frames;
}

TEST(Proxy, IsSmallerButTheSameLength) {
  TempFile source(".mp4");
  TempFile proxy(".mp4");
  ASSERT_TRUE(write_source(source.string(), 640, 360, 30, 30.0));

  const auto result = write_proxy(source.string(), proxy.string(), {.height = 180});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, ProxyResult::Written);

  const auto probed = video_stream(proxy.string());
  ASSERT_TRUE(probed.has_value());
  EXPECT_EQ(probed->height, 180);
  EXPECT_EQ(probed->width, 320) << "the aspect ratio was not kept";

  // Every frame, not merely most of them: a proxy one frame short is one that
  // ends early, and nothing about the picture would say so.
  EXPECT_EQ(count_frames(proxy.string()), 30);
}

TEST(Proxy, IsNeverBiggerThanWhatItStandsIn) {
  // A proxy larger than its source costs more to decode than the original,
  // which is the whole arrangement backwards.
  TempFile source(".mp4");
  TempFile proxy(".mp4");
  ASSERT_TRUE(write_source(source.string(), 320, 180, 10, 30.0));

  const auto result = write_proxy(source.string(), proxy.string(), {.height = 1080});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto probed = video_stream(proxy.string());
  ASSERT_TRUE(probed.has_value());
  EXPECT_EQ(probed->height, 180);
  EXPECT_EQ(probed->width, 320);
}

TEST(Proxy, HasSidesAnEvenNumberOfPixels) {
  // 4:2:0 chroma is half resolution in both directions, and an odd side has no
  // whole chroma sample for its last row. Encoders refuse it, so a source whose
  // aspect works out odd has to be rounded rather than passed through.
  TempFile source(".mp4");
  TempFile proxy(".mp4");
  ASSERT_TRUE(write_source(source.string(), 302, 400, 6, 30.0));

  const auto result = write_proxy(source.string(), proxy.string(), {.height = 201});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto probed = video_stream(proxy.string());
  ASSERT_TRUE(probed.has_value());
  EXPECT_EQ(probed->height % 2, 0);
  EXPECT_EQ(probed->width % 2, 0);
}

TEST(Proxy, ReportsHowFarAlongItIs) {
  TempFile source(".mp4");
  TempFile proxy(".mp4");
  ASSERT_TRUE(write_source(source.string(), 320, 180, 30, 30.0));

  std::vector<double> progress;
  const auto result = write_proxy(source.string(), proxy.string(),
                                  {.height = 90, .on_progress = [&](double done) {
                                     progress.push_back(done);
                                     return true;
                                   }});
  ASSERT_TRUE(result.has_value()) << result.error();

  ASSERT_FALSE(progress.empty()) << "nothing was ever said about progress";
  EXPECT_DOUBLE_EQ(progress.back(), 1.0) << "it never said it had finished";
  for (std::size_t i = 1; i < progress.size(); ++i) {
    EXPECT_GE(progress[i], progress[i - 1]) << "progress went backwards at " << i;
  }
}

TEST(Proxy, StoppingRemovesThePartialFile) {
  // The dangerous outcome is not a cancelled encode; it is a short file left on
  // disk, because a file that exists is one somebody attaches and cuts against.
  TempFile source(".mp4");
  TempFile proxy(".mp4");
  ASSERT_TRUE(write_source(source.string(), 320, 180, 60, 30.0));

  int seen = 0;
  const auto result = write_proxy(source.string(), proxy.string(),
                                  {.height = 90, .on_progress = [&](double) {
                                     ++seen;
                                     return seen < 3;
                                   }});
  ASSERT_TRUE(result.has_value()) << result.error();
  EXPECT_EQ(*result, ProxyResult::Cancelled);
  EXPECT_FALSE(proxy.exists()) << "a half-written proxy was left behind";
}

TEST(Proxy, AFileThatWillNotOpenIsAnErrorAndNotAnEmptyProxy) {
  TempFile proxy(".mp4");
  const auto result = write_proxy("Z:/definitely/not/here.mp4", proxy.string());
  ASSERT_FALSE(result.has_value());
  EXPECT_FALSE(result.error().empty());
  EXPECT_FALSE(proxy.exists());
}

TEST(Proxy, StandsInForRealFootageAtTheSameLength) {
  // The tests above make their sources with this application's own encoder,
  // which writes perfect constant-rate video — so they cannot tell whether the
  // timing is preserved or merely never disturbed. A camera file can, and this
  // is the only place the difference shows.
  const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
  if (dir == nullptr) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";
  const std::filesystem::path original = std::filesystem::path(dir) / "Boiler.mp4";
  if (!std::filesystem::exists(original)) GTEST_SKIP() << "no reference footage at " << original;

  TempFile proxy(".mp4");
  const auto result = write_proxy(original.string(), proxy.string(), {.height = 180});
  ASSERT_TRUE(result.has_value()) << result.error();

  const auto before = probe(original.string());
  const auto after = probe(proxy.string());
  ASSERT_TRUE(before.has_value()) << before.error();
  ASSERT_TRUE(after.has_value()) << after.error();

  ASSERT_NE(before->primary_video(), nullptr);
  ASSERT_NE(after->primary_video(), nullptr);
  EXPECT_EQ(after->primary_video()->height, 180);
  EXPECT_LT(after->primary_video()->width, before->primary_video()->width);

  // Within a frame. Any more and cuts made against the proxy land somewhere
  // else once the original is switched back on, which is the failure this whole
  // arrangement exists to avoid.
  const double tolerance = 2.0 / std::max(1.0, before->primary_video()->fps);
  EXPECT_NEAR(after->duration, before->duration, tolerance)
      << "the proxy is not the same length as the footage it stands in for";

  EXPECT_LT(std::filesystem::file_size(proxy.string()),
            std::filesystem::file_size(original) / 2)
      << "a proxy that is not much smaller has bought nothing";
}

TEST(Proxy, TheDefaultPathSitsBesideTheFootage) {
  const std::string path = default_proxy_path("D:/Footage/Card 1/A001.MXF");
  EXPECT_EQ(std::filesystem::path(path).extension(), ".mp4")
      << "a proxy is written as mp4 whatever the original was";
  EXPECT_EQ(std::filesystem::path(path).parent_path().filename(), "Proxies");
  EXPECT_EQ(std::filesystem::path(path).stem(), "A001");
}

TEST(Proxy, AChosenFolderIsWhereItGoesInstead) {
  const std::string path = default_proxy_path("D:/Footage/Card 1/A001.MXF", "E:/Fast/Proxies");
  EXPECT_EQ(std::filesystem::path(path).parent_path(), "E:/Fast/Proxies");
  EXPECT_EQ(std::filesystem::path(path).extension(), ".mp4");
  EXPECT_NE(std::filesystem::path(path).stem().string().find("A001"), std::string::npos)
      << "the name should still read as the file it stands for";
}

TEST(Proxy, TwoCardsWithTheSameFilenameDoNotCollideInAChosenFolder) {
  // The hazard a folder introduces and beside-the-footage cannot have: two
  // cards both holding A001.MXF are two files beside their own footage and one
  // file in a folder somebody chose, and the second would silently be the
  // first — a proxy showing the wrong shot, which is the worst thing this
  // feature could do.
  const std::string first = default_proxy_path("D:/Card 1/A001.MXF", "E:/Proxies");
  const std::string second = default_proxy_path("D:/Card 2/A001.MXF", "E:/Proxies");
  EXPECT_NE(first, second);

  // And the same source twice is the same proxy, or nothing would ever be
  // found again after it was made.
  EXPECT_EQ(default_proxy_path("D:/Card 1/A001.MXF", "E:/Proxies"), first);
}

TEST(Proxy, TheSameSourceIsTheSameProxyHoweverThePathIsWritten) {
  // Windows takes either slash, and a proxy made through one and looked for
  // through the other would be transcoded twice.
  EXPECT_EQ(default_proxy_path("D:/Card 1/A001.MXF", "E:/Proxies"),
            default_proxy_path(R"(D:\Card 1\A001.MXF)", "E:/Proxies"));
}

}  // namespace
}  // namespace cutline::media
