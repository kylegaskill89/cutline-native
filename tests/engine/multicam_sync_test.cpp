/// Synchronising a multi-camera shoot by sound, from files on disk: the same
/// room recorded by two cameras started at different moments, written out and
/// read back as any import would be.

#include "cutline/engine/multicam_sync.hpp"

#include "cutline/core/multicam.hpp"
#include "cutline/media/encoder.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>
#include <process.h>
#include <random>
#include <string>
#include <vector>

namespace cutline::engine {
namespace {

constexpr int kRate = 48000;

/// Thirty seconds of claps and knocks.
[[nodiscard]] std::vector<float> room() {
  std::mt19937 random(21);
  std::uniform_real_distribution<double> when(0.0, 30.0);
  std::uniform_real_distribution<double> pitch(200.0, 1500.0);
  std::vector<float> sound(static_cast<std::size_t>(30 * kRate), 0.0f);
  for (int k = 0; k < 50; ++k) {
    const auto start = static_cast<std::size_t>(when(random) * kRate);
    const double frequency = pitch(random);
    for (std::size_t i = 0; i < kRate / 4 && start + i < sound.size(); ++i) {
      const double t = static_cast<double>(i) / kRate;
      sound[start + i] += static_cast<float>(0.4 * std::exp(-t * 15.0) *
                                             std::sin(2.0 * std::numbers::pi * frequency * t));
    }
  }
  return sound;
}

/// A camera's file: a small grey picture and the room from `from` to `to`
/// seconds, at its own level.
[[nodiscard]] std::string camera(const std::vector<float>& sound, double from, double to,
                                 double level, const char* name) {
  const std::string path = (std::filesystem::temp_directory_path() /
                            std::format("cutline_{}_{}.mp4", name, _getpid()))
                               .string();
  media::VideoEncodeSettings video;
  video.width = 32;
  video.height = 32;
  video.fps = 10;
  video.preference = media::EncoderPreference::Software;
  media::AudioEncodeSettings audio;
  audio.enabled = true;
  audio.sample_rate = kRate;
  audio.channels = 2;
  auto writer = media::MediaWriter::create(path, video, audio);
  if (!writer) return {};
  const std::vector<std::uint8_t> frame(32 * 32 * 4, 128);
  const auto first = static_cast<std::size_t>(from * kRate);
  const auto seconds = static_cast<int>(to - from);
  for (int s = 0; s < seconds; ++s) {
    std::vector<float> samples(static_cast<std::size_t>(kRate) * 2);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kRate); ++i) {
      const std::size_t at = first + static_cast<std::size_t>(s) * kRate + i;
      const float v = at < sound.size() ? static_cast<float>(sound[at] * level) : 0.0f;
      samples[i * 2] = v;
      samples[i * 2 + 1] = v;
    }
    for (int f = 0; f < 10; ++f) {
      if (!writer.value()->write_frame(frame)) return {};
    }
    if (!writer.value()->write_audio(samples)) return {};
  }
  return writer.value()->finish().has_value() ? path : std::string{};
}

[[nodiscard]] core::Media source(std::string id, std::string path, double length) {
  core::Media m;
  m.id = std::move(id);
  m.name = m.id;
  m.path = std::move(path);
  m.duration = length;
  m.has_video = true;
  m.audio_stream_count = 1;
  return m;
}

}  // namespace

TEST(MulticamSync, CamerasAreStartedWhereTheirSoundAgrees) {
  const std::vector<float> sound = room();
  // The second camera was started three and a half seconds after the first.
  const std::string a = camera(sound, 2.0, 14.0, 1.0, "sync_a");
  const std::string b = camera(sound, 5.5, 17.5, 0.4, "sync_b");
  ASSERT_FALSE(a.empty());
  ASSERT_FALSE(b.empty());

  core::Project p;
  p.media = {source("a", a, 12.0), source("b", b, 12.0)};
  const std::vector<std::string> ids{"b", "a"};
  int told = 0;
  const auto starts = multicam_audio_starts(p, ids, [&](int done, int total) {
    told = done;
    EXPECT_EQ(total, 2);
    return true;
  });
  ASSERT_TRUE(starts.has_value()) << starts.error();
  EXPECT_EQ(told, 2);
  EXPECT_NEAR((*starts)[0], 3.5, 0.01);
  EXPECT_NEAR((*starts)[1], 0.0, 1e-9);

  std::error_code error;
  std::filesystem::remove(a, error);
  std::filesystem::remove(b, error);
}

TEST(MulticamSync, ASourceWithNoSoundIsNamed) {
  core::Project p;
  core::Media silent = source("quiet", "", 5.0);
  silent.name = "quiet.mp4";
  silent.audio_stream_count = 0;
  p.media = {silent};
  const std::vector<std::string> ids{"quiet"};
  const auto starts = multicam_audio_starts(p, ids);
  ASSERT_FALSE(starts.has_value());
  EXPECT_NE(starts.error().find("quiet.mp4"), std::string::npos);
}

}  // namespace cutline::engine
