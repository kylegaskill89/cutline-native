#pragma once

/// Turning a project into the list of audio that should be heard, and how loud.
///
/// The audio counterpart of `plan.hpp`, and pure for the same reason: which
/// clips play, at what gain, and which part of which file they draw from are
/// all decidable from the model alone, so they can be tested exhaustively
/// without decoding anything.
///
/// The shape differs from the video plan in one way that matters. A video plan
/// is resolved at an instant, because a frame is an instant. Audio is a
/// continuous stream, and a mixer wants to know the whole span a clip occupies
/// so it can decode it once and walk through it, rather than seeking per
/// sample. So `plan_audio` resolves the timeline, and gain is evaluated per
/// sample afterwards.

#include "cutline/core/model.hpp"

#include <vector>

namespace cutline::render {

/// One audio clip, resolved against its track's mute and solo state.
struct PlannedAudioClip {
  /// Points into the project the plan was built from and does not outlive it.
  const core::Clip* clip = nullptr;
  const core::Media* media = nullptr;

  /// Which audio stream of the source file — the N in `0:a:N`. Addressed by
  /// ordinal rather than by libav's absolute stream index, because the ordinal
  /// is what survives the file being remuxed.
  int audio_stream = 0;

  /// The timeline span it occupies, in seconds. Half-open: `end` is the first
  /// moment it is no longer heard.
  double start = 0.0;
  double end = 0.0;

  /// The source range consumed, in source seconds, before retiming.
  double source_in = 0.0;
  double source_out = 0.0;

  /// Playback rate and direction. A clip at speed 2 consumes two seconds of
  /// source per timeline second.
  double speed = 1.0;
  bool reverse = false;

  /// Which audio track it came from, counting from the top of the stored
  /// order. Mixing is a sum, so unlike video this does not imply an order —
  /// it is for diagnostics and for the UI.
  int track_index = 0;

  /// The track's own fader and panner, carried on every clip that came off it.
  ///
  /// Copied rather than looked up, for the reason everything else here is: the
  /// plan is what the mixer works from, and a mixer reaching back into the
  /// project for a number would be a mixer that had to be told when the project
  /// changed underneath it.
  double track_gain = 1.0;
  double track_pan = 0.0;

  /// The track's automation, in **timeline** seconds, when it has any. Copied
  /// for the same reason the constants are, and read in preference to them.
  ///
  /// A clip's own keyframes are clip-local and these are not, which is the one
  /// place in the audio path where two keyframe lists on the same entry answer
  /// to different clocks. `audio_gain_at` is given timeline time and derives
  /// the local one from it, so both are available where they are needed — but
  /// it is worth knowing which is which before editing that function.
  std::vector<core::Keyframe> track_gain_keyframes;
  std::vector<core::Keyframe> track_pan_keyframes;

  /// The clip's channel map, copied. Empty is the default — see
  /// `core::Clip::channel_map`.
  std::vector<int> channel_map;
};

/// Every audio clip that can be heard, in track order and then by start time.
///
/// Clips on muted tracks are left out, as are disabled clips, clips whose gain
/// is pinned to zero, and — when any audio track is soloed — clips on tracks
/// that are not. Video tracks contribute nothing: an A/V pair is two linked
/// clips in this model, and the audio half lives on an audio track.
[[nodiscard]] std::vector<PlannedAudioClip> plan_audio(const core::Project& project);

/// The clip's linear gain at timeline time `t`: its automation or constant
/// gain, times its fades, times the track's fader.
///
/// Fades multiply the gain rather than adding to it, matching how
/// `segment_alpha` treats video: a clip that is both faded in and turned down
/// ends up quiet, not merely one of the two.
[[nodiscard]] double audio_gain_at(const PlannedAudioClip& planned, double t) noexcept;

/// What a clip's pan does to the two sides of the bus.
struct StereoGain {
  double left = 1.0;
  double right = 1.0;

  friend bool operator==(const StereoGain&, const StereoGain&) = default;
};

/// The clip's pan at timeline time `t`, as a gain for each side, with the
/// track's panner applied after it.
///
/// The two combine by multiplying the sides rather than by adding the pans: a
/// clip hard left on a track panned hard right should be silent, which is what
/// a pair of balances in series gives and what adding the two would not.
///
/// Balance, not a constant-power pan: one side is let through less and the
/// other left alone, so a centred clip is untouched. See `set_clip_pan` for
/// why that is the right law for what the mixer actually has in front of it.
///
/// Separate from `audio_gain_at` because it is a different shape of answer and
/// because the fades belong to the gain: a pan does not fade.
[[nodiscard]] StereoGain audio_pan_at(const PlannedAudioClip& planned, double t) noexcept;

/// The source time playing at timeline time `t`, clamped inside the clip's
/// trim so a rounding error at a boundary cannot pull in audio from beyond it.
[[nodiscard]] double audio_source_time_at(const PlannedAudioClip& planned, double t) noexcept;

}  // namespace cutline::render
