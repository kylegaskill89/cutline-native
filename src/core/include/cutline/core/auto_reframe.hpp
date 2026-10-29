#pragma once

/// Premiere's Auto Reframe Sequence: a copy of the open sequence in another
/// shape — vertical, square — with Auto Reframe on every clip of picture, so
/// each is scaled to fill the new frame and follows its subject. The looking
/// itself is the renderer's and the engine's; this is what the project does.

#include "cutline/core/model.hpp"

#include <string>

namespace cutline::core {

/// A frame size for the shape `aspect_w` by `aspect_h`, keeping the shorter
/// side of `like`: a 1920 by 1080 sequence made 9:16 is 1080 by 1920, and
/// made square is 1080 by 1080. Always even, as encoders want.
struct FrameSize {
  int width = 0;
  int height = 0;

  friend bool operator==(const FrameSize&, const FrameSize&) = default;
};
[[nodiscard]] FrameSize reframed_size(const Sequence& like, double aspect_w, double aspect_h);

/// Whether Auto Reframe applies to a clip: a picture from footage or a still.
/// Not a title or a graphic, which are laid out for the frame they are in,
/// and not a matte or an adjustment layer, which fill any frame already.
[[nodiscard]] bool reframes(const Project& p, const Clip& clip) noexcept;

/// The open sequence again under a new id, every clip with a new id and
/// links kept between the copies. Opened, as a copy is wanted to be worked on.
[[nodiscard]] Project duplicate_sequence(Project p, std::string name, std::string* made_id = nullptr);

struct AutoReframeOptions {
  /// Empty for the open sequence's name with the shape after it.
  std::string name;
  FrameSize size;
  /// Premiere's Motion Tracking: 0 Slower Motion, 1 Default, 2 Faster Motion.
  int motion = 1;
  /// Premiere's Clip Nesting: the pictures nested first, keeping their cuts
  /// and layering, and the nest reframed as one.
  bool nest = false;
};

/// Makes the reframed copy, opens it, and gives its id through `made_id`.
/// Its sound and its titles are as they were.
[[nodiscard]] Project auto_reframe_sequence(Project p, const AutoReframeOptions& options,
                                            std::string* made_id = nullptr);

}  // namespace cutline::core
