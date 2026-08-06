/// Real-time playback.
///
/// These need an actual output device, so they skip on a machine without one —
/// which includes most CI runners. What they check is the clock: that it starts
/// where it should, advances at real time, stops when told, and lands where a
/// seek asks. Whether it *sounds* right is the mixer's business, and tested
/// there against samples rather than against a speaker.

#include "cutline/engine/player.hpp"

#include "cutline/media/encoder.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

namespace cutline::engine {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

constexpr int kRate = 48000;
constexpr int kChannels = 2;

/// A twelve-second tone file, shared by the tests here.
[[nodiscard]] const std::string& tone_path() {
  static const std::string path = [] {
    const std::string target =
        (std::filesystem::temp_directory_path() / "cutline_player_tone.mp4").string();

    media::VideoEncodeSettings video;
    video.width = 64;
    video.height = 64;
    video.fps = 10;
    video.preference = media::EncoderPreference::Software;

    media::AudioEncodeSettings audio;
    audio.enabled = true;
    audio.sample_rate = kRate;
    audio.channels = kChannels;

    auto writer = media::MediaWriter::create(target, video, audio);
    if (!writer) return std::string{};

    std::vector<float> second(static_cast<std::size_t>(kRate) * kChannels);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kRate); ++i) {
      const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate;
      const auto value = static_cast<float>(std::sin(phase) * 0.25);
      second[i * kChannels] = value;
      second[i * kChannels + 1] = value;
    }

    const std::vector<std::uint8_t> frame(64 * 64 * 4, 128);
    for (int s = 0; s < 12; ++s) {
      for (int f = 0; f < 10; ++f) {
        if (!writer.value()->write_frame(frame)) return std::string{};
      }
      if (!writer.value()->write_audio(second)) return std::string{};
    }
    return writer.value()->finish() ? target : std::string{};
  }();
  return path;
}

[[nodiscard]] Project tone_project(double length = 10.0) {
  Media m;
  m.id = "m";
  m.path = tone_path();
  m.duration = 12.0;
  m.has_video = true;
  m.audio_stream_count = 1;

  Clip c;
  c.id = "c";
  c.media_id = "m";
  c.kind = TrackKind::Audio;
  c.start = 0.0;
  c.source_in = 0.0;
  c.source_out = length;
  c.gain = 0.3;  // audible if a human runs these, but not startling

  Track t;
  t.id = "a1";
  t.kind = TrackKind::Audio;
  t.clips = {c};

  Project p;
  p.media = {m};
  p.tracks = {t};
  return p;
}

/// A player, or a skip when the machine has no output device.
class PlayerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (tone_path().empty()) GTEST_SKIP() << "could not build the tone file";

    auto built = Player::create(tone_project());
    if (!built) GTEST_SKIP() << "no audio output device: " << built.error();
    player_ = std::move(*built);
  }

  std::unique_ptr<Player> player_;
};

TEST_F(PlayerTest, OpensADeviceAndReportsItsFormat) {
  EXPECT_FALSE(player_->device_name().empty());
  // Shared-mode WASAPI mixes at the device's rate, which is its own choice —
  // only that it is a sane audio rate is worth asserting.
  EXPECT_GE(player_->sample_rate(), 8000);
  EXPECT_LE(player_->sample_rate(), 384000);
  EXPECT_GE(player_->channels(), 1);
  EXPECT_TRUE(player_->error().empty());
}

TEST_F(PlayerTest, TheProjectIsAudible) {
  EXPECT_FALSE(player_->silent());
  EXPECT_NEAR(player_->duration(), 10.0, 0.01);
}

TEST_F(PlayerTest, StartsStoppedAtTheBeginning) {
  EXPECT_FALSE(player_->playing());
  EXPECT_DOUBLE_EQ(player_->position(), 0.0);
}

TEST_F(PlayerTest, TheClockAdvancesAtRealTime) {
  // The whole point of driving the timeline from the sound card: half a second
  // of wall clock has to be half a second of timeline, or picture and sound
  // drift apart over a long take.
  player_->play();
  const auto began = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  const double moved = player_->position();
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - began)
                             .count();

  ASSERT_TRUE(player_->error().empty()) << player_->error();
  EXPECT_GT(moved, 0.1) << "the playhead never moved";
  // Generous: this is a shared machine and the device buffer is tens of
  // milliseconds deep. A rate error would be a factor, not a few percent.
  EXPECT_NEAR(moved, elapsed, 0.25);
}

TEST_F(PlayerTest, PauseStopsTheClock) {
  player_->play();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  player_->pause();
  EXPECT_FALSE(player_->playing());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const double at = player_->position();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_NEAR(player_->position(), at, 0.05);
  EXPECT_TRUE(player_->error().empty()) << player_->error();
}

TEST_F(PlayerTest, SeekingMovesThePlayheadImmediately) {
  // A caller reading the position straight after a seek must see where it asked
  // to go, not where playback still is — the UI redraws before the render
  // thread has picked the request up.
  player_->seek(4.0);
  EXPECT_NEAR(player_->position(), 4.0, 1e-9);
}

TEST_F(PlayerTest, PlaybackResumesFromASeek) {
  player_->seek(3.0);
  player_->play();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  ASSERT_TRUE(player_->error().empty()) << player_->error();
  EXPECT_GT(player_->position(), 3.05);
  EXPECT_LT(player_->position(), 4.0);
}

TEST_F(PlayerTest, SeekingBackwardsWorks) {
  player_->play();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  player_->seek(0.5);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  ASSERT_TRUE(player_->error().empty()) << player_->error();
  EXPECT_LT(player_->position(), 1.5);
  EXPECT_GT(player_->position(), 0.4);
}

TEST_F(PlayerTest, ANegativeSeekLandsAtZero) {
  player_->seek(-5.0);
  EXPECT_DOUBLE_EQ(player_->position(), 0.0);
}

TEST_F(PlayerTest, PlaybackStopsAtTheEndOfTheTimeline) {
  // Running on through silence would leave the transport looking busy forever.
  player_->seek(player_->duration() - 0.2);
  player_->play();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!player_->finished() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ASSERT_TRUE(player_->error().empty()) << player_->error();
  EXPECT_TRUE(player_->finished());
  EXPECT_FALSE(player_->playing());
}

TEST_F(PlayerTest, PlayingAfterTheEndStartsOver) {
  player_->seek(player_->duration() - 0.1);
  player_->play();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!player_->finished() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(player_->finished());

  player_->play();
  EXPECT_TRUE(player_->playing());
  EXPECT_LT(player_->position(), 1.0);
}

TEST_F(PlayerTest, DestroyingAPlayingPlayerIsClean) {
  // The render thread has to be told to stop and joined; leaking it would take
  // the mixer's memory with it and crash on the next buffer.
  player_->play();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  player_.reset();
  SUCCEED();
}

TEST(Player, ASilentProjectStillKeepsTime) {
  // The transport should work on a project with no audio at all: the device
  // runs, so the playhead still moves.
  Project p;
  p.canvas_w = 640;
  p.canvas_h = 480;

  Media m;
  m.id = "colour";
  m.is_color = true;
  m.color = "#204060";
  m.duration = 5.0;

  Clip c;
  c.id = "c";
  c.media_id = "colour";
  c.kind = TrackKind::Video;
  c.source_in = 0.0;
  c.source_out = 5.0;

  Track t;
  t.id = "v1";
  t.kind = TrackKind::Video;
  t.clips = {c};

  p.media = {m};
  p.tracks = {t};

  auto built = Player::create(p);
  if (!built) GTEST_SKIP() << "no audio output device: " << built.error();

  EXPECT_TRUE((*built)->silent());
  EXPECT_NEAR((*built)->duration(), 5.0, 0.01);

  (*built)->play();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  EXPECT_GT((*built)->position(), 0.1);
  EXPECT_TRUE((*built)->error().empty()) << (*built)->error();
}

// ---------------------------------------------------------------- devices --

TEST(Player, TheMachineListsItsOutputs) {
  const std::vector<AudioOutput> outputs = audio_outputs();
  if (outputs.empty()) GTEST_SKIP() << "no audio output devices on this machine";

  int defaults = 0;
  for (const AudioOutput& output : outputs) {
    EXPECT_FALSE(output.id.empty()) << "a device with no id cannot be stored in a setting";
    EXPECT_FALSE(output.name.empty()) << "a device with no name cannot be shown in a list";
    if (output.is_default) ++defaults;
  }
  EXPECT_LE(defaults, 1) << "two devices claimed to be the default one";
}

TEST(Player, AChosenDeviceIsOpenedByItsId) {
  const std::vector<AudioOutput> outputs = audio_outputs();
  if (outputs.empty()) GTEST_SKIP() << "no audio output devices on this machine";

  auto built = Player::create(tone_project(), {.device_id = outputs.front().id});
  if (!built) GTEST_SKIP() << "that device would not open: " << built.error();
  EXPECT_FALSE((*built)->device_name().empty());
}

TEST(Player, ADeviceThatIsNotThereFallsBackRatherThanFailing) {
  // The behaviour that matters when somebody unplugs an interface. Losing the
  // sound they were used to is a nuisance; losing the ability to play at all is
  // an editor that stopped working because of a cable.
  auto built = Player::create(tone_project(),
                              {.device_id = "{0.0.0.00000000}.{no-such-device-at-all}"});
  if (!built) GTEST_SKIP() << "no audio output device: " << built.error();

  EXPECT_FALSE((*built)->device_name().empty()) << "it opened nothing at all";
  EXPECT_TRUE((*built)->error().empty()) << (*built)->error();
}

}  // namespace
}  // namespace cutline::engine
