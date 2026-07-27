#pragma once

#include <string>
#include <string_view>

namespace cutline::core {

/// Duration of one frame at `fps`, in seconds. `fps` is clamped to >= 1.
[[nodiscard]] double frame_duration(double fps) noexcept;

/// Snaps `seconds` to the nearest frame boundary at `fps`.
[[nodiscard]] double snap_to_frame(double seconds, double fps) noexcept;

/// Parses "HH:MM:SS.xx", "MM:SS.xx", or a bare seconds value into seconds.
/// Malformed input yields 0, matching the reference implementation.
[[nodiscard]] double time_to_seconds(std::string_view text) noexcept;

/// Formats seconds as "HH:MM:SS.ss" — two decimal places, frame-rate
/// independent. Used for durations and file-scoped times.
[[nodiscard]] std::string seconds_to_timestamp(double seconds);

/// Formats seconds as Premiere-style "HH:MM:SS:FF" at `fps`, where FF counts
/// whole frames within the current second. Used for the playhead readout.
[[nodiscard]] std::string seconds_to_timecode(double seconds, double fps);

/// Human-readable byte count ("12.00 MB"). Negative or non-finite input
/// yields "Unknown Size".
[[nodiscard]] std::string readable_file_size(double bytes);

}  // namespace cutline::core
