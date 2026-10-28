/// Every kind of file somebody is likely to drop in, opened the way the
/// application opens it.
///
/// The fixtures are tiny and committed, unlike the reference footage, because
/// what is being checked is a property of the *format* rather than of a real
/// capture — and several of them cannot be written by this project's own
/// FFmpeg build, which has no mp3 encoder. `fixtures/make.sh` regenerates them.
///
/// This exists because an mp3 would not import and nothing had ever tried one.
/// The cause was cover art: an ID3 sleeve is a one-frame video stream, and it
/// made a song a video file.

#include "cutline/media/audio.hpp"
#include "cutline/media/decoder.hpp"
#include "cutline/media/probe.hpp"
#include "cutline/media/thumbnail.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace cutline::media {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string fixture(const char* name) {
  return (fs::path(CUTLINE_MEDIA_FIXTURES) / name).string();
}

[[nodiscard]] float loudest(const AudioBuffer& audio) {
  float peak = 0.0f;
  for (const float sample : audio.samples) peak = std::max(peak, std::abs(sample));
  return peak;
}

// ---------------------------------------------------------------- sound --

struct SoundFile {
  const char* name;
  double seconds;

  friend void PrintTo(const SoundFile& file, std::ostream* out) { *out << file.name; }
};

class Sound : public ::testing::TestWithParam<SoundFile> {};

TEST_P(Sound, IsSoundAndNothingElse) {
  const auto info = probe(fixture(GetParam().name));
  ASSERT_TRUE(info.has_value()) << info.error();
  // The whole of the mp3 fault: art in the tag must not make this a picture.
  EXPECT_FALSE(info->has_video()) << GetParam().name;
  ASSERT_EQ(info->audio.size(), 1u);
  EXPECT_NEAR(info->duration, GetParam().seconds, 0.1);
}

TEST_P(Sound, DecodesToAToneRatherThanSilence) {
  const auto audio = decode_audio(fixture(GetParam().name));
  ASSERT_TRUE(audio.has_value()) << audio.error();
  EXPECT_NEAR(audio->duration(), GetParam().seconds, 0.1);
  // The fixtures are a sine at an eighth of full scale; anything well above
  // silence says the samples are real rather than a buffer of zeroes.
  EXPECT_GT(loudest(*audio), 0.05f);
}

TEST_P(Sound, HasAWaveform) {
  const auto peaks = extract_waveform(fixture(GetParam().name));
  ASSERT_TRUE(peaks.has_value()) << peaks.error();
  EXPECT_FALSE(peaks->empty());
}

TEST_P(Sound, OffersNoPictureToDecode) {
  // A decoder asked for the picture of a song has to say there is none, rather
  // than handing back the sleeve as a frame of footage.
  EXPECT_FALSE(VideoDecoder::open(fixture(GetParam().name),
                                  {.preferred = Acceleration::Software})
                   .has_value());
}

INSTANTIATE_TEST_SUITE_P(Formats, Sound,
                         ::testing::Values(SoundFile{"tone.mp3", 1.0},
                                           SoundFile{"tone-art.mp3", 1.0},
                                           SoundFile{"tone-art.m4a", 1.0},
                                           SoundFile{"tone-art.flac", 1.0},
                                           SoundFile{"tone.ogg", 1.0},
                                           SoundFile{"tone.opus", 1.0},
                                           SoundFile{"tone-s24.wav", 0.25},
                                           SoundFile{"tone-f32.wav", 0.25},
                                           SoundFile{"tone.aif", 0.25}),
                         [](const auto& info) {
                           std::string name = info.param.name;
                           std::ranges::replace_if(
                               name, [](char c) { return c == '.' || c == '-'; }, '_');
                           return name;
                         });

// -------------------------------------------------------------- picture --

class Picture : public ::testing::TestWithParam<const char*> {};

TEST_P(Picture, ProbesAsAPicture) {
  const auto info = probe(fixture(GetParam()));
  ASSERT_TRUE(info.has_value()) << info.error();
  ASSERT_TRUE(info->has_video());
  EXPECT_EQ(info->primary_video()->width, 64);
  EXPECT_EQ(info->primary_video()->height, 36);
}

TEST_P(Picture, DecodesAFrame) {
  auto decoder = VideoDecoder::open(fixture(GetParam()), {.preferred = Acceleration::Software});
  ASSERT_TRUE(decoder.has_value()) << decoder.error();
  const auto frame = (*decoder)->next_frame();
  ASSERT_TRUE(frame.has_value()) << frame.error();
  EXPECT_TRUE(*frame);
}

TEST_P(Picture, HasAThumbnail) {
  const auto strip = extract_thumbnails(fixture(GetParam()), 1, {.height = 18});
  ASSERT_TRUE(strip.has_value()) << strip.error();
  ASSERT_EQ(strip->size(), 1u);
  EXPECT_EQ(strip->front().height, 18);
}

INSTANTIATE_TEST_SUITE_P(Formats, Picture,
                         ::testing::Values("prores.mov", "vp9.webm", "hevc10.mkv", "bars.gif",
                                           "still.jpg", "still.webp"),
                         [](const auto& info) {
                           std::string name = info.param;
                           std::ranges::replace(name, '.', '_');
                           return name;
                         });

TEST(Formats, TenBitFootageSaysSo) {
  // HDR and 10-bit work start here: a file has to report its depth before
  // anything can be done about it.
  const auto info = probe(fixture("hevc10.mkv"));
  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_EQ(info->primary_video()->color.bits_per_component, 10);
}

TEST(Formats, ATimecodeTrackIsRead) {
  // What a multi-camera source sequence lines cameras up by, written as the
  // file writes it; a file with none says nothing.
  const auto plain = probe(fixture("timecode.mov"));
  ASSERT_TRUE(plain.has_value()) << plain.error();
  EXPECT_EQ(plain->timecode, "10:00:01:12");
  const auto drop = probe(fixture("timecode-df.mov"));
  ASSERT_TRUE(drop.has_value()) << drop.error();
  EXPECT_EQ(drop->timecode, "01:00:00;04");
  EXPECT_EQ(drop->video.size(), 1u) << "the timecode track is not a picture";
  const auto none = probe(fixture("vp9.webm"));
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_EQ(none->timecode, "");
}

TEST(Formats, FootageWithSoundKeepsBoth) {
  const auto info = probe(fixture("prores.mov"));
  ASSERT_TRUE(info.has_value()) << info.error();
  EXPECT_TRUE(info->has_video());
  EXPECT_EQ(info->audio.size(), 1u);
}

}  // namespace
}  // namespace cutline::media
