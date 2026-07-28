#include "cutline/editor/inspector.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
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

std::vector<ParamSpec> clip_parameters(const core::Project& project,
                                       std::string_view clip_id) {
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
  return out;
}

core::Project set_clip_parameter(core::Project project, std::string_view clip_id,
                                 ClipParam param, double value) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

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

}  // namespace cutline::editor
