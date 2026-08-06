#pragma once

/// Bringing a file into a project.
///
/// `MediaSource` is what a probe found out, expressed without any reference to
/// the media layer — `cutline::editor` stays free of FFmpeg, so all of this
/// builds and tests without it and the application does the one conversion
/// where the two meet.
///
/// The interesting behaviour is not the conversion but the pool: importing the
/// same file twice must not put it in twice, or a project accumulates a media
/// entry per drag and the browser fills with duplicates of one clip.

#include "cutline/core/model.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cutline::editor {

/// What is known about a file on disk.
struct MediaSource {
  /// Absolute, and the identity of the media: two imports of the same path are
  /// the same media however differently they were described.
  std::string path;
  /// Shown in the browser. Empty takes the filename.
  std::string name;

  double duration = 0.0;
  bool has_video = false;
  int audio_stream_count = 0;

  /// A still, or an animation that loops rather than having a real duration.
  bool is_image = false;
  bool is_animated = false;

  std::optional<int> width;
  std::optional<int> height;
  std::optional<double> fps;

  friend bool operator==(const MediaSource&, const MediaSource&) = default;
};

/// How long a still is placed for.
///
/// A still has no length of its own, so somebody has to choose one, and until
/// this existed nobody had: libavformat calls a PNG a one-frame video at its
/// own default rate, that duration went straight into the model, and a picture
/// dragged onto the timeline arrived a fortieth of a second long — placed,
/// counted as used, and invisible at every zoom.
///
/// Five seconds is Premiere's default and matches `kDefaultGeneratorLength`,
/// which is the same question already answered correctly for titles and
/// mattes. Kept as its own constant rather than shared with it because
/// Premiere keeps them as two preferences and they are two decisions: how long
/// a photograph should sit on screen and how long a title should are not
/// obliged to agree.
inline constexpr double kStillLength = 5.0;

/// What a source's media duration should be, given what the probe found.
///
/// Everything but a still gets what the file says. A still gets `kStillLength`,
/// and an *animated* one is not a still for this purpose — a GIF has a real
/// running time and overriding it would make it loop at the wrong speed.
[[nodiscard]] double placement_duration(const MediaSource& source) noexcept;

/// The media in `project` that already refers to this path, or null.
[[nodiscard]] const core::Media* find_media_by_path(const core::Project& project,
                                                    std::string_view path) noexcept;

/// Adds the file to the project's media pool.
///
/// A file already in the pool is not added again; its existing id comes back
/// instead. Without that, dragging the same clip in twice gives a browser with
/// two entries for one file and two ids that can drift apart.
///
/// The id is reported through `id` when one is wanted.
[[nodiscard]] core::Project import_media(core::Project project, const MediaSource& source,
                                         std::string* id = nullptr);

/// Imports and places it on the timeline: one video clip plus one clip per
/// audio stream, linked, exactly as `core::place_media` does it.
[[nodiscard]] core::Project import_and_place(core::Project project, const MediaSource& source,
                                             double at,
                                             std::string_view video_track_id = {});

/// Points a pool entry at a file somewhere else, keeping its id.
///
/// What repairs a project whose footage has moved. The id is what clips name,
/// so repointing one entry repairs every clip that used it — which is the whole
/// reason media are referred to by id rather than by path, and it has been true
/// since the model was written without anything taking advantage of it.
///
/// The probed facts come with the path. A relink is usually the same file in a
/// new place, but it is not guaranteed to be, and a source that went on
/// reporting the old duration and the old size would describe something that is
/// not there — which is worse than the offline entry it replaced, because
/// nothing would say so.
///
/// The *name* is deliberately kept. It is a property of the project, renameable
/// in the browser, and somebody who renamed an entry has said what they want it
/// called; relinking is not the moment to overrule that.
///
/// Clips are left exactly as they are. They name times, and a time still means
/// what it meant — a clip reaching past the end of a shorter replacement is a
/// thing to see rather than a thing to silently trim.
[[nodiscard]] core::Project relink_media(core::Project project, std::string_view media_id,
                                         const MediaSource& source);

/// Whether a path looks like a still image, by its extension.
///
/// A guess, and only used where a probe cannot say — some formats a decoder
/// happily reports as a one-frame video are stills as far as an editor is
/// concerned, and treating them as video gives a clip one frame long.
[[nodiscard]] bool looks_like_image(const std::filesystem::path& path);

/// Whether the extension is one worth offering to open at all.
[[nodiscard]] bool looks_like_media(const std::filesystem::path& path);

}  // namespace cutline::editor
