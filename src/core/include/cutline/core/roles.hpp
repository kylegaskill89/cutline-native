#pragma once

/// What a piece of sound is, and the one thing that follows from knowing:
/// getting the music out of the way of the voice.
///
/// Premiere calls this Essential Sound. A clip is given a role — dialogue,
/// music, effects, ambience — and the role is what everything downstream reads:
/// a preset knows roughly what a voice needs, a submix knows which clips belong
/// on it, and ducking knows what has to duck under what.
///
/// Pure, and deliberately model-only. A role is a label until something acts on
/// it, and the two things that act on it live where they belong: the presets are
/// in the audio layer, beside the effect registry that defines what their
/// parameters mean, and ducking is here, because it is keyframes on clips and
/// nothing else.

#include "cutline/core/model.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace cutline::core {

/// Every role, in the order an interface should offer them: `None` first,
/// because it is what a clip starts as, then Premiere's four.
inline constexpr std::array<AudioRole, 5> kAudioRoles{
    AudioRole::None, AudioRole::Dialogue, AudioRole::Music, AudioRole::Effects,
    AudioRole::Ambience,
};

/// The name to show. "None" rather than an empty string: this ends up in a menu
/// beside four other names, and a blank row reads as a broken one.
[[nodiscard]] std::string_view role_name(AudioRole role) noexcept;

/// Sets a clip's role. Audio clips only — a role is about sound, and the
/// picture half of an A/V pair has none.
[[nodiscard]] Project set_clip_role(Project p, std::string_view clip_id, AudioRole role);

/// Sets the role of every clip on a track. What "this whole lane is the
/// interview" costs, rather than one press per clip.
[[nodiscard]] Project set_track_role(Project p, std::string_view track_id, AudioRole role);

/// A stretch of timeline where something is heard.
struct RoleSpan {
  double start = 0.0;
  double end = 0.0;

  friend bool operator==(const RoleSpan&, const RoleSpan&) = default;
};

/// Where a role is heard, across every audible audio track, merged and sorted.
///
/// Merged because two people talking on two tracks is one stretch of dialogue,
/// and ducking under each of them separately would pump the music up between
/// their sentences.
///
/// Muted tracks and disabled clips are left out: they are not heard, so nothing
/// should be getting out of their way.
[[nodiscard]] std::vector<RoleSpan> role_spans(const Project& p, AudioRole role);

/// How music gets out of the way of speech. Premiere's ducking controls.
struct DuckSettings {
  /// What ducks *under* what. Dialogue, nearly always — the point of the
  /// feature is that the words stay intelligible.
  AudioRole against = AudioRole::Dialogue;

  /// How far down, in dB. Negative; -18 is Premiere's default and about what a
  /// music bed under an interview wants.
  double amount_db = -18.0;

  /// How long the ramp takes, in seconds, at both ends.
  double fade = 0.8;

  /// Where the ramp sits relative to the speech, in seconds. Negative starts
  /// the duck early, so the music is already down when the first word lands —
  /// which is what a hand does and why the control exists.
  double position = 0.0;
};

/// Writes a ducking curve onto one clip's volume.
///
/// Replaces whatever volume automation the clip had, which is what Premiere's
/// "Generate Keyframes" does and the only honest thing it can do: a duck is a
/// shape for the whole clip, and laying it over a hand-drawn curve would leave
/// two curves fighting with no way to tell which one anybody meant.
///
/// The un-ducked level is the clip's own gain, so a clip that was already set
/// quiet stays that quiet between the sentences.
[[nodiscard]] Project duck_clip(Project p, std::string_view clip_id,
                                const DuckSettings& settings = {});

/// The same, for every clip carrying a role — "duck the music under the
/// dialogue", which is how anybody actually asks for it.
[[nodiscard]] Project duck_role(Project p, AudioRole role, const DuckSettings& settings = {});

}  // namespace cutline::core
