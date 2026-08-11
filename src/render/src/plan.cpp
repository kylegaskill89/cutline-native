#include "cutline/render/plan.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>
#include <limits>

namespace cutline::render {
namespace {

using core::Media;

[[nodiscard]] LayerContent content_of(const Media* media) noexcept {
  if (media == nullptr) return LayerContent::Video;
  // Before the rest: a nest has no file and no flags, so every other test here
  // would fall through it to `Video` and send it to a decoder.
  if (core::is_nested_sequence(*media)) return LayerContent::Nested;
  if (media->is_text) return LayerContent::Text;
  if (media->is_color) return LayerContent::Color;
  if (media->is_adjustment) return LayerContent::Adjustment;
  if (media->is_image) return LayerContent::Still;
  return LayerContent::Video;
}

/// Sorted alongside each layer so the draw order can be established before the
/// layers themselves are built.
struct Ordered {
  int track_index = 0;
  double start = 0.0;
  core::VideoSeg seg;
};

}  // namespace

std::vector<PlannedLayer> plan_frame(
    const core::Project& project, double t,
    const std::function<double(std::string_view media_id)>& media_duration_of,
    const TextMeasurer& measure_text) {
  std::vector<Ordered> active;

  // Video tracks are stored top-first, so the bottom one is drawn first.
  std::vector<const core::Track*> video_tracks;
  for (const core::Track& track : project.sequence().tracks) {
    if (track.kind == core::TrackKind::Video) video_tracks.push_back(&track);
  }
  std::reverse(video_tracks.begin(), video_tracks.end());

  for (std::size_t index = 0; index < video_tracks.size(); ++index) {
    const core::Track& track = *video_tracks[index];
    if (track.hidden) continue;  // the eye is off

    for (const core::VideoSeg& seg : resolve_video_segments(track, media_duration_of)) {
      if (seg.clip == nullptr || seg.clip->disabled) continue;
      if (t < seg.start || t >= seg.end) continue;
      active.push_back({static_cast<int>(index), seg.start, seg});
    }
  }

  // Within a track, earlier-starting segments draw first, so a dissolve's
  // incoming clip lands on top of the outgoing one rather than under it.
  std::stable_sort(active.begin(), active.end(), [](const Ordered& a, const Ordered& b) {
    if (a.track_index != b.track_index) return a.track_index < b.track_index;
    return a.start < b.start;
  });

  std::vector<PlannedLayer> layers;
  layers.reserve(active.size());

  for (const Ordered& entry : active) {
    const core::VideoSeg& seg = entry.seg;
    const core::Clip& clip = *seg.clip;
    const Media* media = nullptr;
    for (const Media& candidate : project.media) {
      if (candidate.id == clip.media_id) {
        media = &candidate;
        break;
      }
    }

    PlannedLayer layer;
    layer.clip = &clip;
    layer.media = media;
    layer.content = content_of(media);
    layer.track_index = entry.track_index;
    // A title is as big as its text, which only something that can draw text
    // knows. Without a measurer it keeps the fallback — the whole canvas —
    // because a title of no size would simply not be there.
    core::Size measured;
    if (media != nullptr && media->is_text && measure_text) measured = measure_text(*media);
    layer.box =
        core::segment_box(seg, media, project.sequence().canvas_w, project.sequence().canvas_h, t, measured);
    layer.alpha = core::segment_alpha(seg, t);
    layer.blend = clip.blend;
    layer.anti_flicker = std::clamp(clip.transform.anti_flicker, 0.0, 1.0);

    // Clamped inside the segment: a rounding error at a boundary must not pull
    // in a frame from beyond the trim, which would show a flash of the wrong
    // shot for exactly one frame.
    constexpr double kEndGuard = 1e-3;
    const double wanted = core::source_time_at(clip, t);
    layer.source_time =
        std::min(std::max(seg.source_in, wanted), seg.source_out - kEndGuard);

    layers.push_back(layer);
  }

  return layers;
}

std::vector<PlannedLayer> plan_frame(const core::Project& project, double t,
                                     const TextMeasurer& measure_text) {
  return plan_frame(
      project, t,
      [&project](std::string_view media_id) {
        for (const Media& media : project.media) {
          if (media.id != media_id) continue;
          // Stills and generated media have no source to run out of, so their
          // handles are unlimited and a transition can borrow as much as it
          // likes.
          if (core::is_still_like(media)) return std::numeric_limits<double>::infinity();
          // A nest included: its length is what the sequence inside it runs to,
          // which `duration` already mirrors.
          return media.duration;
        }
        return 0.0;
      },
      measure_text);
}

}  // namespace cutline::render
