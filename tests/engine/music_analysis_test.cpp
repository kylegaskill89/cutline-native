/// Remix from a file: a song written here, listened to through the decoder as
/// a clip of it would be, and remixed to a length on the timeline.

#include "cutline/engine/music_analysis.hpp"

#include "cutline/core/query.hpp"
#include "cutline/core/remix.hpp"
#include "cutline/media/encoder.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <numbers>
#include <process.h>
#include <string>
#include <vector>

namespace cutline::engine {
namespace {

constexpr int kRate = 48000;
constexpr double kBeat = 0.5;
constexpr int kBars = 24;

/// Forty-eight seconds at 120 beats a minute: a kick on each beat and a chord
/// a bar, the same chord throughout so any bar may follow any other.
[[nodiscard]] const std::string& song_path() {
  static const std::string path = [] {
    std::string out = (std::filesystem::temp_directory_path() /
                       ("cutline_remix_song_" + std::to_string(_getpid()) + ".mp4"))
                          .string();
    media::VideoEncodeSettings video;
    video.width = 64;
    video.height = 64;
    video.fps = 10;
    video.preference = media::EncoderPreference::Software;
    media::AudioEncodeSettings audio;
    audio.enabled = true;
    audio.sample_rate = kRate;
    audio.channels = 2;
    auto writer = media::MediaWriter::create(out, video, audio);
    if (!writer) return out;

    const auto total = static_cast<std::size_t>(kBars * 4 * kBeat * kRate);
    std::vector<float> mono(total, 0.0f);
    for (std::size_t i = 0; i < total; ++i) {
      const double t = static_cast<double>(i) / kRate;
      const double into = std::fmod(t, kBeat);
      double v = 0.6 * std::exp(-into / 0.06) *
                 std::sin(2.0 * std::numbers::pi * (55.0 + 60.0 * std::exp(-into / 0.02)) * into);
      for (const double f : {261.6, 329.6, 392.0}) v += 0.06 * std::sin(2.0 * std::numbers::pi * f * t);
      mono[i] = static_cast<float>(v);
    }
    const std::vector<std::uint8_t> frame(64 * 64 * 4, 128);
    for (std::size_t second = 0; second * kRate < total; ++second) {
      for (int f = 0; f < 10; ++f) {
        if (!writer.value()->write_frame(frame)) return out;
      }
      std::vector<float> block;
      for (std::size_t i = second * kRate; i < std::min(total, (second + 1) * kRate); ++i) {
        block.push_back(mono[i]);
        block.push_back(mono[i]);
      }
      if (!writer.value()->write_audio(block)) return out;
    }
    (void)writer.value()->finish();
    return out;
  }();
  return path;
}

/// The song on A1 from 1 s, its first forty seconds, and a clip on the same
/// track at 31 s that a remix may not run into.
[[nodiscard]] core::Project with_song(double next_at = 31.0) {
  core::Project p;
  core::Media song;
  song.id = "song";
  song.path = song_path();
  song.duration = kBars * 4 * kBeat;
  song.has_video = true;
  song.audio_stream_count = 1;
  p.media = {song};
  core::Track a1;
  a1.id = "a1";
  a1.kind = core::TrackKind::Audio;
  a1.clips.push_back(core::Clip{.id = "music",
                                .media_id = "song",
                                .kind = core::TrackKind::Audio,
                                .source_in = 0.0,
                                .source_out = 40.0,
                                .start = 1.0});
  a1.clips.push_back(core::Clip{.id = "next",
                                .media_id = "song",
                                .kind = core::TrackKind::Audio,
                                .source_in = 0.0,
                                .source_out = 1.0,
                                .start = next_at});
  p.sequence().tracks = {a1};
  return p;
}

TEST(MusicAnalysisFile, HearsTheBeatOfTheSongAClipPlays) {
  const auto heard = analyse_clip_music(with_song(), "music");
  ASSERT_TRUE(heard.has_value()) << heard.error();
  EXPECT_NEAR(heard->tempo, 120.0, 2.0);
  EXPECT_NEAR(heard->duration, 40.0, 0.05);
  ASSERT_TRUE(heard->usable());
}

TEST(MusicAnalysisFile, RemixesToALengthOnTheTimelineWithinTheRoomTheClipHas) {
  const core::Project p = with_song();
  const auto heard = analyse_clip_music(p, "music");
  ASSERT_TRUE(heard.has_value()) << heard.error();

  const auto shorter = make_remix(p, "music", *heard, 20.0, 50, 50);
  ASSERT_TRUE(shorter.has_value());
  EXPECT_NEAR(core::remix_length(*shorter), 20.0, 1.0);
  EXPECT_DOUBLE_EQ(shorter->spans.front().source_in, 0.0);
  EXPECT_DOUBLE_EQ(shorter->spans.back().source_out, 40.0);

  // Thirty-five asked for, thirty to be had before the next clip.
  const auto walled = make_remix(p, "music", *heard, 35.0, 50, 50);
  ASSERT_TRUE(walled.has_value());
  EXPECT_LE(core::remix_length(*walled), 30.0 + 1e-6);

  const core::Project applied = core::apply_remix(p, "music", *shorter);
  EXPECT_NEAR(core::clip_duration(*core::find_clip(applied, "music")), 20.0, 1.0);
}

TEST(MusicAnalysisFile, RemixesAFasterClipToTheLengthItWillPlayFor) {
  core::Project p = with_song(100.0);
  core::find_clip(p, "music")->speed = 2.0;
  const auto heard = analyse_clip_music(p, "music");
  ASSERT_TRUE(heard.has_value()) << heard.error();
  const auto remix = make_remix(p, "music", *heard, 12.0, 50, 50);
  ASSERT_TRUE(remix.has_value());
  const core::Project applied = core::apply_remix(p, "music", *remix);
  EXPECT_NEAR(core::clip_duration(*core::find_clip(applied, "music")), 12.0, 0.6);
}

}  // namespace
}  // namespace cutline::engine

