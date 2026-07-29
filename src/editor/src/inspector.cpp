#include "cutline/editor/inspector.hpp"

#include "cutline/core/animate.hpp"
#include "cutline/core/keyframe.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

namespace cutline::editor {
namespace {

/// Percentages are shown, fractions are stored. One factor, in one place, so a
/// slider reading 100% and a clip storing 1.0 cannot drift apart.
constexpr double kPercent = 100.0;

/// The fastest and slowest a slider offers. The model allows far more in both
/// directions, but a linear control spanning 0.05 to 100 is unusable — every
/// speed anyone actually wants sits in the first few pixels of it. Typing an
/// extreme value stays possible; dragging to one does not.
constexpr double kSliderMinSpeed = 0.1;
constexpr double kSliderMaxSpeed = 4.0;

/// The animatable property a row edits, or nothing.
///
/// Speed and the fades have no entry: a fade whose length changed over its own
/// duration is not something the model can express, and neither is a speed that
/// varies within a clip — that is a different feature (time remapping) rather
/// than a keyframe on this one.
[[nodiscard]] std::optional<core::AnimProp> anim_prop_of(ClipParam param) noexcept {
  switch (param) {
    case ClipParam::Opacity: return core::AnimProp::Opacity;
    case ClipParam::X: return core::AnimProp::X;
    case ClipParam::Y: return core::AnimProp::Y;
    case ClipParam::ScaleX: return core::AnimProp::ScaleX;
    case ClipParam::ScaleY: return core::AnimProp::ScaleY;
    case ClipParam::Rotation: return core::AnimProp::Rotation;
    default: return std::nullopt;
  }
}

/// The factor between what is stored and what is shown.
[[nodiscard]] double display_scale(ClipParam param) noexcept {
  switch (param) {
    case ClipParam::Opacity:
    case ClipParam::X:
    case ClipParam::Y:
    case ClipParam::ScaleX:
    case ClipParam::ScaleY:
    case ClipParam::Gain:
      return kPercent;
    default:
      return 1.0;
  }
}

[[nodiscard]] bool keyed_at(std::span<const core::Keyframe> frames, double local_t) noexcept {
  return std::ranges::any_of(frames, [local_t](const core::Keyframe& frame) {
    return std::abs(frame.t - local_t) <= core::kKeyframeMatchEps;
  });
}

/// Fills in what a row's animation state is, and what it is worth at `local_t`.
void fill_animation(ParamSpec& row, const core::Clip& clip, double local_t) {
  if (row.param == ClipParam::Gain) {
    row.animatable = true;
    row.animated = core::is_gain_animated(clip);
    if (row.animated) {
      row.value = core::gain_at(clip, local_t) * kPercent;
      row.keyed_here = keyed_at(clip.gain_keyframes, local_t);
    }
    return;
  }

  const std::optional<core::AnimProp> prop = anim_prop_of(row.param);
  if (!prop.has_value()) return;

  row.animatable = true;
  row.animated = core::is_animated(clip, *prop);
  if (row.animated) {
    // What the keyframes are producing, not what is stored: an animated
    // property ignores its stored value, and showing it would put a number on
    // screen that nothing is using.
    row.value = core::animated_value(clip, *prop, local_t) * display_scale(row.param);
    row.keyed_here = keyed_at(clip.keyframes[core::anim_prop_index(*prop)], local_t);
  }
}

}  // namespace

std::string_view to_string(ClipParam param) noexcept {
  switch (param) {
    case ClipParam::Opacity: return "opacity";
    case ClipParam::X: return "x";
    case ClipParam::Y: return "y";
    case ClipParam::ScaleX: return "scale_x";
    case ClipParam::ScaleY: return "scale_y";
    case ClipParam::Rotation: return "rotation";
    case ClipParam::Speed: return "speed";
    case ClipParam::Gain: return "gain";
    case ClipParam::FadeIn: return "fade_in";
    case ClipParam::FadeOut: return "fade_out";
  }
  return "unknown";
}

std::vector<ParamSpec> clip_parameters(const core::Project& project, std::string_view clip_id,
                                       double local_t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return {};

  std::vector<ParamSpec> out;
  const double length = core::clip_duration(*clip);

  if (clip->kind == core::TrackKind::Video) {
    out.push_back(ParamSpec{.param = ClipParam::Opacity,
                            .name = "Opacity",
                            .range = {.minimum = 0.0, .maximum = 100.0},
                            .value = clip->opacity * kPercent,
                            .fallback = 100.0,
                            .suffix = "%"});

    // Position is the clip's centre as a fraction of the canvas, so half is the
    // middle. Shown as a percentage of the canvas rather than in pixels, which
    // is what keeps it independent of the export resolution.
    out.push_back(ParamSpec{.param = ClipParam::X,
                            .name = "Position X",
                            .range = {.minimum = -50.0, .maximum = 150.0},
                            .value = clip->transform.x * kPercent,
                            .fallback = 50.0,
                            .suffix = "%"});
    out.push_back(ParamSpec{.param = ClipParam::Y,
                            .name = "Position Y",
                            .range = {.minimum = -50.0, .maximum = 150.0},
                            .value = clip->transform.y * kPercent,
                            .fallback = 50.0,
                            .suffix = "%"});

    out.push_back(ParamSpec{.param = ClipParam::ScaleX,
                            .name = "Scale X",
                            .range = {.minimum = 0.0, .maximum = 400.0},
                            .value = clip->transform.scale_x * kPercent,
                            .fallback = 100.0,
                            .suffix = "%"});
    out.push_back(ParamSpec{.param = ClipParam::ScaleY,
                            .name = "Scale Y",
                            .range = {.minimum = 0.0, .maximum = 400.0},
                            .value = clip->transform.scale_y * kPercent,
                            .fallback = 100.0,
                            .suffix = "%"});

    out.push_back(ParamSpec{.param = ClipParam::Rotation,
                            .name = "Rotation",
                            .range = {.minimum = -180.0, .maximum = 180.0},
                            .value = clip->transform.rotation,
                            .fallback = 0.0,
                            .suffix = "\xc2\xb0"});  // degree sign, UTF-8
  } else {
    out.push_back(ParamSpec{.param = ClipParam::Gain,
                            .name = "Volume",
                            .range = {.minimum = 0.0, .maximum = core::kMaxGain * kPercent},
                            .value = clip->gain * kPercent,
                            .fallback = 100.0,
                            .suffix = "%"});
  }

  out.push_back(ParamSpec{.param = ClipParam::Speed,
                          .name = "Speed",
                          .range = {.minimum = kSliderMinSpeed, .maximum = kSliderMaxSpeed},
                          .value = core::clip_speed(*clip),
                          .fallback = 1.0,
                          .suffix = "x"});

  // Fades are bounded by the clip they are on: a two second fade on a one
  // second clip is not a shorter fade, it is a mistake.
  out.push_back(ParamSpec{.param = ClipParam::FadeIn,
                          .name = "Fade In",
                          .range = {.minimum = 0.0, .maximum = length},
                          .value = clip->fade_in,
                          .fallback = 0.0,
                          .suffix = "s"});
  out.push_back(ParamSpec{.param = ClipParam::FadeOut,
                          .name = "Fade Out",
                          .range = {.minimum = 0.0, .maximum = length},
                          .value = clip->fade_out,
                          .fallback = 0.0,
                          .suffix = "s"});

  for (ParamSpec& row : out) fill_animation(row, *clip, local_t);
  return out;
}

core::Project set_clip_parameter(core::Project project, std::string_view clip_id,
                                 ClipParam param, double value, double local_t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  // An animated property ignores its stored value, so writing one would look
  // like nothing happened. The keyframe at the playhead is the edit.
  if (param == ClipParam::Gain && core::is_gain_animated(*clip)) {
    return core::set_gain_keyframe(std::move(project), clip_id, local_t, value / kPercent);
  }
  if (const std::optional<core::AnimProp> prop = anim_prop_of(param);
      prop.has_value() && core::is_animated(*clip, *prop)) {
    return core::set_keyframe(std::move(project), clip_id, *prop, local_t,
                              value / display_scale(param));
  }

  // Read, change one field, set the whole value back — which is the contract
  // the property operations were written to, and cheaper than a patch type for
  // something this small.
  core::Transform transform = clip->transform;

  switch (param) {
    case ClipParam::Opacity:
      return core::set_clip_opacity(std::move(project), clip_id, value / kPercent);

    case ClipParam::X:
      transform.x = value / kPercent;
      return core::set_clip_transform(std::move(project), clip_id, transform);
    case ClipParam::Y:
      transform.y = value / kPercent;
      return core::set_clip_transform(std::move(project), clip_id, transform);
    case ClipParam::ScaleX:
      transform.scale_x = value / kPercent;
      return core::set_clip_transform(std::move(project), clip_id, transform);
    case ClipParam::ScaleY:
      transform.scale_y = value / kPercent;
      return core::set_clip_transform(std::move(project), clip_id, transform);
    case ClipParam::Rotation:
      transform.rotation = value;
      return core::set_clip_transform(std::move(project), clip_id, transform);

    case ClipParam::Speed:
      return core::set_clip_speed(std::move(project), clip_id, value);

    case ClipParam::Gain:
      return core::set_clip_gain(std::move(project), clip_id, value / kPercent);

    case ClipParam::FadeIn:
      return core::set_clip_fade(std::move(project), clip_id, core::ClipEdge::In, value);
    case ClipParam::FadeOut:
      return core::set_clip_fade(std::move(project), clip_id, core::ClipEdge::Out, value);
  }
  return project;
}

core::Project set_clip_parameter_animated(core::Project project, std::string_view clip_id,
                                          ClipParam param, bool animated, double local_t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  const bool gain = param == ClipParam::Gain;
  const std::optional<core::AnimProp> prop = anim_prop_of(param);
  if (!gain && !prop.has_value()) return project;

  const bool already = gain ? core::is_gain_animated(*clip) : core::is_animated(*clip, *prop);
  if (already == animated) return project;

  // Read before either edit: switching animation on has to keep the value the
  // property already had, and switching it off has to keep the one the
  // keyframes were producing. Either way nothing about the picture changes at
  // this instant, which is the whole point of the control.
  const double current = gain ? core::gain_at(*clip, local_t) * kPercent
                              : core::animated_value(*clip, *prop, local_t) *
                                    display_scale(param);

  if (animated) {
    return gain ? core::set_gain_keyframe(std::move(project), clip_id, local_t,
                                          current / kPercent)
                : core::set_keyframe(std::move(project), clip_id, *prop, local_t,
                                     current / display_scale(param));
  }

  project = gain ? core::clear_gain_keyframes(std::move(project), clip_id)
                 : core::clear_keyframes(std::move(project), clip_id, *prop);
  // Static now, so this writes the plain value rather than another keyframe.
  return set_clip_parameter(std::move(project), clip_id, param, current, local_t);
}

core::Project toggle_clip_parameter_keyframe(core::Project project, std::string_view clip_id,
                                             ClipParam param, double local_t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  const bool gain = param == ClipParam::Gain;
  const std::optional<core::AnimProp> prop = anim_prop_of(param);
  if (!gain && !prop.has_value()) return project;

  // Not animated: there is no list to add to, and starting one here would make
  // this marker and the stopwatch mean the same thing.
  if (gain) {
    if (!core::is_gain_animated(*clip)) return project;
    if (keyed_at(clip->gain_keyframes, local_t)) {
      return core::remove_gain_keyframe_at(std::move(project), clip_id, local_t);
    }
    return core::set_gain_keyframe(std::move(project), clip_id, local_t,
                                   core::gain_at(*clip, local_t));
  }

  if (!core::is_animated(*clip, *prop)) return project;
  if (keyed_at(clip->keyframes[core::anim_prop_index(*prop)], local_t)) {
    return core::remove_keyframe_at(std::move(project), clip_id, *prop, local_t);
  }
  return core::set_keyframe(std::move(project), clip_id, *prop, local_t,
                            core::animated_value(*clip, *prop, local_t));
}

}  // namespace cutline::editor
