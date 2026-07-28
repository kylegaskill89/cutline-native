/// Filters are tested by their frequency response, not their coefficients.
///
/// Asserting that a high-pass attenuates 20 Hz and passes 1 kHz says what the
/// filter *is*. Asserting a coefficient equals 0.9987 only says it was typed in
/// the same way twice, and still passes when the formula is wrong.

#include "cutline/audio/biquad.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace cutline::audio {
namespace {

constexpr double kRate = 48000.0;

/// Decibels of gain at a frequency, which is how filter specs are written.
[[nodiscard]] double db_at(const Biquad& filter, double freq) {
  return filter.gain_db_at(freq, kRate);
}

/// A sine at `freq`, one second long.
[[nodiscard]] std::vector<float> sine(double freq, std::size_t samples = 4800) {
  std::vector<float> out(samples);
  for (std::size_t i = 0; i < samples; ++i) {
    const double phase = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / kRate;
    out[i] = static_cast<float>(std::sin(phase));
  }
  return out;
}

/// Peak amplitude of the last quarter of a buffer, once the filter has settled.
/// Measuring from the start would include the transient, which is a real part
/// of the filter but not what a steady-state gain assertion is about.
[[nodiscard]] double settled_peak(const std::vector<float>& samples) {
  double peak = 0.0;
  for (std::size_t i = samples.size() * 3 / 4; i < samples.size(); ++i) {
    peak = std::max(peak, static_cast<double>(std::abs(samples[i])));
  }
  return peak;
}

// ------------------------------------------------------------------ passes --

TEST(Biquad, IdentityLeavesASignalAlone) {
  Biquad filter = Biquad::identity();
  auto samples = sine(1000.0);
  const auto original = samples;
  filter.process(samples);
  for (std::size_t i = 0; i < samples.size(); ++i) EXPECT_FLOAT_EQ(samples[i], original[i]);
}

TEST(Biquad, HighPassRemovesLowsAndKeepsHighs) {
  const Biquad filter = Biquad::high_pass(kRate, 100.0);
  EXPECT_LT(db_at(filter, 10.0), -30.0);   // two decades below: gone
  EXPECT_LT(db_at(filter, 50.0), -8.0);    // one octave below: well down
  EXPECT_NEAR(db_at(filter, 5000.0), 0.0, 0.1);  // far above: untouched
}

TEST(Biquad, LowPassRemovesHighsAndKeepsLows) {
  const Biquad filter = Biquad::low_pass(kRate, 8000.0);
  EXPECT_NEAR(db_at(filter, 100.0), 0.0, 0.1);
  EXPECT_LT(db_at(filter, 16000.0), -10.0);
}

TEST(Biquad, TheCornerIsThreeDecibelsDown) {
  // The definition of a Butterworth cutoff, and the check that catches an
  // off-by-2-pi in the frequency warping.
  for (const double corner : {80.0, 500.0, 4000.0}) {
    const Biquad high = Biquad::high_pass(kRate, corner);
    const Biquad low = Biquad::low_pass(kRate, corner);
    EXPECT_NEAR(db_at(high, corner), -3.0, 0.3) << "high-pass at " << corner;
    EXPECT_NEAR(db_at(low, corner), -3.0, 0.3) << "low-pass at " << corner;
  }
}

TEST(Biquad, TheRollOffIsTwelveDecibelsPerOctave) {
  // Two poles, so the skirt falls at 12 dB per octave once it is well past the
  // corner. This is what distinguishes a biquad from a one-pole filter.
  const Biquad filter = Biquad::high_pass(kRate, 1000.0);
  const double at_125 = db_at(filter, 125.0);
  const double at_250 = db_at(filter, 250.0);
  EXPECT_NEAR(at_250 - at_125, 12.0, 1.0);
}

// ----------------------------------------------------------------- shelves --

TEST(Biquad, ALowShelfLiftsBassAndLeavesTrebleAlone) {
  const Biquad filter = Biquad::low_shelf(kRate, 100.0, 12.0);
  EXPECT_NEAR(db_at(filter, 10.0), 12.0, 0.5);     // deep in the shelf
  EXPECT_NEAR(db_at(filter, 10000.0), 0.0, 0.5);   // above it, unchanged
}

TEST(Biquad, AHighShelfLiftsTrebleAndLeavesBassAlone) {
  const Biquad filter = Biquad::high_shelf(kRate, 3000.0, 12.0);
  EXPECT_NEAR(db_at(filter, 20000.0), 12.0, 0.5);
  EXPECT_NEAR(db_at(filter, 50.0), 0.0, 0.5);
}

TEST(Biquad, ShelvesCutAsWellAsBoost) {
  const Biquad filter = Biquad::low_shelf(kRate, 100.0, -12.0);
  EXPECT_NEAR(db_at(filter, 10.0), -12.0, 0.5);
}

TEST(Biquad, AShelfIsHalfItsGainAtTheCorner) {
  // The defining property of the cookbook shelf, and what a shelf drawn in a UI
  // has to line up with.
  const Biquad filter = Biquad::low_shelf(kRate, 200.0, 12.0);
  EXPECT_NEAR(db_at(filter, 200.0), 6.0, 1.0);
}

// -------------------------------------------------------------- peak/notch --

TEST(Biquad, APeakingFilterBoostsOnlyItsBand) {
  const Biquad filter = Biquad::peaking(kRate, 1000.0, 9.0, 1.0);
  EXPECT_NEAR(db_at(filter, 1000.0), 9.0, 0.1);  // exactly the asked-for gain
  EXPECT_NEAR(db_at(filter, 50.0), 0.0, 0.5);
  EXPECT_NEAR(db_at(filter, 18000.0), 0.0, 0.5);
}

TEST(Biquad, AHigherQMakesANarrowerPeak) {
  const Biquad wide = Biquad::peaking(kRate, 1000.0, 12.0, 0.5);
  const Biquad narrow = Biquad::peaking(kRate, 1000.0, 12.0, 8.0);

  // Both hit the same gain at centre; only the skirt differs.
  EXPECT_NEAR(db_at(wide, 1000.0), 12.0, 0.1);
  EXPECT_NEAR(db_at(narrow, 1000.0), 12.0, 0.1);
  EXPECT_GT(db_at(wide, 500.0), db_at(narrow, 500.0));
}

TEST(Biquad, ANotchRejectsItsCentreFrequency) {
  const Biquad filter = Biquad::band_reject(kRate, 1000.0, 4.0);
  EXPECT_LT(db_at(filter, 1000.0), -40.0);
  EXPECT_NEAR(db_at(filter, 100.0), 0.0, 0.5);
  EXPECT_NEAR(db_at(filter, 10000.0), 0.0, 0.5);
}

// ------------------------------------------------------- response vs sound --

TEST(Biquad, TheMeasuredGainMatchesThePredictedResponse) {
  // Ties `magnitude_at` to what actually comes out of `process`. Without this,
  // every test above could agree with a response function that no filter
  // implements.
  const Biquad model = Biquad::low_pass(kRate, 1000.0);

  for (const double freq : {100.0, 1000.0, 4000.0}) {
    Biquad filter = model;
    auto samples = sine(freq);
    filter.process(samples);
    EXPECT_NEAR(settled_peak(samples), model.magnitude_at(freq, kRate), 0.02)
        << "at " << freq << " Hz";
  }
}

TEST(Biquad, FilteringIsUnaffectedByBlockBoundaries) {
  // The mixer picks its own block size; the sound must not depend on it.
  auto whole = sine(300.0);
  auto split = whole;

  Biquad a = Biquad::high_pass(kRate, 200.0);
  a.process(whole);

  Biquad b = Biquad::high_pass(kRate, 200.0);
  const std::span<float> all(split);
  b.process(all.subspan(0, 97));
  b.process(all.subspan(97, 1000));
  b.process(all.subspan(1097));

  for (std::size_t i = 0; i < whole.size(); ++i) EXPECT_FLOAT_EQ(split[i], whole[i]) << "at " << i;
}

TEST(Biquad, ResetClearsTheHistory) {
  Biquad filter = Biquad::high_pass(kRate, 500.0);
  auto first = sine(1000.0, 512);
  filter.process(first);

  filter.reset();
  auto second = sine(1000.0, 512);
  filter.process(second);

  Biquad fresh = Biquad::high_pass(kRate, 500.0);
  auto reference = sine(1000.0, 512);
  fresh.process(reference);

  for (std::size_t i = 0; i < second.size(); ++i) EXPECT_FLOAT_EQ(second[i], reference[i]);
}

TEST(Biquad, AFilterStaysStable) {
  // An unstable biquad diverges rather than merely sounding wrong, and it takes
  // the whole export with it. Worth asserting on the extremes of the ranges the
  // UI allows.
  for (const double freq : {20.0, 100.0, 20000.0, 23000.0}) {
    for (const double q : {0.1, 0.707, 20.0}) {
      Biquad filter = Biquad::peaking(kRate, freq, 24.0, q);
      auto samples = sine(1000.0, 48000);
      filter.process(samples);
      for (const float sample : samples) {
        ASSERT_TRUE(std::isfinite(sample)) << freq << " Hz, Q " << q;
      }
      EXPECT_LT(settled_peak(samples), 100.0) << freq << " Hz, Q " << q;
    }
  }
}

// -------------------------------------------------------------- bad inputs --

TEST(Biquad, NonsenseParametersFallBackToIdentity) {
  // A cutoff above Nyquist has no meaning and a zero Q divides by zero. Both
  // should pass audio through rather than produce silence or NaNs.
  for (const Biquad filter : {Biquad::high_pass(kRate, 40000.0), Biquad::low_pass(kRate, -5.0),
                              Biquad::peaking(kRate, 1000.0, 6.0, 0.0),
                              Biquad::low_pass(0.0, 1000.0)}) {
    EXPECT_NEAR(filter.gain_db_at(1000.0, kRate), 0.0, 1e-9);
  }
}

TEST(Biquad, DecibelConversionRoundTrips) {
  for (const double db : {-24.0, -6.0, 0.0, 6.0, 24.0}) {
    EXPECT_NEAR(linear_to_db(db_to_linear(db)), db, 1e-9);
  }
  EXPECT_NEAR(db_to_linear(0.0), 1.0, 1e-12);
  EXPECT_NEAR(db_to_linear(-6.0), 0.5, 0.01);
}

TEST(Biquad, SilenceHasAFiniteLevel) {
  // A meter has to draw something for silence, so the conversion is floored
  // rather than negative infinity.
  EXPECT_TRUE(std::isfinite(linear_to_db(0.0)));
  EXPECT_LT(linear_to_db(0.0), -100.0);
}

}  // namespace
}  // namespace cutline::audio
