#include "cutline/editor/browser_binding.hpp"

#include "cutline/core/time.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace cutline::editor {
namespace {

[[nodiscard]] std::string lowered(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

/// Compares two names the way a person reads a list: case-insensitively, so
/// `Boiler.mp4` and `beach.mov` are not separated by every capital letter
/// sorting before every lower-case one.
[[nodiscard]] bool name_before(const ui::MediaItem& a, const ui::MediaItem& b) {
  return lowered(a.name) < lowered(b.name);
}

}  // namespace

ui::MediaKind media_kind(const core::Media& media) noexcept {
  // Ordered from most specific: a title is a generated still, and an adjustment
  // layer has neither video nor audio, so testing the flags in the other order
  // would call both of them something else.
  if (media.is_adjustment) return ui::MediaKind::Adjustment;
  if (media.is_text) return ui::MediaKind::Title;
  if (media.is_color) return ui::MediaKind::Color;
  if (media.is_image) return ui::MediaKind::Image;
  if (!media.has_video && media.audio_stream_count > 0) return ui::MediaKind::Audio;
  return ui::MediaKind::Video;
}

std::string media_detail(const core::Media& media, double fps) {
  switch (media_kind(media)) {
    case ui::MediaKind::Adjustment: return "adjustment";
    case ui::MediaKind::Title: return "title";
    case ui::MediaKind::Color: return "color";
    case ui::MediaKind::Image:
      // An animated still has a real running time; a plain one does not, and
      // showing its default placement length as though it did would be a lie.
      return media.is_animated && media.duration > 0.0
                 ? core::seconds_to_timecode(media.duration, fps)
                 : "still";
    case ui::MediaKind::Audio:
    case ui::MediaKind::Video: break;
  }
  if (media.duration <= 0.0) return {};
  return core::seconds_to_timecode(media.duration, fps);
}

int media_uses(const core::Project& project, std::string_view media_id) noexcept {
  if (media_id.empty()) return 0;
  int count = 0;
  for (const core::Track& track : project.tracks) {
    for (const core::Clip& clip : track.clips) {
      if (clip.media_id == media_id) ++count;
    }
  }
  return count;
}

bool media_matches(const core::Media& media, std::string_view query) {
  const std::string terms = lowered(query);
  // Searched together so a term can match either. Two fields checked
  // separately would fail on "boiler mp4" when the name has no extension.
  const std::string haystack = lowered(media.name) + '\n' + lowered(media.path);

  std::size_t at = 0;
  while (at < terms.size()) {
    while (at < terms.size() && std::isspace(static_cast<unsigned char>(terms[at]))) ++at;
    const std::size_t begin = at;
    while (at < terms.size() && !std::isspace(static_cast<unsigned char>(terms[at]))) ++at;
    if (begin == at) break;

    if (haystack.find(terms.substr(begin, at - begin)) == std::string::npos) return false;
  }
  return true;
}

std::vector<ui::MediaItem> browser_items(const core::Project& project,
                                         const BrowserOptions& options) {
  std::vector<ui::MediaItem> items;
  items.reserve(project.media.size());

  for (const core::Media& media : project.media) {
    if (!options.search.empty() && !media_matches(media, options.search)) continue;

    ui::MediaItem item;
    item.id = media.id;
    // The pool is allowed to hold a nameless entry — a path is enough to import
    // one — and a row with no text at all looks broken.
    item.name = media.name.empty() ? media.path : media.name;
    item.kind = media_kind(media);
    item.duration = media.duration;
    item.detail = media_detail(media, project.fps);
    item.uses = media_uses(project, media.id);
    item.offline = !media.path.empty() && std::ranges::find(options.offline, media.path) !=
                                              options.offline.end();
    items.push_back(std::move(item));
  }

  // Stable throughout, so entries that compare equal — two stills, two clips of
  // the same length — stay in the order they were imported rather than
  // shuffling every time the list is rebuilt.
  switch (options.sort) {
    case BrowserSort::Pool: break;
    case BrowserSort::Name:
      std::ranges::stable_sort(items, name_before);
      break;
    case BrowserSort::Kind:
      std::ranges::stable_sort(items, [](const ui::MediaItem& a, const ui::MediaItem& b) {
        return a.kind < b.kind;
      });
      break;
    case BrowserSort::Duration:
      std::ranges::stable_sort(items, [](const ui::MediaItem& a, const ui::MediaItem& b) {
        return a.duration < b.duration;
      });
      break;
    case BrowserSort::Uses:
      std::ranges::stable_sort(items, [](const ui::MediaItem& a, const ui::MediaItem& b) {
        return a.uses < b.uses;
      });
      break;
  }
  if (options.descending) std::ranges::reverse(items);

  return items;
}

core::Project remove_media(core::Project project, std::string_view media_id) {
  if (media_id.empty()) return project;

  for (core::Track& track : project.tracks) {
    std::erase_if(track.clips,
                  [media_id](const core::Clip& clip) { return clip.media_id == media_id; });
  }
  std::erase_if(project.media,
                [media_id](const core::Media& media) { return media.id == media_id; });
  return project;
}

core::Project rename_media(core::Project project, std::string_view media_id, std::string name) {
  if (name.empty()) return project;
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  if (found != project.media.end()) found->name = std::move(name);
  return project;
}

}  // namespace cutline::editor
