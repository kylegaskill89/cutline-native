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
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
/// Everything but a still gets what the file says. A still gets `still_length`,
/// and an *animated* one is not a still for this purpose — a GIF has a real
/// running time and overriding it would make it loop at the wrong speed.
///
/// The length is a parameter rather than the constant, because it is a
/// preference; the constant is what a caller with no opinion gets, and every
/// caller that has one passes it. One decision point either way, which is the
/// point — the bug this replaced came from there being none.
[[nodiscard]] double placement_duration(const MediaSource& source,
                                        double still_length = kStillLength) noexcept;

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
                                         std::string* id = nullptr,
                                         double still_length = kStillLength);

/// Adds several files to the pool as one edit.
///
/// One edit rather than one per file, which is the whole point: a folder of
/// forty rushes brought in as forty separate additions takes forty presses of
/// undo to take back out again, and nobody thinks of an import that way. It
/// happened once, so it is undone once.
///
/// Duplicates are skipped the way the single-file form skips them, and within
/// the batch as well as against the pool — the same path named twice in one
/// selection is one entry, not two.
///
/// `ids` receives one id per source, in the order given, so a caller can find
/// what arrived and select it. A source that was already in the pool reports
/// the id that was already there, which means the vector is always the same
/// length as `sources` and index `i` always answers for source `i`.
[[nodiscard]] core::Project import_media(core::Project project,
                                         std::span<const MediaSource> sources,
                                         std::vector<std::string>* ids = nullptr,
                                         double still_length = kStillLength);

/// The paths a Windows multi-select open dialog left behind in its buffer.
///
/// `OFN_ALLOWMULTISELECT` writes one of two shapes into the caller's buffer and
/// does not say which: a single complete path when one file was picked, or the
/// containing directory followed by bare filenames when several were. Each run
/// ends at a null and the whole thing ends at an empty one, so the only way to
/// tell the shapes apart is to notice whether the first entry is also the last.
///
/// A span rather than a pointer because the buffer is the unit that was handed
/// to the dialog: scanning stops at its end whatever the dialog wrote, so a
/// missing terminator is a short answer rather than a walk off the end.
///
/// Windows-shaped, and here rather than in the application because this is
/// where bringing files into a project is decided and because a parse with two
/// silently different shapes is exactly the kind of thing that wants a test.
[[nodiscard]] std::vector<std::filesystem::path> chosen_paths(std::span<const wchar_t> buffer);

/// Imports and places it on the timeline: one video clip plus one clip per
/// audio stream, linked, exactly as `core::place_media` does it.
[[nodiscard]] core::Project import_and_place(core::Project project, const MediaSource& source,
                                             double at,
                                             std::string_view video_track_id = {},
                                             double still_length = kStillLength);

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
                                         const MediaSource& source,
                                         double still_length = kStillLength);

/// Whether a path looks like a still image, by its extension.
///
/// A guess, and only used where a probe cannot say — some formats a decoder
/// happily reports as a one-frame video are stills as far as an editor is
/// concerned, and treating them as video gives a clip one frame long.
[[nodiscard]] bool looks_like_image(const std::filesystem::path& path);

/// Whether the extension is one worth offering to open at all.
[[nodiscard]] bool looks_like_media(const std::filesystem::path& path);

}  // namespace cutline::editor
