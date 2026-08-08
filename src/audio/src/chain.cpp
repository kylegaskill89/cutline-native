#include "cutline/audio/chain.hpp"

#include "cutline/audio/biquad.hpp"
#include "cutline/audio/compressor.hpp"
#include "cutline/audio/delay.hpp"
#include "cutline/audio/reverb.hpp"
#include "cutline/core/keyframe.hpp"

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
constexpr std::array kDelayParams{
    AudioEffectParamDef{"time", "Time", 1.0, kMaxDelayMs, 1.0, 250.0, "ms"},
    AudioEffectParamDef{"feedback", "Feedback", 0.0, 95.0, 1.0, 35.0, "%"},
    AudioEffectParamDef{"mix", "Mix", 0.0, 100.0, 1.0, 30.0, "%"}};
constexpr std::array kReverbParams{
    AudioEffectParamDef{"size", "Size", 0.0, 100.0, 1.0, 50.0, "%"},
    AudioEffectParamDef{"damping", "Damping", 0.0, 100.0, 1.0, 50.0, "%"},
    AudioEffectParamDef{"mix", "Mix", 0.0, 100.0, 1.0, 25.0, "%"}};

constexpr std::array kDefs{
    AudioEffectDef{"highpass", "High-Pass", kHighPassParams},
    AudioEffectDef{"lowpass", "Low-Pass", kLowPassParams},
    AudioEffectDef{"bass", "Bass", kBassParams},
    AudioEffectDef{"treble", "Treble", kTrebleParams},
    AudioEffectDef{"compressor", "Compressor", kCompressorParams},
    AudioEffectDef{"eqband", "EQ Band", kEqBandParams},
    AudioEffectDef{"notch", "Notch", kNotchParams},
    AudioEffectDef{"gain", "Gain", kGainParams},
    AudioEffectDef{"delay", "Delay", kDelayParams},
    AudioEffectDef{"reverb", "Reverb", kReverbParams},
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

bool audio_effect_param_animated(const core::AudioClipEffect& effect,
                                 std::string_view key) noexcept {
  const auto found = effect.keyframes.find(std::string(key));
  return found != effect.keyframes.end() && !found->second.empty();
}

bool audio_effects_animated(std::span<const core::AudioClipEffect> effects) noexcept {
  return std::ranges::any_of(effects, [](const core::AudioClipEffect& effect) {
    return effect.enabled && std::ranges::any_of(effect.keyframes, [](const auto& entry) {
             return !entry.second.empty();
           });
  });
}

double audio_effect_param(const core::AudioClipEffect& effect, std::string_view key,
                          double local_t) noexcept {
  double fallback = 0.0;
  if (const AudioEffectDef* def = audio_effect_def(effect.type); def != nullptr) {
    const auto param = std::ranges::find(def->params, key, &AudioEffectParamDef::key);
    if (param != def->params.end()) fallback = param->fallback;
  }

  // Keyframes win outright, the way they do everywhere else: an animated
  // parameter's stored number is not a value, it is a leftover.
  if (const auto keys = effect.keyframes.find(std::string(key));
      keys != effect.keyframes.end() && !keys->second.empty()) {
    const double animated = core::eval_keyframes(keys->second, local_t);
    return std::isfinite(animated) ? animated : fallback;
  }

  const auto found = effect.params.find(std::string(key));
  if (found == effect.params.end() || !std::isfinite(found->second)) return fallback;
  return found->second;
}

// ------------------------------------------------------------------- chain --

namespace {

/// One link in the chain. A filter needs its own state per channel, since the
/// state is a memory of that channel's own past.
///
/// It keeps a copy of the effect it came from, which is what lets an animated
/// parameter be re-read later without the chain having to be handed the stack
/// again. A copy rather than a pointer: the mixer runs on its own thread and
/// the project it was built from is edited on another one.
struct Stage {
  core::AudioClipEffect source;
  std::vector<Biquad> filters;
  std::optional<Compressor> compressor;
  std::optional<Delay> delay;
  std::optional<Reverb> reverb;
  float gain = 1.0f;

  void process(std::span<float> interleaved, std::size_t channels) noexcept {
    if (compressor) {
      compressor->process(interleaved);
      return;
    }
    if (delay) {
      delay->process(interleaved);
      return;
    }
    if (reverb) {
      reverb->process(interleaved);
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
    if (delay) delay->reset();
    if (reverb) reverb->reset();
  }
};

/// The two delay-line effects read their controls as percentages, because that
/// is what a mix and a feedback are to anybody setting them. The DSP wants
/// fractions.
[[nodiscard]] DelaySettings delay_shape(const core::AudioClipEffect& effect, double local_t) {
  return DelaySettings{.time_ms = audio_effect_param(effect, "time", local_t),
                       .feedback = audio_effect_param(effect, "feedback", local_t) / 100.0,
                       .mix = audio_effect_param(effect, "mix", local_t) / 100.0};
}

[[nodiscard]] ReverbSettings reverb_shape(const core::AudioClipEffect& effect, double local_t) {
  return ReverbSettings{.size = audio_effect_param(effect, "size", local_t) / 100.0,
                        .damping = audio_effect_param(effect, "damping", local_t) / 100.0,
                        .mix = audio_effect_param(effect, "mix", local_t) / 100.0};
}

/// The filter an effect describes at `local_t`, or nothing when it is not one
/// of the filter types.
///
/// One function rather than a branch in `build` and another in `retune`. Two
/// copies of these formulas would eventually disagree, and the symptom would be
/// a stack that sounds different the moment a parameter is animated.
[[nodiscard]] std::optional<Biquad> filter_shape(const core::AudioClipEffect& effect,
                                                 double sample_rate, double local_t) {
  const auto value = [&effect, local_t](std::string_view key) {
    return audio_effect_param(effect, key, local_t);
  };

  if (effect.type == "highpass") return Biquad::high_pass(sample_rate, value("freq"));
  if (effect.type == "lowpass") return Biquad::low_pass(sample_rate, value("freq"));
  if (effect.type == "bass") {
    return Biquad::low_shelf(sample_rate, kBassFrequency, value("gain"));
  }
  if (effect.type == "treble") {
    return Biquad::high_shelf(sample_rate, kTrebleFrequency, value("gain"));
  }
  if (effect.type == "eqband") {
    return Biquad::peaking(sample_rate, value("freq"), value("gain"), value("q"));
  }
  if (effect.type == "notch") {
    return Biquad::band_reject(sample_rate, value("freq"), value("q"));
  }
  return std::nullopt;
}

[[nodiscard]] CompressorSettings compressor_shape(const core::AudioClipEffect& effect,
                                                  double local_t) {
  CompressorSettings settings;
  settings.threshold_db = audio_effect_param(effect, "threshold", local_t);
  settings.ratio = audio_effect_param(effect, "ratio", local_t);
  return settings;
}

/// Whether an effect earns a stage at all.
///
/// A neutral effect costs time and changes nothing, so it is dropped — but only
/// when the parameter that makes it neutral is a fixed number. Animated, it has
/// to be kept: a gain sweeping up from silence is neutral at the instant the
/// chain is built and is the whole point a moment later, and a stage that was
/// never built cannot appear halfway through a block.
[[nodiscard]] bool contributes(const core::AudioClipEffect& effect, double local_t) {
  const auto decided_by = [&effect, local_t](std::string_view key, auto&& neutral) {
    return audio_effect_param_animated(effect, key) ||
           !neutral(audio_effect_param(effect, key, local_t));
  };
  const auto silent_gain = [](double db) { return std::abs(db) < kNeutralGainDb; };

  if (effect.type == "bass" || effect.type == "treble" || effect.type == "eqband" ||
      effect.type == "gain") {
    return decided_by("gain", silent_gain);
  }
  if (effect.type == "compressor") {
    return decided_by("ratio", [](double ratio) { return ratio <= 1.0; });
  }
  // A delay line at no mix is inaudible and costs a buffer and a copy per
  // sample, which is the most expensive nothing in the catalogue.
  if (effect.type == "delay" || effect.type == "reverb") {
    return decided_by("mix", [](double mix) { return mix <= 0.0; });
  }
  return true;  // a filter is never neutral enough to be worth dropping
}

}  // namespace

struct EffectChain::Impl {
  std::vector<Stage> stages;
  std::size_t channels = 2;
  double sample_rate = 0.0;
  bool animated = false;
};

EffectChain::EffectChain() : impl_(std::make_unique<Impl>()) {}
EffectChain::~EffectChain() = default;
EffectChain::EffectChain(EffectChain&&) noexcept = default;
EffectChain& EffectChain::operator=(EffectChain&&) noexcept = default;

EffectChain EffectChain::build(std::span<const core::AudioClipEffect> effects,
                               double sample_rate, int channels, double local_t) {
  EffectChain chain;
  chain.impl_->channels = static_cast<std::size_t>(std::max(channels, 1));
  chain.impl_->sample_rate = sample_rate;
  if (!(sample_rate > 0.0)) return chain;

  for (const core::AudioClipEffect& effect : effects) {
    if (!effect.enabled) continue;
    if (audio_effect_def(effect.type) == nullptr) continue;
    if (!contributes(effect, local_t)) continue;

    Stage stage;
    stage.source = effect;

    if (const std::optional<Biquad> shape = filter_shape(effect, sample_rate, local_t);
        shape.has_value()) {
      stage.filters.assign(static_cast<std::size_t>(std::max(channels, 1)), *shape);
    } else if (effect.type == "compressor") {
      stage.compressor.emplace(compressor_shape(effect, local_t), sample_rate, channels);
    } else if (effect.type == "delay") {
      stage.delay.emplace(delay_shape(effect, local_t), sample_rate, channels);
    } else if (effect.type == "reverb") {
      stage.reverb.emplace(reverb_shape(effect, local_t), sample_rate, channels);
    } else if (effect.type == "gain") {
      stage.gain = static_cast<float>(db_to_linear(audio_effect_param(effect, "gain", local_t)));
    } else {
      continue;  // known to the registry but not to the builder
    }

    chain.impl_->stages.push_back(std::move(stage));
  }

  chain.impl_->animated = audio_effects_animated(effects);
  return chain;
}

void EffectChain::retune(double local_t) noexcept {
  if (!impl_->animated) return;
  const double sample_rate = impl_->sample_rate;

  for (Stage& stage : impl_->stages) {
    if (!stage.filters.empty()) {
      const std::optional<Biquad> shape = filter_shape(stage.source, sample_rate, local_t);
      if (!shape.has_value()) continue;
      // Coefficients only. The state is a memory of samples that really did
      // come before these, whatever the filter has since become.
      for (Biquad& filter : stage.filters) filter.retune(*shape);
    } else if (stage.compressor) {
      stage.compressor->retune(compressor_shape(stage.source, local_t), sample_rate);
    } else if (stage.delay) {
      stage.delay->retune(delay_shape(stage.source, local_t), sample_rate);
    } else if (stage.reverb) {
      stage.reverb->retune(reverb_shape(stage.source, local_t), sample_rate);
    } else {
      stage.gain =
          static_cast<float>(db_to_linear(audio_effect_param(stage.source, "gain", local_t)));
    }
  }
}

bool EffectChain::fixed() const noexcept { return !impl_->animated; }

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
