#pragma once

/// Captions, as Premiere keeps them.
///
/// A caption track is a track like any other, stored above the picture
/// tracks, and each caption on it is a clip with text and no media. That is
/// Premiere's arrangement and the reason it is worth copying: moving,
/// trimming, splitting, rippling, selecting, locking and undoing a caption are
/// what they already are for a clip, so none of it has to be built twice. The
/// track carries the style its captions are drawn in; see `CaptionStyle`.
///
/// Everything takes a project by value and hands one back, like the rest of
/// core.

#include "cutline/core/model.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::core {

/// How long a caption made at the playhead lasts, as Premiere's do, unless
/// the next caption is closer.
inline constexpr double kDefaultCaptionLength = 3.0;

/// Whether a track holds captions.
[[nodiscard]] bool is_caption_track(const Track& track) noexcept;

/// Adds an empty caption track above every other track, as Premiere puts its
/// caption tracks above the picture, and hands back its id.
[[nodiscard]] Project add_caption_track(Project p, std::string* made_id = nullptr);

/// Adds a caption to a caption track from `start`, for `length` seconds or up
/// to the next caption, whichever comes first. Nothing when `start` is inside
/// a caption already, or the track is not a caption track.
[[nodiscard]] Project add_caption(Project p, std::string_view track_id, double start,
                                  std::string text, double length = kDefaultCaptionLength,
                                  std::string* made_id = nullptr);

/// What a caption says.
[[nodiscard]] Project set_caption_text(Project p, std::string_view clip_id, std::string text);

/// A caption track's style.
[[nodiscard]] Project set_caption_style(Project p, std::string_view track_id, CaptionStyle style);

/// Premiere's Split Caption: one caption becomes two, each half as long, both
/// saying what it said, ready for its text to be divided by hand.
[[nodiscard]] Project split_caption(Project p, std::string_view clip_id);

/// Premiere's Merge Captions: neighbouring captions on one track become one
/// running from the first's start to the last's end, their text joined by a
/// space in time order.
[[nodiscard]] Project merge_captions(Project p, std::span<const std::string> clip_ids);

/// The caption showing at `t`: on the topmost caption track not hidden, and
/// not switched off. Null when there is none.
[[nodiscard]] const Clip* caption_at(const Sequence& s, double t) noexcept;

/// The graphic a caption is drawn as on a canvas of this size: one text layer
/// in its track's style, wrapped to the style's width and standing on its
/// foot.
[[nodiscard]] Graphic caption_graphic(const CaptionStyle& style, std::string_view text,
                                      int canvas_w, int canvas_h);

// -------------------------------------------------------------------- SRT --

/// One caption read from or written to a file: when, and what.
struct CaptionCue {
  double start = 0.0;
  double end = 0.0;
  std::string text;

  friend bool operator==(const CaptionCue&, const CaptionCue&) = default;
};

/// The cues in a SubRip (.srt) file. Forgiving of what real files contain:
/// a byte-order mark, Windows line endings, a missing index, a full stop for
/// the comma before milliseconds, blank lines where there should be one. Its
/// styling tags are taken out of the text. An error only when nothing in it
/// reads as a cue.
[[nodiscard]] std::expected<std::vector<CaptionCue>, std::string> parse_srt(std::string_view text);

/// A caption track as a SubRip file, its captions numbered from one in time
/// order. With `styled`, Premiere's Include SRT Styling: bold, italic and
/// colour from the track's style written as SRT's own tags.
[[nodiscard]] std::string write_srt(const Track& track, bool styled = false);

/// What an export from `from` to `to` writes beside itself: the topmost
/// visible caption track's captions in that span, cut to it and timed from
/// its start, as SubRip. Nothing when the sequence has no visible caption
/// track.
[[nodiscard]] std::optional<std::string> sidecar_srt(const Sequence& s, double from, double to,
                                                     bool styled = false);

/// A new caption track holding these cues, from `offset` seconds into the
/// sequence: Premiere's import of a captions file, whose Start Point is where
/// the file's zero lands.
[[nodiscard]] Project import_captions(Project p, std::span<const CaptionCue> cues,
                                      double offset = 0.0, std::string* made_id = nullptr);

}  // namespace cutline::core
