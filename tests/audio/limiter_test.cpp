/// The master limiter. Its whole job is a promise — nothing leaves above the
/// ceiling — so most of these check that promise against material designed to
/// break it.

#include "cutline/audio/limiter.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

namespace cutline::audio {
namespace {

constexpr double kRate = 48000.0;
constexpr int kChannels = 2;

[[nodiscard]] Limiter limiter(double limit = 0.95) {
  LimiterSettings settings;
  settings.limit = limit;
  return Limiter(settings, kRate, kChannels);
}

[[nodiscard]] std::vector<float> stereo(std::size_t frames, float value) {
  return std::vector<float>(frames * kChannels, value);
}

[[nodiscard]] std::vector<float> stereo_sine(double freq, double amplitude,
                                             std::size_t frames) {
  std::vector<float> out(frames * kChannels);
  for (std::size_t i = 0; i < frames; ++i) {
    const double phase = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / kRate;
    const auto value = static_cast<float>(std::sin(phase) * amplitude);
    out[i * kChannels] = value;
    out[i * kChannels + 1] = value;
  }
  return out;
}

[[nodiscard]] float peak_of(const std::vector<float>& samples, std::size_t from_frame = 0) {
  float peak = 0.0f;
  for (std::size_t i = from_frame * kChannels; i < samples.size(); ++i) {
    peak = std::max(peak, std::abs(samples[i]));
  }
  return peak;
}

// ------------------------------------------------------------------ ceiling --

TEST(Limiter, NothingEscapesAboveTheCeiling) {
  auto lim = limiter(0.95);
  auto samples = stereo_sine(220.0, 4.0, 48000);  // four times over full scale
  lim.process(samples);
  EXPECT_LE(peak_of(samples), 0.95f + 1e-5f);
}

TEST(Limiter, ASuddenTransientDoesNotSlipThrough) {
  // The reason for look-ahead. A compressor's attack would let the first few
  // milliseconds of this past the ceiling; a limiter must not.
  auto lim = limiter(0.95);

  auto quiet = stereo(4800, 0.05f);
  lim.process(quiet);

  auto spike = stereo(480, 0.0f);
  spike[0] = 8.0f;
  spike[1] = 8.0f;
  lim.process(spike);

  EXPECT_LE(peak_of(spike), 0.95f + 1e-5f);
}

TEST(Limiter, TheCeilingIsConfigurable) {
  auto lim = limiter(0.5);
  auto samples = stereo_sine(220.0, 2.0, 24000);
  lim.process(samples);
  EXPECT_LE(peak_of(samples), 0.5f + 1e-5f);
}

// ----------------------------------------------------------------- transparency --

TEST(Limiter, QuietMaterialPassesThroughUnchanged) {
  // A mix that never approaches the ceiling should come out bit for bit the
  // same, only delayed. A limiter that colours quiet material is a compressor.
  auto lim = limiter();
  const std::size_t latency = lim.latency_frames();

  auto samples = stereo_sine(440.0, 0.3, 9600);
  const auto original = samples;
  lim.process(samples);

  for (std::size_t frame = latency; frame < 9600; ++frame) {
    const std::size_t out_index = frame * kChannels;
    const std::size_t in_index = (frame - latency) * kChannels;
    ASSERT_NEAR(samples[out_index], original[in_index], 1e-6f) << "frame " << frame;
  }
}

TEST(Limiter, TheOutputIsDelayedByTheLookAhead) {
  auto lim = limiter();
  EXPECT_GT(lim.latency_frames(), 0u);

  auto samples = stereo(1000, 0.5f);
  lim.process(samples);

  // The first frames out are the silence that was in the look-ahead.
  EXPECT_FLOAT_EQ(samples[0], 0.0f);
  EXPECT_NEAR(samples[999 * kChannels], 0.5f, 1e-6f);
}

TEST(Limiter, FlushRecoversTheTail) {
  // Without this the last few milliseconds of a timeline are simply lost.
  auto lim = limiter();
  const std::size_t latency = lim.latency_frames();

  auto samples = stereo(2000, 0.4f);
  lim.process(samples);

  std::vector<float> tail(latency * kChannels);
  lim.flush(tail);

  EXPECT_NEAR(tail[0], 0.4f, 1e-6f);
  EXPECT_NEAR(tail[(latency - 1) * kChannels], 0.4f, 1e-6f);
}

// -------------------------------------------------------------- behaviour --

TEST(Limiter, ChannelsAreLinked) {
  // One gain per frame, from the loudest channel, so limiting never shifts the
  // stereo image sideways.
  auto lim = limiter();

  std::vector<float> samples(4800 * kChannels);
  for (std::size_t i = 0; i < 4800; ++i) {
    samples[i * kChannels] = 3.0f;
    samples[i * kChannels + 1] = 0.3f;
  }
  lim.process(samples);

  const std::size_t last = 4799 * kChannels;
  ASSERT_GT(samples[last + 1], 0.0f);
  EXPECT_NEAR(samples[last] / samples[last + 1], 10.0f, 0.01f);
}

TEST(Limiter, GainRecoversAfterAPeakPasses) {
  auto lim = limiter();

  auto loud = stereo_sine(220.0, 4.0, 4800);
  lim.process(loud);

  // A second of quiet material afterwards should be back at its own level, not
  // still ducked by the burst. Measured past the first tenth of a second, since
  // the start of this buffer is where the burst's own tail emerges from the
  // look-ahead and where the release is still running.
  auto quiet = stereo_sine(220.0, 0.2, 48000);
  lim.process(quiet);
  EXPECT_NEAR(peak_of(quiet, 4800), 0.2f, 0.01f);
}

TEST(Limiter, ProcessingIsUnaffectedByBlockBoundaries) {
  const auto run = [](std::vector<std::size_t> block_frames) {
    auto lim = limiter();
    auto samples = stereo_sine(300.0, 2.0, 6000);
    std::span<float> rest(samples);
    for (const std::size_t frames : block_frames) {
      lim.process(rest.subspan(0, frames * kChannels));
      rest = rest.subspan(frames * kChannels);
    }
    lim.process(rest);
    return samples;
  };

  const auto whole = run({});
  const auto split = run({137, 2000, 11});
  for (std::size_t i = 0; i < whole.size(); ++i) EXPECT_FLOAT_EQ(split[i], whole[i]) << "at " << i;
}

TEST(Limiter, ResetReturnsItToItsInitialState) {
  auto lim = limiter();
  auto loud = stereo_sine(220.0, 6.0, 4800);
  lim.process(loud);
  lim.reset();

  auto after = stereo(2400, 0.5f);
  lim.process(after);

  auto fresh = limiter();
  auto reference = stereo(2400, 0.5f);
  fresh.process(reference);

  for (std::size_t i = 0; i < after.size(); ++i) EXPECT_FLOAT_EQ(after[i], reference[i]);
}

TEST(Limiter, AShortBlockIsIgnoredRatherThanReadingPastTheEnd) {
  auto lim = limiter();
  std::vector<float> single(1, 0.5f);
  lim.process(single);
  EXPECT_FLOAT_EQ(single[0], 0.5f);
}

TEST(Limiter, TheOutputStaysFinite) {
  auto lim = limiter();
  auto samples = stereo_sine(50.0, 40.0, 48000);  // absurdly hot
  lim.process(samples);
  for (const float sample : samples) ASSERT_TRUE(std::isfinite(sample));
}

}  // namespace
}  // namespace cutline::audio
