#pragma once

/// Level metering for the master bus.
///
/// A meter is ballistics, not arithmetic: the numbers are easy and what makes
/// one readable is how fast it rises and how slowly it falls. Three readings
/// come out of every channel, because they answer different questions and no
/// single one of them is enough:
///
///  * **peak** — the loudest sample lately, falling steadily. This is what says
///    whether a mix is close to running out of headroom.
///  * **RMS** — the same signal averaged over a third of a second, which is
///    roughly what a needle-and-scale VU does and roughly what loudness is.
///    Speech peaks ten decibels above its RMS; music can peak twenty above.
///  * **hold** — where the peak got to, kept still long enough to read. Peaks
///    are milliseconds long and a bar that shows them honestly shows nothing an
///    eye can catch.
///
/// The fall rates are the conventions rather than anything derived: 20 dB per
/// second for the bar, which is the PPM standard, and a hold that sits for a
/// second and a half before falling more slowly still.
///
/// Metering is *pre-limiter*. A meter after the limiter can never read above
/// the ceiling, so it would go quiet about exactly the moment the mix got into
/// trouble; ahead of it, `over` means the limiter is having to work, which is
/// the thing worth being told.

#include <array>
#include <cstddef>
#include <span>

namespace cutline::audio {

/// The floor a level is reported at, standing in for silence. Finite rather
/// than negative infinity so every reader can do arithmetic on it.
inline constexpr double kMeterFloorDb = -100.0;

/// More than any output this mixes to; a fixed size keeps a reading a plain
/// value that can be copied out from under a lock in one go.
inline constexpr std::size_t kMaxMeterChannels = 8;

struct MeterSettings {
  /// How fast the bar falls, in decibels per second. The PPM convention.
  double fall_db_per_second = 20.0;
  /// How long the peak-hold mark sits still before it starts to fall, and how
  /// fast it falls once it does.
  double hold_seconds = 1.5;
  double hold_fall_db_per_second = 8.0;
  /// The averaging window for the RMS reading, in seconds.
  double rms_seconds = 0.3;
  /// The level `over` latches at, in linear amplitude — the limiter's ceiling,
  /// since that is the point past which something is being held back.
  double over = 0.95;
};

struct ChannelLevel {
  double peak_db = kMeterFloorDb;
  double rms_db = kMeterFloorDb;
  double hold_db = kMeterFloorDb;
  /// Latched: once the mix has been over the ceiling it stays lit until the
  /// meter is reset, which in practice means for the rest of the pass. A
  /// warning that cleared itself after a few milliseconds would be one nobody
  /// ever saw, since that is exactly how long the peak that tripped it lasted.
  bool over = false;

  friend bool operator==(const ChannelLevel&, const ChannelLevel&) = default;
};

struct MeterReading {
  std::array<ChannelLevel, kMaxMeterChannels> channels{};
  int count = 0;

  friend bool operator==(const MeterReading&, const MeterReading&) = default;
};

class Meter {
 public:
  Meter(double sample_rate, int channels, MeterSettings settings = {}) noexcept;

  /// Measures one interleaved block. Does not change it — a meter is a tap.
  void process(std::span<const float> interleaved) noexcept;

  /// The current levels. Cheap: everything is kept up to date as blocks go by,
  /// so a caller polling at frame rate costs nothing.
  [[nodiscard]] MeterReading read() const noexcept;

  /// Back to silence, `over` included.
  ///
  /// Levels only fall while audio is being measured, so a meter that stops
  /// being fed holds its last reading. That is what a paused meter should do
  /// for a moment and not what it should do for a minute, so whoever stops the
  /// audio resets it.
  void reset() noexcept;

 private:
  MeterSettings settings_;
  std::size_t channels_ = 2;
  double rate_ = 48000.0;

  /// Per-sample multipliers, precomputed: a constant number of decibels per
  /// second is a constant ratio per sample.
  double fall_ = 1.0;
  double hold_fall_ = 1.0;
  double rms_alpha_ = 1.0;

  struct State {
    double peak = 0.0;
    double hold = 0.0;
    double held_for = 0.0;  ///< seconds since the hold mark was last renewed
    double mean_square = 0.0;
    bool over = false;
  };
  std::array<State, kMaxMeterChannels> state_{};
};

/// Where a level sits on a meter's scale, as 0 at the floor to 1 at the top.
///
/// Shared so the widget and its tests agree, and so a caller drawing a scale
/// marks it in the same place the bar reaches.
[[nodiscard]] double meter_fraction(double db, double floor_db, double ceiling_db) noexcept;

}  // namespace cutline::audio
