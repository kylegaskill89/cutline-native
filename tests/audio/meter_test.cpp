/// The master meter. Everything interesting about it is ballistics — how fast
/// it rises and how slowly it falls — so most of these feed it a signal whose
/// level is known and then check the shape of what comes back, not just the
/// number.

#include "cutline/audio/meter.hpp"

#include "cutline/audio/biquad.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace cutline::audio {
namespace {

constexpr double kRate = 48000.0;
constexpr int kChannels = 2;

[[nodiscard]] std::vector<float> stereo(double seconds, float value) {
  return std::vector<float>(static_cast<std::size_t>(seconds * kRate) * kChannels, value);
}

/// One channel loud and the other quiet, to catch a meter that reads the whole
/// interleaved buffer as if it were one signal.
[[nodiscard]] std::vector<float> lopsided(double seconds, float left, float right) {
  const auto frames = static_cast<std::size_t>(seconds * kRate);
  std::vector<float> out(frames * kChannels);
  for (std::size_t i = 0; i < frames; ++i) {
    out[i * kChannels] = left;
    out[i * kChannels + 1] = right;
  }
  return out;
}

TEST(Meter, ReadsSilenceAsTheFloorRatherThanNegativeInfinity) {
  Meter meter(kRate, kChannels);
  const MeterReading reading = meter.read();

  EXPECT_EQ(reading.count, kChannels);
  EXPECT_DOUBLE_EQ(reading.channels[0].peak_db, kMeterFloorDb);
  EXPECT_DOUBLE_EQ(reading.channels[0].rms_db, kMeterFloorDb);
  EXPECT_FALSE(reading.channels[0].over);
}

TEST(Meter, ReachesAFullScaleSignalsLevelAtOnce) {
  Meter meter(kRate, kChannels);
  // A single millisecond: a peak reading that needed time to arrive would miss
  // exactly the transients it exists to catch.
  meter.process(stereo(0.001, 1.0f));

  EXPECT_NEAR(meter.read().channels[0].peak_db, 0.0, 0.01);
}

TEST(Meter, AveragesTowardsTheSignalRatherThanJumpingToIt) {
  Meter meter(kRate, kChannels);
  meter.process(stereo(0.001, 1.0f));

  // A millisecond into a 300 ms window, the average is nowhere near the peak.
  // That difference is the whole reason both readings exist.
  const ChannelLevel early = meter.read().channels[0];
  EXPECT_LT(early.rms_db, early.peak_db - 10.0);

  meter.process(stereo(2.0, 1.0f));
  EXPECT_NEAR(meter.read().channels[0].rms_db, 0.0, 0.5);
}

TEST(Meter, MeasuresEachChannelSeparately) {
  Meter meter(kRate, kChannels);
  meter.process(lopsided(0.5, 1.0f, 0.1f));

  const MeterReading reading = meter.read();
  EXPECT_NEAR(reading.channels[0].peak_db, 0.0, 0.01);
  EXPECT_NEAR(reading.channels[1].peak_db, linear_to_db(0.1), 0.01);
}

TEST(Meter, FallsAtTheRateItWasGiven) {
  MeterSettings settings;
  settings.fall_db_per_second = 20.0;
  Meter meter(kRate, kChannels, settings);

  meter.process(stereo(0.01, 1.0f));
  const double loud = meter.read().channels[0].peak_db;

  // Half a second of silence, so ten decibels of fall.
  meter.process(stereo(0.5, 0.0f));
  EXPECT_NEAR(meter.read().channels[0].peak_db, loud - 10.0, 0.2);
}

TEST(Meter, HoldsThePeakStillBeforeLettingItGo) {
  MeterSettings settings;
  settings.hold_seconds = 1.0;
  settings.hold_fall_db_per_second = 8.0;
  Meter meter(kRate, kChannels, settings);

  meter.process(stereo(0.01, 1.0f));
  const double mark = meter.read().channels[0].hold_db;

  // Well inside the hold: the bar has fallen a long way and the mark has not
  // moved, which is what makes a peak readable at all.
  meter.process(stereo(0.9, 0.0f));
  const MeterReading held = meter.read();
  EXPECT_NEAR(held.channels[0].hold_db, mark, 0.01);
  EXPECT_LT(held.channels[0].peak_db, mark - 15.0);

  meter.process(stereo(1.0, 0.0f));
  EXPECT_LT(meter.read().channels[0].hold_db, mark - 5.0);
}

TEST(Meter, LatchesOverAndKeepsItLitAfterThePeakHasGone) {
  MeterSettings settings;
  settings.over = 0.95;
  Meter meter(kRate, kChannels, settings);

  meter.process(stereo(0.001, 0.9f));
  EXPECT_FALSE(meter.read().channels[0].over);

  meter.process(stereo(0.001, 0.96f));
  EXPECT_TRUE(meter.read().channels[0].over);

  // Seconds of silence later it is still lit. The peak that tripped it lasted
  // a millisecond; a warning lasting the same is one nobody sees.
  meter.process(stereo(3.0, 0.0f));
  EXPECT_TRUE(meter.read().channels[0].over);
}

TEST(Meter, ReadsNegativeSamplesAsLoudlyAsPositiveOnes) {
  Meter meter(kRate, kChannels);
  meter.process(stereo(0.01, -1.0f));

  EXPECT_NEAR(meter.read().channels[0].peak_db, 0.0, 0.01);
}

TEST(Meter, ForgetsEverythingWhenReset) {
  Meter meter(kRate, kChannels);
  meter.process(stereo(0.5, 1.0f));
  meter.reset();

  const MeterReading reading = meter.read();
  EXPECT_DOUBLE_EQ(reading.channels[0].peak_db, kMeterFloorDb);
  EXPECT_DOUBLE_EQ(reading.channels[0].hold_db, kMeterFloorDb);
  EXPECT_FALSE(reading.channels[0].over);
}

TEST(MeterFraction, PutsTheFloorAtNothingAndTheCeilingAtEverything) {
  EXPECT_DOUBLE_EQ(meter_fraction(-60.0, -60.0, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(meter_fraction(0.0, -60.0, 0.0), 1.0);
  EXPECT_DOUBLE_EQ(meter_fraction(-30.0, -60.0, 0.0), 0.5);
}

TEST(MeterFraction, ClampsRatherThanRunningOffTheScale) {
  EXPECT_DOUBLE_EQ(meter_fraction(-90.0, -60.0, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(meter_fraction(3.0, -60.0, 0.0), 1.0);
}

}  // namespace
}  // namespace cutline::audio
