#include "cutline/core/attributes.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>

namespace cutline::core {
namespace {

/// The first copied clip of this kind, or null.
[[nodiscard]] const Clip* source_of(std::span<const ClipCopy> from, TrackKind kind) noexcept {
  for (const ClipCopy& copy : from) {
    if (copy.clip.kind == kind) return &copy.clip;
  }
  return nullptr;
}

/// Copies a keyframe list across, stretching its times when asked.
///
/// The stretch is one multiplication because keyframe times are clip-relative:
/// zero is the head of the clip whatever the clip is, so the same numbers mean
/// the same places and only the scale between them is in question.
void carry(std::vector<Keyframe>& into, const std::vector<Keyframe>& from, double stretch) {
  into = from;
  if (stretch == 1.0) return;
  for (Keyframe& key : into) {
    key.t *= stretch;
    // The handles are offsets in the same units as the times, so they have to
    // travel with them or a stretched animation keeps the easing of the length
    // it was drawn at and reads as a different move.
    key.in_x *= stretch;
    key.out_x *= stretch;
  }
}

/// How much longer the clip being landed on is than the one copied, or 1 when
/// the times are to be left as they are.
[[nodiscard]] double stretch_between(const Clip& from, const Clip& onto, bool scale) noexcept {
  if (!scale) return 1.0;
  const double was = clip_duration(from);
  const double now = clip_duration(onto);
  // A zero-length clip either side leaves the times alone. Multiplying by zero
  // would pile every keyframe onto the head of the clip, which is not a
  // scaled animation — it is a lost one.
  if (was <= 0.0 || now <= 0.0) return 1.0;
  return now / was;
}

void carry_picture(Clip& onto, const Clip& from, const ClipAttributes& which, double stretch) {
  if (which.motion) {
    onto.transform = from.transform;
    for (const AnimProp prop : {AnimProp::X, AnimProp::Y, AnimProp::ScaleX, AnimProp::ScaleY,
                                AnimProp::Rotation, AnimProp::AnchorX, AnimProp::AnchorY}) {
      carry(onto.keyframes[anim_prop_index(prop)],
            from.keyframes[anim_prop_index(prop)], stretch);
    }
  }

  if (which.opacity) {
    onto.opacity = from.opacity;
    carry(onto.keyframes[anim_prop_index(AnimProp::Opacity)],
          from.keyframes[anim_prop_index(AnimProp::Opacity)], stretch);
    onto.fade_in = from.fade_in;
    onto.fade_out = from.fade_out;
  }

  if (which.blend) onto.blend = from.blend;
  if (which.effects) onto.effects = from.effects;
}

void carry_sound(Clip& onto, const Clip& from, const ClipAttributes& which, double stretch) {
  if (which.volume) {
    onto.gain = from.gain;
    carry(onto.gain_keyframes, from.gain_keyframes, stretch);
    onto.fade_in = from.fade_in;
    onto.fade_out = from.fade_out;
  }

  if (which.pan) {
    onto.pan = from.pan;
    carry(onto.keyframes[anim_prop_index(AnimProp::Pan)],
          from.keyframes[anim_prop_index(AnimProp::Pan)], stretch);
  }

  if (which.channels) onto.channel_map = from.channel_map;
  if (which.role) onto.role = from.role;
  if (which.audio_effects) onto.audio_effects = from.audio_effects;
}

}  // namespace

Project paste_attributes(Project p, std::span<const ClipCopy> from,
                         std::span<const std::string> clip_ids, const ClipAttributes& which) {
  if (from.empty() || clip_ids.empty() || which.none()) return p;

  const Clip* picture = source_of(from, TrackKind::Video);
  const Clip* sound = source_of(from, TrackKind::Audio);

  for (const std::string& clip_id : clip_ids) {
    Clip* onto = find_clip(p, clip_id);
    if (onto == nullptr) continue;

    // Nothing is copied onto itself. It would be a no-op in every group, but
    // going through the motions would make an edit out of a gesture that
    // changed nothing, and put an entry in the undo stack that undoes nothing.
    const Clip* source = onto->kind == TrackKind::Video ? picture : sound;
    if (source == nullptr || source->id == onto->id) continue;

    const double stretch = stretch_between(*source, *onto, which.scale_to_length);
    if (onto->kind == TrackKind::Video) {
      carry_picture(*onto, *source, which, stretch);
    } else {
      carry_sound(*onto, *source, which, stretch);
    }
  }

  return p;
}

}  // namespace cutline::core
