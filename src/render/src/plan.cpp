#include "cutline/render/plan.hpp"

#include "cutline/core/captions.hpp"
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
  if (media->is_graphic) return LayerContent::Graphic;
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
    const std::function<double(std::string_view media_id)>& media_duration_of) {
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
    layer.box =
        core::segment_box(seg, media, project.sequence().canvas_w, project.sequence().canvas_h, t);
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

  // The caption showing, last and so over every picture, on the whole frame:
  // where it sits within the frame is its style's business.
  if (const core::Clip* caption = core::caption_at(project.sequence(), t); caption != nullptr) {
    PlannedLayer layer;
    layer.clip = caption;
    layer.content = LayerContent::Caption;
    const double w = project.sequence().canvas_w;
    const double h = project.sequence().canvas_h;
    layer.box = core::LayerBox{.center_x = w / 2.0, .center_y = h / 2.0, .width = w, .height = h};
    layer.track_index = static_cast<int>(video_tracks.size());
    layers.push_back(layer);
  }

  return layers;
}

std::vector<PlannedLayer> plan_frame(const core::Project& project, double t) {
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
      });
}

}  // namespace cutline::render
