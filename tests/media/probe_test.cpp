/// Media-layer tests run against real captures rather than synthetic clips,
/// because the things worth checking — long-GOP behaviour, colour tagging,
/// multi-stream audio — are properties of real encoder output.
///
/// The footage is not in the repository, so these skip unless
/// CUTLINE_TEST_MEDIA_DIR points at a directory holding it. A skip is a real
/// result here: it says the check did not run, rather than quietly passing.

#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <process.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace cutline::media {
namespace {

namespace fs = std::filesystem;

/// The 8-second 4K60 HEVC clip behind the original slow-export report.
constexpr const char* kShortClip = "Boiler.mp4";

[[nodiscard]] fs::path media_dir() {
  const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
  return dir == nullptr ? fs::path{} : fs::path(dir);
}

[[nodiscard]] std::string reference_clip(const char* name) {
  const fs::path dir = media_dir();
  if (dir.empty()) return {};
  const fs::path candidate = dir / name;
  std::error_code ec;
  return fs::exists(candidate, ec) ? candidate.string() : std::string{};
}

class ReferenceMedia : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = reference_clip(kShortClip);
    if (path_.empty()) {
      GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to a directory containing " << kShortClip;
    }
  }
  std::string path_;
};

TEST_F(ReferenceMedia, ProbeReadsStructure) {
  const auto info = probe(path_);
  ASSERT_TRUE(info.has_value()) << info.error();

  EXPECT_GT(info->duration, 0.0);
  ASSERT_TRUE(info->has_video());

  const VideoStreamInfo* video = info->primary_video();
  EXPECT_EQ(video->width, 3840);
  EXPECT_EQ(video->height, 2160);
  EXPECT_NEAR(video->fps, 60.0, 0.1);
  EXPECT_EQ(video->codec, "hevc");
}

// The check that would have caught this footage being mistagged: HDR is decided
// by the transfer function, never by bit depth.
TEST_F(ReferenceMedia, ProbeReadsColourMetadata) {
  const auto info = probe(path_);
  ASSERT_TRUE(info.has_value()) << info.error();

  const ColorInfo& color = info->primary_video()->color;
  EXPECT_EQ(color.primaries, ColorPrimaries::Bt709);
  EXPECT_EQ(color.transfer, TransferCharacteristic::Bt709);
  EXPECT_EQ(color.bits_per_component, 8);
  EXPECT_FALSE(color.is_hdr());
}

TEST_F(ReferenceMedia, DecodesFramesInOrder) {
  auto decoder = VideoDecoder::open(path_);
  ASSERT_TRUE(decoder.has_value()) << decoder.error();

  double previous = -1.0;
  int frames = 0;
  for (; frames < 120; ++frames) {
    const auto got = (*decoder)->next_frame();
    ASSERT_TRUE(got.has_value()) << got.error();
    if (!*got) break;

    EXPECT_NE((*decoder)->frame(), nullptr);
    // Presentation order must be monotonic even though HEVC decodes B-frames
    // out of order.
    EXPECT_GT((*decoder)->timestamp(), previous);
    previous = (*decoder)->timestamp();
  }
  EXPECT_EQ(frames, 120);
}

// ----------------------------------------------------------- hardware decode --

TEST_F(ReferenceMedia, SoftwareDecodingOffersNoHardwareTexture) {
  auto decoder = VideoDecoder::open(path_, {.preferred = Acceleration::Software});
  ASSERT_TRUE(decoder.has_value()) << decoder.error();
  ASSERT_TRUE((*decoder)->next_frame().value_or(false));

  EXPECT_EQ((*decoder)->acceleration(), Acceleration::Software);
  EXPECT_FALSE((*decoder)->hardware_texture().has_value());
}

TEST_F(ReferenceMedia, HardwareDecodingKeepsFramesOnTheGpu) {
  // Skips rather than fails where d3d12va is unavailable: support varies by
  // driver and by codec, and falling back is the designed behaviour.
  auto decoder = VideoDecoder::open(path_, {.preferred = Acceleration::D3D12Va});
  ASSERT_TRUE(decoder.has_value()) << decoder.error();
  if ((*decoder)->acceleration() != Acceleration::D3D12Va) {
    GTEST_SKIP() << "d3d12va decoding is not available here, got "
                 << to_string((*decoder)->acceleration());
  }

  ASSERT_TRUE((*decoder)->next_frame().value_or(false));

  const auto texture = (*decoder)->hardware_texture();
  ASSERT_TRUE(texture.has_value()) << "a hardware frame carried no texture";
  EXPECT_NE(texture->resource, nullptr);
  // The fence is what says when the frame is finished. Sampling without waiting
  // on it reads a half-decoded picture, intermittently.
  EXPECT_NE(texture->fence, nullptr);
  EXPECT_GE(texture->subresource, 0);
}

TEST_F(ReferenceMedia, HardwareDecodingProducesTheSameTimestamps) {
  // Whichever path the pixels take, the timeline must not shift.
  auto hardware = VideoDecoder::open(path_, {.preferred = Acceleration::D3D12Va});
  auto software = VideoDecoder::open(path_, {.preferred = Acceleration::Software});
  ASSERT_TRUE(hardware.has_value()) << hardware.error();
  ASSERT_TRUE(software.has_value()) << software.error();
  if ((*hardware)->acceleration() != Acceleration::D3D12Va) {
    GTEST_SKIP() << "d3d12va decoding is not available here";
  }

  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE((*hardware)->next_frame().value_or(false)) << "hardware frame " << i;
    ASSERT_TRUE((*software)->next_frame().value_or(false)) << "software frame " << i;
    EXPECT_NEAR((*hardware)->timestamp(), (*software)->timestamp(), 1e-9) << "frame " << i;
  }
}

TEST_F(ReferenceMedia, RequestingHardwareWithoutADeviceStillDecodes) {
  // No compositor device supplied: D3D12VA makes its own, which decodes fine
  // and merely cannot be sampled without a copy.
  auto decoder = VideoDecoder::open(path_, {.preferred = Acceleration::D3D12Va});
  ASSERT_TRUE(decoder.has_value()) << decoder.error();
  EXPECT_TRUE((*decoder)->next_frame().value_or(false));
}

TEST_F(ReferenceMedia, SeekLandsAtOrBeforeTheTarget) {
  auto decoder = VideoDecoder::open(path_);
  ASSERT_TRUE(decoder.has_value()) << decoder.error();

  constexpr double target = 4.0;
  ASSERT_TRUE((*decoder)->seek(target).has_value());

  const auto got = (*decoder)->next_frame();
  ASSERT_TRUE(got.has_value()) << got.error();
  ASSERT_TRUE(*got);
  // Seeking is to the enclosing keyframe, so the first frame after it is at or
  // before the requested time; reaching the exact frame means decoding forward.
  EXPECT_LE((*decoder)->timestamp(), target + 1e-6);
}

TEST_F(ReferenceMedia, DecodingRunsToTheEndOfTheStream) {
  auto decoder = VideoDecoder::open(path_);
  ASSERT_TRUE(decoder.has_value()) << decoder.error();

  int frames = 0;
  while (true) {
    const auto got = (*decoder)->next_frame();
    ASSERT_TRUE(got.has_value()) << got.error();
    if (!*got) break;
    ++frames;
    ASSERT_LT(frames, 100000) << "decoder never reported end of stream";
  }
  // Eight seconds at 60 fps, allowing for the encoder's exact frame count.
  EXPECT_GT(frames, 400);
  EXPECT_LT(frames, 600);
}

TEST_F(ReferenceMedia, SoftwareDecodeIsAlwaysAvailable) {
  auto decoder = VideoDecoder::open(path_, {.preferred = Acceleration::Software});
  ASSERT_TRUE(decoder.has_value()) << decoder.error();
  EXPECT_EQ((*decoder)->acceleration(), Acceleration::Software);

  const auto got = (*decoder)->next_frame();
  ASSERT_TRUE(got.has_value()) << got.error();
  EXPECT_TRUE(*got);
}

TEST(Probe, ReportsAnErrorForAMissingFile) {
  const auto info = probe("Z:/definitely/not/here.mkv");
  EXPECT_FALSE(info.has_value());
}

TEST(Probe, ReportsAnErrorForANonMediaFile) {
  // Named for this process: `ctest -j` runs the suites as separate processes,
  // and a fixed name is one file two of them write and delete underneath each
  // other.
  const fs::path temp =
      fs::temp_directory_path() / ("cutline_not_media_" + std::to_string(_getpid()) + ".txt");
  {
    std::ofstream out(temp);
    out << "this is not a video";
  }
  const auto info = probe(temp.string());
  EXPECT_FALSE(info.has_value());
  std::error_code ec;
  fs::remove(temp, ec);
}

}  // namespace
}  // namespace cutline::media
