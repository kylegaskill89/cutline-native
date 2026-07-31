#include "cutline/editor/monitor_binding.hpp"

#include "cutline/core/layout.hpp"
#include "cutline/core/query.hpp"
#include "cutline/editor/inspector.hpp"

#include <algorithm>

namespace cutline::editor {
namespace {

[[nodiscard]] const core::Media* media_of(const core::Project& project,
                                          const core::Clip& clip) noexcept {
  const auto found = std::ranges::find(project.media, clip.media_id, &core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// The clip's aspect-fit size as a fraction of the canvas, which is what a
/// scale of 1 covers. Zero on either axis means there is nothing to scale.
[[nodiscard]] core::Size natural_fraction(const core::Project& project,
                                          const core::Clip& clip) noexcept {
  const auto canvas_w = static_cast<double>(project.canvas_w);
  const auto canvas_h = static_cast<double>(project.canvas_h);
  if (canvas_w <= 0.0 || canvas_h <= 0.0) return {};

  // A title is measured by the text layer, which is nowhere near here, so its
  // stored dimensions are what there is. That makes the handles on a title
  // approximate until it has been laid out — better than no handles, and the
  // box is drawn from the same numbers the drag writes back, so the gesture is
  // consistent with itself either way.
  const core::Size natural =
      core::natural_size(media_of(project, clip), canvas_w, canvas_h);
  return {natural.width / canvas_w, natural.height / canvas_h};
}

}  // namespace

std::optional<ui::MonitorBox> monitor_box(const core::Project& project,
                                          std::string_view clip_id, double t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || clip->kind != core::TrackKind::Video) return std::nullopt;

  const core::Media* media = media_of(project, *clip);
  if (media == nullptr || !media->has_video) return std::nullopt;

  // An adjustment layer contributes a filter rather than a picture, so there is
  // no picture to place. It still sets `has_video`, which is why this is a
  // separate check and not an accident waiting to happen.
  if (media->is_adjustment) return std::nullopt;

  // Only while it is the thing on screen. A selected clip the playhead has run
  // past is not in the picture, and handles drawn over whatever *is* would
  // move something the eye cannot see move.
  if (t < clip->start || t >= clip->start + core::clip_duration(*clip)) return std::nullopt;

  const core::Size natural = natural_fraction(project, *clip);
  if (natural.width <= 0.0 || natural.height <= 0.0) return std::nullopt;

  const core::Transform transform = core::animated_transform(*clip, t - clip->start);
  return ui::MonitorBox{
      .x = transform.x,
      .y = transform.y,
      .width = natural.width * transform.scale_x,
      .height = natural.height * transform.scale_y,
      .rotation = transform.rotation,
  };
}

core::Project apply_monitor_box(core::Project project, std::string_view clip_id,
                                const ui::MonitorBox& box, double t) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  const core::Size natural = natural_fraction(project, *clip);
  if (natural.width <= 0.0 || natural.height <= 0.0) return project;

  const double local_t = t - clip->start;

  // Percentages, because that is what the inspector's rows are in and what
  // `set_clip_parameter` divides back out. Going through it rather than around
  // it is the point: a drag on the monitor and a drag on a slider have to be
  // the same edit, keyframes and all.
  constexpr double kPercent = 100.0;
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::X, box.x * kPercent,
                               local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::Y, box.y * kPercent,
                               local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::ScaleX,
                               box.width / natural.width * kPercent, local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::ScaleY,
                               box.height / natural.height * kPercent, local_t);
  project = set_clip_parameter(std::move(project), clip_id, ClipParam::Rotation, box.rotation,
                               local_t);
  return project;
}

}  // namespace cutline::editor
