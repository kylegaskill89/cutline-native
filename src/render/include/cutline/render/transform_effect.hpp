#pragma once

/// Premiere's Transform effect: Motion's geometry as an effect in the stack.
///
/// Motion is applied last, after every effect, which is why a drop shadow added
/// before it turns with the layer. Transform is the same geometry at a place in
/// the stack of the editor's choosing, so a shadow after it stays where the
/// light is; and it adds what Motion lacks, skew and a motion blur from its own
/// shutter.
///
/// It is one pass that resamples the layer within the layer's own frame, as
/// Premiere's does: what it moves past the frame's edge is cut there. Anchor
/// Point and Position are stored as fractions of that frame, like Motion's
/// anchor, and shown in its pixels.
///
/// With a shutter, the pass carries the transform at several moments across
/// the shutter's opening and the shader averages the picture through each.

#include "cutline/core/model.hpp"
#include "cutline/render/effect_passes.hpp"

#include <cstddef>
#include <vector>

namespace cutline::render {

/// The pass's data: a header, then one transform per moment.
enum TransformSlot : std::size_t {
  kTransformSamples = 0,   ///< how many moments follow
  kTransformBicubic = 1,   ///< 1 for Bicubic sampling
  kTransformOpacity = 2,   ///< 0 to 1
  kTransformHeader = 4,
};

// One moment's transform, after the header. Constants rather than an enum, so
// they add to the header's slots without converting.
inline constexpr std::size_t kSampleAnchor = 0;    ///< x and y, fractions of the frame
inline constexpr std::size_t kSamplePosition = 2;  ///< x and y, fractions of the frame
inline constexpr std::size_t kSampleScale = 4;     ///< width and height, multipliers
inline constexpr std::size_t kSampleSkew = 6;      ///< radians
inline constexpr std::size_t kSampleSkewAxis = 7;  ///< radians
inline constexpr std::size_t kSampleRotation = 8;  ///< radians, clockwise
inline constexpr std::size_t kSampleSize = 9;

/// The most moments a shutter is divided into. Sixteen is where AE's own
/// default sits, and past it the steps stop showing at any real speed.
inline constexpr int kMaxShutterSamples = 16;

/// Transform's pass for `effect` (its keyframes still on it) at `local_t`, or
/// none when it would leave the picture as it is. `frame_duration` is how long
/// a frame lasts, which a shutter angle is a share of; at nought there is no
/// motion blur whatever the angle.
void plan_transform(const core::ClipEffect& effect, double local_t, double frame_duration,
                    std::vector<EffectPass>& passes);

}  // namespace cutline::render
