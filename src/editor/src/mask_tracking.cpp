#include "cutline/editor/mask_tracking.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/core/query.hpp"

#include <cmath>
#include <string>

namespace cutline::editor {
namespace {

[[nodiscard]] const core::Mask* mask_of(const core::Project& project, std::string_view clip_id,
                                        std::size_t index) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr || index >= clip->effects.size()) return nullptr;
  return &clip->effects[index].mask;
}

}  // namespace

core::Project apply_tracked_masks(core::Project project, std::string_view clip_id,
                                  std::size_t index, std::span<const core::TrackedMask> frames) {
  const core::Mask* current = mask_of(project, clip_id, index);
  if (current == nullptr || frames.empty()) return project;
  const core::MaskTracking tracking = current->tracking;
  const bool path = current->shape == core::MaskShape::Path;

  const std::string id(clip_id);
  for (const core::TrackedMask& frame : frames) {
    const auto key = [&](const char* name, double value) {
      project = core::set_effect_keyframe(std::move(project), id, index, name, frame.t, value);
    };
    key("mask.x", frame.mask.x);
    key("mask.y", frame.mask.y);
    if (tracking != core::MaskTracking::Position) key("mask.rotation", frame.mask.rotation);
    if (tracking == core::MaskTracking::PositionScaleRotation) {
      if (path) {
        core::Mask mask = *mask_of(project, clip_id, index);
        core::set_mask_path_key(mask, frame.t, frame.mask.points);
        project = core::set_effect_mask(std::move(project), id, index, std::move(mask));
      } else {
        key("mask.width", frame.mask.width);
        key("mask.height", frame.mask.height);
      }
    }
  }
  return project;
}

core::Project set_mask_tracking(core::Project project, std::string_view clip_id,
                                std::size_t index, core::MaskTracking tracking) {
  const core::Mask* current = mask_of(project, clip_id, index);
  if (current == nullptr || current->tracking == tracking) return project;
  core::Mask mask = *current;
  mask.tracking = tracking;
  return core::set_effect_mask(std::move(project), std::string(clip_id), index, std::move(mask));
}

core::Project set_mask_path_animated(core::Project project, std::string_view clip_id,
                                     std::size_t index, bool animated, double t) {
  const core::Mask* current = mask_of(project, clip_id, index);
  if (current == nullptr) return project;
  core::Mask mask = *current;
  core::set_mask_path_animated(mask, animated, t);
  if (mask == *current) return project;
  return core::set_effect_mask(std::move(project), std::string(clip_id), index, std::move(mask));
}

core::Project toggle_mask_path_key(core::Project project, std::string_view clip_id,
                                   std::size_t index, double t) {
  const core::Mask* current = mask_of(project, clip_id, index);
  if (current == nullptr) return project;
  core::Mask mask = *current;
  if (!core::remove_mask_path_key(mask, t)) {
    core::set_mask_path_key(mask, t, core::mask_path_at(mask, t));
  }
  return core::set_effect_mask(std::move(project), std::string(clip_id), index, std::move(mask));
}

}  // namespace cutline::editor
