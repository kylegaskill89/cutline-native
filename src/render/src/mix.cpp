#include "cutline/render/mix.hpp"

#include "cutline/core/interpret.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/segments.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cutline::render {
namespace {

/// An Exponential Fade's range: silence is this many decibels down, which is
/// below where a fade is still heard as one.
constexpr double kFadeRangeDb = 60.0;

/// Moves an entry's end later by `seconds`, reading that much more of its file.
void extend_end(PlannedAudioClip& entry, double seconds) {
  const double file = seconds * entry.speed * entry.conform;
  entry.end += seconds;
  (entry.reverse ? entry.source_in -= file : entry.source_out += file);
}

/// Moves an entry's start earlier by `seconds`, the same way.
void extend_start(PlannedAudioClip& entry, double seconds) {
  const double file = seconds * entry.speed * entry.conform;
  entry.start -= seconds;
  (entry.reverse ? entry.source_out += file : entry.source_in -= file);
}

/// Each audio transition becomes an overlap: the clip going out plays on into
/// its tail and the one coming in starts early from its head, half the
/// transition each side of the cut, each fading by the kind's curve. Only as
/// far as both have media to lend, so the crossfade stays centred on the cut.
void plan_crossfades(const core::Project& project, std::vector<PlannedAudioClip>& planned) {
  for (std::size_t i = 0; i + 1 < planned.size(); ++i) {
    PlannedAudioClip& out = planned[i];
    PlannedAudioClip& in = planned[i + 1];
    if (out.track_index != in.track_index) continue;
    const std::optional<core::Transition>& transition = out.clip->transition_out;
    if (!transition.has_value() || !core::is_audio_transition(transition->kind)) continue;
    if (std::abs(in.clip_start - out.clip_end) > core::kTransitionEps) continue;

    double half = transition->duration / 2.0;
    half = std::min({half, core::source_handles(project, *out.clip).tail,
                     core::source_handles(project, *in.clip).head,
                     out.clip_end - out.clip_start, in.clip_end - in.clip_start});
    if (!(half > core::kTransitionEps)) continue;

    const double cut = out.clip_end;
    extend_end(out, half);
    extend_start(in, half);
    out.crossfade_out = PlannedCrossfade{.kind = transition->kind, .from = cut - half,
                                         .to = cut + half};
    in.crossfade_in = out.crossfade_out;
  }
}

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
      entry.clip_start = entry.start;
      entry.clip_end = entry.end;
      // Into the file's own time here, once, so that everything downstream —
      // the decode window, the read position, the retime — is talking about
      // the thing a decoder can actually be asked for.
      entry.conform = media != nullptr ? core::conform_speed(*media) : 1.0;
      entry.source_in = clip.source_in * entry.conform;
      entry.source_out = clip.source_out * entry.conform;
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

  plan_crossfades(project, planned);
  return planned;
}

double crossfade_gain(core::TransitionKind kind, double progress, bool rising) noexcept {
  const double p = std::clamp(rising ? progress : 1.0 - progress, 0.0, 1.0);
  switch (kind) {
    case core::TransitionKind::ConstantGain:
      return p;
    case core::TransitionKind::ExponentialFade:
      if (p <= 0.0) return 0.0;
      return std::pow(10.0, (p - 1.0) * kFadeRangeDb / 20.0);
    default:
      return std::sin(p * std::numbers::pi / 2.0);
  }
}

double clip_gain_at(const PlannedAudioClip& planned, double t) noexcept {
  if (planned.clip == nullptr) return 0.0;
  const core::Clip& clip = *planned.clip;

  if (t < planned.start || t > planned.end) return 0.0;
  // From the clip's own start, so its fades and keyframes stay where they are
  // when a transition lengthens what is heard either side of it.
  const double length = planned.clip_end - planned.clip_start;
  const double local = std::clamp(t - planned.clip_start, 0.0, length);

  double gain = core::gain_at(clip, local);
  const auto through = [t](const PlannedCrossfade& fade) {
    return (t - fade.from) / (fade.to - fade.from);
  };
  if (planned.crossfade_in.active() && t < planned.crossfade_in.to) {
    gain *= crossfade_gain(planned.crossfade_in.kind, through(planned.crossfade_in), true);
  }
  if (planned.crossfade_out.active() && t > planned.crossfade_out.from) {
    gain *= crossfade_gain(planned.crossfade_out.kind, through(planned.crossfade_out), false);
  }

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
  return sides(core::pan_at(*planned.clip, t - planned.clip_start));
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
  //
  // Off the plan throughout rather than reaching back into the clip for half of
  // it: the plan's range is in file seconds and the clip's is in source
  // seconds, and mixing the two was how a conformed clip read from the wrong
  // part of its file.
  const double local = (t - planned.start) * planned.speed * planned.conform;
  const double wanted =
      planned.reverse ? planned.source_out - local : planned.source_in + local;
  return std::min(std::max(planned.source_in, wanted), planned.source_out);
}

}  // namespace cutline::render
