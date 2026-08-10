#include "cutline/editor/titles.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <utility>

namespace cutline::editor {
namespace {

[[nodiscard]] core::Media* find_media(core::Project& project, std::string_view media_id) {
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// What a title is called in the project panel.
///
/// The first line of its own text, which is what anyone would call it, and
/// falling back to something rather than a nameless entry.
[[nodiscard]] std::string name_for(const core::TextSpec& spec) {
  const std::size_t newline = spec.content.find('\n');
  std::string first = spec.content.substr(0, newline);
  if (first.empty()) return "Title";
  // Long enough to identify, short enough for a list.
  constexpr std::size_t kMaxName = 32;
  if (first.size() > kMaxName) first = first.substr(0, kMaxName - 1) + "...";
  return first;
}

}  // namespace

core::TextSpec default_title_spec() {
  // The model's own defaults, which are white bold centred text — deliberately
  // not overridden here, so there is one answer to what a title looks like.
  return core::TextSpec{};
}

core::Project add_title(core::Project project, core::TextSpec spec, std::string* id) {
  core::Media media;
  media.id = core::new_id("title");
  media.name = name_for(spec);
  media.is_text = true;
  // `has_video` means this contributes picture, which a title does — it is what
  // `place_media` looks at to decide whether anything goes on a video track, so
  // without it a title could be made and never placed.
  media.has_video = true;
  // Nothing on disk, and no inherent length. The duration is what a placement
  // uses when there is no source to take one from.
  media.duration = kDefaultTitleLength;
  media.text = std::move(spec);

  if (id != nullptr) *id = media.id;
  project.media.push_back(std::move(media));
  return project;
}

core::Project add_title_at(core::Project project, core::TextSpec spec, double at,
                           std::string_view video_track_id, std::string* clip_id) {
  std::string media_id;
  project = add_title(std::move(project), std::move(spec), &media_id);

  std::vector<std::string> existing;
  for (const core::Track& track : project.sequence().tracks) {
    for (const core::Clip& clip : track.clips) existing.push_back(clip.id);
  }

  project = core::place_media(std::move(project), media_id, at, video_track_id);

  // The placement decides the clip's id; finding it is how a caller can select
  // what it just made. Comparing against what was there beats guessing at the
  // id generator's next value.
  if (clip_id != nullptr) {
    for (const core::Track& track : project.sequence().tracks) {
      for (const core::Clip& clip : track.clips) {
        if (clip.media_id != media_id) continue;
        if (std::ranges::find(existing, clip.id) != existing.end()) continue;
        *clip_id = clip.id;
      }
    }
  }
  return project;
}

const core::TextSpec* title_spec(const core::Project& project,
                                 std::string_view media_id) noexcept {
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  if (found == project.media.end() || !found->is_text || !found->text.has_value()) {
    return nullptr;
  }
  return &*found->text;
}

const core::TextSpec* clip_title_spec(const core::Project& project,
                                      std::string_view clip_id) noexcept {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return nullptr;
  return title_spec(project, clip->media_id);
}

core::Project set_title_spec(core::Project project, std::string_view media_id,
                             core::TextSpec spec) {
  core::Media* media = find_media(project, media_id);
  if (media == nullptr || !media->is_text) return project;
  if (media->text.has_value() && *media->text == spec) return project;

  // The name follows the words, unless someone has renamed it to something the
  // text no longer explains. Comparing against the old text's name is what
  // tells those two apart.
  if (!media->text.has_value() || media->name == name_for(*media->text)) {
    media->name = name_for(spec);
  }
  media->text = std::move(spec);
  return project;
}

}  // namespace cutline::editor
