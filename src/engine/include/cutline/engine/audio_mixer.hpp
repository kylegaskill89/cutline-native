#pragma once

/// Mixing a project's audio tracks into a single stream.
///
/// The audio counterpart of `FrameRenderer`, and built the same way: the pure
/// layer (`render::plan_audio`) decides what plays and how loud, this joins
/// that decision to decoded samples and the DSP.
///
/// Mixing is a plain sum, matching the reference's `amix` with `normalize=0`:
/// four tracks at unity really are four times as loud. A master limiter catches
/// what that produces rather than letting it clip.
///
/// Per-clip processing order is gain and fades first, then the effect stack,
/// which is the order the reference's filter chain used. It is worth being
/// deliberate about: a compressor placed after a fade partly undoes it, pulling
/// the quiet end back up, and a project mixed against that behaviour would
/// change if the order were tidied.

#include "cutline/audio/meter.hpp"
#include "cutline/core/model.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cutline::engine {

struct AudioMixSettings {
  int sample_rate = 48000;
  int channels = 2;

  /// Mix only the clips from this audio track, counting from the top of the
  /// stored order. Unset mixes the whole project, which is what playback and an
  /// ordinary export want.
  ///
  /// One track at a time is how an export writes separate streams: a mixer per
  /// track, each producing exactly what that track contributes to the mix. The
  /// master fader and the limiter still apply to each — the alternative is a
  /// set of stems that sum to something louder than the file anybody checked.
  std::optional<int> only_track;

  /// Whether this mixer is feeding a sound card.
  ///
  /// It decides one thing: what happens when a long source is asked for audio
  /// it has not read yet. Off — an export, a test — the samples are decoded
  /// there and then, which is simple, exactly reproducible, and free of
  /// threads. On, nothing may touch a file on the mixing thread, because that
  /// thread has a few milliseconds to fill a buffer and a decode is tens of
  /// them; a reader keeps the window ahead of the playhead instead, and audio
  /// it has not reached yet mixes as silence rather than as a missed deadline.
  ///
  /// Off by default, so anything that is not real time gets the deterministic
  /// behaviour without having to know this exists.
  bool realtime = false;
};

class AudioMixer {
 public:
  /// Builds the plan and decodes every source it names.
  ///
  /// A clip short enough to hold is decoded here and whole, which is nearly
  /// every clip anybody cuts and keeps the simplest thing the common thing.
  /// A clip longer than `kWholeSourceSeconds` gets a **window** instead: the
  /// part of it around wherever the mix has reached, refilled as that moves.
  ///
  /// This is what stops a long capture costing its whole length in memory
  /// before a note is heard. Measured on the reference ten-minute capture with
  /// its four audio streams, one clip each spanning the whole source:
  ///
  /// | | whole | windowed |
  /// |---|---|---|
  /// | before the player is ready | 2.9 s | 0.41 s |
  /// | held | 887 MB | 119 MB |
  ///
  /// Decoding audio runs at something like eight hundred times real time, so a
  /// reader has no difficulty staying in front of a playhead; the window is
  /// generous either side precisely so that ordinary scrubbing stays inside it
  /// and never waits for anything.
  ///
  /// **A retimed clip is still decoded whole**, however long it is: reversing
  /// it and stretching it are done once, up front, over the clip's whole span,
  /// and a window cannot be handed to a process that reads its input end to
  /// end. Speed and reverse are the exception rather than the rule, and this is
  /// where to start if they stop being.
  ///
  /// A source that cannot be read is not an error: it is recorded in
  /// `missing_media` and mixed as silence, so an export completes with a hole
  /// rather than failing at the last step.
  [[nodiscard]] static std::expected<std::unique_ptr<AudioMixer>, std::string> create(
      const core::Project& project, AudioMixSettings settings = {});

  AudioMixer(const AudioMixer&) = delete;
  AudioMixer& operator=(const AudioMixer&) = delete;
  ~AudioMixer();

  /// Mixes `out.size() / channels` frames beginning at timeline time `t`,
  /// overwriting whatever `out` held.
  ///
  /// Calls are expected to walk forwards; each clip's effect chain carries its
  /// state between them, so splitting a span into blocks gives the same samples
  /// as mixing it whole. Seeking backwards or jumping needs `reset` first, or
  /// the filters ring with audio that no longer precedes what is playing.
  [[nodiscard]] std::expected<void, std::string> mix(double t, std::span<float> out);

  /// Changes the master fader while mixing continues.
  ///
  /// The one property of a mix that can be changed without rebuilding it, and
  /// the reason is what a master fader is for: it is adjusted *by ear*, against
  /// what is playing, and a fader that only took effect once playback had been
  /// torn down and rebuilt could not be used that way.
  void set_master_gain(double gain) noexcept;
  [[nodiscard]] double master_gain() const noexcept;

  /// Changes one track's fader while mixing continues.
  ///
  /// The same argument as the master's, one level down: a track fader is
  /// balanced *against the others*, by ear, while they are all playing. One
  /// that only took effect after the mix had been torn down and rebuilt could
  /// not be used that way — and rebuilding is what applying the edit to the
  /// document does, so the document waits for the gesture to end.
  ///
  /// Kept as a ratio against the gain the mix was **built** with rather than
  /// replacing it, so a mixer whose faders nobody has touched multiplies by
  /// exactly one and an export is bit-for-bit what it always was.
  ///
  /// A track gain is a constant per lane today. When it becomes animatable —
  /// which is what the automation modes need — this becomes a trim over the
  /// curve rather than over a number, and that is the same arithmetic.
  void set_track_gain(int track_index, double gain) noexcept;
  /// What the fader is set to now, which is the built gain until somebody
  /// moves it. Silence for a lane this mixer has none of.
  [[nodiscard]] double track_gain(int track_index) const noexcept;

  /// Levels of the mix as of the last block, for a meter to draw.
  ///
  /// Measured after the master fader and before the limiter, so it reads what
  /// the limiter is being asked to hold back rather than what it let through.
  [[nodiscard]] audio::MeterReading levels() const noexcept;

  /// Levels of one track's own output as of the last block.
  ///
  /// What a mixer strip's meter shows, and the reason it is worth having beside
  /// the master's: a mix that is too hot says nothing about which track is
  /// making it so, and finding out by soloing each one in turn is the job a
  /// meter per strip exists to remove.
  ///
  /// Measured **before** the master fader, unlike `levels`. A track meter
  /// answers "what is this track putting out", and pulling the master down does
  /// not change that — strips that all dropped together when the master moved
  /// would be several meters showing one thing.
  ///
  /// Everything that makes the track's sound is in it: clip gain, fades, the
  /// panner and the effect chain, summed across every clip that lane has
  /// playing at once. A lane with nothing on it, or an index this mixer was
  /// not built for, reads as silence rather than as an error.
  [[nodiscard]] audio::MeterReading track_levels(int track_index) const noexcept;

  /// How many lanes have a meter — one past the highest that carries audio.
  [[nodiscard]] std::size_t track_count() const noexcept;

  /// Drains the limiter's look-ahead into `out`, which should hold
  /// `latency_frames()` frames. The tail of a timeline is lost without it.
  void flush(std::span<float> out);

  /// Frames of delay the master limiter introduces.
  [[nodiscard]] std::size_t latency_frames() const noexcept;

  /// Clears every filter's history. Required after a seek.
  void reset();

  /// True when nothing is planned, so a caller can skip writing an audio
  /// stream at all rather than writing silence.
  [[nodiscard]] bool silent() const noexcept;

  [[nodiscard]] const AudioMixSettings& settings() const noexcept;

  /// Media ids whose audio could not be decoded, deduplicated.
  [[nodiscard]] const std::vector<std::string>& missing_media() const noexcept;

  /// Measures a project's programme loudness, in LUFS, by mixing all of it.
  ///
  /// Offline and complete: loudness is *integrated* over a programme, so there
  /// is no such thing as measuring the part you happen to be looking at. A
  /// ten-minute sequence takes a few seconds, because audio decodes at some
  /// hundreds of times real time and this reads no pictures at all.
  ///
  /// Mixed exactly as it would be exported — through every clip's stack, every
  /// track's, the master fader and the master's — so the number is about the
  /// file that would be written rather than about an intermediate nobody hears.
  /// The limiter is included for the same reason.
  ///
  /// `kAbsoluteGateLufs` for a sequence with no audio in it, which is what
  /// `audio::LoudnessMeter` reports for silence and is the honest answer.
  [[nodiscard]] static std::expected<double, std::string> measure_loudness(
      const core::Project& project, AudioMixSettings settings = {});

 private:
  struct Impl;
  explicit AudioMixer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::engine
