#include "cutline/media/audio.hpp"

#include "cutline/media/probe.hpp"
#include "cutline/media/thumbnail.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include <string>

namespace cutline::media {
namespace {

namespace fs = std::filesystem;

constexpr const char* kShortClip = "Boiler.mp4";

[[nodiscard]] std::string reference_clip() {
  const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
  if (dir == nullptr) return {};
  const fs::path candidate = fs::path(dir) / kShortClip;
  std::error_code ec;
  return fs::exists(candidate, ec) ? candidate.string() : std::string{};
}

// ------------------------------------------------------- peaks, pure maths --
//
// Peak reduction is arithmetic over a buffer, so it is tested with buffers
// built here rather than decoded from a file.

AudioBuffer make_buffer(std::vector<float> samples, int channels = 1, int rate = 48000) {
  AudioBuffer buffer;
  buffer.channels = channels;
  buffer.sample_rate = rate;
  buffer.samples = std::move(samples);
  return buffer;
}

TEST(Peaks, EmptyAudioYieldsNoBuckets) {
  EXPECT_TRUE(compute_peaks(make_buffer({})).empty());
}

TEST(Peaks, CaptureTheExtremesOfEachBucket) {
  // Ten samples per bucket at 100 buckets per second and 1000 Hz.
  const AudioBuffer audio = make_buffer({0.0f, 0.5f, -0.25f, 0.0f, 0.0f,
                                         0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                         0.1f, 0.2f, -0.9f, 0.0f, 0.0f,
                                         0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                                        1, 1000);
  const WaveformPeaks peaks = compute_peaks(audio, 100);

  ASSERT_EQ(peaks.size(), 2u);
  EXPECT_FLOAT_EQ(peaks.maximum[0], 0.5f);
  EXPECT_FLOAT_EQ(peaks.minimum[0], -0.25f);
  EXPECT_FLOAT_EQ(peaks.maximum[1], 0.2f);
  EXPECT_FLOAT_EQ(peaks.minimum[1], -0.9f);
}

// A transient in one channel has to survive, which is why the envelope folds
// channels together instead of averaging them.
TEST(Peaks, FoldChannelsTogether) {
  const AudioBuffer audio = make_buffer({0.0f, 1.0f, 0.0f, -1.0f}, 2, 2);
  const WaveformPeaks peaks = compute_peaks(audio, 1);

  ASSERT_EQ(peaks.size(), 1u);
  EXPECT_FLOAT_EQ(peaks.maximum[0], 1.0f);
  EXPECT_FLOAT_EQ(peaks.minimum[0], -1.0f);
}

TEST(Peaks, TrailingPartialBucketIsKept) {
  // Twelve frames at ten per bucket leaves a partial final bucket.
  std::vector<float> samples(12, 0.25f);
  samples.back() = 0.75f;
  const WaveformPeaks peaks = compute_peaks(make_buffer(std::move(samples), 1, 1000), 100);

  ASSERT_EQ(peaks.size(), 2u);
  EXPECT_FLOAT_EQ(peaks.maximum[1], 0.75f);
}

TEST(Peaks, BucketRateIsClampedToSomethingUsable) {
  const AudioBuffer audio = make_buffer(std::vector<float>(100, 0.1f), 1, 1000);
  EXPECT_FALSE(compute_peaks(audio, 0).empty());
  EXPECT_FALSE(compute_peaks(audio, -5).empty());
}

TEST(Peaks, BucketCountTracksDuration) {
  // One second at 1000 Hz, sampled at 100 buckets per second.
  const AudioBuffer audio = make_buffer(std::vector<float>(1000, 0.0f), 1, 1000);
  EXPECT_EQ(compute_peaks(audio, 100).size(), 100u);
}

TEST(AudioBufferShape, ReportsFramesAndDuration) {
  const AudioBuffer audio = make_buffer(std::vector<float>(96000, 0.0f), 2, 48000);
  EXPECT_EQ(audio.frame_count(), 48000u);
  EXPECT_DOUBLE_EQ(audio.duration(), 1.0);
}

// ------------------------------------------------------- against real media --

class ReferenceAudio : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = reference_clip();
    if (path_.empty()) {
      GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to a directory containing " << kShortClip;
    }
  }
  std::string path_;
};

TEST_F(ReferenceAudio, DecodesToTheRequestedFormat) {
  const auto audio = decode_audio(path_);
  ASSERT_TRUE(audio.has_value()) << audio.error();

  EXPECT_EQ(audio->sample_rate, 48000);
  EXPECT_EQ(audio->channels, 2);
  EXPECT_GT(audio->frame_count(), 0u);

  // The clip is eight seconds; the decoded length should agree closely.
  EXPECT_NEAR(audio->duration(), 8.0, 0.2);
}

TEST_F(ReferenceAudio, ResamplesToADifferentRate) {
  const auto audio = decode_audio(path_, 0, {.sample_rate = 24000, .channels = 1});
  ASSERT_TRUE(audio.has_value()) << audio.error();

  EXPECT_EQ(audio->sample_rate, 24000);
  EXPECT_EQ(audio->channels, 1);
  EXPECT_NEAR(audio->duration(), 8.0, 0.2);
}

TEST_F(ReferenceAudio, SamplesStayWithinRange) {
  const auto audio = decode_audio(path_);
  ASSERT_TRUE(audio.has_value()) << audio.error();

  for (const float sample : audio->samples) {
    ASSERT_TRUE(std::isfinite(sample));
    ASSERT_GE(sample, -4.0f);  // generous: float output is not hard-clipped
    ASSERT_LE(sample, 4.0f);
  }
}

TEST_F(ReferenceAudio, RejectsAnAbsentStream) {
  EXPECT_FALSE(decode_audio(path_, 99).has_value());
}

TEST_F(ReferenceAudio, WaveformSpansTheWholeClip) {
  const auto peaks = extract_waveform(path_, 0, 100);
  ASSERT_TRUE(peaks.has_value()) << peaks.error();

  // Eight seconds at a hundred buckets per second.
  EXPECT_NEAR(static_cast<double>(peaks->size()), 800.0, 20.0);
  EXPECT_EQ(peaks->minimum.size(), peaks->maximum.size());

  // Real audio is not silence, so something has to be non-zero.
  const bool has_signal = std::ranges::any_of(peaks->maximum, [](float v) { return v > 0.001f; });
  EXPECT_TRUE(has_signal);
}

// ------------------------------------------------------------- thumbnails --

class ReferenceThumbnails : public ReferenceAudio {};

TEST_F(ReferenceThumbnails, ExtractsTheRequestedCount) {
  const auto thumbnails = extract_thumbnails(path_, 5, {.height = 48});
  ASSERT_TRUE(thumbnails.has_value()) << thumbnails.error();
  ASSERT_EQ(thumbnails->size(), 5u);

  for (const Thumbnail& t : *thumbnails) {
    EXPECT_EQ(t.height, 48);
    EXPECT_EQ(t.width, 85);  // 48 * 3840/2160, rounded
    EXPECT_EQ(t.rgba.size(), static_cast<std::size_t>(t.width) * t.height * 4);
  }
}

TEST_F(ReferenceThumbnails, AreSpreadAcrossTheClipInOrder) {
  const auto thumbnails = extract_thumbnails(path_, 4, {.height = 32});
  ASSERT_TRUE(thumbnails.has_value()) << thumbnails.error();
  ASSERT_EQ(thumbnails->size(), 4u);

  double previous = -1.0;
  for (const Thumbnail& t : *thumbnails) {
    EXPECT_GT(t.timestamp, previous);
    previous = t.timestamp;
  }
  // Sampled at bucket centres, so the first is not at zero and the last is not
  // at the very end.
  EXPECT_GT(thumbnails->front().timestamp, 0.0);
  EXPECT_LT(thumbnails->back().timestamp, 8.0);
}

TEST_F(ReferenceThumbnails, HonourARange) {
  const auto thumbnails = extract_thumbnails(path_, 3, {.height = 32, .start = 2.0, .end = 4.0});
  ASSERT_TRUE(thumbnails.has_value()) << thumbnails.error();
  ASSERT_EQ(thumbnails->size(), 3u);

  for (const Thumbnail& t : *thumbnails) {
    EXPECT_GE(t.timestamp, 2.0);
    EXPECT_LE(t.timestamp, 4.0);
  }
}

TEST_F(ReferenceThumbnails, DecodeActualPictureContent) {
  const auto thumbnails = extract_thumbnails(path_, 1, {.height = 32});
  ASSERT_TRUE(thumbnails.has_value()) << thumbnails.error();
  ASSERT_EQ(thumbnails->size(), 1u);

  // A uniformly zero buffer would mean the scaler ran but decoded nothing.
  const auto& rgba = thumbnails->front().rgba;
  const bool has_content = std::ranges::any_of(rgba, [](std::uint8_t v) { return v != 0; });
  EXPECT_TRUE(has_content);

  // RGBA alpha is opaque throughout.
  for (std::size_t i = 3; i < rgba.size(); i += 4) ASSERT_EQ(rgba[i], 255);
}

TEST_F(ReferenceThumbnails, ZeroCountIsEmptyRatherThanAnError) {
  const auto thumbnails = extract_thumbnails(path_, 0);
  ASSERT_TRUE(thumbnails.has_value());
  EXPECT_TRUE(thumbnails->empty());
}

TEST(Thumbnails, RejectAMissingFile) {
  EXPECT_FALSE(extract_thumbnails("Z:/definitely/not/here.mkv", 3).has_value());
}

}  // namespace
}  // namespace cutline::media
