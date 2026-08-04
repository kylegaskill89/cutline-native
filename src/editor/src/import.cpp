#include "cutline/editor/import.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/id.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <string>
#include <utility>

namespace cutline::editor {
namespace {

/// Extensions an editor should treat as a still rather than as video, whatever
/// a decoder makes of them.
constexpr std::array<std::string_view, 9> kImageExtensions{
    ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".webp", ".tga", ".gif",
};

constexpr std::array<std::string_view, 16> kVideoExtensions{
    ".mp4", ".mov", ".mkv", ".avi", ".webm", ".m4v", ".mpg", ".mpeg",
    ".wmv", ".flv", ".mts", ".m2ts", ".mxf", ".braw", ".r3d", ".avchd",
};

constexpr std::array<std::string_view, 8> kAudioExtensions{
    ".wav", ".mp3", ".aac", ".flac", ".ogg", ".m4a", ".wma", ".aif",
};

[[nodiscard]] std::string lowered_extension(const std::filesystem::path& path) {
  std::string text = path.extension().string();
  std::ranges::transform(text, text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

[[nodiscard]] bool contains(std::span<const std::string_view> list, std::string_view value) {
  return std::ranges::find(list, value) != list.end();
}

}  // namespace

bool looks_like_image(const std::filesystem::path& path) {
  return contains(kImageExtensions, lowered_extension(path));
}

bool looks_like_media(const std::filesystem::path& path) {
  const std::string extension = lowered_extension(path);
  return contains(kImageExtensions, extension) || contains(kVideoExtensions, extension) ||
         contains(kAudioExtensions, extension);
}

const core::Media* find_media_by_path(const core::Project& project,
                                      std::string_view path) noexcept {
  if (path.empty()) return nullptr;
  const auto found = std::ranges::find(project.media, path, &core::Media::path);
  return found == project.media.end() ? nullptr : &*found;
}

core::Project import_media(core::Project project, const MediaSource& source, std::string* id) {
  // Already in the pool: hand back what is there. Adding it again would give
  // the browser two entries for one file, and two ids that can drift apart as
  // soon as either is edited.
  if (const core::Media* existing = find_media_by_path(project, source.path);
      existing != nullptr) {
    if (id != nullptr) *id = existing->id;
    return project;
  }

  core::Media media;
  media.id = core::new_id("media");
  media.path = source.path;
  media.name = source.name.empty() ? std::filesystem::path(source.path).filename().string()
                                   : source.name;
  media.duration = source.duration;
  media.has_video = source.has_video;
  media.audio_stream_count = source.audio_stream_count;
  media.is_image = source.is_image;
  media.is_animated = source.is_animated;
  media.width = source.width;
  media.height = source.height;
  media.fps = source.fps;

  if (id != nullptr) *id = media.id;
  return core::add_media(std::move(project), std::move(media));
}

core::Project import_and_place(core::Project project, const MediaSource& source, double at,
                               std::string_view video_track_id) {
  std::string id;
  project = import_media(std::move(project), source, &id);
  if (id.empty()) return project;
  // Before it lands, because the rule is about an *empty* sequence and this is
  // the moment it stops being one. A 4K60 clip dropped into a 1080p30 sequence
  // otherwise plays every second frame and is scaled down for the privilege.
  project = core::match_sequence_to(std::move(project), id);
  return core::place_media(std::move(project), id, std::max(0.0, at), video_track_id);
}

}  // namespace cutline::editor
