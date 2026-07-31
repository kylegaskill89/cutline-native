#include "cutline/engine/audio_mixer.hpp"

#include "cutline/audio/chain.hpp"
#include "cutline/audio/limiter.hpp"
#include "cutline/audio/meter.hpp"
#include "cutline/audio/stretch.hpp"
#include "cutline/media/audio.hpp"
#include "cutline/render/mix.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace cutline::engine {
namespace {

/// A decoded source range. Keyed by file, stream ordinal and the range itself,
/// so two clips with the same trim decode once and two clips with different
/// trims do not each drag in the whole file.
///
/// Decoding whole streams was the obvious first implementation and does not
/// survive contact with the reference footage: those captures run ten minutes
/// with four audio streams each, where placing a twenty-second clip cost about
/// four seconds and 230 MB.
struct SourceKey {
  std::string path;
  int stream = 0;
  double source_in = 0.0;
  double source_out = 0.0;

  friend auto operator<=>(const SourceKey&, const SourceKey&) = default;
};

/// A little either side of the trim, so interpolating at the very last sample
/// has a neighbour to reach for and a seek landing slightly late does not clip
/// the start.
constexpr double kDecodeMargin = 0.05;

/// One planned clip, with the samples and DSP it needs.
struct Voice {
  render::PlannedAudioClip planned;
  /// Points into the mixer's source cache, or at `retimed` when the clip is
  /// sped up, slowed down or reversed. Null when the media could not be read,
  /// in which case the voice contributes silence.
  const media::AudioBuffer* source = nullptr;
  /// Owned when retiming applies: the clip's own audio, already stretched and
  /// reversed, laid out in *timeline* time so reading it is a plain sequential
  /// walk. Empty otherwise, since the shared source serves directly.
  media::AudioBuffer retimed;
  bool timeline_indexed = false;
  audio::EffectChain chain;
};

/// Whether a clip needs its audio rendered ahead of time rather than read
/// straight from the source.
[[nodiscard]] bool needs_retiming(const render::PlannedAudioClip& planned) noexcept {
  return planned.reverse || std::abs(planned.speed - 1.0) > 1e-6;
}

/// Renders a retimed clip's audio into timeline time.
///
/// Reading a source faster would shorten it *and* raise its pitch, which is a
/// tape-speed effect rather than a speed change. The reference used FFmpeg's
/// `atempo`, so the length changes here and the pitch does not.
[[nodiscard]] media::AudioBuffer render_retimed(const media::AudioBuffer& source,
                                                const render::PlannedAudioClip& planned,
                                                int sample_rate, int channels) {
  media::AudioBuffer out;
  out.sample_rate = sample_rate;
  out.channels = channels;

  const auto lanes = static_cast<std::size_t>(channels);
  const auto rate = static_cast<double>(sample_rate);
  const auto frames = source.frame_count();

  // Relative to the buffer's own start, since only the clip's range was
  // decoded and it begins wherever the decoder was asked to begin.
  const auto begin = static_cast<std::size_t>(
      std::max(0.0, (planned.source_in - source.start_time) * rate));
  const auto end = std::min(
      frames,
      static_cast<std::size_t>(std::max(0.0, (planned.source_out - source.start_time) * rate)));
  if (begin >= end) return out;

  std::vector<float> trimmed(
      source.samples.begin() + static_cast<std::ptrdiff_t>(begin * lanes),
      source.samples.begin() + static_cast<std::ptrdiff_t>(end * lanes));

  if (planned.reverse) {
    // Frame by frame, not sample by sample: reversing the interleaved buffer
    // outright would swap the channels as well as the time.
    const std::size_t count = trimmed.size() / lanes;
    for (std::size_t i = 0; i < count / 2; ++i) {
      for (std::size_t c = 0; c < lanes; ++c) {
        std::swap(trimmed[i * lanes + c], trimmed[(count - 1 - i) * lanes + c]);
      }
    }
  }

  // A clip at speed 2 lasts half as long, so the stretch factor is its inverse.
  out.samples = audio::time_stretch(trimmed, channels, 1.0 / planned.speed);
  return out;
}

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

  /// Atomic because the master fader is the one thing about a mix that can be
  /// changed while it is playing: the player's render thread reads this on
  /// every block while the interface writes it from the message loop.
  std::atomic<double> master_gain{1.0};

  /// The meter belongs to whichever thread is mixing; its reading is published
  /// under a lock for whoever is drawing. A reading is a few dozen bytes and is
  /// taken once a block, so the lock is never contended for long enough to
  /// matter — and the alternative, tearing a stereo pair across two atomics,
  /// shows up as a meter whose channels disagree.
  std::unique_ptr<audio::Meter> meter;
  mutable std::mutex levels_lock;
  audio::MeterReading levels;

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
      // Only the clip's own trim, not the whole file.
      const double from = std::max(0.0, entry.source_in - kDecodeMargin);
      const double to = entry.source_out + kDecodeMargin;
      const SourceKey key{entry.media->path, entry.audio_stream, from, to};

      auto found = impl->sources.find(key);
      if (found == impl->sources.end()) {
        auto decoded = media::decode_audio(entry.media->path, entry.audio_stream,
                                           {.sample_rate = settings.sample_rate,
                                            .channels = settings.channels,
                                            .start = from,
                                            .duration = to - from});
        if (decoded) {
          found = impl->sources.emplace(key, std::move(*decoded)).first;
        } else {
          // Recorded and mixed as silence: an export should complete with a
          // hole rather than fail at the last step.
          missing.insert(entry.media->id);
        }
      }
      if (found != impl->sources.end()) {
        if (needs_retiming(entry)) {
          voice.retimed = render_retimed(found->second, entry, settings.sample_rate,
                                         settings.channels);
          // `source` is left null and pointed at the voice's own buffer below,
          // once the vector has stopped moving its elements around.
          voice.timeline_indexed = true;
        } else {
          // Safe to hold across later insertions: a map is node-based, so its
          // values never move once they are in it.
          voice.source = &found->second;
        }
      }
    } else {
      missing.insert(entry.clip->media_id);
    }

    impl->voices.push_back(std::move(voice));
  }

  // Retimed voices own their audio, so their pointers can only be taken once
  // every push_back is done and the vector's elements have stopped moving.
  for (Voice& voice : impl->voices) {
    if (!voice.timeline_indexed) continue;
    voice.source = voice.retimed.frame_count() > 0 ? &voice.retimed : nullptr;
  }

  impl->missing.assign(missing.begin(), missing.end());
  impl->master_gain.store(std::clamp(project.master_gain, 0.0, core::kMaxMasterGain),
                          std::memory_order_relaxed);
  const audio::LimiterSettings limiting;
  impl->limiter = std::make_unique<audio::Limiter>(
      limiting, static_cast<double>(settings.sample_rate), settings.channels);
  // The meter's "over" is the limiter's ceiling rather than full scale, so the
  // warning means "the limiter is working" — which happens first, and is the
  // thing that can still be acted on.
  impl->meter = std::make_unique<audio::Meter>(static_cast<double>(settings.sample_rate),
                                               settings.channels,
                                               audio::MeterSettings{.over = limiting.limit});
  impl->levels = impl->meter->read();

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

      // A retimed voice was rendered into timeline time already, so its read
      // position is just how far into the clip we are; anything else is read
      // from the shared source at the source time the plan asks for.
      const double position =
          voice.timeline_indexed
              ? (now - voice.planned.start) * rate
              : (render::audio_source_time_at(voice.planned, now) - voice.source->start_time) *
                    rate;

      sample_into(*voice.source, position, channels,
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

  // The master fader, ahead of the limiter: pulling it down has to take a hot
  // mix *out* of limiting, and after the limiter it would only make an already
  // squashed mix quieter.
  const double master = impl_->master_gain.load(std::memory_order_relaxed);
  if (master != 1.0) {
    const auto gain = static_cast<float>(master);
    for (float& sample : out) sample *= gain;
  }

  // Metered here, between the fader and the limiter: this is the mix as it was
  // made, which is what a meter is for. See `levels`.
  impl_->meter->process(out);
  {
    const std::lock_guard lock(impl_->levels_lock);
    impl_->levels = impl_->meter->read();
  }

  impl_->limiter->process(out);
  return {};
}

void AudioMixer::set_master_gain(double gain) noexcept {
  impl_->master_gain.store(std::clamp(gain, 0.0, core::kMaxMasterGain),
                           std::memory_order_relaxed);
}

double AudioMixer::master_gain() const noexcept {
  return impl_->master_gain.load(std::memory_order_relaxed);
}

audio::MeterReading AudioMixer::levels() const noexcept {
  const std::lock_guard lock(impl_->levels_lock);
  return impl_->levels;
}

void AudioMixer::flush(std::span<float> out) { impl_->limiter->flush(out); }

std::size_t AudioMixer::latency_frames() const noexcept {
  return impl_->limiter->latency_frames();
}

void AudioMixer::reset() {
  for (Voice& voice : impl_->voices) voice.chain.reset();
  impl_->limiter->reset();

  // The meter too: levels only fall while audio is being measured, so a seek
  // that lands somewhere quiet would otherwise leave the bars where the last
  // loud thing left them.
  impl_->meter->reset();
  const std::lock_guard lock(impl_->levels_lock);
  impl_->levels = impl_->meter->read();
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
