#pragma once

/// Segment resolution: turning a track's stored clips into what actually gets
/// drawn.
///
/// The model keeps clips strictly non-overlapping. Transitions are stored as a
/// property of the outgoing clip's edge, and expanded here into real timeline
/// overlaps by borrowing each side's unused source — its trim "handles". This
/// is deliberately not FFmpeg's `xfade`, which was fragile against the overlap
/// model; geometry plus handle borrowing proved robust and is kept.
///
/// Segments are derived, never stored.
///
/// The reference implementation also carried `fixedTransform`, `fixedOpacity`,
/// and `fixedEffects` on each segment, used to bake animation into short
/// fixed-transform slices because FFmpeg filtergraphs cannot animate a
/// transform per frame. The native compositor evaluates keyframes at each real
/// output frame, so that machinery does not exist here.

#include "cutline/core/model.hpp"

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace cutline::core {

/// Clips closer together than this are treated as abutting.
inline constexpr double kTransitionEps = 1e-3;

/// Which side of a geometric transition a segment plays.
enum class SlideRole {
  In,   ///< incoming clip slides from the right to centred
  Out,  ///< outgoing clip pushes from centred off to the left
};

/// The timeline window a geometric transition ramps across.
struct SlideWindow {
  double start = 0.0;
  double end = 0.0;

  friend bool operator==(const SlideWindow&, const SlideWindow&) = default;
};

/// A clip resolved for rendering.
///
/// `clip` points into the track that produced the segment and does not outlive
/// it. `x_in`/`x_out` are transition alpha ramps in seconds, applied on top of
/// any manual fades; the incoming clip draws over the outgoing one, so `x_in`
/// cross-fades.
struct VideoSeg {
  const Clip* clip = nullptr;
  double start = 0.0;
  double end = 0.0;
  double source_in = 0.0;
  double source_out = 0.0;

  double x_in = 0.0;
  double x_out = 0.0;
  /// Dip-to-black uses opaque fades rather than a cross-fade partner.
  bool to_black = false;

  /// Set only for push and slide.
  std::optional<TransitionKind> slide_kind;
  std::optional<SlideRole> slide_role;
  std::optional<SlideWindow> slide_win;
};

/// The x-offset, as a fraction of canvas width, added to a segment's position
/// at timeline time `t` for a push or slide transition. Zero outside the
/// window, and zero for segments with no geometric transition.
[[nodiscard]] double seg_slide_offset_x(const VideoSeg& seg, double t) noexcept;

/// Resolves one video track's clips into draw segments, expanding transitions
/// into overlaps.
///
/// `media_duration_of` returns a clip's source length, and should return
/// infinity for stills and generated media, which have unlimited handles.
[[nodiscard]] std::vector<VideoSeg> resolve_video_segments(
    const Track& track,
    const std::function<double(std::string_view media_id)>& media_duration_of);

}  // namespace cutline::core
