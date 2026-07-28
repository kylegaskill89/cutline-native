/// The registry and the processor a clip's effect stack becomes.

#include "cutline/audio/chain.hpp"

#include "cutline/audio/biquad.hpp"
#include "cutline/audio/compressor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <vector>

namespace cutline::audio {
namespace {

constexpr double kRate = 48000.0;
constexpr int kChannels = 2;

[[nodiscard]] core::AudioClipEffect effect(std::string type,
                                           std::map<std::string, double> params = {}) {
  core::AudioClipEffect e;
  e.type = std::move(type);
  e.params = std::move(params);
  return e;
}

/// An interleaved stereo sine.
[[nodiscard]] std::vector<float> stereo_sine(double freq, double amplitude = 1.0,
                                             std::size_t frames = 4800) {
  std::vector<float> out(frames * kChannels);
  for (std::size_t i = 0; i < frames; ++i) {
    const double phase = 2.0 * std::numbers::pi * freq * static_cast<double>(i) / kRate;
    const auto value = static_cast<float>(std::sin(phase) * amplitude);
    out[i * kChannels] = value;
    out[i * kChannels + 1] = value;
  }
  return out;
}

[[nodiscard]] double settled_peak(const std::vector<float>& samples) {
  double peak = 0.0;
  for (std::size_t i = samples.size() * 3 / 4; i < samples.size(); ++i) {
    peak = std::max(peak, static_cast<double>(std::abs(samples[i])));
  }
  return peak;
}

[[nodiscard]] EffectChain chain_of(std::vector<core::AudioClipEffect> effects) {
  return EffectChain::build(effects, kRate, kChannels);
}

// ---------------------------------------------------------------- registry --

TEST(AudioRegistry, EveryEffectTheReferenceHadIsPresent) {
  // Project files name effects by id, so a missing one means a clip silently
  // loses its sound when an old project is opened.
  for (const std::string_view id : {"highpass", "lowpass", "bass", "treble", "compressor",
                                    "eqband", "notch", "gain"}) {
    EXPECT_NE(audio_effect_def(id), nullptr) << id;
  }
  EXPECT_EQ(audio_effect_defs().size(), 8u);
}

TEST(AudioRegistry, AnUnknownEffectIsNotAnError) {
  // A project written by a newer build should still open and play.
  EXPECT_EQ(audio_effect_def("reverb"), nullptr);
  EXPECT_TRUE(chain_of({effect("reverb")}).empty());
}

TEST(AudioRegistry, EveryEffectDeclaresAtLeastOneParameter) {
  for (const AudioEffectDef& def : audio_effect_defs()) {
    EXPECT_FALSE(def.params.empty()) << def.id;
    EXPECT_FALSE(def.name.empty()) << def.id;
  }
}

TEST(AudioRegistry, EveryDefaultLiesInsideItsOwnRange) {
  // A slider whose default sits outside its track is a UI bug that is easier to
  // catch here than to notice.
  for (const AudioEffectDef& def : audio_effect_defs()) {
    for (const AudioEffectParamDef& param : def.params) {
      EXPECT_GE(param.fallback, param.minimum) << def.id << "." << param.key;
      EXPECT_LE(param.fallback, param.maximum) << def.id << "." << param.key;
      EXPECT_GT(param.step, 0.0) << def.id << "." << param.key;
    }
  }
}

TEST(AudioRegistry, AMissingParameterFallsBackToItsDefault) {
  // Missing is not zero: an absent cutoff means 100 Hz, not DC.
  EXPECT_DOUBLE_EQ(audio_effect_param(effect("highpass"), "freq"), 100.0);
  EXPECT_DOUBLE_EQ(audio_effect_param(effect("lowpass"), "freq"), 8000.0);
  EXPECT_DOUBLE_EQ(audio_effect_param(effect("compressor"), "ratio"), 4.0);
  EXPECT_DOUBLE_EQ(audio_effect_param(effect("eqband"), "q"), 1.0);
}

TEST(AudioRegistry, APresentParameterWins) {
  EXPECT_DOUBLE_EQ(audio_effect_param(effect("highpass", {{"freq", 250.0}}), "freq"), 250.0);
}

TEST(AudioRegistry, ANonFiniteParameterFallsBackRatherThanPoisoningTheFilter) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_DOUBLE_EQ(audio_effect_param(effect("highpass", {{"freq", nan}}), "freq"), 100.0);
}

// ------------------------------------------------------------------ builds --

TEST(EffectChain, AnEmptyStackDoesNothing) {
  EXPECT_TRUE(chain_of({}).empty());
  EXPECT_EQ(chain_of({}).stage_count(), 0u);
}

TEST(EffectChain, DisabledEffectsAreDropped) {
  core::AudioClipEffect off = effect("highpass", {{"freq", 500.0}});
  off.enabled = false;
  EXPECT_TRUE(chain_of({off}).empty());
}

TEST(EffectChain, NeutralEffectsCostNothing) {
  // A stack of eight untouched effects is the common case once a user has
  // clicked through the list, and it should not make playback do any work.
  EXPECT_TRUE(chain_of({effect("bass", {{"gain", 0.0}})}).empty());
  EXPECT_TRUE(chain_of({effect("treble", {{"gain", 0.0}})}).empty());
  EXPECT_TRUE(chain_of({effect("gain", {{"gain", 0.0}})}).empty());
  EXPECT_TRUE(chain_of({effect("eqband", {{"gain", 0.0}})}).empty());
  EXPECT_TRUE(chain_of({effect("compressor", {{"ratio", 1.0}})}).empty());
}

TEST(EffectChain, EachContributingEffectBecomesOneStage) {
  const auto chain = chain_of({effect("highpass"), effect("bass", {{"gain", 6.0}}),
                               effect("gain", {{"gain", 3.0}})});
  EXPECT_EQ(chain.stage_count(), 3u);
  EXPECT_FALSE(chain.empty());
}

// --------------------------------------------------------------- behaviour --

TEST(EffectChain, AHighPassStackActuallyFiltersTheAudio) {
  auto chain = chain_of({effect("highpass", {{"freq", 1000.0}})});

  auto low = stereo_sine(60.0);
  chain.process(low);
  EXPECT_LT(settled_peak(low), 0.05);

  chain.reset();
  // 6 kHz divides the sample rate evenly, so a sample lands on the crest and
  // the measured peak is the filter's gain rather than the sampling grid's.
  auto high = stereo_sine(6000.0);
  chain.process(high);
  EXPECT_NEAR(settled_peak(high), 1.0, 0.05);
}

TEST(EffectChain, GainScalesByDecibels) {
  auto chain = chain_of({effect("gain", {{"gain", -6.0}})});
  auto samples = stereo_sine(1000.0);
  chain.process(samples);
  EXPECT_NEAR(settled_peak(samples), 0.5, 0.01);
}

TEST(EffectChain, ChannelsAreFilteredIndependently) {
  // One shared filter state across an interleaved stereo pair would mix the
  // channels together, which sounds like a phasey mono collapse.
  auto chain = chain_of({effect("lowpass", {{"freq", 500.0}})});

  std::vector<float> samples(2000 * kChannels, 0.0f);
  for (std::size_t i = 0; i < 2000; ++i) {
    samples[i * kChannels] = 1.0f;      // left: constant
    samples[i * kChannels + 1] = 0.0f;  // right: silent
  }
  chain.process(samples);

  // The right channel saw nothing but zeros, so it must still be silent.
  for (std::size_t i = 0; i < 2000; ++i) {
    ASSERT_NEAR(samples[i * kChannels + 1], 0.0f, 1e-6f) << "frame " << i;
  }
  EXPECT_GT(samples[1999 * kChannels], 0.9f);  // the left settled at DC
}

TEST(EffectChain, StagesApplyInOrder) {
  // Order matters for anything non-linear. A boost then a compressor is not the
  // same as a compressor then a boost, and the stored order is the apply order.
  auto boost_then_squash = chain_of({effect("gain", {{"gain", 12.0}}),
                                     effect("compressor", {{"threshold", -20.0},
                                                           {"ratio", 20.0}})});
  auto squash_then_boost = chain_of({effect("compressor", {{"threshold", -20.0},
                                                           {"ratio", 20.0}}),
                                     effect("gain", {{"gain", 12.0}})});

  auto a = stereo_sine(200.0, 0.3);
  auto b = a;
  boost_then_squash.process(a);
  squash_then_boost.process(b);

  EXPECT_GT(std::abs(settled_peak(a) - settled_peak(b)), 0.05);
}

TEST(EffectChain, ProcessingIsUnaffectedByBlockBoundaries) {
  auto whole_chain = chain_of({effect("highpass", {{"freq", 300.0}}),
                               effect("treble", {{"gain", 6.0}})});
  auto split_chain = chain_of({effect("highpass", {{"freq", 300.0}}),
                               effect("treble", {{"gain", 6.0}})});

  auto whole = stereo_sine(500.0);
  auto split = whole;
  whole_chain.process(whole);

  const std::span<float> all(split);
  split_chain.process(all.subspan(0, 128 * kChannels));
  split_chain.process(all.subspan(128 * kChannels, 977 * kChannels));
  split_chain.process(all.subspan((128 + 977) * kChannels));

  for (std::size_t i = 0; i < whole.size(); ++i) EXPECT_FLOAT_EQ(split[i], whole[i]) << "at " << i;
}

TEST(EffectChain, ResetReturnsTheChainToItsInitialState) {
  auto chain = chain_of({effect("highpass", {{"freq", 400.0}})});

  auto first = stereo_sine(1000.0, 1.0, 512);
  chain.process(first);
  chain.reset();

  auto second = stereo_sine(1000.0, 1.0, 512);
  chain.process(second);

  auto fresh_chain = chain_of({effect("highpass", {{"freq", 400.0}})});
  auto reference = stereo_sine(1000.0, 1.0, 512);
  fresh_chain.process(reference);

  for (std::size_t i = 0; i < second.size(); ++i) EXPECT_FLOAT_EQ(second[i], reference[i]);
}

TEST(EffectChain, AShortBlockIsIgnoredRatherThanReadingPastTheEnd) {
  auto chain = chain_of({effect("highpass")});
  std::vector<float> single(1, 0.5f);  // fewer samples than channels
  chain.process(single);
  EXPECT_FLOAT_EQ(single[0], 0.5f);
}

// -------------------------------------------------------------- compressor --

TEST(Compressor, QuietMaterialPassesUntouched) {
  CompressorSettings settings;
  settings.threshold_db = -20.0;
  settings.ratio = 4.0;
  Compressor compressor(settings, kRate, kChannels);

  auto samples = stereo_sine(200.0, 0.01);  // -40 dB, far below the threshold
  const auto original = samples;
  compressor.process(samples);

  for (std::size_t i = 0; i < samples.size(); ++i) EXPECT_NEAR(samples[i], original[i], 1e-4f);
}

TEST(Compressor, LoudMaterialIsTurnedDown) {
  CompressorSettings settings;
  settings.threshold_db = -20.0;
  settings.ratio = 4.0;
  Compressor compressor(settings, kRate, kChannels);

  auto samples = stereo_sine(200.0, 1.0);
  compressor.process(samples);
  EXPECT_LT(settled_peak(samples), 0.6);
  EXPECT_LT(compressor.reduction_db(), -5.0);
}

TEST(Compressor, AHigherRatioReducesMore) {
  const auto peak_at_ratio = [](double ratio) {
    CompressorSettings settings;
    settings.threshold_db = -20.0;
    settings.ratio = ratio;
    Compressor compressor(settings, kRate, kChannels);
    auto samples = stereo_sine(200.0, 1.0);
    compressor.process(samples);
    return settled_peak(samples);
  };

  EXPECT_GT(peak_at_ratio(2.0), peak_at_ratio(4.0));
  EXPECT_GT(peak_at_ratio(4.0), peak_at_ratio(20.0));
}

TEST(Compressor, TheReductionFollowsTheRatio) {
  // 0 dBFS into a -20 dB threshold at 4:1 leaves 20/4 = 5 dB above it, so the
  // output settles around -15 dBFS. The knee widens that slightly.
  CompressorSettings settings;
  settings.threshold_db = -20.0;
  settings.ratio = 4.0;
  settings.attack_ms = 1.0;  // settle quickly, so one buffer is enough
  Compressor compressor(settings, kRate, kChannels);

  auto samples = stereo_sine(200.0, 1.0);
  compressor.process(samples);
  EXPECT_NEAR(linear_to_db(settled_peak(samples)), -15.0, 1.5);
}

TEST(Compressor, ARatioOfOneIsANoOp) {
  CompressorSettings settings;
  settings.ratio = 1.0;
  Compressor compressor(settings, kRate, kChannels);
  EXPECT_TRUE(compressor.transparent());

  auto samples = stereo_sine(200.0, 1.0);
  const auto original = samples;
  compressor.process(samples);
  for (std::size_t i = 0; i < samples.size(); ++i) EXPECT_FLOAT_EQ(samples[i], original[i]);
}

TEST(Compressor, ChannelsAreLinked) {
  // A hard-panned transient must not pull the stereo image sideways: both
  // channels take the same gain, computed from the louder one.
  CompressorSettings settings;
  settings.threshold_db = -30.0;
  settings.ratio = 8.0;
  Compressor compressor(settings, kRate, kChannels);

  // Left loud, right at a fixed quiet level.
  std::vector<float> samples(2000 * kChannels);
  for (std::size_t i = 0; i < 2000; ++i) {
    samples[i * kChannels] = 1.0f;
    samples[i * kChannels + 1] = 0.1f;
  }
  compressor.process(samples);

  // The ratio between the channels is preserved, because the same gain applied
  // to both.
  const std::size_t last = 1999 * kChannels;
  EXPECT_NEAR(samples[last] / samples[last + 1], 10.0f, 0.01f);
}

TEST(Compressor, ReleaseIsSlowerThanAttack) {
  CompressorSettings settings;
  settings.threshold_db = -30.0;
  settings.ratio = 8.0;
  settings.attack_ms = 5.0;
  settings.release_ms = 500.0;
  Compressor compressor(settings, kRate, kChannels);

  // A loud burst, then silence. After the burst the compressor should still be
  // holding some reduction, which is what release time means.
  auto loud = stereo_sine(200.0, 1.0, 4800);
  compressor.process(loud);
  const double after_burst = compressor.reduction_db();
  ASSERT_LT(after_burst, -5.0);

  std::vector<float> quiet(480 * kChannels, 0.0f);  // 10 ms of silence
  compressor.process(quiet);
  EXPECT_LT(compressor.reduction_db(), 0.0) << "reduction snapped back instantly";
  EXPECT_GT(compressor.reduction_db(), after_burst);
}

TEST(Compressor, ProcessingIsUnaffectedByBlockBoundaries) {
  const auto run = [](std::vector<std::size_t> block_frames) {
    CompressorSettings settings;
    settings.threshold_db = -24.0;
    settings.ratio = 6.0;
    Compressor compressor(settings, kRate, kChannels);

    auto samples = stereo_sine(300.0, 0.8, 3000);
    std::span<float> rest(samples);
    for (const std::size_t frames : block_frames) {
      compressor.process(rest.subspan(0, frames * kChannels));
      rest = rest.subspan(frames * kChannels);
    }
    compressor.process(rest);
    return samples;
  };

  const auto whole = run({});
  const auto split = run({97, 1000, 3});
  for (std::size_t i = 0; i < whole.size(); ++i) EXPECT_FLOAT_EQ(split[i], whole[i]) << "at " << i;
}

TEST(Compressor, TheKneeIsSmoothAcrossTheThreshold) {
  // A hard corner is audible as a click on material sitting right at the
  // threshold. Stepping the input level across it should give a gain reduction
  // that never jumps.
  CompressorSettings settings;
  settings.threshold_db = -20.0;
  settings.ratio = 8.0;
  settings.attack_ms = 0.0;  // no smoothing, so the curve itself is measured
  settings.release_ms = 0.0;

  double previous = 0.0;
  for (int step = 0; step <= 40; ++step) {
    const double level_db = -30.0 + step * 0.5;
    Compressor compressor(settings, kRate, kChannels);
    std::vector<float> block(64 * kChannels, static_cast<float>(db_to_linear(level_db)));
    compressor.process(block);

    const double reduction = compressor.reduction_db();
    EXPECT_LE(reduction, previous + 1e-6) << "reduction went backwards at " << level_db;
    EXPECT_GT(reduction - previous, -1.0) << "reduction jumped at " << level_db;
    previous = reduction;
  }
  EXPECT_LT(previous, -5.0);  // it did engage by the end
}

}  // namespace
}  // namespace cutline::audio
