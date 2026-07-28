/// Time stretching. The whole point is that length changes and pitch does not,
/// so that is what these measure — the length by counting, the pitch with a
/// Goertzel filter, which answers "how much 440 Hz is in this?" without a full
/// spectrum.

#include "cutline/audio/stretch.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace cutline::audio {
namespace {

constexpr double kRate = 48000.0;
constexpr int kChannels = 2;

[[nodiscard]] std::vector<float> stereo_sine(double freq, std::size_t frames,
                                             double amplitude = 0.5) {
  std::vector<float> out(frames * kChannels);
  for (std::size_t i = 0; i < frames; ++i) {
    const double phase = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / kRate;
    const auto value = static_cast<float>(std::sin(phase) * amplitude);
    out[i * kChannels] = value;
    out[i * kChannels + 1] = value;
  }
  return out;
}

/// Energy at one frequency, by the Goertzel algorithm. Cheaper than an FFT and
/// exactly the question being asked.
[[nodiscard]] double energy_at(const std::vector<float>& interleaved, double freq) {
  const std::size_t frames = interleaved.size() / kChannels;
  if (frames == 0) return 0.0;

  const double omega = 2.0 * std::numbers::pi * freq / kRate;
  const double coefficient = 2.0 * std::cos(omega);

  double s1 = 0.0;
  double s2 = 0.0;
  for (std::size_t i = 0; i < frames; ++i) {
    const double s0 = static_cast<double>(interleaved[i * kChannels]) + coefficient * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double magnitude = std::sqrt(s1 * s1 + s2 * s2 - coefficient * s1 * s2);
  return magnitude / static_cast<double>(frames);
}

[[nodiscard]] std::size_t frames_of(const std::vector<float>& interleaved) {
  return interleaved.size() / kChannels;
}

[[nodiscard]] double peak_of(const std::vector<float>& samples) {
  double peak = 0.0;
  for (const float sample : samples) peak = std::max(peak, static_cast<double>(std::abs(sample)));
  return peak;
}

// ------------------------------------------------------------------ length --

TEST(TimeStretch, TheResultIsAsLongAsAsked) {
  const auto input = stereo_sine(440.0, 48000);

  for (const double factor : {0.5, 0.75, 1.5, 2.0}) {
    const auto out = time_stretch(input, kChannels, factor);
    const auto expected = static_cast<std::size_t>(48000.0 * factor);
    EXPECT_NEAR(static_cast<double>(frames_of(out)), static_cast<double>(expected), 2.0)
        << "factor " << factor;
  }
}

TEST(TimeStretch, AFactorOfOneChangesNothing) {
  const auto input = stereo_sine(440.0, 48000);
  const auto out = time_stretch(input, kChannels, 1.0);
  ASSERT_EQ(out.size(), input.size());
  for (std::size_t i = 0; i < input.size(); ++i) EXPECT_FLOAT_EQ(out[i], input[i]);
}

// ------------------------------------------------------------------- pitch --

TEST(TimeStretch, PitchSurvivesTheStretch) {
  // The property that makes this a speed change rather than a tape-speed
  // effect. Resampling would put a 2x clip's energy at 880 Hz instead.
  const auto input = stereo_sine(440.0, 96000);

  for (const double factor : {0.5, 2.0}) {
    const auto out = time_stretch(input, kChannels, factor);
    const double at_440 = energy_at(out, 440.0);
    const double at_880 = energy_at(out, 880.0);
    const double at_220 = energy_at(out, 220.0);

    EXPECT_GT(at_440, at_880 * 5.0) << "factor " << factor << ": pitch rose";
    EXPECT_GT(at_440, at_220 * 5.0) << "factor " << factor << ": pitch fell";
  }
}

TEST(TimeStretch, TheToneIsStillThereAtFullStrength) {
  // A stretch that destroys the signal would pass the pitch test by having no
  // energy anywhere in particular.
  const auto input = stereo_sine(440.0, 96000);
  const double before = energy_at(input, 440.0);

  for (const double factor : {0.5, 1.25, 2.0}) {
    const auto out = time_stretch(input, kChannels, factor);
    EXPECT_GT(energy_at(out, 440.0), before * 0.7) << "factor " << factor;
  }
}

TEST(TimeStretch, TheLevelIsHeldAcrossTheWholeResult) {
  // Windowed overlap-add fades in and out over its first and last window unless
  // the window sum is divided back out. That would put a ~10 ms ramp on the
  // edge of every retimed clip.
  const auto input = stereo_sine(440.0, 96000);
  const auto out = time_stretch(input, kChannels, 2.0);
  ASSERT_GT(frames_of(out), 4800u);

  const std::vector<float> head(out.begin(), out.begin() + 2400 * kChannels);
  const std::vector<float> tail(out.end() - 2400 * kChannels, out.end());
  const std::vector<float> middle(out.begin() + frames_of(out) / 2 * kChannels,
                                  out.begin() + (frames_of(out) / 2 + 2400) * kChannels);

  EXPECT_NEAR(peak_of(head), peak_of(middle), 0.08);
  EXPECT_NEAR(peak_of(tail), peak_of(middle), 0.08);
}

TEST(TimeStretch, ChannelsStayInStep) {
  // The same window offset must be chosen for every channel, or a stereo pair
  // drifts apart into a phasey mess.
  std::vector<float> input(48000 * kChannels);
  for (std::size_t i = 0; i < 48000; ++i) {
    const double phase = 2.0 * std::numbers::pi * 300.0 * static_cast<double>(i) / kRate;
    input[i * kChannels] = static_cast<float>(std::sin(phase) * 0.5);
    input[i * kChannels + 1] = static_cast<float>(std::sin(phase) * 0.25);  // half as loud
  }

  const auto out = time_stretch(input, kChannels, 1.5);
  ASSERT_GT(frames_of(out), 1000u);

  // The right channel should still be exactly half the left, everywhere.
  for (std::size_t i = 1000; i < frames_of(out) - 1000; i += 97) {
    const float left = out[i * kChannels];
    if (std::abs(left) < 0.05f) continue;  // near a zero crossing, the ratio is noise
    EXPECT_NEAR(out[i * kChannels + 1] / left, 0.5f, 0.02f) << "frame " << i;
  }
}

// -------------------------------------------------------------- bad inputs --

TEST(TimeStretch, NonsenseFactorsReturnTheInputUnchanged) {
  const auto input = stereo_sine(440.0, 48000);
  for (const double factor : {0.0, -1.0}) {
    const auto out = time_stretch(input, kChannels, factor);
    EXPECT_EQ(out.size(), input.size()) << "factor " << factor;
  }
}

TEST(TimeStretch, AnInputTooShortToWindowIsReturnedUnchanged) {
  const auto input = stereo_sine(440.0, 100);
  const auto out = time_stretch(input, kChannels, 2.0);
  EXPECT_EQ(out.size(), input.size());
}

TEST(TimeStretch, AnEmptyInputIsHandled) {
  const std::vector<float> empty;
  EXPECT_TRUE(time_stretch(empty, kChannels, 2.0).empty());
}

TEST(TimeStretch, MonoWorksToo) {
  std::vector<float> mono(48000);
  for (std::size_t i = 0; i < mono.size(); ++i) {
    const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / kRate;
    mono[i] = static_cast<float>(std::sin(phase) * 0.5);
  }

  const auto out = time_stretch(mono, 1, 2.0);
  EXPECT_NEAR(static_cast<double>(out.size()), 96000.0, 2.0);
  for (const float sample : out) ASSERT_TRUE(std::isfinite(sample));
}

TEST(TimeStretch, TheOutputStaysFinite) {
  const auto input = stereo_sine(440.0, 48000);
  for (const double factor : {0.25, 0.5, 1.1, 4.0}) {
    const auto out = time_stretch(input, kChannels, factor);
    for (const float sample : out) ASSERT_TRUE(std::isfinite(sample)) << "factor " << factor;
    EXPECT_LT(peak_of(out), 1.0) << "factor " << factor;
  }
}

}  // namespace
}  // namespace cutline::audio
