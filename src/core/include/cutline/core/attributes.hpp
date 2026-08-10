#pragma once

/// Premiere's Paste Attributes: taking *some* of what makes one clip look or
/// sound the way it does and putting it on others.
///
/// Copying a clip already copies everything about it. This is the operation for
/// when that is too much — the grade off that shot but not its framing, the
/// level off that take but not its filters — and it is reached often enough in
/// a real cut to have its own key.
///
/// What travels is a set of named *groups* rather than a set of fields, because
/// "Motion" is one decision to a person and seven numbers plus their animation
/// to the model. The groups are Premiere's, so somebody who knows that dialogue
/// knows this one.

#include "cutline/core/edit.hpp"
#include "cutline/core/model.hpp"

#include <span>
#include <string>

namespace cutline::core {

/// Which groups of properties travel.
///
/// Nothing is on by default. An empty set is a dialogue nobody has answered
/// yet, not a licence to overwrite — and a paste that quietly took everything
/// is exactly the operation this one exists to be an alternative to.
struct ClipAttributes {
  // ------------------------------------------------------------- picture --

  /// Position, scale, rotation, the anchor point and the anti-flicker filter,
  /// with the keyframes on all of them.
  ///
  /// Premiere's Motion, and one group rather than seven because moving a shot
  /// and leaving its animation behind is not a thing anybody wants: the clip
  /// would jump back to where the first keyframe puts it the moment it played.
  bool motion = false;

  /// Opacity, its keyframes, and the two fades.
  ///
  /// Premiere writes a video fade *as* opacity keyframes, so keeping the fades
  /// in this group is what makes it mean the same thing there and here.
  bool opacity = false;

  bool blend = false;

  /// The whole video effect stack, replacing what is there.
  bool effects = false;

  // --------------------------------------------------------------- sound --

  /// The gain, its automation, and the fades on a sound clip — which are the
  /// same two fields the picture's fades are, meaning gain instead of alpha.
  bool volume = false;

  /// Where it sits across the stereo image, and the automation on it.
  bool pan = false;

  /// Which source channel feeds which output. Premiere's Channel Volume is the
  /// nearest row; this is the mapping, which is the thing that actually goes
  /// wrong on a two-microphone camera.
  bool channels = false;

  /// What the sound *is* — dialogue, music, effects, ambience.
  bool role = false;

  /// The whole audio effect stack, replacing what is there.
  bool audio_effects = false;

  // ---------------------------------------------------------- and how far --

  /// Whether keyframe times are stretched to the clip they land on.
  ///
  /// Premiere's "Scale Attributes Time", and it is the difference between a
  /// two-second push-in landing on a ten-second shot as a two-second push-in
  /// or as a ten-second one. Off by default, which is Premiere's default and
  /// the safer one: an unscaled animation is recognisably the one that was
  /// copied, and a stretched one is a different move.
  bool scale_to_length = false;

  /// Whether anything at all was chosen.
  [[nodiscard]] bool none() const noexcept {
    return !motion && !opacity && !blend && !effects && !volume && !pan && !channels &&
           !role && !audio_effects;
  }

  friend bool operator==(const ClipAttributes&, const ClipAttributes&) = default;
};

/// Puts the chosen attributes of what was copied onto every clip named.
///
/// **Matched by kind, one side at a time.** A shot on this timeline is two
/// linked clips, so copying one copies both halves and selecting three more
/// selects six clips. Each target takes its attributes from the first copied
/// clip of its own kind: the picture groups reach the video halves and the
/// sound groups reach the audio ones, which is what makes "make these three
/// shots like that one" a single gesture rather than two.
///
/// A group whose kind is not in the clipboard simply does not travel. Copying
/// a video-only clip and asking for Volume changes nothing, rather than
/// silencing anything.
///
/// Returns the project unchanged when nothing would alter — no selection, an
/// empty clipboard, or a set with nothing ticked in it.
[[nodiscard]] Project paste_attributes(Project p, std::span<const ClipCopy> from,
                                       std::span<const std::string> clip_ids,
                                       const ClipAttributes& which);

}  // namespace cutline::core
