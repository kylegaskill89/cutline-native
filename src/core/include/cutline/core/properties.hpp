#pragma once

/// Property edits: clip attributes, generated-media settings, tracks, and
/// markers. Same contract as `edit.hpp` — take a project, return a new one.
///
/// The reference expressed several of these as partial-object merges, which is
/// natural in JavaScript. Here the value setters take whole values instead:
/// callers read the current value, change what they want, and set it back.
/// `Transform` and `TextSpec` are value types, so this is the same edit with
/// less machinery. Track header props keep a patch, since a caller genuinely
/// wants to set one flag without disturbing the others.

#include "cutline/core/model.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cutline::core {

// -------------------------------------------------------- clip properties --

/// Disabled clips keep their place on the timeline but are not rendered.
[[nodiscard]] Project set_clips_enabled(Project p, std::span<const std::string> clip_ids,
                                        bool enabled);

[[nodiscard]] Project set_clip_blend(Project p, std::string_view clip_id, BlendMode mode);

/// Sets or clears the out-edge transition. A transition of zero duration clears
/// it, since a zero-length transition is just a cut.
[[nodiscard]] Project set_clip_transition(Project p, std::string_view clip_id,
                                          std::optional<Transition> transition);

/// Sets the clip's linear audio gain, clamped to the allowed range.
[[nodiscard]] Project set_clip_gain(Project p, std::string_view clip_id, double gain);

/// Sets a fade duration, clamped so the two fades together never exceed the
/// clip's length.
[[nodiscard]] Project set_clip_fade(Project p, std::string_view clip_id, ClipEdge edge,
                                    double duration);

/// Sets playback speed, and optionally reverse, across the linked group so
/// video and its audio retime together. The source in and out are kept, so the
/// timeline length changes. Fades are re-clamped to the new duration.
[[nodiscard]] Project set_clip_speed(Project p, std::string_view clip_id, double speed,
                                     std::optional<bool> reverse = std::nullopt);

[[nodiscard]] Project set_clip_opacity(Project p, std::string_view clip_id, double opacity);

[[nodiscard]] Project set_clip_transform(Project p, std::string_view clip_id, Transform transform);

// ------------------------------------------------------------------ canvas --

/// The smallest and largest sequence this will hold. The floor is where a
/// picture stops being one; the ceiling is well past 8K, and is there to stop a
/// typed digit too many asking the compositor for a target it cannot make.
inline constexpr int kMinCanvas = 16;
inline constexpr int kMaxCanvas = 16384;

/// Resizes the sequence, clamped to the allowed range.
///
/// Nothing else moves. Transforms are canvas fractions, so a resize is the same
/// picture at a different size — and a change of *shape* reframes everything,
/// which is what changing the shape of a sequence means and what anybody doing
/// it is asking for.
[[nodiscard]] Project set_canvas(Project p, int width, int height);

// ------------------------------------------------------------------ master --

/// Sets the gain applied to the whole mix, clamped to the allowed range.
[[nodiscard]] Project set_master_gain(Project p, double gain);

// ------------------------------------------------------- generated media --

/// Replaces a text media's styling. No-op unless the media is a title.
[[nodiscard]] Project set_text_spec(Project p, std::string_view media_id, TextSpec spec);

/// Sets a colour matte's primary fill. No-op unless the media is a matte.
[[nodiscard]] Project set_matte_color(Project p, std::string_view media_id, std::string color);

/// Sets or clears a colour matte's linear gradient; clearing leaves a solid fill.
[[nodiscard]] Project set_matte_gradient(Project p, std::string_view media_id,
                                         std::optional<MatteGradient> gradient);

// ------------------------------------------------------------------ tracks --

/// Adds an empty video track at the top of the stack, which is the topmost
/// compositing layer — new overlay footage goes above what is already there.
[[nodiscard]] Project add_video_track(Project p);

/// Adds an empty audio track at the bottom of the stack.
[[nodiscard]] Project add_audio_track(Project p);

/// Sets a track's display label; an empty or blank label clears it.
[[nodiscard]] Project set_track_label(Project p, std::string_view track_id, std::string label);

/// Header flags a caller may change independently of one another.
struct TrackPropsPatch {
  std::optional<bool> muted;
  std::optional<bool> solo;
  std::optional<bool> locked;
  std::optional<bool> hidden;
  std::optional<double> height;
};

[[nodiscard]] Project update_track(Project p, std::string_view track_id,
                                   const TrackPropsPatch& patch);

/// Removes a track and every clip on it.
[[nodiscard]] Project remove_track(Project p, std::string_view track_id);

// ----------------------------------------------------------------- markers --

/// The marker nearest `time` within `tolerance`, if any.
[[nodiscard]] const Marker* marker_near(const Project& p, double time, double tolerance) noexcept;

[[nodiscard]] Project add_marker(Project p, double time, std::string label = {},
                                 std::string color = {});

[[nodiscard]] Project remove_marker(Project p, std::string_view marker_id);

[[nodiscard]] Project clear_markers(Project p);

/// The nearest marker strictly after / before `time`, for jump-to-marker.
[[nodiscard]] const Marker* next_marker(const Project& p, double time) noexcept;
[[nodiscard]] const Marker* previous_marker(const Project& p, double time) noexcept;

// ------------------------------------------------------------- in and out --

/// Marks the in point, or clears it when given nothing.
///
/// An in past the current out clears the out rather than crossing it, so the
/// pair can never be inverted and nothing downstream has to check for it.
/// Negative times are clamped to the start of the sequence.
[[nodiscard]] Project set_in_point(Project p, std::optional<double> time);

/// The same the other way round: an out before the current in clears the in.
[[nodiscard]] Project set_out_point(Project p, std::optional<double> time);

[[nodiscard]] Project clear_marks(Project p);

/// Whether either mark is set. What greys out "export the marked range".
[[nodiscard]] bool has_marks(const Project& p) noexcept;

/// The span the marks describe: where it starts, and how long, in seconds.
///
/// A missing mark falls back to the end of the sequence it is missing from, so
/// an in alone means "from here to the end" and an out alone means "from the
/// start to here" — which is the whole point of marking one and not the other.
/// With neither, the whole timeline.
struct MarkedSpan {
  double start = 0.0;
  double duration = 0.0;

  friend bool operator==(const MarkedSpan&, const MarkedSpan&) = default;
};

[[nodiscard]] MarkedSpan marked_span(const Project& p) noexcept;

// --------------------------------------------------------------- factories --

/// A project with empty tracks and no media.
[[nodiscard]] Project empty_project(int video_tracks = 1, int audio_tracks = 2);

}  // namespace cutline::core
