#include "cutline/core/time.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <vector>

namespace cutline::core {
namespace {

constexpr std::string_view kWhitespace = " \t\n\r\f\v";

/// Frame counts above this are treated as out of range rather than cast to an
/// integer (which would be undefined behaviour for absurd inputs).
constexpr double kMaxFrames = 1e15;

/// JavaScript `Math.round`: ties round toward +infinity, whereas `std::round`
/// rounds ties away from zero. Kept explicit so ported timing behaviour matches
/// the TypeScript reference exactly for negative inputs.
[[nodiscard]] double js_round(double x) noexcept { return std::floor(x + 0.5); }

[[nodiscard]] std::string_view trim(std::string_view s) noexcept {
  const auto first = s.find_first_not_of(kWhitespace);
  if (first == std::string_view::npos) return {};
  const auto last = s.find_last_not_of(kWhitespace);
  return s.substr(first, last - first + 1);
}

/// `Number(string)` over the subset that appears in timecode fields: leading
/// and trailing whitespace is ignored, an empty field is 0, and anything else
/// must parse in full or the conversion fails.
[[nodiscard]] bool js_number(std::string_view s, double& out) noexcept {
  s = trim(s);
  if (s.empty()) {
    out = 0.0;
    return true;
  }
  if (s.front() == '+') s.remove_prefix(1);  // from_chars rejects a leading '+'

  double value = 0.0;
  const char* begin = s.data();
  const char* end = begin + s.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  out = value;
  return true;
}

}  // namespace

double frame_duration(double fps) noexcept { return 1.0 / std::max(1.0, fps); }

double snap_to_frame(double seconds, double fps) noexcept {
  const double f = std::max(1.0, fps);
  return js_round(seconds * f) / f;
}

double time_to_seconds(std::string_view text) noexcept {
  const std::string_view s = trim(text);
  if (s.empty()) return 0.0;

  std::vector<double> nums;
  std::size_t pos = 0;
  while (true) {
    const auto colon = s.find(':', pos);
    const std::string_view part =
        colon == std::string_view::npos ? s.substr(pos) : s.substr(pos, colon - pos);
    double value = 0.0;
    if (!js_number(part, value)) return 0.0;
    nums.push_back(value);
    if (colon == std::string_view::npos) break;
    pos = colon + 1;
  }

  if (nums.size() == 3) return nums[0] * 3600.0 + nums[1] * 60.0 + nums[2];
  if (nums.size() == 2) return nums[0] * 60.0 + nums[1];
  return nums[0];
}

std::string seconds_to_timestamp(double seconds) {
  double s = seconds;
  if (!std::isfinite(s) || s < 0.0) s = 0.0;

  const auto hours = static_cast<std::int64_t>(std::floor(s / 3600.0));
  const auto minutes = static_cast<std::int64_t>(std::floor(std::fmod(s, 3600.0) / 60.0));
  const double secs = std::fmod(s, 60.0);
  return std::format("{:02}:{:02}:{:05.2f}", hours, minutes, secs);
}

bool supports_drop_frame(double fps) noexcept {
  // The rates that are a thousand-and-first slower than a whole number, which
  // is the only case timecode can drift from the clock at. Compared against the
  // exact ratios rather than against 29.97, because that is what a file holds
  // and what a preset writes.
  if (!std::isfinite(fps)) return false;
  return std::abs(fps - 30000.0 / 1001.0) < 0.01 || std::abs(fps - 60000.0 / 1001.0) < 0.01;
}

namespace {

/// How many frame numbers are skipped at the start of a dropped minute: two at
/// 29.97, four at 59.94. It is the count that makes the timecode track the
/// clock, and it falls out of the rate rather than being a table.
[[nodiscard]] std::int64_t drop_count(std::int64_t nominal) noexcept { return nominal / 15; }

/// Frame index to the number a drop-frame clock would show.
///
/// Nine minutes in every ten lose their first `drop` numbers; the tenth keeps
/// them, which is what stops the correction overshooting.
[[nodiscard]] std::int64_t to_drop_frame(std::int64_t frames, std::int64_t nominal) noexcept {
  const std::int64_t drop = drop_count(nominal);
  const std::int64_t per_minute = 60 * nominal - drop;
  const std::int64_t per_ten_minutes = 10 * 60 * nominal - 9 * drop;

  const std::int64_t tens = frames / per_ten_minutes;
  const std::int64_t rest = frames % per_ten_minutes;

  std::int64_t shown = frames + 9 * drop * tens;
  // The first `drop` frames of a ten-minute block belong to its un-dropped
  // minute, so they take no correction of their own.
  if (rest > drop) shown += drop * ((rest - drop) / per_minute);
  return shown;
}

/// And back: the number a drop-frame clock shows to the frame index it means.
[[nodiscard]] std::int64_t from_drop_frame(std::int64_t hours, std::int64_t minutes,
                                           std::int64_t seconds, std::int64_t frames,
                                           std::int64_t nominal) noexcept {
  const std::int64_t drop = drop_count(nominal);
  const std::int64_t total_minutes = 60 * hours + minutes;
  const std::int64_t shown =
      ((hours * 60 + minutes) * 60 + seconds) * nominal + frames;
  // Every minute lost `drop` numbers except every tenth, so the index is the
  // number shown less what was never counted.
  return shown - drop * (total_minutes - total_minutes / 10);
}

}  // namespace

std::string seconds_to_timecode(double seconds, double fps, bool drop_frame) {
  // std::max returns the first argument when the second is NaN, so a garbage
  // frame rate degrades to 1 fps rather than propagating NaN.
  const double rate = std::max(1.0, std::isfinite(fps) ? fps : 1.0);
  const auto nominal = static_cast<std::int64_t>(std::max(1.0, js_round(rate)));

  // The index counts at the *actual* rate: frame k of the sequence is timecode
  // number k, whatever the fields are then counted at. Counting the index at
  // the nominal rate skips a number about every thousand frames at 29.97.
  double frames_exact = js_round(seconds * rate);
  if (!std::isfinite(frames_exact) || frames_exact < 0.0) frames_exact = 0.0;
  frames_exact = std::min(frames_exact, kMaxFrames);

  std::int64_t total_frames = static_cast<std::int64_t>(frames_exact);
  const bool dropping = drop_frame && supports_drop_frame(rate);
  if (dropping) total_frames = to_drop_frame(total_frames, nominal);

  const std::int64_t frames = total_frames % nominal;
  const std::int64_t total_seconds = total_frames / nominal;
  return std::format("{:02}:{:02}:{:02}{}{:02}", total_seconds / 3600,
                     (total_seconds % 3600) / 60, total_seconds % 60, dropping ? ';' : ':',
                     frames);
}

std::optional<double> timecode_to_seconds(std::string_view text, double fps,
                                          bool drop_frame) noexcept {
  const std::string_view s = trim(text);
  if (s.empty()) return std::nullopt;

  // A full stop anywhere means somebody wrote seconds rather than a timecode:
  // there is no decimal point in HH:MM:SS:FF, and "1.5" is unambiguous.
  if (s.find('.') != std::string_view::npos) {
    double seconds = 0.0;
    if (!js_number(s, seconds)) return std::nullopt;
    return snap_to_frame(std::max(0.0, seconds), fps);
  }

  std::vector<double> fields;
  std::size_t pos = 0;
  while (true) {
    // Either separator, and they mean the same thing. Whether the digits are
    // drop-frame is the caller's to say; somebody pasting a timecode out of an
    // email should not have to get the semicolon right for it to land.
    const auto sep = s.find_first_of(":;", pos);
    const std::string_view part =
        sep == std::string_view::npos ? s.substr(pos) : s.substr(pos, sep - pos);
    double value = 0.0;
    if (!js_number(part, value) || value < 0.0) return std::nullopt;
    fields.push_back(value);
    if (sep == std::string_view::npos) break;
    pos = sep + 1;
  }
  if (fields.size() > 4) return std::nullopt;

  const double rate = std::max(1.0, std::isfinite(fps) ? fps : 1.0);
  const auto nominal = static_cast<std::int64_t>(std::max(1.0, js_round(rate)));

  // Counted from the right, so a partial timecode means the small end of one:
  // "12" is twelve frames and "2:12" is two seconds and twelve frames, which is
  // how anybody types into one of these in a hurry.
  std::array<double, 4> parts{0.0, 0.0, 0.0, 0.0};
  const std::size_t first = parts.size() - fields.size();
  for (std::size_t i = 0; i < fields.size(); ++i) parts[first + i] = fields[i];

  if (drop_frame && supports_drop_frame(rate)) {
    // Through the frame index, because a dropped minute is shorter than sixty
    // seconds' worth of numbers and no fixed weight per field can say that.
    const std::int64_t index = from_drop_frame(
        static_cast<std::int64_t>(parts[0]), static_cast<std::int64_t>(parts[1]),
        static_cast<std::int64_t>(parts[2]), static_cast<std::int64_t>(parts[3]), nominal);
    return snap_to_frame(std::max(0.0, static_cast<double>(index) / rate), rate);
  }

  const double index = ((parts[0] * 60.0 + parts[1]) * 60.0 + parts[2]) *
                           static_cast<double>(nominal) +
                       parts[3];
  return snap_to_frame(std::max(0.0, index / rate), rate);
}

std::string readable_file_size(double bytes) {
  if (!std::isfinite(bytes) || bytes < 0.0) return "Unknown Size";

  double size = bytes;
  for (const std::string_view unit : {"B", "KB", "MB", "GB"}) {
    if (size < 1024.0) return std::format("{:.2f} {}", size, unit);
    size /= 1024.0;
  }
  return std::format("{:.2f} TB", size);
}

}  // namespace cutline::core
