#include "cutline/core/segments.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cutline::core {
namespace {

/// How much unused source lies past each end of a clip — its trim handles —
/// measured in timeline seconds.
struct Handles {
  double head = 0.0;
  double tail = 0.0;
};

[[nodiscard]] Handles source_handles(const Clip& c, double media_duration) noexcept {
  const double speed = clip_speed(c);
  const double before_in = c.source_in / speed;
  const double after_out = (media_duration - c.source_out) / speed;
  // Reverse swaps which physical handle feeds the head versus the tail edge.
  if (c.reverse) return {.head = after_out, .tail = before_in};
  return {.head = before_in, .tail = after_out};
}

}  // namespace

double seg_slide_offset_x(const VideoSeg& seg, double t) noexcept {
  if (!seg.slide_kind.has_value() || !seg.slide_win.has_value()) return 0.0;

  const SlideWindow win = *seg.slide_win;
  const double span = win.end - win.start;
  if (span <= 0.0) return 0.0;

  const double p = std::clamp((t - win.start) / span, 0.0, 1.0);
  if (seg.slide_role == SlideRole::In) return 1.0 - p;   // off-right to centred
  if (seg.slide_role == SlideRole::Out) return -p;       // centred to off-left
  return 0.0;
}

std::vector<VideoSeg> resolve_video_segments(
    const Track& track,
    const std::function<double(std::string_view)>& media_duration_of) {
  std::vector<const Clip*> clips;
  clips.reserve(track.clips.size());
  for (const Clip& c : track.clips) {
    if (!c.disabled) clips.push_back(&c);
  }
  std::ranges::stable_sort(clips, {}, [](const Clip* c) { return c->start; });

  std::vector<VideoSeg> segs;
  segs.reserve(clips.size());
  for (const Clip* c : clips) {
    segs.push_back(VideoSeg{
        .clip = c,
        .start = c->start,
        .end = clip_end(*c),
        .source_in = c->source_in,
        .source_out = c->source_out,
    });
  }

  for (std::size_t i = 0; i < clips.size(); ++i) {
    const Clip& a = *clips[i];
    if (!a.transition_out.has_value() || a.transition_out->duration <= 0.0) continue;
    if (i + 1 >= clips.size()) continue;

    const Clip& b = *clips[i + 1];
    if (std::abs(b.start - clip_end(a)) > kTransitionEps) continue;  // must abut

    VideoSeg& seg_a = segs[i];
    VideoSeg& seg_b = segs[i + 1];
    const Transition& transition = *a.transition_out;
    const double half = transition.duration / 2.0;

    if (transition.kind == TransitionKind::DipBlack) {
      // Sequential opaque fades at the cut: A to black over [T-half, T], then
      // black to B over [T, T+half]. No overlap, so no handles are needed.
      seg_a.x_out = std::max(seg_a.x_out, half);
      seg_a.to_black = true;
      seg_b.x_in = std::max(seg_b.x_in, half);
      seg_b.to_black = true;
      continue;
    }

    // Everything else needs a real overlap: borrow up to `half` of the handle
    // available on each side.
    const double a_ext = std::min(half, source_handles(a, media_duration_of(a.media_id)).tail);
    const double b_ext = std::min(half, source_handles(b, media_duration_of(b.media_id)).head);
    const double overlap = a_ext + b_ext;
    if (overlap <= kTransitionEps) continue;  // no handles: nothing to dissolve

    // Extend A's tail forward and B's head backward.
    seg_a.end += a_ext;
    if (a.reverse) {
      seg_a.source_in -= a_ext * clip_speed(a);
    } else {
      seg_a.source_out += a_ext * clip_speed(a);
    }

    seg_b.start -= b_ext;
    if (b.reverse) {
      seg_b.source_out += b_ext * clip_speed(b);
    } else {
      seg_b.source_in -= b_ext * clip_speed(b);
    }

    if (transition.kind == TransitionKind::Push || transition.kind == TransitionKind::Slide) {
      // Geometric: B slides in across the overlap, and push also slides A out.
      const SlideWindow win{.start = seg_b.start, .end = seg_a.end};
      seg_b.slide_kind = transition.kind;
      seg_b.slide_role = SlideRole::In;
      seg_b.slide_win = win;
      if (transition.kind == TransitionKind::Push) {
        seg_a.slide_kind = transition.kind;
        seg_a.slide_role = SlideRole::Out;
        seg_a.slide_win = win;
      }
    } else {
      // Cross-dissolve: B fades in over the whole overlap, so A reaches zero
      // exactly as B reaches full.
      seg_b.x_in = std::max(seg_b.x_in, overlap);
    }
  }

  return segs;
}

}  // namespace cutline::core
