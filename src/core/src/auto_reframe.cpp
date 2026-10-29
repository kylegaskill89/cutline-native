#include "cutline/core/auto_reframe.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/nesting.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/sequences.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <vector>

namespace cutline::core {

FrameSize reframed_size(const Sequence& like, double aspect_w, double aspect_h) {
  const int shorter = std::max(2, std::min(like.canvas_w, like.canvas_h));
  if (!(aspect_w > 0.0) || !(aspect_h > 0.0)) return {like.canvas_w, like.canvas_h};
  const auto even = [](double v) { return std::max(2, static_cast<int>(std::lround(v / 2.0)) * 2); };
  const double ratio = aspect_w / aspect_h;
  if (ratio <= 1.0) return {even(shorter), even(shorter / ratio)};
  return {even(shorter * ratio), even(shorter)};
}

bool reframes(const Project& p, const Clip& clip) noexcept {
  if (clip.kind != TrackKind::Video) return false;
  const auto media = std::ranges::find(p.media, clip.media_id, &Media::id);
  if (media == p.media.end()) return false;
  if (media->is_graphic || media->is_color || media->is_adjustment) return false;
  return media->has_video || media->is_image || !media->sequence_id.empty();
}

Project duplicate_sequence(Project p, std::string name, std::string* made_id) {
  Sequence copy = p.sequence();
  copy.id = new_id("seq");
  copy.name = unused_sequence_name(p, name.empty() ? copy.name : name);
  // New ids throughout, since a clip is found by its id; a link between two
  // clips is kept as a link between their copies.
  std::map<std::string, std::string> groups;
  for (Track& track : copy.tracks) {
    track.id = new_id("track");
    for (Clip& clip : track.clips) {
      clip.id = new_id("clip");
      if (clip.group_id.has_value()) {
        auto [at, fresh] = groups.try_emplace(*clip.group_id);
        if (fresh) at->second = new_id("grp");
        clip.group_id = at->second;
      }
    }
  }
  if (made_id != nullptr) *made_id = copy.id;
  p.sequences.push_back(std::move(copy));
  p.open = p.sequences.size() - 1;
  return p;
}

Project auto_reframe_sequence(Project p, const AutoReframeOptions& options, std::string* made_id) {
  const Sequence& was = p.sequence();
  const int original_w = was.canvas_w;
  const int original_h = was.canvas_h;
  // Premiere's name for one: the sequence's, and the new shape after it.
  const std::string name =
      options.name.empty()
          ? std::format("{} ({}x{})", was.name.empty() ? std::string("Sequence") : was.name,
                        options.size.width, options.size.height)
          : options.name;

  std::string id;
  p = duplicate_sequence(std::move(p), name, &id);

  std::vector<std::string> pictures;
  for (const Track& track : p.sequence().tracks) {
    for (const Clip& clip : track.clips) {
      if (reframes(p, clip)) pictures.push_back(clip.id);
    }
  }

  // Nested while the sequence is still the old shape, so the nest is made at
  // the frame its pictures were cut for, and it is the nest that is filled.
  std::string nest_id;
  if (options.nest && !pictures.empty()) {
    p = nest_clips(std::move(p), pictures, name + " Nest", &nest_id);
    pictures.clear();
    if (!nest_id.empty()) pictures.push_back(nest_id);
  }

  p.sequence().canvas_w = std::max(2, options.size.width);
  p.sequence().canvas_h = std::max(2, options.size.height);
  if (const Clip* nest = nest_id.empty() ? nullptr : find_clip(p, nest_id); nest != nullptr) {
    // The nest is its own sequence and keeps the shape it was made in, which
    // its pool entry is then told.
    const auto media = std::ranges::find(p.media, nest->media_id, &Media::id);
    if (media != p.media.end()) {
      const std::size_t inner = sequence_index(p, media->sequence_id);
      if (inner < p.sequences.size()) {
        p.sequences[inner].canvas_w = original_w;
        p.sequences[inner].canvas_h = original_h;
      }
    }
    p = sync_nested_media(std::move(p));
  }

  const auto motion = static_cast<double>(std::clamp(options.motion, 0, 2));
  for (Track& track : p.sequence().tracks) {
    for (Clip& clip : track.clips) {
      if (std::ranges::find(pictures, clip.id) == pictures.end()) continue;
      const auto existing = std::ranges::find(clip.effects, std::string("autoreframe"),
                                              &ClipEffect::type);
      if (existing != clip.effects.end()) {
        existing->params["motion_preset"] = motion;
      } else {
        clip.effects.push_back(
            ClipEffect{.type = "autoreframe", .params = {{"motion_preset", motion}}});
      }
    }
  }
  if (made_id != nullptr) *made_id = id;
  return p;
}

}  // namespace cutline::core
