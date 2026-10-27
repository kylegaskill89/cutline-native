#pragma once

/// Premiere's mask tracking: a mask carried from frame to frame with what is
/// under it.
///
/// What is tracked is the clip's own picture, as its effects receive it: the
/// clip drawn alone, with no effects, no Motion and nothing above or below it,
/// at the size of its own frame. That is the space a mask is drawn in, so a
/// point found in the picture is a point of the mask without converting it,
/// and a clip that is a nest, a title or a retimed shot is tracked as it is
/// seen. It is drawn by the same renderer as everything else, which is what
/// makes that true.
///
/// Each frame, corners are found inside the mask as it is on that frame and
/// followed to the next, and the mask is moved by what they did. Tracking
/// stops at the end of the clip, at a frame with too little inside the mask to
/// follow, or when it is told to.

#include "cutline/core/mask_path.hpp"
#include "cutline/core/model.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/track/tracker.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cutline::engine {

/// The mask's outline in the pixels of a frame `width` by `height`.
[[nodiscard]] std::vector<track::Point> mask_outline(const core::Mask& mask, double width,
                                                     double height);

/// The mask after `motion`, found in the pixels of a frame `width` by
/// `height`: moved, turned and scaled as `mask.tracking` allows, and its
/// keyframes untouched.
[[nodiscard]] core::Mask moved_mask(const core::Mask& mask, const track::Similarity& motion,
                                    double width, double height);

/// The motion a tracking method allows.
[[nodiscard]] track::Motion motion_for(core::MaskTracking tracking) noexcept;

/// Which way, and how far.
struct MaskTrackRequest {
  std::string clip_id;
  /// The effect whose mask is tracked.
  std::size_t effect = 0;
  /// Clip-local seconds to start from, where the mask is as it is drawn.
  double from = 0.0;
  /// Forward or backward.
  bool backward = false;
  /// One frame, as Premiere's single-step buttons do, or to the clip's end.
  bool one_frame = false;
};

/// Where the mask went: one entry per frame, the first the frame tracking
/// started from, in the order they were tracked.
struct MaskTrackResult {
  std::vector<core::TrackedMask> frames;
  /// Why it stopped short of the clip's end, or empty when it did not.
  std::string stopped;
};

/// Told after each frame, with how many there are to do; false stops it,
/// keeping what was tracked.
using MaskTrackProgress = std::function<bool(int done, int total)>;

/// Tracks `request`'s mask through `project`. An error only when nothing could
/// be tracked at all: a clip or effect that is not there, or a frame that could
/// not be drawn.
[[nodiscard]] std::expected<MaskTrackResult, std::string> track_mask(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const MaskTrackRequest& request, const MaskTrackProgress& progress = {});

}  // namespace cutline::engine
