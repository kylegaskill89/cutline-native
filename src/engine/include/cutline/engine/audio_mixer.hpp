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
};

class AudioMixer {
 public:
  /// Builds the plan and decodes every source it names.
  ///
  /// Decoding is eager and whole-file, which is what the reference did and what
  /// real-time playback scheduling wants — roughly 230 MB per ten minutes of
  /// 48 kHz stereo. A long timeline of long sources is the case that will
  /// eventually need streaming decode; the interface does not assume either.
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

  /// Levels of the mix as of the last block, for a meter to draw.
  ///
  /// Measured after the master fader and before the limiter, so it reads what
  /// the limiter is being asked to hold back rather than what it let through.
  [[nodiscard]] audio::MeterReading levels() const noexcept;

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

 private:
  struct Impl;
  explicit AudioMixer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::engine
