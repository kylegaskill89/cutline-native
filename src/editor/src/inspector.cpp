#include "cutline/editor/inspector.hpp"

#include "cutline/audio/biquad.hpp"
#include "cutline/core/animate.hpp"
#include "cutline/core/keyframe.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
// For `kGainFloorDb`: the timeline's rubber band and this slider are two views
// of one number, and a floor that differed between them would mean a clip
// dragged to silence on one reading as audible on the other.
#include "cutline/ui/timeline.hpp"

#include <algorithm>
#include <array>
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
///
/// Gain is deliberately absent: it is the one parameter whose display is not a
/// scale of what is stored. See `gain_shown` below.
[[nodiscard]] double display_scale(ClipParam param) noexcept {
  switch (param) {
    case ClipParam::Opacity:
    case ClipParam::X:
    case ClipParam::Y:
    case ClipParam::ScaleX:
    case ClipParam::ScaleY:
      return kPercent;
    default:
      return 1.0;
  }
}

/// Volume, shown in decibels rather than as a percentage of unity.
///
/// Gain is stored as a linear multiplier and every other row here shows its
/// stored value scaled, which is why this one was a percentage. It made the
/// slider unusable: half its travel covers +0 to +6 dB, and everything from a
/// gentle -20 dB trim down to silence is squeezed into the last tenth, so a
/// clip pulled down on the timeline's rubber band read as pinned to the left
/// whatever it had actually been set to.
///
/// The two controls agree exactly, by construction: the same floor, the same
/// ceiling, and the same rule that the bottom of the range is silence rather
/// than merely very quiet.
[[nodiscard]] double gain_shown(double stored) noexcept { return ui::gain_to_fader_db(stored); }

[[nodiscard]] double gain_stored(double shown) noexcept {
  return ui::fader_db_to_gain(shown, core::kMaxGain);
}

/// The loudest the model will hold, in the units the row is in. Taken from the
/// model rather than written as a round number of decibels, so the top of the
/// slider is a gain that can actually be stored.
[[nodiscard]] double gain_ceiling_db() noexcept { return audio::linear_to_db(core::kMaxGain); }

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
      row.value = gain_shown(core::gain_at(clip, local_t));
      row.keyed_here = keyed_at(clip.gain_keyframes, local_t);
      row.interp = core::gain_keyframe_interp_of(clip);
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
    row.interp = core::keyframe_interp_of(clip, *prop);
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

std::string_view param_name(ClipParam param) noexcept {
  switch (param) {
    case ClipParam::Opacity: return "Opacity";
    case ClipParam::X: return "Position X";
    case ClipParam::Y: return "Position Y";
    case ClipParam::ScaleX: return "Scale X";
    case ClipParam::ScaleY: return "Scale Y";
    case ClipParam::Rotation: return "Rotation";
    case ClipParam::Speed: return "Speed";
    // Volume rather than Gain: it is what the control is called everywhere
    // else, and the row shows decibels.
    case ClipParam::Gain: return "Volume";
    case ClipParam::FadeIn: return "Fade In";
    case ClipParam::FadeOut: return "Fade Out";
  }
  return "Unknown";
}

std::span<const core::BlendMode> blend_modes() noexcept {
  // Premiere's order: Normal, then the modes that lighten, then the ones that
  // darken, then the odd one out.
  static constexpr std::array kModes{
      core::BlendMode::Normal,  core::BlendMode::Add,     core::BlendMode::Screen,
      core::BlendMode::Lighten, core::BlendMode::Multiply, core::BlendMode::Darken,
      core::BlendMode::Overlay, core::BlendMode::Difference};
  return kModes;
}

std::string_view blend_name(core::BlendMode mode) noexcept {
  switch (mode) {
    case core::BlendMode::Normal: return "Normal";
    case core::BlendMode::Add: return "Add";
    case core::BlendMode::Screen: return "Screen";
    case core::BlendMode::Multiply: return "Multiply";
    case core::BlendMode::Overlay: return "Overlay";
    case core::BlendMode::Darken: return "Darken";
    case core::BlendMode::Lighten: return "Lighten";
    case core::BlendMode::Difference: return "Difference";
  }
  return "Normal";
}

std::vector<ParamSpec> clip_parameters(const core::Project& project, std::string_view clip_id,
                                       double local_t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return {};

  std::vector<ParamSpec> out;
  const double length = core::clip_duration(*clip);

  if (clip->kind == core::TrackKind::Video) {
    out.push_back(ParamSpec{.param = ClipParam::Opacity,
                            .name = std::string(param_name(ClipParam::Opacity)),
                            .range = {.minimum = 0.0, .maximum = 100.0},
                            .value = clip->opacity * kPercent,
                            .fallback = 100.0,
                            .suffix = "%"});

    // Position is the clip's centre as a fraction of the canvas, so half is the
    // middle. Shown as a percentage of the canvas rather than in pixels, which
    // is what keeps it independent of the export resolution.
    out.push_back(ParamSpec{.param = ClipParam::X,
                            .name = std::string(param_name(ClipParam::X)),
                            .range = {.minimum = -50.0, .maximum = 150.0},
                            .value = clip->transform.x * kPercent,
                            .fallback = 50.0,
                            .suffix = "%"});
    out.push_back(ParamSpec{.param = ClipParam::Y,
                            .name = std::string(param_name(ClipParam::Y)),
                            .range = {.minimum = -50.0, .maximum = 150.0},
                            .value = clip->transform.y * kPercent,
                            .fallback = 50.0,
                            .suffix = "%"});

    out.push_back(ParamSpec{.param = ClipParam::ScaleX,
                            .name = std::string(param_name(ClipParam::ScaleX)),
                            .range = {.minimum = 0.0, .maximum = 400.0},
                            .value = clip->transform.scale_x * kPercent,
                            .fallback = 100.0,
                            .suffix = "%"});
    out.push_back(ParamSpec{.param = ClipParam::ScaleY,
                            .name = std::string(param_name(ClipParam::ScaleY)),
                            .range = {.minimum = 0.0, .maximum = 400.0},
                            .value = clip->transform.scale_y * kPercent,
                            .fallback = 100.0,
                            .suffix = "%"});

    out.push_back(ParamSpec{.param = ClipParam::Rotation,
                            .name = std::string(param_name(ClipParam::Rotation)),
                            .range = {.minimum = -180.0, .maximum = 180.0},
                            .value = clip->transform.rotation,
                            .fallback = 0.0,
                            .suffix = "\xc2\xb0"});  // degree sign, UTF-8
  } else {
    out.push_back(ParamSpec{.param = ClipParam::Gain,
                            .name = std::string(param_name(ClipParam::Gain)),
                            .range = {.minimum = ui::kGainFloorDb, .maximum = gain_ceiling_db()},
                            .value = gain_shown(clip->gain),
                            // Unity, which is where a volume control resets to.
                            .fallback = 0.0,
                            .suffix = "dB"});
  }

  out.push_back(ParamSpec{.param = ClipParam::Speed,
                          .name = std::string(param_name(ClipParam::Speed)),
                          .range = {.minimum = kSliderMinSpeed, .maximum = kSliderMaxSpeed},
                          .value = core::clip_speed(*clip),
                          .fallback = 1.0,
                          .suffix = "x"});

  // Fades are bounded by the clip they are on: a two second fade on a one
  // second clip is not a shorter fade, it is a mistake.
  out.push_back(ParamSpec{.param = ClipParam::FadeIn,
                          .name = std::string(param_name(ClipParam::FadeIn)),
                          .range = {.minimum = 0.0, .maximum = length},
                          .value = clip->fade_in,
                          .fallback = 0.0,
                          .suffix = "s"});
  out.push_back(ParamSpec{.param = ClipParam::FadeOut,
                          .name = std::string(param_name(ClipParam::FadeOut)),
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
    return core::set_gain_keyframe(std::move(project), clip_id, local_t, gain_stored(value));
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
      return core::set_clip_gain(std::move(project), clip_id, gain_stored(value));

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
  const double current = gain ? gain_shown(core::gain_at(*clip, local_t))
                              : core::animated_value(*clip, *prop, local_t) *
                                    display_scale(param);

  if (animated) {
    return gain ? core::set_gain_keyframe(std::move(project), clip_id, local_t,
                                          gain_stored(current))
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

std::string_view interp_name(core::Interp mode) noexcept {
  switch (mode) {
    case core::Interp::Linear: return "Linear";
    case core::Interp::Hold: return "Hold";
    case core::Interp::Ease: return "Ease";
  }
  return "Linear";
}

core::Interp next_interp(core::Interp mode) noexcept {
  switch (mode) {
    case core::Interp::Linear: return core::Interp::Hold;
    case core::Interp::Hold: return core::Interp::Ease;
    case core::Interp::Ease: return core::Interp::Linear;
  }
  return core::Interp::Linear;
}

core::Project set_clip_parameter_interp(core::Project project, std::string_view clip_id,
                                        ClipParam param, core::Interp mode) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  if (param == ClipParam::Gain) {
    // Nothing to set it on. A curve without keyframes is a setting that would
    // be silently discarded the moment the stopwatch was pressed.
    if (!core::is_gain_animated(*clip)) return project;
    return core::set_gain_keyframe_interp(std::move(project), clip_id, mode);
  }

  const std::optional<core::AnimProp> prop = anim_prop_of(param);
  if (!prop.has_value() || !core::is_animated(*clip, *prop)) return project;
  return core::set_keyframe_interp(std::move(project), clip_id, *prop, mode);
}

}  // namespace cutline::editor
