/// Exports, checked by reading the file back.
///
/// The export path had no test of its own until the output size became
/// something a person could choose: until then the only thing that could go
/// wrong was the encoder, which has its own tests. A size that the dialog
/// offers and the exporter ignores is exactly the sort of failure that looks
/// fine on screen, so it is checked against the file rather than the settings.
///
/// A colour matte stands in for footage, so these run without any. The ones
/// about audio need a real source — nothing generated makes a sound — and skip
/// unless CUTLINE_TEST_MEDIA_DIR points at the reference footage.

#include "cutline/engine/exporter.hpp"

#include "cutline/core/model.hpp"
#include "cutline/media/probe.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace cutline::engine {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

constexpr int kWidth = 128;
constexpr int kHeight = 64;

std::shared_ptr<gpu::Device> shared_device() {
  static std::shared_ptr<gpu::Device> device = [] {
    auto created = gpu::Device::create({.allow_software = true});
    return created ? *created : nullptr;
  }();
  return device;
}

/// A scratch path that cleans itself up.
class TempFile {
 public:
  explicit TempFile(std::string suffix)
      : path_(std::filesystem::temp_directory_path() /
              ("cutline_export_" + std::to_string(++counter_) + suffix)) {}

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] std::string string() const { return path_.string(); }

 private:
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

/// Half a second of a red matte, which is long enough to be a file and short
/// enough that a software encoder is not the slowest thing in the suite.
[[nodiscard]] Project matte_project() {
  Media m;
  m.id = "m";
  m.is_color = true;
  m.color = "#ff0000";

  Clip c;
  c.id = "c";
  c.media_id = "m";
  c.start = 0.0;
  c.source_in = 0.0;
  c.source_out = 0.5;

  Track t;
  t.id = "v1";
  t.kind = TrackKind::Video;
  t.clips = {std::move(c)};

  Project p;
  p.canvas_w = kWidth;
  p.canvas_h = kHeight;
  p.fps = 30.0;
  p.media = {std::move(m)};
  p.tracks = {std::move(t)};
  return p;
}

class ExportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    device_ = shared_device();
    if (!device_) GTEST_SKIP() << "no Direct3D 12 device available";
  }

  /// Exports and probes in one step, since every test here asks the same
  /// question of the result: what actually landed in the file.
  [[nodiscard]] std::expected<media::MediaInfo, std::string> write(
      const Project& project, ExportSettings settings, const TempFile& file) {
    settings.path = file.string();
    if (auto result = export_project(device_, project, settings); !result) {
      return std::unexpected(result.error());
    }
    return media::probe(file.string());
  }

  std::shared_ptr<gpu::Device> device_;
};

// ------------------------------------------------------------- resolution --

TEST_F(ExportTest, WritesTheProjectsOwnCanvasByDefault) {
  const TempFile file(".mp4");
  const auto info = write(matte_project(), {}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  const media::VideoStreamInfo* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->width, kWidth);
  EXPECT_EQ(video->height, kHeight);
}

TEST_F(ExportTest, ARequestedSizeIsTheSizeWritten) {
  const TempFile file(".mp4");
  const auto info = write(matte_project(), {.width = kWidth / 2, .height = kHeight / 2}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  const media::VideoStreamInfo* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->width, kWidth / 2);
  EXPECT_EQ(video->height, kHeight / 2);
}

TEST_F(ExportTest, AnOddSizeIsRoundedDownRatherThanRefused) {
  const TempFile file(".mp4");
  // Both codecs encode in even-sized blocks. The encoder rounds down to reach
  // one regardless; the exporter has to agree with it, or it would render
  // frames of a size the encoder is not expecting.
  const auto info = write(matte_project(), {.width = 65, .height = 33}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  const media::VideoStreamInfo* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->width, 64);
  EXPECT_EQ(video->height, 32);
}

TEST_F(ExportTest, ASizeTooSmallToRoundToIsRefused) {
  const TempFile file(".mp4");
  ExportSettings settings{.width = 1, .height = 1};
  settings.path = file.string();

  const auto result = export_project(device_, matte_project(), settings);
  ASSERT_FALSE(result.has_value()) << "one pixel rounds down to none";
  EXPECT_NE(result.error().find("two pixels"), std::string::npos) << result.error();
}

TEST_F(ExportTest, TheFrameRateIsTheProjectsUnlessAskedOtherwise) {
  const TempFile file(".mp4");
  const auto info = write(matte_project(), {.fps = 15.0}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  const media::VideoStreamInfo* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_NEAR(video->fps, 15.0, 0.01);
}

// ------------------------------------------------------------------ audio --

/// Audio needs a real source: a colour matte is silent, and a silent project
/// deliberately produces a file with no audio stream at all.
class ExportAudioTest : public ExportTest {
 protected:
  void SetUp() override {
    ExportTest::SetUp();
    const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
    if (dir == nullptr) {
      GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to a directory containing Boiler.mp4";
    }
    const std::filesystem::path source = std::filesystem::path(dir) / "Boiler.mp4";
    if (!std::filesystem::exists(source)) {
      GTEST_SKIP() << "no Boiler.mp4 in CUTLINE_TEST_MEDIA_DIR";
    }
    source_ = source.string();
  }

  [[nodiscard]] Project audible_project() const {
    Project p = matte_project();

    Media m;
    m.id = "sound";
    m.path = source_;
    m.duration = 60.0;
    m.has_video = true;
    m.audio_stream_count = 1;
    p.media.push_back(std::move(m));

    Clip c;
    c.id = "a";
    c.media_id = "sound";
    c.kind = TrackKind::Audio;
    c.audio_stream = 0;
    c.start = 0.0;
    c.source_in = 0.0;
    c.source_out = 0.5;

    Track t;
    t.id = "a1";
    t.kind = TrackKind::Audio;
    t.clips = {std::move(c)};
    p.tracks.push_back(std::move(t));
    return p;
  }

  std::string source_;
};

TEST_F(ExportAudioTest, StereoByDefault) {
  const TempFile file(".mp4");
  const auto info = write(audible_project(), {}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  ASSERT_EQ(info->audio.size(), 1u);
  EXPECT_EQ(info->audio.front().channels, 2);
}

TEST_F(ExportAudioTest, OneChannelMixesDownToMono) {
  const TempFile file(".mp4");
  const auto info = write(audible_project(), {.audio_channels = 1}, file);
  ASSERT_TRUE(info.has_value()) << info.error();

  ASSERT_EQ(info->audio.size(), 1u);
  EXPECT_EQ(info->audio.front().channels, 1);
}

TEST_F(ExportAudioTest, AudioTurnedOffLeavesNoStream) {
  const TempFile file(".mp4");
  const auto info = write(audible_project(), {.audio = false}, file);
  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_TRUE(info->audio.empty());
}

}  // namespace
}  // namespace cutline::engine
