#pragma once

/// Real-time playback, with the audio device as the clock.
///
/// Which subsystem keeps time is a decision, not a detail. Audio is the one
/// that cannot be nudged: dropping or repeating a video frame is invisible at
/// 60 Hz, while a gap of the same length in audio is a click, and a clock that
/// drifts against the sound card eventually produces one. So the sound card's
/// own consumption drives the timeline, and the preview asks where the playhead
/// is rather than deciding.
///
/// This owns a render thread. WASAPI hands out a buffer every few milliseconds
/// and expects it filled before the next one; missing that deadline is audible,
/// so the thread does nothing but mix — no decoding, no allocation, no file
/// access, all of which happened when the player was created.

#include "cutline/audio/meter.hpp"
#include "cutline/core/model.hpp"

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace cutline::engine {

/// An output the machine offers, as something to choose from a list.
struct AudioOutput {
  /// The system's own identifier, which survives a reboot and a rename. What a
  /// preference stores.
  std::string id;
  /// What it is called, which is what somebody reads. Not stable — a device can
  /// be renamed, and two identical interfaces have the same name — so it is
  /// never what a setting is keyed by.
  std::string name;
  bool is_default = false;
};

/// Every output device that could be played through, most useful first.
///
/// Empty when the machine has none, which is normal on a build server and is
/// not an error: playback simply fails later, with a message about it.
[[nodiscard]] std::vector<AudioOutput> audio_outputs();

struct PlayerSettings {
  /// Preferred format. The device's own mix format wins when they differ —
  /// shared-mode WASAPI resamples anything else, and doing it twice is worse
  /// than letting the mixer produce what the card already wants.
  int sample_rate = 48000;
  int channels = 2;

  /// Where playback is about to begin, in timeline seconds.
  ///
  /// Only the audio about to be heard is decoded before `create` returns; the
  /// rest is fetched while the first seconds play. Without it a press of Play
  /// paid one decode per cut in the whole sequence — see
  /// `AudioMixSettings::start_at`, which this is passed straight to.
  ///
  /// Seeking afterwards is still free: what has not been read yet mixes as
  /// silence for a moment rather than stalling, and the reader goes where the
  /// playhead went.
  double start_at = 0.0;

  /// Which output to play through, or empty for whichever the system prefers.
  ///
  /// A device that is not there falls back to the default rather than failing.
  /// An interface somebody unplugs should cost them the sound they were used
  /// to, not the ability to play at all — and keeping the setting means
  /// plugging it back in resumes without anybody visiting a dialog.
  std::string device_id;
};

class Player {
 public:
  /// Opens the chosen output device — or the default — and decodes everything
  /// the project needs.
  /// Fails when there is no usable output device, which is normal on a
  /// headless machine.
  [[nodiscard]] static std::expected<std::unique_ptr<Player>, std::string> create(
      const core::Project& project, PlayerSettings settings = {});

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  ~Player();

  void play();
  void pause();
  [[nodiscard]] bool playing() const noexcept;

  /// Where the playhead is, in timeline seconds.
  ///
  /// Derived from what the device has actually *played*, not from what has been
  /// submitted to it — those differ by the buffer depth, and using the latter
  /// would run the picture ahead of the sound by a few tens of milliseconds.
  [[nodiscard]] double position() const noexcept;

  /// Moves the playhead. Takes effect on the next buffer, and drops whatever
  /// was already queued so the jump is not preceded by a moment of the old
  /// position.
  void seek(double seconds);

  /// Moves the master fader, taking effect on the next block. The one edit a
  /// player accepts without being rebuilt, because a master fader is set by
  /// ear against what is playing.
  void set_master_gain(double gain);

  /// Moves one track's fader, taking effect on the next block. The same
  /// argument as the master's one level down: a track is balanced against the
  /// others, by ear, while they are all playing.
  void set_track_gain(int track_index, double gain);

  /// The mix's levels as of the last block. Safe to poll at frame rate.
  ///
  /// Reads as silence while paused: levels fall only while audio is being
  /// measured, and a meter frozen at whatever was playing when the space bar
  /// was pressed says something untrue.
  [[nodiscard]] audio::MeterReading levels() const;

  /// One track's own levels as of the last block, for a mixer strip's meter.
  ///
  /// Reads as silence while paused, for the same reason `levels` does: a meter
  /// falls only while audio is going past it, so one frozen at whatever was
  /// playing when the space bar was pressed is saying something untrue.
  [[nodiscard]] audio::MeterReading track_levels(int track_index) const;

  /// Timeline length, so a caller knows where playback will stop.
  [[nodiscard]] double duration() const noexcept;

  /// True once the playhead has run past the end. Playback stops there rather
  /// than running on through silence.
  [[nodiscard]] bool finished() const noexcept;

  [[nodiscard]] const std::string& device_name() const noexcept;
  [[nodiscard]] int sample_rate() const noexcept;
  [[nodiscard]] int channels() const noexcept;

  /// Whether the project had anything audible. A silent project still keeps
  /// time — the device runs, so the playhead still moves.
  [[nodiscard]] bool silent() const noexcept;

  /// An error the render thread hit. Non-empty means playback has stopped.
  [[nodiscard]] std::string error() const;

 private:
  struct Impl;
  explicit Player(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::engine
