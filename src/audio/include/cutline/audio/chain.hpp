#pragma once

/// The per-clip audio effect registry, and the processor a stack of them
/// becomes.
///
/// This mirrors the visual effect registry: each effect declares its parameters
/// once, with ranges and defaults, and the UI builds its controls from that
/// rather than hardcoding a panel per effect. The difference is that a visual
/// effect emits shader uniforms, while an audio effect becomes a stateful
/// processor — a filter remembers the samples before the one it is working on,
/// so it cannot be a pure function the way `resolve_effect_params` is.
///
/// The effect ids match the reference implementation's, so existing projects
/// keep their sound.

#include "cutline/core/model.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace cutline::audio {

struct AudioEffectParamDef {
  std::string_view key;
  std::string_view label;
  double minimum;
  double maximum;
  double step;
  double fallback;
  std::string_view unit;
};

struct AudioEffectDef {
  std::string_view id;
  std::string_view name;
  std::span<const AudioEffectParamDef> params;
};

/// Every audio effect, in the order the UI should offer them.
[[nodiscard]] std::span<const AudioEffectDef> audio_effect_defs() noexcept;

/// The definition for an id, or null when unknown. An unknown effect is not an
/// error: a project written by a newer build should still open and play, just
/// without the effect it names.
[[nodiscard]] const AudioEffectDef* audio_effect_def(std::string_view id) noexcept;

/// A parameter's value, falling back to the registry default when absent or
/// not finite. Missing is not zero — an absent cutoff means 100 Hz, not DC.
[[nodiscard]] double audio_effect_param(const core::AudioClipEffect& effect,
                                        std::string_view key) noexcept;

/// A clip's effect stack, realised as something that can process samples.
///
/// Built once for a given sample rate and channel count and then fed blocks.
/// Disabled effects, unknown ids, and effects whose parameters make them
/// neutral (a 0 dB shelf, a 1:1 compressor) are dropped at build time, so a
/// stack of eight untouched effects costs nothing to run.
class EffectChain {
 public:
  EffectChain();
  ~EffectChain();
  EffectChain(EffectChain&&) noexcept;
  EffectChain& operator=(EffectChain&&) noexcept;
  EffectChain(const EffectChain&) = delete;
  EffectChain& operator=(const EffectChain&) = delete;

  [[nodiscard]] static EffectChain build(std::span<const core::AudioClipEffect> effects,
                                         double sample_rate, int channels);

  /// Processes one interleaved block in place. Splitting a buffer across calls
  /// gives the same samples as processing it whole, which is what lets the
  /// mixer choose its block size freely.
  void process(std::span<float> interleaved) noexcept;

  /// Clears every stage's history. Playback jumping to a new position must do
  /// this, or the filters ring with samples that no longer precede the ones
  /// being played.
  void reset() noexcept;

  /// True when nothing contributes, so the caller can skip the block.
  [[nodiscard]] bool empty() const noexcept;

  /// How many stages survived the build, for tests and diagnostics.
  [[nodiscard]] std::size_t stage_count() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::audio
