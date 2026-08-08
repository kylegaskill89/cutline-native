#include "cutline/engine/audio_mixer.hpp"

#include "cutline/audio/chain.hpp"
#include "cutline/audio/limiter.hpp"
#include "cutline/audio/meter.hpp"
#include "cutline/audio/stretch.hpp"
#include "cutline/core/query.hpp"
#include "cutline/media/audio.hpp"
#include "cutline/render/mix.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <thread>
#include <condition_variable>
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

/// A clip's range up to this long is decoded whole and never thought about
/// again.
///
/// Two minutes of stereo is about 46 MB, and a cut is made of clips far shorter
/// than that — so the windowing below is machinery that the ordinary project
/// never touches, and the simplest behaviour stays the common one. It is the
/// ten-minute capture dropped in untrimmed that needs the other path.
constexpr double kWholeSourceSeconds = 120.0;

/// How much of a long source is kept either side of where the mix has reached.
///
/// Ahead is what playback eats into and behind is what a scrub back lands in.
/// Both are generous on purpose: a window is only worth having if ordinary
/// movement stays inside it, since going outside costs a decode. Seventy-five
/// seconds of stereo is about 29 MB per stream.
constexpr double kWindowBehind = 15.0;
constexpr double kWindowAhead = 60.0;

/// What has been read of one source, and where in the source it sits.
///
/// Immutable once published. That is what lets the mixing thread take a
/// pointer to it and read for a whole block while a reader thread is building
/// the next one: nothing it is looking at can change underneath it, and the
/// swap is a single atomic store.
struct Window {
  double from = 0.0;  ///< source seconds this window begins at
  double to = 0.0;    ///< and ends at
  media::AudioBuffer audio;
};

/// One source's audio, as much of it as is worth holding at once.
struct StreamedSource {
  SourceKey key;
  int sample_rate = 48000;
  int channels = 2;
  /// True when the whole range is resident and there is nothing to refill.
  bool whole = false;

  /// The resident window. Read on the mixing thread, replaced on the reader's.
  ///
  /// An atomic shared pointer rather than a mutex, and that is the whole reason
  /// this is safe: the mixing thread takes a reference and reads through it for
  /// the length of a block, while the reader builds the next window beside it
  /// and publishes with one store. Nothing blocks, and nothing the mixer is
  /// looking at can be freed while it looks.
  std::atomic<std::shared_ptr<const Window>> resident;

  /// Where the mix last needed audio, in source seconds. Written by the mixing
  /// thread, read by the reader.
  std::atomic<double> wanted{0.0};
  /// Set once when the file will not decode, so a broken source is not tried
  /// again on every block for the life of the mix.
  std::atomic<bool> failed{false};
};

/// Decodes the part of `source` that should be resident when the mix is at
/// `at`, or the whole range when it is short enough to hold.
[[nodiscard]] std::shared_ptr<const Window> read_window(const StreamedSource& source, double at) {
  const double from = source.whole
                          ? source.key.source_in
                          : std::clamp(at - kWindowBehind, source.key.source_in,
                                       source.key.source_out);
  const double to = source.whole
                        ? source.key.source_out
                        : std::min(source.key.source_out, from + kWindowBehind + kWindowAhead);
  if (!(to > from)) return nullptr;

  auto decoded = media::decode_audio(source.key.path, source.key.stream,
                                     {.sample_rate = source.sample_rate,
                                      .channels = source.channels,
                                      .start = from,
                                      .duration = to - from});
  if (!decoded) return nullptr;

  auto window = std::make_shared<Window>();
  window->from = from;
  window->to = to;
  window->audio = std::move(*decoded);
  return window;
}

/// Whether what is resident still answers for a mix that has reached `at`.
///
/// Refilled before the edge rather than at it: a window whose end is exactly
/// where playback has got to is one that has already run out.
[[nodiscard]] bool covers(const Window* window, const StreamedSource& source, double at) noexcept {
  if (window == nullptr) return false;
  if (source.whole) return true;
  // Comfortably inside, or up against the end of the source, where there is
  // nothing further to read and the window is as complete as it can be.
  const bool room_ahead =
      at + kWindowAhead * 0.25 < window->to || window->to >= source.key.source_out - 1e-6;
  const bool room_behind =
      at >= window->from || window->from <= source.key.source_in + 1e-6;
  return room_ahead && room_behind;
}

/// How often an animated effect parameter is re-read, in frames.
///
/// A control rate rather than a sample rate. Sixty-four frames is about a
/// millisecond and a half at 48 kHz — finer than any automation curve anybody
/// draws, and coarse enough that recomputing a filter's coefficients is not
/// what the mixer spends its time on. Aligned to the timeline rather than to
/// the caller's buffer, which is what keeps the preview and the export the
/// same.
constexpr std::int64_t kControlBlock = 64;

/// Runs a chain over one interleaved block, retuning animated parameters on the
/// control grid.
///
/// An animated stack is retuned on a fixed grid **aligned to the timeline**,
/// not once per call. Retuning per call would have been simpler and would have
/// made the preview and the export disagree: they ask for different buffer
/// sizes, so a sweep would land on different coefficients in each. Aligning the
/// grid to absolute sample positions means splitting a buffer anywhere gives
/// the same samples, which is the guarantee the rest of this mixer keeps.
///
/// Per sample would be both expensive and wrong — moving an IIR filter's
/// coefficients under state that assumes the old ones is how a filter is made
/// to ring.
///
/// `origin` is what the chain's keyframe times are measured from: a clip's
/// start for a clip's stack, and zero for a track's, whose keyframes are in
/// timeline seconds. Shared by both so the grid cannot come to mean two things.
void run_chain_over(audio::EffectChain& chain, std::span<float> block,
                    std::int64_t first_sample, double rate, std::size_t channels,
                    double origin) {
  if (channels == 0) return;
  const std::size_t frames = block.size() / channels;
  if (frames == 0) return;

  if (chain.fixed()) {
    chain.process(block);
    return;
  }

  std::size_t at = 0;
  while (at < frames) {
    const std::int64_t absolute = first_sample + static_cast<std::int64_t>(at);
    const std::int64_t into = ((absolute % kControlBlock) + kControlBlock) % kControlBlock;
    const auto until = static_cast<std::size_t>(kControlBlock - into);
    const std::size_t chunk = std::min(frames - at, until);

    // Retuned to the time of the grid step this chunk sits *in*, not to where
    // the chunk happens to begin. A call that starts halfway through a step has
    // to use the coefficients that step already had, or the sound would depend
    // on where the caller's buffer boundaries fell.
    chain.retune(static_cast<double>(absolute - into) / rate - origin);
    chain.process(block.subspan(at * channels, chunk * channels));
    at += chunk;
  }
}

/// One planned clip, with the samples and DSP it needs.
struct Voice {
  render::PlannedAudioClip planned;
  /// The shared source this reads from, or null when it is retimed — which
  /// reads its own baked buffer — or when the media could not be read at all,
  /// in which case the voice contributes silence.
  StreamedSource* stream = nullptr;
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

  /// Unique pointers so a source's address, and the atomics in it, survive the
  /// map growing.
  std::map<SourceKey, std::unique_ptr<StreamedSource>> sources;
  std::vector<Voice> voices;
  std::vector<std::string> missing;

  /// Reads ahead of the mix, for real-time playback only. Nothing offline has
  /// one: an export decodes what it needs where it needs it, which is simpler
  /// and exactly reproducible.
  std::thread reader;
  std::mutex reader_lock;
  std::condition_variable reader_wake;
  bool reader_stopping = false;

  ~Impl() {
    {
      const std::lock_guard<std::mutex> lock(reader_lock);
      reader_stopping = true;
    }
    reader_wake.notify_all();
    if (reader.joinable()) reader.join();
  }

  std::unique_ptr<audio::Limiter> limiter;

  /// Atomic because the master fader is the one thing about a mix that can be
  /// changed while it is playing: the player's render thread reads this on
  /// every block while the interface writes it from the message loop.
  std::atomic<double> master_gain{1.0};

  /// The master's automation, copied at build time. Empty when there is none or
  /// the mode is Off, so the mix reads the live fader instead — deciding it
  /// here rather than per block keeps `mix` free of what Off means.
  std::vector<core::Keyframe> master_curve;

  /// The meter belongs to whichever thread is mixing; its reading is published
  /// under a lock for whoever is drawing. A reading is a few dozen bytes and is
  /// taken once a block, so the lock is never contended for long enough to
  /// matter — and the alternative, tearing a stereo pair across two atomics,
  /// shows up as a meter whose channels disagree.
  std::unique_ptr<audio::Meter> meter;
  mutable std::mutex levels_lock;
  audio::MeterReading levels;

  /// One meter per track lane, so a mixer strip can show what its own track is
  /// putting into the mix rather than what the mix came to.
  ///
  /// Indexed by the plan's `track_index`, and as long as the highest lane that
  /// carries audio — a lane with nothing on it keeps a meter that reads silence
  /// rather than being absent, so a caller never has to ask whether a track has
  /// an entry before it can draw one.
  ///
  /// They share `levels_lock` with the master's reading. The whole set is
  /// published in one place once a block, which is also what stops a strip
  /// showing a level from one block beside a neighbour showing the last.
  std::vector<std::unique_ptr<audio::Meter>> track_meters;
  std::vector<audio::MeterReading> track_levels;

  /// What each lane's fader was set to when the mix was planned, and what it is
  /// set to now. The mix multiplies by the ratio, so a mixer nobody has touched
  /// multiplies by exactly one — an export is unchanged by this existing.
  ///
  /// Atomic for the same reason the master's is: the render thread reads them
  /// on every block while the interface writes them from the message loop.
  std::vector<double> built_track_gain;
  std::vector<std::atomic<double>> live_track_gain;

  /// One effect chain per lane, run on what the lane sums to before it reaches
  /// the mix. Empty for a track with no stack, which is nearly all of them and
  /// costs nothing to skip.
  std::vector<audio::EffectChain> track_chains;

  /// Which meter belongs to a project track, or -1 for one carrying no audio.
  ///
  /// Two things have been called a track index. The **plan** numbers audio
  /// tracks 0, 1, 2 and skips the video ones; everything above counts tracks as
  /// the project lists them. The two agree exactly when a project is all audio,
  /// which is what let the confusion past a green suite and a set of tests —
  /// and then showed up on screen as a meter that stayed dark on the one
  /// arrangement anybody actually has, a video track with sound under it.
  ///
  /// The public API takes the *project's* numbering, because that is what
  /// "track 1" means everywhere else. Inside, a lane is the plan's.
  std::vector<int> lane_of_track;

  [[nodiscard]] int lane_for(int track_index) const noexcept {
    if (track_index < 0) return -1;
    const auto at = static_cast<std::size_t>(track_index);
    return at < lane_of_track.size() ? lane_of_track[at] : -1;
  }

  /// The ratio to apply to a lane, by the plan's numbering.
  [[nodiscard]] double lane_trim(int lane_index) const noexcept {
    if (lane_index < 0) return 1.0;
    const auto lane = static_cast<std::size_t>(lane_index);
    if (lane >= built_track_gain.size()) return 1.0;
    const double built = built_track_gain[lane];
    // A lane built silent has no ratio that can bring it back, so moving its
    // fader off zero is a rebuild rather than a trim. Returning 1 leaves it as
    // the mix was planned rather than dividing by nothing.
    if (built <= 0.0) return 1.0;
    return live_track_gain[lane].load(std::memory_order_relaxed) / built;
  }

  /// Scratch, reused across calls so mixing does not allocate per block.
  std::vector<float> voice_block;
  /// One block's worth per track, summed as the voices go by. A track can hold
  /// several clips at once, so a track's level is not any one voice's — it is
  /// what they come to together, which is only known once they have all been
  /// through.
  std::vector<std::vector<float>> track_blocks;
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
    // One track at a time when asked for, which is what an export writing a
    // stream per track does. Skipped before the source is decoded rather than
    // muted after: a mixer for one track should not pay for the others' audio.
    if (settings.only_track.has_value() && entry.track_index != *settings.only_track) continue;

    Voice voice;
    voice.planned = entry;
    // Built at the clip's own start, which is where playback of it begins and
    // therefore what an animated parameter is worth the first time it is heard.
    voice.chain = audio::EffectChain::build(entry.clip->audio_effects,
                                            static_cast<double>(settings.sample_rate),
                                            settings.channels, 0.0);

    if (entry.media != nullptr && !entry.media->path.empty()) {
      // Only the clip's own trim, not the whole file.
      const double from = std::max(0.0, entry.source_in - kDecodeMargin);
      const double to = entry.source_out + kDecodeMargin;
      const SourceKey key{entry.media->path, entry.audio_stream, from, to};

      auto found = impl->sources.find(key);
      if (found == impl->sources.end()) {
        auto source = std::make_unique<StreamedSource>();
        source->key = key;
        source->sample_rate = settings.sample_rate;
        source->channels = settings.channels;
        // Short enough to hold, or wanted end to end by a retime that reads it
        // that way. Either forces the whole range resident, and a retimed clip
        // is why this is decided per source rather than per length alone.
        source->whole = (to - from) <= kWholeSourceSeconds || needs_retiming(entry);
        source->wanted.store(from, std::memory_order_relaxed);

        // The first window is read here, so a mixer that has been created is
        // one that can be mixed from — an export never waits on a thread and
        // the player has sound the instant it starts.
        auto window = read_window(*source, from);
        if (window == nullptr) {
          // Recorded and mixed as silence: an export should complete with a
          // hole rather than fail at the last step.
          missing.insert(entry.media->id);
          source->failed.store(true, std::memory_order_relaxed);
        }
        source->resident.store(std::move(window));
        found = impl->sources.emplace(key, std::move(source)).first;
      }

      StreamedSource& source = *found->second;
      if (!source.failed.load(std::memory_order_relaxed)) {
        if (needs_retiming(entry)) {
          // Whole by construction, so this is the entire clip's audio.
          const std::shared_ptr<const Window> window = source.resident.load();
          if (window != nullptr) {
            voice.retimed =
                render_retimed(window->audio, entry, settings.sample_rate, settings.channels);
          }
          voice.timeline_indexed = true;
        } else {
          // Safe to hold: the map owns the source through a unique pointer, so
          // its address survives every later insertion.
          voice.stream = &source;
        }
      }
    } else {
      missing.insert(entry.clip->media_id);
    }

    impl->voices.push_back(std::move(voice));
  }

  // A retimed voice whose stretch produced nothing has no audio to read, and
  // saying so here is what keeps the mixing loop from checking every block.
  for (Voice& voice : impl->voices) {
    if (voice.timeline_indexed && voice.retimed.frame_count() == 0) {
      voice.timeline_indexed = false;
      voice.stream = nullptr;
    }
  }

  // Whatever the retimes left behind. A source read only to be stretched is a
  // whole clip's audio held twice over — once raw and once retimed — and only
  // one of them is ever read again.
  for (auto& [key, source] : impl->sources) {
    const bool wanted_raw = std::ranges::any_of(
        impl->voices, [&](const Voice& voice) { return voice.stream == source.get(); });
    if (!wanted_raw) source->resident.store(nullptr);
  }

  impl->missing.assign(missing.begin(), missing.end());
  impl->master_gain.store(std::clamp(project.master_gain, 0.0, core::kMaxMasterGain),
                          std::memory_order_relaxed);
  if (core::is_master_gain_animated(project)) impl->master_curve = project.master_gain_keyframes;
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

  // A meter per lane that carries audio. Sized from the plan rather than from
  // the project's track list, because the plan is what says which lanes have
  // anything on them — and `only_track` means a mixer can legitimately hold a
  // voice for one lane and nothing for the rest.
  std::size_t lanes = 0;
  for (const Voice& voice : impl->voices) {
    lanes = std::max(lanes, static_cast<std::size_t>(voice.planned.track_index) + 1);
  }
  impl->track_meters.reserve(lanes);
  for (std::size_t i = 0; i < lanes; ++i) {
    impl->track_meters.push_back(std::make_unique<audio::Meter>(
        static_cast<double>(settings.sample_rate), settings.channels,
        audio::MeterSettings{.over = limiting.limit}));
  }
  impl->track_levels.assign(lanes, impl->meter->read());
  impl->track_blocks.resize(lanes);

  // Which lane each project track owns, walked in the same order the plan
  // walks them so the two numberings cannot disagree.
  impl->lane_of_track.assign(impl->project.tracks.size(), -1);
  impl->built_track_gain.assign(lanes, 1.0);
  impl->track_chains.resize(lanes);
  int ordinal = 0;
  for (std::size_t i = 0; i < impl->project.tracks.size(); ++i) {
    if (impl->project.tracks[i].kind != core::TrackKind::Audio) continue;
    const int lane = ordinal++;
    impl->lane_of_track[i] = lane;

    // The lane's own stack. Built at zero because a track's effect keyframes
    // are in timeline seconds and the sequence starts there.
    if (static_cast<std::size_t>(lane) < lanes &&
        !impl->project.tracks[i].audio_effects.empty()) {
      impl->track_chains[static_cast<std::size_t>(lane)] = audio::EffectChain::build(
          impl->project.tracks[i].audio_effects, static_cast<double>(settings.sample_rate),
          settings.channels, 0.0);
    }
    // The gain the lane was planned with, so a live fader can be a ratio
    // against it. Read from the project rather than from the plan: the plan
    // folds track gain into each clip's, and unpicking it from there would be
    // reading a product for one of its factors.
    if (static_cast<std::size_t>(lane) < lanes) {
      impl->built_track_gain[static_cast<std::size_t>(lane)] = impl->project.tracks[i].gain;
    }
  }
  impl->live_track_gain = std::vector<std::atomic<double>>(lanes);
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    impl->live_track_gain[lane].store(impl->built_track_gain[lane], std::memory_order_relaxed);
  }

  // Only when something can actually run out. Every source held whole means
  // there is nothing for a reader to do, which is the ordinary project.
  const bool anything_windowed =
      std::ranges::any_of(impl->sources, [](const auto& entry) { return !entry.second->whole; });
  if (settings.realtime && anything_windowed) {
    Impl* raw = impl.get();
    raw->reader = std::thread([raw] {
      for (;;) {
        {
          std::unique_lock<std::mutex> lock(raw->reader_lock);
          // A short wait rather than a signal per block: the mixing thread must
          // not pay for waking anybody, and audio decodes hundreds of times
          // faster than it plays, so checking ten times a second is far more
          // often than a window can be used up.
          raw->reader_wake.wait_for(lock, std::chrono::milliseconds(100),
                                    [raw] { return raw->reader_stopping; });
          if (raw->reader_stopping) return;
        }

        for (auto& [key, source] : raw->sources) {
          if (source->whole || source->failed.load(std::memory_order_relaxed)) continue;
          const double at = source->wanted.load(std::memory_order_relaxed);
          const std::shared_ptr<const Window> have = source->resident.load();
          if (covers(have.get(), *source, at)) continue;

          // Built beside what is being read rather than over it, so the mixing
          // thread's pointer stays valid for as long as it holds it.
          if (auto fresh = read_window(*source, at)) {
            source->resident.store(std::move(fresh));
          } else {
            source->failed.store(true, std::memory_order_relaxed);
          }
        }
      }
    });
  }

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

  // Cleared rather than reallocated. `assign` on a vector that has already held
  // a block of this size keeps its storage, which is what keeps the mixing
  // thread free of the allocator — the same reason `voice_block` is a member.
  for (std::vector<float>& lane : impl_->track_blocks) lane.assign(out.size(), 0.0f);

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
    if (voice.stream == nullptr && !voice.timeline_indexed) continue;
    if (block_end <= voice.planned.start || begin >= voice.planned.end) continue;

    // Resolved once for the block rather than per frame. A retimed voice reads
    // its own baked audio; a shared source is whatever window is resident, and
    // holding the pointer for the whole block is what makes it safe to read
    // while a reader thread builds the next one.
    std::shared_ptr<const Window> window;
    const media::AudioBuffer* audio = nullptr;
    if (voice.timeline_indexed) {
      audio = &voice.retimed;
    } else {
      const double at =
          render::audio_source_time_at(voice.planned, std::max(begin, voice.planned.start));
      voice.stream->wanted.store(at, std::memory_order_relaxed);

      window = voice.stream->resident.load();
      if (!impl_->settings.realtime && !covers(window.get(), *voice.stream, at)) {
        // Nothing is waiting on this thread, so read it here and now. That is
        // what makes an export deterministic: no thread decides how much had
        // arrived by the time a block was mixed.
        if (auto fresh = read_window(*voice.stream, at)) {
          voice.stream->resident.store(fresh);
          window = std::move(fresh);
        }
      }
      if (window == nullptr) continue;  // not read yet: this voice is silent
      audio = &window->audio;
    }
    if (audio == nullptr || audio->frame_count() == 0) continue;

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

    // Read once for the block rather than per sample. A fader moved mid-block
    // lands on the next one, which is a millisecond or two — far below anything
    // a hand can aim at, and it keeps the whole block on one gain rather than
    // stepping partway through it.
    const double trim = impl_->lane_trim(voice.planned.track_index);

    for (std::size_t i = 0; i < span; ++i) {
      const double now =
          static_cast<double>(base + static_cast<std::int64_t>(first + i)) / rate;

      // A retimed voice was rendered into timeline time already, so its read
      // position is just how far into the clip we are; anything else is read
      // from the shared source at the source time the plan asks for.
      const double position =
          voice.timeline_indexed
              ? (now - voice.planned.start) * rate
              : (render::audio_source_time_at(voice.planned, now) - audio->start_time) * rate;

      sample_into(*audio, position, channels,
                  std::span(impl_->voice_block).subspan(i * channels, channels));

      // Gain and fades before the effects, matching the reference's chain. The
      // live trim rides on the same number, so a fader moved while playing
      // lands exactly where the track's own gain already sits in the chain.
      const auto gain = static_cast<float>(render::audio_gain_at(voice.planned, now) * trim);
      for (std::size_t c = 0; c < channels; ++c) impl_->voice_block[i * channels + c] *= gain;

      // The panner, on the two sides of a stereo bus and nothing else. A bus
      // with one channel has no sides to balance, and one with more than two
      // has sides this control does not name — turning only the first two of a
      // 5.1 mix would be worse than leaving it alone.
      if (channels == 2) {
        const render::StereoGain pan = render::audio_pan_at(voice.planned, now);
        impl_->voice_block[i * channels] *= static_cast<float>(pan.left);
        impl_->voice_block[i * channels + 1] *= static_cast<float>(pan.right);
      }
    }

    // Clip-local, because a clip's effect keyframes are.
    run_chain_over(voice.chain, impl_->voice_block, base + static_cast<std::int64_t>(first), rate,
                   channels, voice.planned.start);

    // Into the lane's own block rather than straight into the mix. A plain sum:
    // `normalize=0`, so clips add rather than being scaled down by how many of
    // them there are.
    //
    // This used to add into `out` as well, with the lane block kept alongside
    // as a tap for the meter. It cannot any more: a track has an effect stack
    // now, and a stack has to run on what the lane came to before that reaches
    // the mix. So the lane block *is* the signal path, and the sum into `out`
    // happens once per lane after its chain, below.
    const auto lane = static_cast<std::size_t>(voice.planned.track_index);
    if (lane < impl_->track_blocks.size()) {
      std::vector<float>& into = impl_->track_blocks[lane];
      for (std::size_t i = 0; i < span * channels; ++i) {
        into[first * channels + i] += impl_->voice_block[i];
      }
    }
  }

  // Each lane through its own stack, then into the mix.
  for (std::size_t lane = 0; lane < impl_->track_blocks.size(); ++lane) {
    std::vector<float>& block = impl_->track_blocks[lane];
    if (lane < impl_->track_chains.size() && !impl_->track_chains[lane].empty()) {
      run_chain_over(impl_->track_chains[lane], block, base, rate, channels, 0.0);
    }
    for (std::size_t i = 0; i < block.size(); ++i) out[i] += block[i];
  }

  // The master fader, ahead of the limiter: pulling it down has to take a hot
  // mix *out* of limiting, and after the limiter it would only make an already
  // squashed mix quieter.
  //
  // The curve wins over the live fader when there is one, exactly as a track's
  // does: an automated fader is being *played back*, and letting the atomic
  // override it would mean the number the interface last wrote silently
  // flattening somebody's pass.
  const double master = impl_->master_curve.empty()
                            ? impl_->master_gain.load(std::memory_order_relaxed)
                            : std::clamp(core::eval_keyframes(impl_->master_curve, begin), 0.0,
                                         core::kMaxMasterGain);
  if (master != 1.0) {
    const auto gain = static_cast<float>(master);
    for (float& sample : out) sample *= gain;
  }

  // Metered here, between the fader and the limiter: this is the mix as it was
  // made, which is what a meter is for. See `levels`.
  impl_->meter->process(out);

  // The lanes are metered *before* the master fader, unlike the mix. A track
  // meter answers "what is this track putting out", and pulling the master down
  // does not change that — a set of strips that all dropped together when the
  // master moved would be five meters showing one thing.
  for (std::size_t lane = 0; lane < impl_->track_meters.size(); ++lane) {
    impl_->track_meters[lane]->process(impl_->track_blocks[lane]);
  }

  // One lock for the whole set, so a strip can never show a level from this
  // block beside a neighbour still showing the last.
  {
    const std::lock_guard lock(impl_->levels_lock);
    impl_->levels = impl_->meter->read();
    for (std::size_t lane = 0; lane < impl_->track_meters.size(); ++lane) {
      impl_->track_levels[lane] = impl_->track_meters[lane]->read();
    }
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

audio::MeterReading AudioMixer::track_levels(int track_index) const noexcept {
  const std::lock_guard lock(impl_->levels_lock);
  const int lane = impl_->lane_for(track_index);
  if (lane < 0 || static_cast<std::size_t>(lane) >= impl_->track_levels.size()) {
    // A track carrying no audio, or one this mixer was not built for. Silence
    // is the true answer and saves every caller a check it would get wrong.
    audio::MeterReading quiet;
    quiet.count = impl_->settings.channels;
    return quiet;
  }
  return impl_->track_levels[static_cast<std::size_t>(lane)];
}

std::size_t AudioMixer::track_count() const noexcept { return impl_->track_levels.size(); }

void AudioMixer::set_track_gain(int track_index, double gain) noexcept {
  const int lane = impl_->lane_for(track_index);
  if (lane < 0 || static_cast<std::size_t>(lane) >= impl_->live_track_gain.size()) return;
  impl_->live_track_gain[static_cast<std::size_t>(lane)].store(
      std::clamp(gain, 0.0, core::kMaxGain), std::memory_order_relaxed);
}

double AudioMixer::track_gain(int track_index) const noexcept {
  const int lane = impl_->lane_for(track_index);
  if (lane < 0 || static_cast<std::size_t>(lane) >= impl_->live_track_gain.size()) return 0.0;
  return impl_->live_track_gain[static_cast<std::size_t>(lane)].load(std::memory_order_relaxed);
}

void AudioMixer::flush(std::span<float> out) { impl_->limiter->flush(out); }

std::size_t AudioMixer::latency_frames() const noexcept {
  return impl_->limiter->latency_frames();
}

void AudioMixer::reset() {
  for (Voice& voice : impl_->voices) voice.chain.reset();
  // The track stacks as well, for exactly the reason the voices' are: a filter
  // carries a memory of what came before, and after a seek what came before is
  // not what is about to be played.
  for (audio::EffectChain& chain : impl_->track_chains) chain.reset();
  impl_->limiter->reset();

  // The meter too: levels only fall while audio is being measured, so a seek
  // that lands somewhere quiet would otherwise leave the bars where the last
  // loud thing left them.
  impl_->meter->reset();
  for (const std::unique_ptr<audio::Meter>& meter : impl_->track_meters) meter->reset();

  const std::lock_guard lock(impl_->levels_lock);
  impl_->levels = impl_->meter->read();
  for (std::size_t lane = 0; lane < impl_->track_meters.size(); ++lane) {
    impl_->track_levels[lane] = impl_->track_meters[lane]->read();
  }
}

bool AudioMixer::silent() const noexcept {
  return std::ranges::none_of(impl_->voices, [](const Voice& v) {
    return v.stream != nullptr || v.timeline_indexed;
  });
}

const AudioMixSettings& AudioMixer::settings() const noexcept { return impl_->settings; }

const std::vector<std::string>& AudioMixer::missing_media() const noexcept {
  return impl_->missing;
}

}  // namespace cutline::engine
