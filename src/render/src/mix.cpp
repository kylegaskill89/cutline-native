#include "cutline/render/mix.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>

namespace cutline::render {
namespace {

/// A clip this quiet is silence, and decoding it would be work with no audible
/// result. Around -100 dB, well below the noise floor of any real recording.
constexpr double kInaudibleGain = 1e-5;

/// Whether a clip's gain is zero for its whole length. Automation is checked
/// point by point, because a clip that starts silent and rises must still play.
[[nodiscard]] bool always_silent(const core::Clip& clip) noexcept {
  if (!core::is_gain_animated(clip)) return clip.gain <= kInaudibleGain;
  return std::ranges::all_of(clip.gain_keyframes, [](const core::Keyframe& k) {
    return k.v <= kInaudibleGain;
  });
}

}  // namespace

std::vector<PlannedAudioClip> plan_audio(const core::Project& project) {
  std::vector<PlannedAudioClip> planned;

  int track_index = 0;
  for (const core::Track& track : project.tracks) {
    if (track.kind != core::TrackKind::Audio) continue;
    const int index = track_index++;
    if (!core::is_track_audible(project, track)) continue;

    for (const core::Clip& clip : track.clips) {
      if (clip.disabled) continue;
      if (always_silent(clip)) continue;

      const double duration = core::clip_duration(clip);
      if (!(duration > 0.0)) continue;

      const core::Media* media = nullptr;
      for (const core::Media& candidate : project.media) {
        if (candidate.id == clip.media_id) {
          media = &candidate;
          break;
        }
      }
      // Generated media -- titles, colour mattes, adjustment layers -- have no
      // file behind them and so nothing to decode.
      if (media != nullptr && core::is_generated_media(*media)) continue;

      PlannedAudioClip entry;
      entry.clip = &clip;
      entry.media = media;
      entry.audio_stream = clip.audio_stream;
      entry.start = clip.start;
      entry.end = clip.start + duration;
      entry.source_in = clip.source_in;
      entry.source_out = clip.source_out;
      entry.speed = core::clip_speed(clip);
      entry.reverse = clip.reverse;
      entry.track_index = index;
      planned.push_back(entry);
    }
  }

  std::stable_sort(planned.begin(), planned.end(),
                   [](const PlannedAudioClip& a, const PlannedAudioClip& b) {
                     if (a.track_index != b.track_index) return a.track_index < b.track_index;
                     return a.start < b.start;
                   });

  return planned;
}

double audio_gain_at(const PlannedAudioClip& planned, double t) noexcept {
  if (planned.clip == nullptr) return 0.0;
  const core::Clip& clip = *planned.clip;

  const double local = t - planned.start;
  const double length = planned.end - planned.start;
  if (local < 0.0 || local > length) return 0.0;

  double gain = core::gain_at(clip, local);

  // Ramps are linear in amplitude, which is what the reference did and what a
  // fade handle in the UI implies. A constant-power ramp would sound smoother
  // across a crossfade but would not match an existing project's exports.
  if (clip.fade_in > 0.0 && local < clip.fade_in) {
    gain *= std::max(0.0, local / clip.fade_in);
  }
  const double tail = length - local;
  if (clip.fade_out > 0.0 && tail < clip.fade_out) {
    gain *= std::max(0.0, tail / clip.fade_out);
  }

  return std::max(gain, 0.0);
}

StereoGain audio_pan_at(const PlannedAudioClip& planned, double t) noexcept {
  if (planned.clip == nullptr) return {};

  const double local = t - planned.start;
  const double pan = core::pan_at(*planned.clip, local);

  // Balance: one side is let through less and the other is left alone. Centre
  // is exactly unity on both, so a project made before there was a panner
  // sounds the same to the sample.
  return {.left = pan > 0.0 ? 1.0 - pan : 1.0, .right = pan < 0.0 ? 1.0 + pan : 1.0};
}

double audio_source_time_at(const PlannedAudioClip& planned, double t) noexcept {
  if (planned.clip == nullptr) return 0.0;
  const double wanted = core::source_time_at(*planned.clip, t);
  return std::min(std::max(planned.source_in, wanted), planned.source_out);
}

}  // namespace cutline::render
