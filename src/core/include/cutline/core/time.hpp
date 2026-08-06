#pragma once

#include <optional>
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

/// Whether drop-frame timecode means anything at this rate.
///
/// True only for the rates that are a thousand-and-first slower than a whole
/// number — 29.97 and 59.94. At 25 or at 30 the timecode already counts real
/// seconds and there is nothing to drop; offering the choice there would be
/// offering two names for one thing.
[[nodiscard]] bool supports_drop_frame(double fps) noexcept;

/// Formats seconds as Premiere-style "HH:MM:SS:FF" at `fps`.
///
/// The frame index comes from the *actual* rate and the fields are counted at
/// the nominal one, which is what timecode is: at 29.97 the thirtieth frame of
/// a second is `:29` and the next is the following second's `:00`. Counting the
/// index at the nominal rate instead — which this did until drop-frame went in
/// — skips a number about every thousand frames, so the readout visibly
/// stutters while the picture does not.
///
/// Non-drop timecode therefore falls behind the clock, by about 3.6 seconds an
/// hour at 29.97. That is not an error; it is the whole reason drop-frame
/// exists. With `drop_frame`, frame *numbers* are skipped — two at the start of
/// every minute except every tenth — so the timecode tracks the clock again,
/// and the separator before the frames becomes `;` to say so.
[[nodiscard]] std::string seconds_to_timecode(double seconds, double fps,
                                              bool drop_frame = false);

/// And back: "HH:MM:SS:FF" at `fps` into seconds, snapped to a frame.
///
/// The inverse of `seconds_to_timecode`, and it has to be its own function
/// rather than `time_to_seconds` — the last field of a timecode is *frames*,
/// not hundredths, so reading "00:00:01:15" as a second and fifteen hundredths
/// is off by half a second at 30 fps.
///
/// Forgiving about what is typed, because a timecode field is typed into in a
/// hurry: fewer fields count from the right, so "12" is twelve frames, "2:12"
/// is two seconds and twelve frames, and a bare number with a full stop in it
/// is taken as seconds. Nothing that parses at all returns nothing — malformed
/// input yields nothing, which is what leaves the playhead where it was rather
/// than sending it to zero.
///
/// A `;` is accepted anywhere a `:` is, and means the same thing: what decides
/// whether the digits are read as drop-frame is `drop_frame`, not the
/// punctuation. Somebody copying a timecode out of an email should not have to
/// get the semicolon right for it to land in the right place.
[[nodiscard]] std::optional<double> timecode_to_seconds(std::string_view text, double fps,
                                                        bool drop_frame = false) noexcept;

/// Human-readable byte count ("12.00 MB"). Negative or non-finite input
/// yields "Unknown Size".
[[nodiscard]] std::string readable_file_size(double bytes);

}  // namespace cutline::core
