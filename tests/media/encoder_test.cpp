/// Encoder round-trips: what goes in has to come out.
///
/// These write a file and then decode it back, because that is the only way to
/// tell whether frames survived. An encoder that silently drops one is
/// indistinguishable from a working one until something counts.

#include "cutline/media/encoder.hpp"

#include "cutline/media/audio.hpp"
#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <process.h>

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
  /// The process id is in the name, and it is not decoration. `ctest -j` runs
  /// these as separate *processes*, each with its own counter starting at one —
  /// so two of them write `cutline_test_1.mp4` at the same time and truncate
  /// each other's file. It fails as an unrelated-looking encoder assertion that
  /// passes the moment the test is run on its own.
  explicit TempFile(std::string suffix)
      : path_(std::filesystem::temp_directory_path() /
              ("cutline_test_" + std::to_string(_getpid()) + "_" +
               std::to_string(++counter_) + suffix)) {}

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

// ------------------------------------------------------------------- audio --

constexpr int kSampleRate = 48000;
constexpr int kAudioChannels = 2;

[[nodiscard]] AudioEncodeSettings audio_settings() {
  AudioEncodeSettings s;
  s.enabled = true;
  s.sample_rate = kSampleRate;
  s.channels = kAudioChannels;
  return s;
}

/// Interleaved stereo tone, so a decoded file can be told apart from silence.
[[nodiscard]] std::vector<float> tone(std::size_t frames) {
  std::vector<float> samples(frames * kAudioChannels);
  for (std::size_t i = 0; i < frames; ++i) {
    const auto value = static_cast<float>(
        0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(i) / kSampleRate));
    samples[i * kAudioChannels] = value;
    samples[i * kAudioChannels + 1] = value;
  }
  return samples;
}

TEST(Encoder, AWriterWithoutAudioProducesNoAudioStream) {
  // A project with nothing audible should give a video-only file rather than a
  // silent track nobody asked for.
  const TempFile file(".mp4");
  ASSERT_EQ(round_trip(file.string(), 10, settings()), 10);

  const auto info = probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_TRUE(info->audio.empty());
}

TEST(Encoder, AudioAppearsAsAStreamWithTheRequestedFormat) {
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings(30), audio_settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();
  ASSERT_TRUE((*writer)->has_audio());

  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE((*writer)->write_frame(numbered_frame(i)).has_value());
    ASSERT_TRUE((*writer)->write_audio(tone(kSampleRate / 30)).has_value());
  }
  ASSERT_TRUE((*writer)->finish().has_value());

  const auto info = probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();
  ASSERT_EQ(info->audio.size(), 1u);
  EXPECT_EQ(info->audio[0].sample_rate, kSampleRate);
  EXPECT_EQ(info->audio[0].channels, kAudioChannels);
  EXPECT_EQ(info->audio[0].codec, "aac");
}

TEST(Encoder, EveryAudioSampleWrittenIsAccountedFor) {
  // The counterpart of the video frame count. Audio arrives in blocks that do
  // not line up with the encoder's own 1024-sample frames, so the buffering is
  // exactly where samples would go missing.
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings(), audio_settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();

  std::int64_t written = 0;
  for (const std::size_t block : {1u, 100u, 1023u, 1024u, 1025u, 4096u, 7u}) {
    ASSERT_TRUE((*writer)->write_audio(tone(block)).has_value());
    written += static_cast<std::int64_t>(block);
  }
  ASSERT_TRUE((*writer)->write_frame(numbered_frame(0)).has_value());
  ASSERT_TRUE((*writer)->finish().has_value());

  // Every sample handed over reached the encoder, including the short final
  // block that does not fill one of its frames.
  EXPECT_EQ((*writer)->audio_frame_count(), written);
}

TEST(Encoder, AudioAndVideoEndUpTheSameLength) {
  // Drift between the two is the failure that matters here: a file whose audio
  // runs short goes quiet before the picture ends.
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings(30), audio_settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();

  constexpr int kFrames = 60;  // two seconds at 30fps
  for (int i = 0; i < kFrames; ++i) {
    ASSERT_TRUE((*writer)->write_frame(numbered_frame(i)).has_value());
    ASSERT_TRUE((*writer)->write_audio(tone(kSampleRate / 30)).has_value());
  }
  ASSERT_TRUE((*writer)->finish().has_value());

  EXPECT_EQ(count_frames(file.string()), kFrames);
  EXPECT_EQ((*writer)->audio_frame_count(), kSampleRate * 2);

  const auto info = probe(file.string());
  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_NEAR(info->duration, 2.0, 0.05);
}

TEST(Encoder, TheAudioThatComesBackIsNotSilence) {
  // Counting samples proves nothing about whether any signal survived.
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings(), audio_settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();
  ASSERT_TRUE((*writer)->write_frame(numbered_frame(0)).has_value());
  ASSERT_TRUE((*writer)->write_audio(tone(kSampleRate)).has_value());
  ASSERT_TRUE((*writer)->finish().has_value());

  const auto decoded = decode_audio(file.string(), 0,
                                    {.sample_rate = kSampleRate, .channels = kAudioChannels});
  ASSERT_TRUE(decoded.has_value()) << decoded.error();
  EXPECT_NEAR(decoded->duration(), 1.0, 0.1);

  float peak = 0.0f;
  for (const float sample : decoded->samples) peak = std::max(peak, std::abs(sample));
  EXPECT_GT(peak, 0.3f);
}

TEST(Encoder, WritingAudioToAVideoOnlyWriterIsRefused) {
  // Silently ignoring it would lose a whole export's sound with no sign.
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();
  EXPECT_FALSE((*writer)->has_audio());
  EXPECT_FALSE((*writer)->write_audio(tone(128)).has_value());
}

TEST(Encoder, APartialAudioFrameIsRefused) {
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings(), audio_settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();

  const std::vector<float> odd(2 * kAudioChannels + 1, 0.0f);
  EXPECT_FALSE((*writer)->write_audio(odd).has_value());
}

TEST(Encoder, AudioIsRefusedAfterFinish) {
  const TempFile file(".mp4");
  auto writer = MediaWriter::create(file.string(), settings(), audio_settings());
  ASSERT_TRUE(writer.has_value()) << writer.error();
  ASSERT_TRUE((*writer)->write_frame(numbered_frame(0)).has_value());
  ASSERT_TRUE((*writer)->write_audio(tone(1024)).has_value());
  ASSERT_TRUE((*writer)->finish().has_value());
  EXPECT_FALSE((*writer)->write_audio(tone(128)).has_value());
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
