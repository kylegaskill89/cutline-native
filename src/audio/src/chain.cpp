#include "cutline/audio/chain.hpp"

#include "cutline/audio/biquad.hpp"
#include "cutline/audio/compressor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>

namespace cutline::audio {
namespace {

// The registry. Ranges and defaults match the reference implementation, so a
// slider that read 8 kHz there reads 8 kHz here.

constexpr std::array kHighPassParams{
    AudioEffectParamDef{"freq", "Cutoff", 20.0, 2000.0, 10.0, 100.0, "Hz"}};
constexpr std::array kLowPassParams{
    AudioEffectParamDef{"freq", "Cutoff", 500.0, 20000.0, 100.0, 8000.0, "Hz"}};
constexpr std::array kBassParams{
    AudioEffectParamDef{"gain", "Gain", -24.0, 24.0, 1.0, 0.0, "dB"}};
constexpr std::array kTrebleParams{
    AudioEffectParamDef{"gain", "Gain", -24.0, 24.0, 1.0, 0.0, "dB"}};
constexpr std::array kCompressorParams{
    AudioEffectParamDef{"threshold", "Threshold", -60.0, 0.0, 1.0, -18.0, "dB"},
    AudioEffectParamDef{"ratio", "Ratio", 1.0, 20.0, 0.5, 4.0, ":1"}};
constexpr std::array kEqBandParams{
    AudioEffectParamDef{"freq", "Frequency", 20.0, 20000.0, 10.0, 1000.0, "Hz"},
    AudioEffectParamDef{"gain", "Gain", -24.0, 24.0, 1.0, 0.0, "dB"},
    AudioEffectParamDef{"q", "Q", 0.1, 10.0, 0.1, 1.0, ""}};
constexpr std::array kNotchParams{
    AudioEffectParamDef{"freq", "Frequency", 20.0, 20000.0, 10.0, 1000.0, "Hz"},
    AudioEffectParamDef{"q", "Q", 0.1, 20.0, 0.1, 4.0, ""}};
constexpr std::array kGainParams{
    AudioEffectParamDef{"gain", "Gain", -24.0, 24.0, 0.5, 0.0, "dB"}};

constexpr std::array kDefs{
    AudioEffectDef{"highpass", "High-Pass", kHighPassParams},
    AudioEffectDef{"lowpass", "Low-Pass", kLowPassParams},
    AudioEffectDef{"bass", "Bass", kBassParams},
    AudioEffectDef{"treble", "Treble", kTrebleParams},
    AudioEffectDef{"compressor", "Compressor", kCompressorParams},
    AudioEffectDef{"eqband", "EQ Band", kEqBandParams},
    AudioEffectDef{"notch", "Notch", kNotchParams},
    AudioEffectDef{"gain", "Gain", kGainParams},
};

/// FFmpeg's `bass` and `treble` corner frequencies, which the reference did not
/// expose and so always left at their defaults.
constexpr double kBassFrequency = 100.0;
constexpr double kTrebleFrequency = 3000.0;

/// A shelf or peak gain smaller than this is inaudible, and building a stage
/// for it would only cost time.
constexpr double kNeutralGainDb = 1e-6;

}  // namespace

std::span<const AudioEffectDef> audio_effect_defs() noexcept { return kDefs; }

const AudioEffectDef* audio_effect_def(std::string_view id) noexcept {
  const auto found = std::ranges::find(kDefs, id, &AudioEffectDef::id);
  return found == kDefs.end() ? nullptr : &*found;
}

double audio_effect_param(const core::AudioClipEffect& effect, std::string_view key) noexcept {
  double fallback = 0.0;
  if (const AudioEffectDef* def = audio_effect_def(effect.type); def != nullptr) {
    const auto param = std::ranges::find(def->params, key, &AudioEffectParamDef::key);
    if (param != def->params.end()) fallback = param->fallback;
  }

  const auto found = effect.params.find(std::string(key));
  if (found == effect.params.end() || !std::isfinite(found->second)) return fallback;
  return found->second;
}

// ------------------------------------------------------------------- chain --

namespace {

/// One link in the chain. A filter needs its own state per channel, since the
/// state is a memory of that channel's own past.
struct Stage {
  std::vector<Biquad> filters;
  std::optional<Compressor> compressor;
  float gain = 1.0f;

  void process(std::span<float> interleaved, std::size_t channels) noexcept {
    if (compressor) {
      compressor->process(interleaved);
      return;
    }
    if (!filters.empty()) {
      const std::size_t frames = interleaved.size() / channels;
      for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t base = frame * channels;
        for (std::size_t c = 0; c < channels; ++c) {
          interleaved[base + c] = filters[c].process(interleaved[base + c]);
        }
      }
      return;
    }
    for (float& sample : interleaved) sample *= gain;
  }

  void reset() noexcept {
    for (Biquad& filter : filters) filter.reset();
    if (compressor) compressor->reset();
  }
};

/// Copies one configured filter into a per-channel bank.
[[nodiscard]] Stage filter_stage(const Biquad& prototype, int channels) {
  Stage stage;
  stage.filters.assign(static_cast<std::size_t>(std::max(channels, 1)), prototype);
  return stage;
}

}  // namespace

struct EffectChain::Impl {
  std::vector<Stage> stages;
  std::size_t channels = 2;
};

EffectChain::EffectChain() : impl_(std::make_unique<Impl>()) {}
EffectChain::~EffectChain() = default;
EffectChain::EffectChain(EffectChain&&) noexcept = default;
EffectChain& EffectChain::operator=(EffectChain&&) noexcept = default;

EffectChain EffectChain::build(std::span<const core::AudioClipEffect> effects,
                               double sample_rate, int channels) {
  EffectChain chain;
  chain.impl_->channels = static_cast<std::size_t>(std::max(channels, 1));
  if (!(sample_rate > 0.0)) return chain;

  for (const core::AudioClipEffect& effect : effects) {
    if (!effect.enabled) continue;
    if (audio_effect_def(effect.type) == nullptr) continue;

    const auto value = [&effect](std::string_view key) {
      return audio_effect_param(effect, key);
    };

    if (effect.type == "highpass") {
      chain.impl_->stages.push_back(
          filter_stage(Biquad::high_pass(sample_rate, value("freq")), channels));
    } else if (effect.type == "lowpass") {
      chain.impl_->stages.push_back(
          filter_stage(Biquad::low_pass(sample_rate, value("freq")), channels));
    } else if (effect.type == "bass") {
      const double gain_db = value("gain");
      if (std::abs(gain_db) < kNeutralGainDb) continue;
      chain.impl_->stages.push_back(
          filter_stage(Biquad::low_shelf(sample_rate, kBassFrequency, gain_db), channels));
    } else if (effect.type == "treble") {
      const double gain_db = value("gain");
      if (std::abs(gain_db) < kNeutralGainDb) continue;
      chain.impl_->stages.push_back(
          filter_stage(Biquad::high_shelf(sample_rate, kTrebleFrequency, gain_db), channels));
    } else if (effect.type == "eqband") {
      const double gain_db = value("gain");
      if (std::abs(gain_db) < kNeutralGainDb) continue;
      chain.impl_->stages.push_back(filter_stage(
          Biquad::peaking(sample_rate, value("freq"), gain_db, value("q")), channels));
    } else if (effect.type == "notch") {
      chain.impl_->stages.push_back(filter_stage(
          Biquad::band_reject(sample_rate, value("freq"), value("q")), channels));
    } else if (effect.type == "compressor") {
      const double ratio = value("ratio");
      if (ratio <= 1.0) continue;  // 1:1 does nothing
      CompressorSettings settings;
      settings.threshold_db = value("threshold");
      settings.ratio = ratio;
      Stage stage;
      stage.compressor.emplace(settings, sample_rate, channels);
      chain.impl_->stages.push_back(std::move(stage));
    } else if (effect.type == "gain") {
      const double gain_db = value("gain");
      if (std::abs(gain_db) < kNeutralGainDb) continue;
      Stage stage;
      stage.gain = static_cast<float>(db_to_linear(gain_db));
      chain.impl_->stages.push_back(std::move(stage));
    }
  }

  return chain;
}

void EffectChain::process(std::span<float> interleaved) noexcept {
  const std::size_t channels = impl_->channels;
  if (channels == 0 || interleaved.size() < channels) return;
  for (Stage& stage : impl_->stages) stage.process(interleaved, channels);
}

void EffectChain::reset() noexcept {
  for (Stage& stage : impl_->stages) stage.reset();
}

bool EffectChain::empty() const noexcept { return impl_->stages.empty(); }

std::size_t EffectChain::stage_count() const noexcept { return impl_->stages.size(); }

}  // namespace cutline::audio
