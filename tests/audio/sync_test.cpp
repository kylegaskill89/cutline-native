/// Lining recordings up by their sound.
///
/// Two "cameras" hear one event through different microphones: one louder,
/// one muffled, each with its own hiss, started at different moments. The
/// offset found has to be the one they were started at, to within a couple of
/// milliseconds; recordings of different events have to be told apart rather
/// than lined up somewhere.

#include "cutline/audio/sync.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <random>
#include <vector>

namespace cutline::audio {
namespace {

constexpr int kRate = 8000;

/// Forty seconds of a room: claps, knocks and words, scattered.
[[nodiscard]] std::vector<float> event(unsigned seed) {
  std::mt19937 random(seed);
  std::uniform_real_distribution<double> when(0.0, 40.0);
  std::uniform_real_distribution<double> length(0.02, 0.4);
  std::uniform_real_distribution<double> pitch(150.0, 2000.0);
  std::vector<float> sound(static_cast<std::size_t>(40 * kRate), 0.0f);
  for (int k = 0; k < 60; ++k) {
    const auto start = static_cast<std::size_t>(when(random) * kRate);
    const auto span = static_cast<std::size_t>(length(random) * kRate);
    const double frequency = pitch(random);
    for (std::size_t i = 0; i < span && start + i < sound.size(); ++i) {
      const double t = static_cast<double>(i) / kRate;
      sound[start + i] += static_cast<float>(0.4 * std::exp(-t * 12.0) *
                                             std::sin(2.0 * 3.14159265 * frequency * t));
    }
  }
  return sound;
}

/// What a camera hears of it: from `from` to `to` seconds, at its own level,
/// through a microphone that smooths it by `muffle`, with its own hiss.
[[nodiscard]] std::vector<float> heard(const std::vector<float>& sound, double from, double to,
                                       double level, double muffle, unsigned seed) {
  std::mt19937 random(seed);
  std::normal_distribution<double> hiss(0.0, 0.01);
  std::vector<float> out;
  double smoothed = 0.0;
  for (auto i = static_cast<std::size_t>(from * kRate);
       i < static_cast<std::size_t>(to * kRate) && i < sound.size(); ++i) {
    smoothed += (1.0 - muffle) * (sound[i] - smoothed);
    out.push_back(static_cast<float>(level * smoothed + hiss(random)));
  }
  return out;
}

}  // namespace

TEST(AudioSync, TwoCamerasOfOneEventAreFoundTheirOffsetApart) {
  const std::vector<float> room = event(1);
  const std::vector<float> first = heard(room, 3.0, 25.0, 1.0, 0.0, 2);
  const std::vector<float> second = heard(room, 7.25, 38.0, 0.3, 0.7, 3);
  const auto match = match_envelopes(onset_envelope(first, kRate), onset_envelope(second, kRate));
  ASSERT_TRUE(match.has_value());
  // The second started 4.25 seconds into the first.
  EXPECT_NEAR(match->offset, 4.25, 0.003);
  EXPECT_GT(match->confidence, 1.5);
}

TEST(AudioSync, ACameraStartedFirstComesOutNegative) {
  const std::vector<float> room = event(4);
  const std::vector<float> first = heard(room, 10.0, 30.0, 1.0, 0.2, 5);
  const std::vector<float> second = heard(room, 2.5, 20.0, 0.6, 0.5, 6);
  const auto match = match_envelopes(onset_envelope(first, kRate), onset_envelope(second, kRate));
  ASSERT_TRUE(match.has_value());
  EXPECT_NEAR(match->offset, -7.5, 0.003);
}

TEST(AudioSync, DifferentEventsAreNotLinedUp) {
  const std::vector<float> one = heard(event(7), 0.0, 20.0, 1.0, 0.0, 8);
  const std::vector<float> other = heard(event(9), 0.0, 20.0, 1.0, 0.0, 10);
  EXPECT_FALSE(match_envelopes(onset_envelope(one, kRate), onset_envelope(other, kRate))
                   .has_value());
}

TEST(AudioSync, SilenceHasNothingToMatch) {
  const std::vector<float> one = heard(event(11), 0.0, 20.0, 1.0, 0.0, 12);
  const std::vector<float> quiet(static_cast<std::size_t>(20 * kRate), 0.0f);
  EXPECT_FALSE(match_envelopes(onset_envelope(one, kRate), onset_envelope(quiet, kRate))
                   .has_value());
}

}  // namespace cutline::audio
