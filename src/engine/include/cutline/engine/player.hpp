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

#include "cutline/core/model.hpp"

#include <expected>
#include <memory>
#include <string>

namespace cutline::engine {

struct PlayerSettings {
  /// Preferred format. The device's own mix format wins when they differ —
  /// shared-mode WASAPI resamples anything else, and doing it twice is worse
  /// than letting the mixer produce what the card already wants.
  int sample_rate = 48000;
  int channels = 2;
};

class Player {
 public:
  /// Opens the default output device and decodes everything the project needs.
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
