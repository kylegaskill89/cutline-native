#include "cutline/editor/generators.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace cutline::editor {
namespace {

[[nodiscard]] core::Media* find_media(core::Project& project, std::string_view media_id) {
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// The clip a placement just made, found by elimination.
///
/// The placement decides the id, and comparing against what was there beats
/// guessing at the id generator's next value. The same trick `add_title_at`
/// uses, and the reason it is here is that it is now used twice.
[[nodiscard]] std::vector<std::string> clip_ids(const core::Project& project) {
  std::vector<std::string> ids;
  for (const core::Track& track : project.sequence().tracks) {
    for (const core::Clip& clip : track.clips) ids.push_back(clip.id);
  }
  return ids;
}

void report_new_clip(const core::Project& project, std::string_view media_id,
                     const std::vector<std::string>& before, std::string* clip_id) {
  if (clip_id == nullptr) return;
  for (const core::Track& track : project.sequence().tracks) {
    for (const core::Clip& clip : track.clips) {
      if (clip.media_id != media_id) continue;
      if (std::ranges::find(before, clip.id) != before.end()) continue;
      *clip_id = clip.id;
    }
  }
}

/// What a matte is called in the project panel. Its colour, which is the only
/// thing there is to say about one.
[[nodiscard]] std::string name_for(const MatteFill& fill) {
  return fill.gradient.has_value() ? "Gradient " + fill.color : "Matte " + fill.color;
}

}  // namespace

core::Project add_color_matte(core::Project project, MatteFill fill, std::string* id) {
  core::Media media;
  media.id = core::new_id("matte");
  media.name = name_for(fill);
  media.is_color = true;
  // `has_video` means this contributes picture, which a matte does. It is what
  // `place_media` checks before putting anything on a video track.
  media.has_video = true;
  media.duration = kDefaultGeneratorLength;
  media.color = std::move(fill.color);
  media.gradient = std::move(fill.gradient);

  if (id != nullptr) *id = media.id;
  project.media.push_back(std::move(media));
  return project;
}

core::Project add_color_matte_at(core::Project project, MatteFill fill, double at,
                                 std::string_view video_track_id, std::string* clip_id) {
  std::string media_id;
  project = add_color_matte(std::move(project), std::move(fill), &media_id);

  const std::vector<std::string> before = clip_ids(project);
  project = core::place_media(std::move(project), media_id, at, video_track_id);
  report_new_clip(project, media_id, before, clip_id);
  return project;
}

core::Project add_adjustment_layer(core::Project project, std::string* id) {
  core::Media media;
  media.id = core::new_id("adjust");
  media.name = "Adjustment Layer";
  media.is_adjustment = true;
  // Still true, and still for the same reason: it has to reach a video track.
  // What it contributes is a filter over everything beneath rather than a
  // picture of its own, which the compositor knows and `place_media` does not
  // need to.
  media.has_video = true;
  media.duration = kDefaultGeneratorLength;

  if (id != nullptr) *id = media.id;
  project.media.push_back(std::move(media));
  return project;
}

core::Project add_adjustment_layer_at(core::Project project, double at,
                                      std::string_view video_track_id, std::string* clip_id) {
  std::string media_id;
  project = add_adjustment_layer(std::move(project), &media_id);

  const std::vector<std::string> before = clip_ids(project);
  project = core::place_media(std::move(project), media_id, at, video_track_id);
  report_new_clip(project, media_id, before, clip_id);
  return project;
}

std::optional<MatteFill> matte_fill(const core::Project& project, std::string_view media_id) {
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  if (found == project.media.end() || !found->is_color) return std::nullopt;
  return MatteFill{.color = found->color, .gradient = found->gradient};
}

std::optional<MatteFill> clip_matte_fill(const core::Project& project,
                                         std::string_view clip_id) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return std::nullopt;
  return matte_fill(project, clip->media_id);
}

core::Project set_matte_fill(core::Project project, std::string_view media_id,
                             MatteFill fill) {
  core::Media* media = find_media(project, media_id);
  if (media == nullptr || !media->is_color) return project;
  if (media->color == fill.color && media->gradient == fill.gradient) return project;

  // The name follows the colour, unless someone has renamed it to something the
  // colour no longer explains — the same rule a title's name follows its words.
  const MatteFill was{.color = media->color, .gradient = media->gradient};
  if (media->name == name_for(was)) media->name = name_for(fill);

  media->color = std::move(fill.color);
  media->gradient = std::move(fill.gradient);
  return project;
}

bool clip_is_adjustment(const core::Project& project, std::string_view clip_id) noexcept {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return false;
  const auto media = std::ranges::find(project.media, clip->media_id, &core::Media::id);
  return media != project.media.end() && media->is_adjustment;
}

}  // namespace cutline::editor
