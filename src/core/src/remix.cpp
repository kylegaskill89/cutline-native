#include "cutline/core/remix.hpp"

#include "cutline/core/query.hpp"

#include <algorithm>
#include <limits>

namespace cutline::core {

std::optional<std::string> why_not_remix(const Project& p, std::string_view clip_id) {
  const Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr) return "Select a clip of music in the timeline first.";
  if (clip->kind != TrackKind::Audio) return "Remix works on audio clips.";
  const auto media = std::ranges::find(p.media, clip->media_id, &Media::id);
  if (media == p.media.end() || media->audio_stream_count <= 0) {
    return "This clip has no sound to remix.";
  }
  if (!media->sequence_id.empty()) return "A nested sequence cannot be remixed.";
  if (is_time_remapped(*clip)) return "A clip with time remapping cannot be remixed.";
  return std::nullopt;
}

std::optional<RemixSource> remix_source(const Project& p, std::string_view clip_id) {
  const Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr) return std::nullopt;
  RemixSource source{.media_id = clip->media_id, .audio_stream = clip->audio_stream};
  if (clip->remix.has_value()) {
    source.source_in = clip->remix->source_in;
    source.source_out = clip->remix->source_out;
  } else {
    source.source_in = clip->source_in;
    source.source_out = clip->source_out;
  }
  return source;
}

double remix_room(const Project& p, std::string_view clip_id) noexcept {
  const Track* track = track_of_clip(p, clip_id);
  const Clip* clip = find_clip(p, clip_id);
  double room = std::numeric_limits<double>::infinity();
  if (track == nullptr || clip == nullptr) return room;
  for (const Clip& other : track->clips) {
    if (other.id == clip->id || other.start < clip->start + 1e-9) continue;
    room = std::min(room, other.start - clip->start);
  }
  return room;
}

Project apply_remix(Project p, std::string_view clip_id, Remix remix) {
  if (why_not_remix(p, clip_id).has_value() || remix.spans.empty()) return p;
  const std::optional<RemixSource> source = remix_source(p, clip_id);
  Clip* clip = find_clip(p, clip_id);
  remix.source_in = source->source_in;
  remix.source_out = source->source_out;
  clip->source_in = 0.0;
  clip->source_out = remix_length(remix);
  clip->remix = std::move(remix);
  // A frame held from the song names a moment in the file, and survives; the
  // fades are lengths, and are kept as long as the clip still has room.
  const double length = clip_duration(*clip);
  clip->fade_in = std::min(clip->fade_in, length);
  clip->fade_out = std::min(clip->fade_out, length - clip->fade_in);
  return p;
}

Project clear_remix(Project p, std::string_view clip_id) {
  const double room = remix_room(p, clip_id);
  Clip* clip = find_clip(p, clip_id);
  if (clip == nullptr || !clip->remix.has_value()) return p;
  const double speed = clip_speed(*clip);
  clip->source_in = clip->remix->source_in;
  clip->source_out = std::min(clip->remix->source_out, clip->source_in + room * speed);
  clip->remix.reset();
  const double length = clip_duration(*clip);
  clip->fade_in = std::min(clip->fade_in, length);
  clip->fade_out = std::min(clip->fade_out, length - clip->fade_in);
  return p;
}

}  // namespace cutline::core
