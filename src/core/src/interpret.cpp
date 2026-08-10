#include "cutline/core/interpret.hpp"

#include "cutline/core/query.hpp"

#include "ripple.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace cutline::core {
namespace {

/// How far two rates have to differ before conforming one to the other means
/// anything. Coarse on purpose: 23.976 and 24 are different rates and both are
/// offered, while a rate that came back off a probe as 29.999999 is 30.
constexpr double kRateEps = 1e-6;

/// The smallest and largest rates a conform will accept.
///
/// Not the clip speed limits, which are ratios; these are frame rates, and the
/// range is the one the sequence presets cover with room either side. A rate of
/// zero would divide the source's length by nothing.
constexpr double kMinRate = 1.0;
constexpr double kMaxRate = 1000.0;

}  // namespace

double conform_speed(const Media& m) noexcept {
  if (!m.assumed_fps.has_value() || !m.fps.has_value()) return 1.0;
  if (*m.fps <= 0.0 || *m.assumed_fps <= 0.0) return 1.0;
  return *m.assumed_fps / *m.fps;
}

bool is_conformed(const Media& m) noexcept {
  return std::abs(conform_speed(m) - 1.0) > kRateEps;
}

std::optional<double> playback_fps(const Media& m) noexcept {
  if (m.assumed_fps.has_value() && *m.assumed_fps > 0.0) return m.assumed_fps;
  return m.fps;
}

bool can_interpret(const Media& m) noexcept {
  if (is_generated_media(m) || m.is_image) return false;
  return m.fps.has_value() && *m.fps > 0.0;
}

Project interpret_media(Project p, std::string_view media_id,
                        std::optional<double> assumed_fps) {
  Media* media = nullptr;
  for (Media& candidate : p.media) {
    if (candidate.id == media_id) {
      media = &candidate;
      break;
    }
  }
  if (media == nullptr || !can_interpret(*media)) return p;

  // A rate matching the file's own is not a conform, it is the absence of one.
  // Normalising here rather than storing it means `is_conformed` never has to
  // ask twice and a file cannot be written claiming an override that does
  // nothing.
  std::optional<double> wanted = assumed_fps;
  if (wanted.has_value()) {
    if (!(*wanted >= kMinRate && *wanted <= kMaxRate)) return p;
    if (std::abs(*wanted - *media->fps) <= kRateEps) wanted.reset();
  }

  const double was_rate = playback_fps(*media).value_or(0.0);
  const double now_rate = wanted.value_or(*media->fps);
  if (was_rate <= 0.0 || std::abs(was_rate - now_rate) <= kRateEps) return p;

  // The same frames at a different rate: a length in seconds scales by the
  // inverse of the rate, so this one factor moves the source's duration, its
  // marks, and every range cut out of it, all in step.
  const double scale = was_rate / now_rate;

  // Restored from what was kept rather than divided back, so clearing a
  // conform gives the length the file actually has and not a value two
  // roundings away from it.
  const std::string id{media_id};
  if (!wanted.has_value()) {
    if (media->file_duration.has_value()) media->duration = *media->file_duration;
    media->file_duration.reset();
  } else {
    if (!media->file_duration.has_value()) media->file_duration = media->duration;
    media->duration = *media->file_duration * *media->fps / *wanted;
  }
  media->assumed_fps = wanted;

  // The marks belong to the asset and are in the same seconds its clips are.
  if (media->in_point.has_value()) *media->in_point *= scale;
  if (media->out_point.has_value()) *media->out_point *= scale;

  // Clips keep their frames. Scaling the ranges is what makes that true: a
  // clip on frames 100 to 200 is still on frames 100 to 200, at a rate that
  // makes it longer or shorter on the timeline.
  std::unordered_set<std::string> stretched;
  std::vector<RippleShift> shifts;
  for (Track& track : p.sequence().tracks) {
    for (Clip& clip : track.clips) {
      if (clip.media_id != id) continue;

      const double was_end = clip_end(clip);
      const double was = clip_duration(clip);

      clip.source_in *= scale;
      clip.source_out *= scale;
      if (clip.hold.has_value()) *clip.hold *= scale;

      // A conform can shorten a clip out from under its own fades, exactly as
      // a retime can.
      const double length = clip_duration(clip);
      clip.fade_in = std::min(clip.fade_in, length);
      clip.fade_out = std::min(clip.fade_out, std::max(0.0, length - clip.fade_in));

      stretched.insert(clip.id);
      if (length != was) shifts.emplace_back(was_end, length - was);
    }
  }

  return ripple_after(std::move(p), std::move(shifts), stretched);
}

}  // namespace cutline::core
