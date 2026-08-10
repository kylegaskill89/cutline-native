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
  for (const core::Track& track : project.sequence().tracks) {
    if (track.kind != core::TrackKind::Audio) continue;
    const int index = track_index++;
    // The ordinal is taken first, so a bus still owns its lane: the mixer sums
    // into that lane and the numbering has to agree with `core::bus_routes`.
    // What it does not have is clips. Nothing in the application can place one
    // there — see `track_indices_of_kind` — and a file can say anything.
    if (track.submix) continue;
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
      entry.channel_map = clip.channel_map;
      entry.track_gain = std::clamp(track.gain, 0.0, core::kMaxGain);
      entry.track_pan = std::clamp(track.pan, -1.0, 1.0);
      // Through the query rather than copied straight off the track, so a lane
      // switched to Off plans as the constant it is now using. Copying the
      // curve and deciding later would mean the mixer, the exporter and the
      // interface each needing to know what Off means.
      if (core::is_track_gain_animated(track)) entry.track_gain_keyframes = track.gain_keyframes;
      if (core::is_track_pan_animated(track)) entry.track_pan_keyframes = track.pan_keyframes;
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

double clip_gain_at(const PlannedAudioClip& planned, double t) noexcept {
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

double audio_gain_at(const PlannedAudioClip& planned, double t) noexcept {
  // The track's fader last, and read at *timeline* time. The clip's own
  // automation above is clip-local; a track has no local time, so its curve is
  // anchored to the sequence. Both clocks are in play on one entry and this is
  // the line where they meet.
  const double track = planned.track_gain_keyframes.empty()
                           ? planned.track_gain
                           : std::clamp(core::eval_keyframes(planned.track_gain_keyframes, t),
                                        0.0, core::kMaxGain);
  return std::max(clip_gain_at(planned, t) * track, 0.0);
}

namespace {

/// Balance: one side is let through less and the other is left alone. Centre is
/// exactly unity on both, so a project made before there was a panner sounds
/// the same to the sample.
[[nodiscard]] StereoGain sides(double pan) noexcept {
  return StereoGain{.left = pan > 0.0 ? 1.0 - pan : 1.0, .right = pan < 0.0 ? 1.0 + pan : 1.0};
}

}  // namespace

StereoGain clip_pan_at(const PlannedAudioClip& planned, double t) noexcept {
  if (planned.clip == nullptr) return {};
  return sides(core::pan_at(*planned.clip, t - planned.start));
}

StereoGain audio_pan_at(const PlannedAudioClip& planned, double t) noexcept {
  if (planned.clip == nullptr) return {};

  // The clip's panner and then the track's, multiplied rather than summed: a
  // clip hard left on a track panned hard right is silent, which is what two
  // balances in series give and what adding the two pans would not.
  const StereoGain clip = clip_pan_at(planned, t);
  // Timeline time for the track's, clip-local for the clip's — see
  // `audio_gain_at`.
  const StereoGain track =
      sides(planned.track_pan_keyframes.empty()
                ? planned.track_pan
                : std::clamp(core::eval_keyframes(planned.track_pan_keyframes, t), -1.0, 1.0));
  return {.left = clip.left * track.left, .right = clip.right * track.right};
}

double audio_source_time_at(const PlannedAudioClip& planned, double t) noexcept {
  if (planned.clip == nullptr) return 0.0;

  // At the *constant* rate, even when the clip is time-remapped. Premiere does
  // the same and for the same reason: a speed ramp on the picture is a ramp in
  // pitch on the sound, and the retiming that would avoid that has to vary
  // continuously — which is a different piece of DSP from the fixed stretch a
  // constant speed needs. Ramping a clip and hearing it slide is worse than
  // ramping it and having to handle the sound yourself.
  const core::Clip& clip = *planned.clip;
  const double local = (t - clip.start) * planned.speed;
  const double wanted = clip.reverse ? clip.source_out - local : clip.source_in + local;
  return std::min(std::max(planned.source_in, wanted), planned.source_out);
}

}  // namespace cutline::render
