#include "cutline/engine/audio_mixer.hpp"

#include "cutline/audio/chain.hpp"
#include "cutline/audio/limiter.hpp"
#include "cutline/media/audio.hpp"
#include "cutline/render/mix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <utility>

namespace cutline::engine {
namespace {

/// A decoded source, shared by every clip that draws from it. Keyed by file and
/// stream ordinal, so two clips trimmed from the same take decode once.
using SourceKey = std::pair<std::string, int>;

/// One planned clip, with the samples and DSP it needs.
struct Voice {
  render::PlannedAudioClip planned;
  /// Points into the mixer's source cache; null when the media could not be
  /// read, in which case the voice contributes silence.
  const media::AudioBuffer* source = nullptr;
  audio::EffectChain chain;
};

/// One frame of a decoded source at a fractional position, linearly
/// interpolated.
///
/// Interpolation matters for retimed clips, where the read position advances by
/// a non-integer step: point sampling would quantise it to whole samples and
/// add a rasp of quantisation noise on top of the retime.
void sample_into(const media::AudioBuffer& source, double position, std::size_t channels,
                 std::span<float> frame) noexcept {
  const auto frames = source.frame_count();
  if (frames == 0 || position < 0.0) return;

  const double floored = std::floor(position);
  const auto index = static_cast<std::size_t>(floored);
  if (index >= frames) return;

  const auto source_channels = static_cast<std::size_t>(std::max(source.channels, 1));
  const auto fraction = static_cast<float>(position - floored);
  const std::size_t next = std::min(index + 1, frames - 1);

  for (std::size_t c = 0; c < channels; ++c) {
    // A mono source feeds every output channel; a source with more channels
    // than the mix has is truncated rather than downmixed, which is what the
    // decoder's own resampling already arranged for.
    const std::size_t source_channel = std::min(c, source_channels - 1);
    const float a = source.samples[index * source_channels + source_channel];
    const float b = source.samples[next * source_channels + source_channel];
    frame[c] = a + (b - a) * fraction;
  }
}

}  // namespace

struct AudioMixer::Impl {
  /// Owned rather than referenced: the plan points into it, and a mixer that
  /// outlived the project it was built from would be reading freed clips.
  core::Project project;
  AudioMixSettings settings;

  std::map<SourceKey, media::AudioBuffer> sources;
  std::vector<Voice> voices;
  std::vector<std::string> missing;

  std::unique_ptr<audio::Limiter> limiter;

  /// Scratch, reused across calls so mixing does not allocate per block.
  std::vector<float> voice_block;
};

AudioMixer::AudioMixer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioMixer::~AudioMixer() = default;

std::expected<std::unique_ptr<AudioMixer>, std::string> AudioMixer::create(
    const core::Project& project, AudioMixSettings settings) {
  if (settings.sample_rate <= 0) return std::unexpected("the sample rate must be positive");
  if (settings.channels <= 0) return std::unexpected("the channel count must be positive");

  auto impl = std::make_unique<Impl>();
  impl->project = project;
  impl->settings = settings;

  const auto planned = render::plan_audio(impl->project);

  std::set<std::string> missing;
  for (const render::PlannedAudioClip& entry : planned) {
    Voice voice;
    voice.planned = entry;
    voice.chain = audio::EffectChain::build(entry.clip->audio_effects,
                                            static_cast<double>(settings.sample_rate),
                                            settings.channels);

    if (entry.media != nullptr && !entry.media->path.empty()) {
      const SourceKey key{entry.media->path, entry.audio_stream};
      auto found = impl->sources.find(key);
      if (found == impl->sources.end()) {
        auto decoded = media::decode_audio(
            entry.media->path, entry.audio_stream,
            {.sample_rate = settings.sample_rate, .channels = settings.channels});
        if (decoded) {
          found = impl->sources.emplace(key, std::move(*decoded)).first;
        } else {
          // Recorded and mixed as silence: an export should complete with a
          // hole rather than fail at the last step.
          missing.insert(entry.media->id);
        }
      }
      // Safe to hold across later insertions: a map is node-based, so its
      // values never move once they are in it.
      if (found != impl->sources.end()) voice.source = &found->second;
    } else {
      missing.insert(entry.clip->media_id);
    }

    impl->voices.push_back(std::move(voice));
  }

  impl->missing.assign(missing.begin(), missing.end());
  impl->limiter = std::make_unique<audio::Limiter>(
      audio::LimiterSettings{}, static_cast<double>(settings.sample_rate), settings.channels);

  return std::unique_ptr<AudioMixer>(new AudioMixer(std::move(impl)));
}

std::expected<void, std::string> AudioMixer::mix(double t, std::span<float> out) {
  const auto channels = static_cast<std::size_t>(impl_->settings.channels);
  if (channels == 0) return std::unexpected("the mixer has no channels");
  if (out.size() % channels != 0) {
    return std::unexpected("the output block is not a whole number of frames");
  }

  std::ranges::fill(out, 0.0f);
  const std::size_t frames = out.size() / channels;
  if (frames == 0) return {};

  const double rate = static_cast<double>(impl_->settings.sample_rate);

  // Sample positions come from an absolute index rather than from the caller's
  // `t` arithmetic. Deriving each frame's time as `t + i/rate` makes the result
  // depend on where the caller happened to split its blocks, because the two
  // groupings round differently; anchoring to an index means a span mixed in
  // one call and the same span mixed in twenty are bit-for-bit identical.
  const auto base = static_cast<std::int64_t>(std::llround(t * rate));
  const double begin = static_cast<double>(base) / rate;
  const double block_end = static_cast<double>(base + static_cast<std::int64_t>(frames)) / rate;

  for (Voice& voice : impl_->voices) {
    if (voice.source == nullptr) continue;
    if (block_end <= voice.planned.start || begin >= voice.planned.end) continue;

    // The sub-range of this block the clip actually covers. Feeding the effect
    // chain only these frames keeps it contiguous in the clip's own time, which
    // is what its filter state is a memory of.
    const auto first = static_cast<std::size_t>(
        std::max(0.0, std::ceil(voice.planned.start * rate) - static_cast<double>(base)));
    const auto last = std::min(
        frames, static_cast<std::size_t>(std::max(
                    0.0, std::ceil(voice.planned.end * rate) - static_cast<double>(base))));
    if (first >= last) continue;

    const std::size_t span = last - first;
    impl_->voice_block.assign(span * channels, 0.0f);

    for (std::size_t i = 0; i < span; ++i) {
      const double now =
          static_cast<double>(base + static_cast<std::int64_t>(first + i)) / rate;
      const double source_time = render::audio_source_time_at(voice.planned, now);

      sample_into(*voice.source, source_time * rate, channels,
                  std::span(impl_->voice_block).subspan(i * channels, channels));

      // Gain and fades before the effects, matching the reference's chain.
      const auto gain = static_cast<float>(render::audio_gain_at(voice.planned, now));
      for (std::size_t c = 0; c < channels; ++c) impl_->voice_block[i * channels + c] *= gain;
    }

    voice.chain.process(impl_->voice_block);

    // A plain sum: `normalize=0`, so tracks add rather than being scaled down
    // by how many of them there are.
    for (std::size_t i = 0; i < span * channels; ++i) out[first * channels + i] += impl_->voice_block[i];
  }

  impl_->limiter->process(out);
  return {};
}

void AudioMixer::flush(std::span<float> out) { impl_->limiter->flush(out); }

std::size_t AudioMixer::latency_frames() const noexcept {
  return impl_->limiter->latency_frames();
}

void AudioMixer::reset() {
  for (Voice& voice : impl_->voices) voice.chain.reset();
  impl_->limiter->reset();
}

bool AudioMixer::silent() const noexcept {
  return std::ranges::none_of(impl_->voices,
                              [](const Voice& v) { return v.source != nullptr; });
}

const AudioMixSettings& AudioMixer::settings() const noexcept { return impl_->settings; }

const std::vector<std::string>& AudioMixer::missing_media() const noexcept {
  return impl_->missing;
}

}  // namespace cutline::engine
