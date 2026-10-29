/// Dialogue repair on sound made here: a tone in hiss, a voice-like burst in a
/// room, hum under a note, an "s" over a vowel, and rumble under the lot. Each
/// effect is asked to take out what it is for and to leave what it is not.

#include "cutline/audio/repair.hpp"

#include "cutline/audio/chain.hpp"
#include "cutline/audio/reverb.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <random>
#include <span>
#include <vector>

namespace cutline::audio {
namespace {

constexpr double kRate = 48000.0;

[[nodiscard]] std::size_t frames_of(double seconds) {
  return static_cast<std::size_t>(seconds * kRate);
}

void add_sine(std::vector<float>& into, double freq, double amplitude, double from = 0.0,
              double to = 1e9) {
  for (std::size_t i = 0; i < into.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    if (t < from || t >= to) continue;
    into[i] += static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * freq * t));
  }
}

void add_noise(std::vector<float>& into, double rms, unsigned seed, double from = 0.0,
               double to = 1e9) {
  std::mt19937 random(seed);
  std::normal_distribution<float> noise(0.0f, static_cast<float>(rms));
  for (std::size_t i = 0; i < into.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    const float n = noise(random);
    if (t >= from && t < to) into[i] += n;
  }
}

[[nodiscard]] double rms(std::span<const float> samples, double from, double to) {
  const std::size_t a = frames_of(from);
  const std::size_t b = std::min(samples.size(), frames_of(to));
  double sum = 0.0;
  for (std::size_t i = a; i < b; ++i) sum += static_cast<double>(samples[i]) * samples[i];
  return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, b - a)));
}

/// How strong one frequency is over a stretch, as an amplitude.
[[nodiscard]] double amplitude_at(std::span<const float> samples, double freq, double from,
                                  double to) {
  const std::size_t a = frames_of(from);
  const std::size_t b = std::min(samples.size(), frames_of(to));
  double re = 0.0;
  double im = 0.0;
  for (std::size_t i = a; i < b; ++i) {
    const double phase = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / kRate;
    re += samples[i] * std::cos(phase);
    im += samples[i] * std::sin(phase);
  }
  return 2.0 * std::hypot(re, im) / static_cast<double>(b - a);
}

[[nodiscard]] double db(double ratio) { return 20.0 * std::log10(ratio); }

/// Run through in blocks of an awkward size, as a mixer would, and with the
/// latency taken off the front so the result lines up with what went in.
template <typename Effect>
[[nodiscard]] std::vector<float> through(Effect& effect, std::vector<float> samples,
                                         std::size_t latency = 0, std::size_t block = 733) {
  samples.resize(samples.size() + latency, 0.0f);
  for (std::size_t at = 0; at < samples.size(); at += block) {
    effect.process(std::span(samples).subspan(at, std::min(block, samples.size() - at)));
  }
  samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(latency));
  return samples;
}

}  // namespace

// -------------------------------------------------------------- spectral --

TEST(SpectralRepair, AtNothingTheFramesGiveBackWhatWentInLaterByTheirLatency) {
  std::vector<float> in(frames_of(1.0), 0.0f);
  add_sine(in, 440.0, 0.5);
  add_noise(in, 0.05, 1);
  Dereverberator nothing({.amount = 0.0}, kRate, 1);
  EXPECT_EQ(nothing.latency(), 2048u);
  const std::vector<float> out = through(nothing, in, nothing.latency());
  // Past the first frame, which had nothing before it to overlap with.
  for (std::size_t i = 2048; i < in.size(); ++i) ASSERT_NEAR(out[i], in[i], 1e-4) << i;
}

// ---------------------------------------------------------------- denoise --

TEST(DeNoise, TakesSteadyHissDownAndLeavesTheToneAboveIt) {
  std::vector<float> in(frames_of(4.0), 0.0f);
  add_noise(in, 0.02, 2);
  add_sine(in, 440.0, 0.3, 2.0);
  Denoiser denoise({.amount = 0.6}, kRate, 1);
  const std::vector<float> out = through(denoise, in, denoise.latency());
  // The hiss alone, once it has been heard for a moment.
  EXPECT_LT(db(rms(out, 1.0, 1.9) / rms(in, 1.0, 1.9)), -10.0);
  // The tone, as loud as it was.
  EXPECT_NEAR(db(amplitude_at(out, 440.0, 2.5, 3.9) / amplitude_at(in, 440.0, 2.5, 3.9)), 0.0,
              1.0);
}

TEST(DeNoise, HowItIsSplitIntoBlocksChangesNothing) {
  std::vector<float> in(frames_of(1.0), 0.0f);
  add_noise(in, 0.05, 3);
  add_sine(in, 300.0, 0.2, 0.5);
  Denoiser one({.amount = 0.5}, kRate, 1);
  Denoiser other({.amount = 0.5}, kRate, 1);
  const std::vector<float> a = through(one, in, 0, 480);
  const std::vector<float> b = through(other, in, 0, 1031);
  EXPECT_EQ(a, b);
}

TEST(DeNoise, TreatsBothSidesOfAStereoPairAlike) {
  // Linked, so a noise that is louder on one side does not pull the image
  // about: what one channel loses at a moment the other loses too.
  std::vector<float> left(frames_of(2.0), 0.0f);
  add_noise(left, 0.02, 4);
  add_sine(left, 500.0, 0.2, 1.0);
  std::vector<float> stereo(left.size() * 2);
  for (std::size_t i = 0; i < left.size(); ++i) {
    stereo[i * 2] = left[i];
    stereo[i * 2 + 1] = left[i] * 0.5f;
  }
  Denoiser denoise({.amount = 0.6}, kRate, 2);
  const std::vector<float> out = through(denoise, stereo, denoise.latency() * 2, 734);
  for (std::size_t i = frames_of(1.2); i < frames_of(1.9); ++i) {
    ASSERT_NEAR(out[i * 2 + 1], out[i * 2] * 0.5f, 1e-5);
  }
}

// ---------------------------------------------------------------- dereverb --

TEST(DeReverb, ShortensTheRoomsTailAndKeepsWhatWasSaid) {
  // Short bursts with silence between, in a big room.
  std::vector<float> dry(frames_of(4.0), 0.0f);
  for (double at = 0.0; at < 4.0; at += 0.5) add_noise(dry, 0.2, 5 + static_cast<unsigned>(at * 4), at, at + 0.06);
  Reverb room({.size = 0.85, .damping = 0.3, .mix = 0.5}, kRate, 1);
  const std::vector<float> wet = through(room, dry);
  Dereverberator dereverb({.amount = 0.8}, kRate, 1);
  const std::vector<float> out = through(dereverb, wet, dereverb.latency());

  double tail_in = 0.0;
  double tail_out = 0.0;
  double said_in = 0.0;
  double said_out = 0.0;
  for (double at = 1.0; at < 3.9; at += 0.5) {
    tail_in += rms(wet, at + 0.2, at + 0.45);
    tail_out += rms(out, at + 0.2, at + 0.45);
    said_in += rms(wet, at, at + 0.06);
    said_out += rms(out, at, at + 0.06);
  }
  EXPECT_LT(db(tail_out / tail_in), -4.0);
  EXPECT_GT(db(said_out / said_in), -3.0);
}

// ------------------------------------------------------------------- dehum --

TEST(DeHummer, CutsTheHumAndItsHarmonicsAndLeavesTheVoice) {
  std::vector<float> in(frames_of(2.0), 0.0f);
  for (const double hz : {60.0, 120.0, 180.0}) add_sine(in, hz, 0.1);
  add_sine(in, 1000.0, 0.1);
  Dehummer dehum({.frequency = 60.0, .harmonics = 4, .reduction_db = 30.0}, kRate, 1);
  const std::vector<float> out = through(dehum, in);
  for (const double hz : {60.0, 120.0, 180.0}) {
    EXPECT_LT(db(amplitude_at(out, hz, 1.0, 2.0) / 0.1), -20.0) << hz;
  }
  EXPECT_NEAR(db(amplitude_at(out, 1000.0, 1.0, 2.0) / 0.1), 0.0, 0.5);
}

TEST(DeHummer, FiftyHertzMainsIsCutAtFifty) {
  std::vector<float> in(frames_of(2.0), 0.0f);
  add_sine(in, 50.0, 0.1);
  add_sine(in, 60.0, 0.1);
  Dehummer dehum({.frequency = 50.0, .harmonics = 1, .reduction_db = 30.0}, kRate, 1);
  const std::vector<float> out = through(dehum, in);
  EXPECT_LT(db(amplitude_at(out, 50.0, 1.0, 2.0) / 0.1), -20.0);
  EXPECT_GT(db(amplitude_at(out, 60.0, 1.0, 2.0) / 0.1), -3.0);
}

// ------------------------------------------------------------------- deess --

TEST(DeEsser, TurnsDownALoudEssAndNotTheVowelUnderIt) {
  std::vector<float> in(frames_of(2.0), 0.0f);
  add_sine(in, 300.0, 0.3);
  add_sine(in, 7000.0, 0.3, 1.0, 1.5);
  Deesser deess({.threshold_db = -30.0, .frequency = 5000.0, .reduction_db = 12.0}, kRate, 1);
  const std::vector<float> out = through(deess, in);
  EXPECT_LT(db(amplitude_at(out, 7000.0, 1.1, 1.5) / 0.3), -6.0);
  EXPECT_NEAR(db(amplitude_at(out, 300.0, 1.1, 1.5) / 0.3), 0.0, 0.5);
}

TEST(DeEsser, LeavesAVoiceWithNoSibilanceExactlyAsItWas) {
  std::vector<float> in(frames_of(1.0), 0.0f);
  add_sine(in, 300.0, 0.3);
  Deesser deess({}, kRate, 1);
  const std::vector<float> out = through(deess, in);
  for (std::size_t i = 0; i < in.size(); ++i) ASSERT_NEAR(out[i], in[i], 1e-5) << i;
}

// ------------------------------------------------------------------ rumble --

TEST(ReduceRumble, TakesOutWhatIsBelowAVoiceAndNotTheVoice) {
  std::vector<float> in(frames_of(2.0), 0.0f);
  add_sine(in, 30.0, 0.2);
  add_sine(in, 1000.0, 0.2);
  RumbleFilter rumble(0.5, kRate, 1);
  const std::vector<float> out = through(rumble, in);
  EXPECT_LT(db(amplitude_at(out, 30.0, 1.0, 2.0) / 0.2), -20.0);
  EXPECT_NEAR(db(amplitude_at(out, 1000.0, 1.0, 2.0) / 0.2), 0.0, 0.1);
  EXPECT_DOUBLE_EQ(RumbleFilter::corner(0.0), 40.0);
  EXPECT_DOUBLE_EQ(RumbleFilter::corner(1.0), 120.0);
}

// ------------------------------------------------------------- the chain --

namespace {

[[nodiscard]] core::AudioClipEffect repair(std::string type) {
  core::AudioClipEffect e;
  e.type = std::move(type);
  return e;
}

}  // namespace

TEST(RepairInTheChain, AClipsStackSaysHowFarAheadItHasToBeFed) {
  const std::vector<core::AudioClipEffect> stack{repair("denoise"), repair("dereverb"),
                                                 repair("deesser")};
  const EffectChain chain = EffectChain::build(stack, kRate, 2);
  EXPECT_EQ(chain.stage_count(), 3u);
  EXPECT_EQ(chain.latency(), 4096u);
}

TEST(RepairInTheChain, ATracksStackLeavesOutWhatWouldPutItOutOfStep) {
  const std::vector<core::AudioClipEffect> stack{repair("denoise"), repair("dehummer")};
  const EffectChain chain = EffectChain::build(stack, kRate, 2, 0.0, false);
  EXPECT_EQ(chain.stage_count(), 1u);
  EXPECT_EQ(chain.latency(), 0u);
}

TEST(RepairInTheChain, ARepairAtNothingIsLeftOut) {
  core::AudioClipEffect quiet = repair("denoise");
  quiet.params["amount"] = 0.0;
  core::AudioClipEffect flat = repair("dehummer");
  flat.params["reduction"] = 0.0;
  const std::vector<core::AudioClipEffect> stack{quiet, flat};
  EXPECT_TRUE(EffectChain::build(stack, kRate, 2).empty());
}

}  // namespace cutline::audio
