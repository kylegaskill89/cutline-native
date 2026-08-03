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

std::string seconds_to_timecode(double seconds, double fps) {
  // std::max returns the first argument when the second is NaN, so a garbage
  // frame rate degrades to 1 fps rather than propagating NaN.
  const auto per_second = static_cast<std::int64_t>(std::max(1.0, js_round(fps)));

  double frames_exact = js_round(seconds * static_cast<double>(per_second));
  if (!std::isfinite(frames_exact) || frames_exact < 0.0) frames_exact = 0.0;
  frames_exact = std::min(frames_exact, kMaxFrames);

  const auto total_frames = static_cast<std::int64_t>(frames_exact);
  const std::int64_t frames = total_frames % per_second;
  const std::int64_t total_seconds = total_frames / per_second;
  return std::format("{:02}:{:02}:{:02}:{:02}", total_seconds / 3600,
                     (total_seconds % 3600) / 60, total_seconds % 60, frames);
}

std::optional<double> timecode_to_seconds(std::string_view text, double fps) noexcept {
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
    const auto colon = s.find(':', pos);
    const std::string_view part =
        colon == std::string_view::npos ? s.substr(pos) : s.substr(pos, colon - pos);
    double value = 0.0;
    if (!js_number(part, value) || value < 0.0) return std::nullopt;
    fields.push_back(value);
    if (colon == std::string_view::npos) break;
    pos = colon + 1;
  }
  if (fields.size() > 4) return std::nullopt;

  // Counted from the right, so a partial timecode means the small end of one:
  // "12" is twelve frames and "2:12" is two seconds and twelve frames, which is
  // how anybody types into one of these in a hurry.
  const std::array<double, 4> weight{3600.0, 60.0, 1.0, 1.0 / std::max(1.0, js_round(fps))};
  const std::size_t first = weight.size() - fields.size();

  double seconds = 0.0;
  for (std::size_t i = 0; i < fields.size(); ++i) seconds += fields[i] * weight[first + i];
  return snap_to_frame(seconds, fps);
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
