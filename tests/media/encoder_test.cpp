/// Encoder round-trips: what goes in has to come out.
///
/// These write a file and then decode it back, because that is the only way to
/// tell whether frames survived. An encoder that silently drops one is
/// indistinguishable from a working one until something counts.

#include "cutline/media/encoder.hpp"

#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cutline::media {
namespace {

constexpr int kWidth = 160;
constexpr int kHeight = 120;

/// A frame whose brightness encodes its index, so a decoded frame can be
/// identified. Kept well inside the legal range: 4:2:0 and studio-range
/// conversion both move values around, and this is about counting frames rather
/// than about colour accuracy.
[[nodiscard]] std::vector<std::uint8_t> numbered_frame(int index) {
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4);
  const auto value = static_cast<std::uint8_t>(32 + (index * 4) % 192);
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = value;
    rgba[i + 1] = value;
    rgba[i + 2] = value;
    rgba[i + 3] = 255;
  }
  return rgba;
}

/// A scratch path that cleans itself up.
class TempFile {
 public:
  explicit TempFile(std::string suffix)
      : path_(std::filesystem::temp_directory_path() /
              ("cutline_test_" + std::to_string(++counter_) + suffix)) {}

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

/// Decodes a file completely and returns how many frames came out.
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

[[nodiscard]] VideoEncodeSettings settings(int frames_per_second = 30) {
  VideoEncodeSettings s;
  s.width = kWidth;
  s.height = kHeight;
  s.fps = frames_per_second;
  // Software: the hardware encoders are not present on every machine, and this
  // is testing the muxing and frame accounting rather than any one encoder.
  s.preference = EncoderPreference::Software;
  return s;
}

/// Writes `count` frames and returns the path's decoded frame count.
[[nodiscard]] int round_trip(const std::string& path, int count,
                            VideoEncodeSettings encode) {
  auto writer = MediaWriter::create(path, encode);
  EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error());
  if (!writer) return -1;

  for (int i = 0; i < count; ++i) {
    const auto frame = numbered_frame(i);
    auto ok = (*writer)->write_frame(frame);
    EXPECT_TRUE(ok.has_value()) << (ok ? "" : ok.error());
    if (!ok) return -1;
  }

  auto finished = (*writer)->finish();
  EXPECT_TRUE(finished.has_value()) << (finished ? "" : finished.error());
  if (!finished) return -1;

  return count_frames(path);
}

// ------------------------------------------------------------------- counts --

TEST(Encoder, EveryFrameWrittenComesBackOut) {
  // The property the exporter depends on. A dropped frame shifts everything
  // after it, which on a long timeline is a drift nobody would trace back here.
  const TempFile file(".mp4");
  EXPECT_EQ(round_trip(file.string(), 90, settings()), 90);
}

TEST(Encoder, ASingleFrameSurvives) {
  const TempFile file(".mp4");
  EXPECT_EQ(round_trip(file.string(), 1, settings()), 1);
}

TEST(Encoder, AFrameCountSpanningSeveralKeyframeIntervalsSurvives) {
  const TempFile file(".mp4");
  EXPECT_EQ(round_trip(file.string(), 300, settings()), 300);
}

TEST(Encoder, TheFrameCounterMatchesWhatWasWritten) {
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();

  for (int i = 0; i < 12; ++i) {
    ASSERT_TRUE((*writer)->write_frame(numbered_frame(i)).has_value());
  }
  EXPECT_EQ((*writer)->frame_count(), 12);
  ASSERT_TRUE((*writer)->finish().has_value());
}

// ---------------------------------------------------------------- metadata --

TEST(Encoder, TheOutputHasTheRequestedShapeAndRate) {
  const TempFile file(".mp4");
  ASSERT_EQ(round_trip(file.string(), 60, settings(30)), 60);

  const auto info = probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();

  const auto* video = info->primary_video();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->width, kWidth);
  EXPECT_EQ(video->height, kHeight);
  EXPECT_NEAR(video->fps, 30.0, 0.1);
}

TEST(Encoder, TheOutputIsTaggedAsBt709Sdr) {
  // The compositor reads back sRGB; a stream tagged otherwise would be
  // displayed with the wrong transfer and look washed out or crushed.
  const TempFile file(".mp4");
  ASSERT_EQ(round_trip(file.string(), 10, settings()), 10);

  const auto info = probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();
  ASSERT_NE(info->primary_video(), nullptr);
  EXPECT_FALSE(info->primary_video()->color.is_hdr());
}

TEST(Encoder, DurationReflectsTheFrameCountAndRate) {
  const TempFile file(".mp4");
  ASSERT_EQ(round_trip(file.string(), 60, settings(30)), 60);

  const auto info = probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();
  // 60 frames at 30fps is two seconds. Allow one frame of slack for how
  // containers record the final sample's duration.
  EXPECT_NEAR(info->duration, 2.0, 1.0 / 30.0 + 1e-3);
}

// ------------------------------------------------------------------ codecs --

TEST(Encoder, HevcAlsoRoundTrips) {
  VideoEncodeSettings hevc = settings();
  hevc.codec = VideoCodec::Hevc;

  const TempFile file(".mp4");
  EXPECT_EQ(round_trip(file.string(), 30, hevc), 30);
}

TEST(Encoder, OddDimensionsAreAcceptedByRoundingDown) {
  // 4:2:0 chroma needs even dimensions, and failing deep inside the encoder
  // would be a confusing way to learn that.
  VideoEncodeSettings odd = settings();
  odd.width = kWidth + 1;
  odd.height = kHeight + 1;

  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), odd);
  ASSERT_TRUE(writer.has_value()) << writer.error();

  // The writer rounded the size down, so a frame of the rounded size is what it
  // now expects.
  const std::size_t rounded =
      static_cast<std::size_t>(odd.width & ~1) * (odd.height & ~1) * 4;
  const std::vector<std::uint8_t> frame(rounded, 128);
  ASSERT_TRUE((*writer)->write_frame(frame).has_value());
  ASSERT_TRUE((*writer)->finish().has_value());

  const auto info = probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();
  ASSERT_NE(info->primary_video(), nullptr);
  EXPECT_EQ(info->primary_video()->width % 2, 0);
  EXPECT_EQ(info->primary_video()->height % 2, 0);
}

// ------------------------------------------------------------------ errors --

TEST(Encoder, RejectsAnUnwritablePath) {
  const auto writer = MediaWriter::create("d:/no/such/directory/out.mp4", settings());
  EXPECT_FALSE(writer.has_value());
}

TEST(Encoder, RejectsAContainerItCannotInfer) {
  const auto writer = MediaWriter::create("output.unknown-extension", settings());
  EXPECT_FALSE(writer.has_value());
}

TEST(Encoder, RejectsAZeroSizedOutput) {
  VideoEncodeSettings empty = settings();
  empty.width = 0;

  const TempFile file(".mp4");
  EXPECT_FALSE(MediaWriter::create(file.string(), empty).has_value());
}

TEST(Encoder, RejectsAFrameOfTheWrongSize) {
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();

  const std::vector<std::uint8_t> too_small(16, 0);
  EXPECT_FALSE((*writer)->write_frame(too_small).has_value());
}

TEST(Encoder, WritingAfterFinishIsRefused) {
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();

  ASSERT_TRUE((*writer)->write_frame(numbered_frame(0)).has_value());
  ASSERT_TRUE((*writer)->finish().has_value());
  EXPECT_FALSE((*writer)->write_frame(numbered_frame(1)).has_value());
}

TEST(Encoder, FinishIsIdempotent) {
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();

  ASSERT_TRUE((*writer)->write_frame(numbered_frame(0)).has_value());
  EXPECT_TRUE((*writer)->finish().has_value());
  EXPECT_TRUE((*writer)->finish().has_value());
}

}  // namespace
}  // namespace cutline::media
