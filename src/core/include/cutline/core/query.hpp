#pragma once

/// Derived reads over the data model: time mapping, animation, and lookups.
/// Everything here is a pure function of the model — nothing caches, nothing
/// mutates.

#include "cutline/core/model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cutline::core {

// ------------------------------------------------------------------- media --

/// Generated media (title, colour matte, adjustment layer) have no source file
/// and no audio, and behave like stills: no in/out range, unlimited handles.
[[nodiscard]] bool is_generated_media(const Media& m) noexcept;

/// True when a media has no real timeline source — a still, GIF, or generated.
[[nodiscard]] bool is_still_like(const Media& m) noexcept;

// ------------------------------------------------------------- clip timing --

/// Playback speed, guarded to always be positive.
[[nodiscard]] double clip_speed(const Clip& c) noexcept;

/// Length of source consumed, in source seconds, before retiming.
[[nodiscard]] double source_span(const Clip& c) noexcept;

/// Timeline duration: the source span retimed by speed.
[[nodiscard]] double clip_duration(const Clip& c) noexcept;

[[nodiscard]] double clip_end(const Clip& c) noexcept;

/// How much unused source lies past each end of a clip — its trim handles —
/// measured in timeline seconds.
///
/// What a transition borrows. A clip trimmed to the very end of its footage has
/// no tail to lend, and a cross-dissolve there has nothing to dissolve with;
/// this is how anything offering one finds that out beforehand rather than
/// leaving somebody to wonder why nothing happened.
struct Handles {
  double head = 0.0;
  double tail = 0.0;

  friend bool operator==(const Handles&, const Handles&) = default;
};

/// The clip's handles, given how long its media is. Reverse swaps which
/// physical handle feeds the head and which the tail.
[[nodiscard]] Handles source_handles(const Clip& c, double media_duration) noexcept;

/// The same, looking the media up in the project. Still-like media have no
/// source to run out of, so their handles are unbounded.
[[nodiscard]] Handles source_handles(const Project& p, const Clip& c) noexcept;

/// The source-media time playing at timeline time `t`, which must lie within
/// the clip. Accounts for speed and reverse.
[[nodiscard]] double source_time_at(const Clip& c, double t) noexcept;

/// The source range that plays during the clip's timeline sub-window [a, b].
/// Reverse-aware, so splitting or carving a retimed or reversed clip keeps each
/// piece's frames correct.
struct SourceRange {
  double source_in = 0.0;
  double source_out = 0.0;

  friend bool operator==(const SourceRange&, const SourceRange&) = default;
};

[[nodiscard]] SourceRange clip_sub_source(const Clip& c, double a, double b) noexcept;

/// Total timeline length: the furthest clip end across all tracks.
[[nodiscard]] double timeline_duration(const Project& p) noexcept;

// ---------------------------------------------------------------- animation --

/// Whether a clip animates a property, meaning it has at least one keyframe.
[[nodiscard]] bool is_animated(const Clip& c, AnimProp prop) noexcept;

/// The transform at clip-local time `local_t`, honouring any keyframes.
[[nodiscard]] Transform animated_transform(const Clip& c, double local_t) noexcept;

/// The opacity at clip-local time `local_t`, honouring any keyframes,
/// clamped to 0..1.
[[nodiscard]] double animated_opacity(const Clip& c, double local_t) noexcept;

/// The value of any animatable property — animated at `local_t`, else static.
[[nodiscard]] double animated_value(const Clip& c, AnimProp prop, double local_t) noexcept;

// -------------------------------------------------------------------- audio --

/// Whether a clip's volume is automated with gain keyframes.
[[nodiscard]] bool is_gain_animated(const Clip& c) noexcept;

/// The clip's linear gain at clip-local time `local_t`, from automation when
/// present and the constant gain otherwise. Clamped to 0..kMaxGain.
[[nodiscard]] double gain_at(const Clip& c, double local_t) noexcept;

/// Where the clip sits across the stereo image at clip-local `local_t`, from
/// automation when present and the constant pan otherwise. Clamped to -1..1,
/// where -1 is hard left and 1 hard right.
[[nodiscard]] double pan_at(const Clip& c, double local_t) noexcept;

/// Whether an audio track is heard: not muted, and — if any audio track is
/// soloed — only soloed tracks play.
[[nodiscard]] bool is_track_audible(const Project& p, const Track& track) noexcept;

// ------------------------------------------------------------------ lookups --

/// The clip with this id, or null. The non-const overload permits editing
/// operations to work against a cloned project.
[[nodiscard]] const Clip* find_clip(const Project& p, std::string_view clip_id) noexcept;
[[nodiscard]] Clip* find_clip(Project& p, std::string_view clip_id) noexcept;

/// The track holding this clip, or null.
[[nodiscard]] const Track* track_of_clip(const Project& p, std::string_view clip_id) noexcept;
[[nodiscard]] Track* track_of_clip(Project& p, std::string_view clip_id) noexcept;

/// Ids of every clip linked to `clip_id`, including itself. An unlinked clip
/// yields just its own id; an unknown clip yields nothing.
[[nodiscard]] std::vector<std::string> group_members(const Project& p,
                                                     std::string_view clip_id);

/// The clip covering time `t` on a track, if any, where start <= t < end.
[[nodiscard]] const Clip* clip_at_time(const Track& track, double t) noexcept;

}  // namespace cutline::core
