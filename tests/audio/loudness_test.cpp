/// Loudness to BS.1770.
///
/// The standard publishes compliance material with expected readings, and the
/// cases below are the ones that can be generated rather than downloaded: a
/// full-scale sine, a level change, and silence. They pin the parts that are
/// easy to get wrong — the offset that sets where the scale sits, and the two
/// gates.

#include "cutline/audio/loudness.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace cutline::audio {
namespace {

constexpr double kRate = 48000.0;
constexpr int kChannels = 2;

/// Interleaved stereo sine of a given amplitude, `seconds` long.
[[nodiscard]] std::vector<float> sine(double amplitude, double seconds, double freq = 1000.0) {
  const auto frames = static_cast<std::size_t>(seconds * kRate);
  std::vector<float> out(frames * kChannels);
  for (std::size_t i = 0; i < frames; ++i) {
    const double phase = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / kRate;
    const auto value = static_cast<float>(std::sin(phase) * amplitude);
    out[i * kChannels] = value;
    out[i * kChannels + 1] = value;
  }
  return out;
}

[[nodiscard]] double measure(const std::vector<float>& samples) {
  LoudnessMeter meter(kRate, kChannels);
  meter.process(samples);
  return meter.integrated_lufs();
}

TEST(Loudness, AFullScaleSineOnOneChannelReadsWhereTheStandardSaysItShould) {
  // BS.1770's own anchor, and it is stated for *one* channel: a 0 dBFS 1 kHz
  // sine applied to the left, centre or right reads -3.01 LKFS. Feeding both
  // channels is 3 dB louder and reads about zero, which is what the test below
  // asserts — getting those two the wrong way round is the easiest mistake to
  // make here and the one that would put every other number out by three.
  std::vector<float> left = sine(1.0, 5.0);
  for (std::size_t i = 1; i < left.size(); i += kChannels) left[i] = 0.0f;

  EXPECT_NEAR(measure(left), -3.01, 0.5);
}

TEST(Loudness, TheSameSineOnBothChannelsIsThreeDecibelsLouder) {
  EXPECT_NEAR(measure(sine(1.0, 5.0)), 0.0, 0.5);
}

TEST(Loudness, HalvingTheAmplitudeCostsSixDecibels) {
  const double loud = measure(sine(1.0, 5.0));
  const double quiet = measure(sine(0.5, 5.0));
  EXPECT_NEAR(loud - quiet, 6.02, 0.3);
}

TEST(Loudness, SilenceIsNotAQuietProgramme) {
  // It has no loudness at all, and saying "-70" rather than a huge negative
  // number is what keeps every caller from testing for an empty measurement.
  EXPECT_DOUBLE_EQ(measure(std::vector<float>(kChannels * 48000, 0.0f)), kAbsoluteGateLufs);
}

TEST(Loudness, AMeasurementTooShortToMeanAnythingSaysSo) {
  // Under one block, there is nothing to average.
  LoudnessMeter meter(kRate, kChannels);
  meter.process(sine(1.0, 0.2));
  EXPECT_EQ(meter.blocks(), 0u);
  EXPECT_DOUBLE_EQ(meter.integrated_lufs(), kAbsoluteGateLufs);
}

TEST(Loudness, TheRelativeGateIgnoresTheQuietParts) {
  // A programme that is loud for five seconds and nearly silent for twenty
  // should read close to the loud part. Without the relative gate the long
  // quiet stretch would drag it down, and the number would be about how much
  // room tone there is rather than about the programme.
  std::vector<float> mixed = sine(1.0, 5.0);
  const std::vector<float> quiet = sine(0.002, 20.0);
  mixed.insert(mixed.end(), quiet.begin(), quiet.end());

  EXPECT_NEAR(measure(mixed), 0.0, 1.5);
}

TEST(Loudness, TheAbsoluteGateThrowsAwaySilenceRatherThanAveragingIt) {
  std::vector<float> mixed = sine(1.0, 5.0);
  const std::vector<float> nothing(kChannels * static_cast<std::size_t>(20 * kRate), 0.0f);
  mixed.insert(mixed.end(), nothing.begin(), nothing.end());

  EXPECT_NEAR(measure(mixed), 0.0, 1.0);
}

TEST(Loudness, SplittingTheInputChangesNothing) {
  // A caller mixes in blocks of whatever size it likes, and the answer cannot
  // depend on that.
  const std::vector<float> whole = sine(0.7, 4.0);

  LoudnessMeter once(kRate, kChannels);
  once.process(whole);

  LoudnessMeter split(kRate, kChannels);
  const std::size_t chunk = (whole.size() / kChannels / 7) * kChannels;
  for (std::size_t at = 0; at < whole.size(); at += chunk) {
    split.process(std::span(whole).subspan(at, std::min(chunk, whole.size() - at)));
  }

  EXPECT_NEAR(once.integrated_lufs(), split.integrated_lufs(), 0.01);
}

TEST(Loudness, KWeightingLiftsTheTopAndCutsTheBottom) {
  // The whole reason loudness is not RMS: a rumble and a presence-band tone at
  // the same energy are not equally loud.
  const double low = measure(sine(0.5, 5.0, 40.0));
  const double mid = measure(sine(0.5, 5.0, 1000.0));
  const double high = measure(sine(0.5, 5.0, 6000.0));

  EXPECT_LT(low, mid - 6.0) << "38 Hz high-pass takes the rumble down";
  EXPECT_GT(high, mid + 1.0) << "the shelf lifts the top end";
}

TEST(Loudness, ResetForgetsEverything) {
  LoudnessMeter meter(kRate, kChannels);
  meter.process(sine(1.0, 3.0));
  ASSERT_GT(meter.blocks(), 0u);
  meter.reset();
  EXPECT_EQ(meter.blocks(), 0u);
  EXPECT_DOUBLE_EQ(meter.integrated_lufs(), kAbsoluteGateLufs);
}

TEST(LoudnessOffset, ItIsTheDifference) {
  EXPECT_NEAR(loudness_offset_db(-20.0, -14.0), 6.0, 1e-9);
  EXPECT_NEAR(loudness_offset_db(-9.0, -23.0), -14.0, 1e-9);
}

TEST(LoudnessOffset, AnUnmeasurableProgrammeIsLeftAlone) {
  // Amplifying silence by the difference to the target turns its noise floor
  // into the programme.
  EXPECT_DOUBLE_EQ(loudness_offset_db(kAbsoluteGateLufs, -14.0), 0.0);
  EXPECT_DOUBLE_EQ(loudness_offset_db(-std::numeric_limits<double>::infinity(), -14.0), 0.0);
}

}  // namespace
}  // namespace cutline::audio
