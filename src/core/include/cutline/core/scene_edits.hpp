#pragma once

/// What Premiere's Scene Edit Detection does with the cuts it finds in a clip
/// of footage that was edited before it arrived.
///
/// Finding them is the engine's work, since it means looking at every frame;
/// this is the edit made from the answer, which is ordinary project arithmetic
/// and undoes as one step.

#include "cutline/core/model.hpp"

#include <span>
#include <string>
#include <string_view>

namespace cutline::core {

/// The three things the dialog offers, any or all of them.
struct SceneEditOptions {
  /// Apply a cut at each detected cut point, to the clip and what is linked
  /// to it.
  bool cut = true;
  /// Create subclips from each detected cut point in a bin, one per shot, in
  /// a new bin beside the source.
  bool subclips = false;
  /// Create clip markers at each detected cut point, on the source.
  bool markers = false;
};

/// Does what `options` ask at each timeline time in `cuts` inside the clip.
/// Times outside the clip, or at its ends, are ignored.
[[nodiscard]] Project apply_scene_edits(Project p, std::string_view clip_id,
                                        std::span<const double> cuts,
                                        const SceneEditOptions& options);

}  // namespace cutline::core
