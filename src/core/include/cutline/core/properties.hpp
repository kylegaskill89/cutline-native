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

#include <cstddef>
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

/// Colours the named clips on the timeline. An empty colour puts them back to
/// the theme's. Takes a span because labelling is something done to a selection
/// — a shot is usually several clips, and colouring the picture and leaving the
/// sound is not what anybody means by it.
[[nodiscard]] Project set_clips_label(Project p, std::span<const std::string> clip_ids,
                                      std::string color);

/// Sets or clears the out-edge transition. A transition of zero duration clears
/// it, since a zero-length transition is just a cut.
[[nodiscard]] Project set_clip_transition(Project p, std::string_view clip_id,
                                          std::optional<Transition> transition);

/// Sets the clip's linear audio gain, clamped to the allowed range.
[[nodiscard]] Project set_clip_gain(Project p, std::string_view clip_id, double gain);

/// Sets where the clip sits across the stereo image: -1 hard left, 0 centred,
/// 1 hard right. Clamped to that range.
///
/// Balance rather than a constant-power pan. What the mixer has in front of it
/// is an interleaved stereo bus, and turning that bus is a matter of letting
/// one side through less — a constant-power law belongs to placing a *mono*
/// source in a field, which is a different control on a different signal.
/// Centre is therefore untouched rather than pulled down 3 dB, which is what
/// keeps every project made before there was a panner sounding the same.
[[nodiscard]] Project set_clip_pan(Project p, std::string_view clip_id, double pan);

/// Sets a fade duration, clamped so the two fades together never exceed the
/// clip's length.
[[nodiscard]] Project set_clip_fade(Project p, std::string_view clip_id, ClipEdge edge,
                                    double duration);

/// Sets playback speed, and optionally reverse, across the linked group so
/// video and its audio retime together. The source in and out are kept, so the
/// timeline length changes. Fades are re-clamped to the new duration.
[[nodiscard]] Project set_clip_speed(Project p, std::string_view clip_id, double speed,
                                     std::optional<bool> reverse = std::nullopt);

/// Retimes a whole selection, and optionally moves what follows out of the way.
///
/// `ripple` is Premiere's "shifting trailing clips", and it is what makes a
/// retime usable on a cut sequence: slowing a shot down without it either
/// overruns the next clip or leaves the gap it used to fill. Everything after
/// each retimed clip's old end moves by however much that clip grew or shrank,
/// on every track a sync lock holds together — the retime is an edit like any
/// other, and a pinned track sits still through it.
///
/// Without `ripple` this is `set_clip_speed` across a selection, which is the
/// right default for a clip standing on its own.
[[nodiscard]] Project set_clips_speed(Project p, std::span<const std::string> clip_ids,
                                      double speed, std::optional<bool> reverse, bool ripple);

/// Freezes the clip on one source frame, or lets it play again.
///
/// The *picture* only, and the whole selection at once — a shot is usually
/// several clips, and holding the frame while its sound keeps running is the
/// point rather than an oversight.
///
/// The source time is each clip's own, worked out from where it is asked to
/// hold, so a linked pair freezes on the same instant even when the two clips
/// are trimmed differently. `std::nullopt` releases it.
[[nodiscard]] Project set_clips_hold(Project p, std::span<const std::string> clip_ids,
                                     std::optional<double> at_timeline_time);

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

inline constexpr double kMinFps = 1.0;
inline constexpr double kMaxFps = 240.0;

/// Sets the sequence's frame rate, clamped to the allowed range.
///
/// Nothing about the clips changes. Every time in the model is in seconds, so
/// the rate is how finely that continuum is sampled — a cut stays where it was
/// put, and what moves is which frames land either side of it.
[[nodiscard]] Project set_fps(Project p, double fps);

/// Whether timecode is shown drop-frame where the rate allows it.
///
/// Changes nothing about the cut — every time in the model is in seconds, and
/// this is only how those seconds are written down. Stored on the project
/// rather than as a preference because a sequence handed to somebody else has
/// to number its frames the same way there.
[[nodiscard]] Project set_drop_frame(Project p, bool drop_frame);

/// Makes an **empty** sequence take its shape and rate from a media.
///
/// A sequence starts 1080p at 30, because it has to start as something. Drop
/// 4K60 footage into it and every second frame is simply never shown — which
/// looks exactly like a stuttering editor and is in fact an editor faithfully
/// playing a 30 fps sequence. The picture is scaled down too, so the decode is
/// paid for at full size and half of it thrown away.
///
/// Premiere asks about this with a dialogue. There is nothing to ask while the
/// sequence is still empty: nothing has been placed, nothing can be reframed,
/// and the only sensible answer is the footage's own format.
///
/// Does nothing once anything has been placed — after that, changing the shape
/// of the sequence reframes work somebody has already done — and nothing for a
/// media with no size or rate of its own, which is every title, matte and
/// adjustment layer. A generated source takes the canvas's shape rather than
/// giving it one.
[[nodiscard]] Project match_sequence_to(Project p, std::string_view media_id);

// ------------------------------------------------------------------ master --

/// Sets a track's own fader, clamped to the allowed range. Audio tracks only:
/// a video track has nothing to make louder.
[[nodiscard]] Project set_track_gain(Project p, std::string_view track_id, double gain);

/// Sets a track's panner, clamped to -1..1. Audio tracks only.
[[nodiscard]] Project set_track_pan(Project p, std::string_view track_id, double pan);

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
  std::optional<bool> targeted;
  std::optional<bool> sync_locked;
  std::optional<double> height;
};

[[nodiscard]] Project update_track(Project p, std::string_view track_id,
                                   const TrackPropsPatch& patch);

/// Sets the lane's height, or gives it back to whatever the interface's default
/// is when given nothing.
///
/// Its own function rather than a field of the patch, because clearing it is
/// half of what it is for and an optional inside an optional patch cannot say
/// "put this back" — only "leave it alone".
///
/// Stored as given. What is usable is a question about the interface drawing
/// it, and `ui::kMinTrackHeight` is where that is answered; a file written by a
/// build with different limits stays readable rather than being silently
/// rewritten on the way in.
[[nodiscard]] Project set_track_height(Project p, std::string_view track_id,
                                       std::optional<double> height);

/// Removes a track and every clip on it.
[[nodiscard]] Project remove_track(Project p, std::string_view track_id);

// ----------------------------------------------------------------- markers --

/// The marker nearest `time` within `tolerance`, if any.
[[nodiscard]] const Marker* marker_near(const Project& p, double time, double tolerance) noexcept;

[[nodiscard]] Project add_marker(Project p, double time, std::string label = {},
                                 std::string color = {});

/// Rewrites a marker in place: its name, its note, its colour and how long it
/// covers. Everything a marker carries except when it is, which is what moving
/// one means and has its own gesture.
///
/// One call rather than four setters, because the dialogue that edits a marker
/// edits all of it at once and four undo entries for one press would be four
/// presses to get back.
[[nodiscard]] Project set_marker(Project p, std::string_view marker_id, std::string label,
                                 std::string comment, std::string color, double duration);

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

/// The same pair, marked on a **source** rather than on the sequence, in source
/// time. Naming a media that is not in the project changes nothing.
///
/// Times are clamped into the source, which the sequence marks have no need of:
/// a sequence is as long as what is in it, a file is the length it is.
[[nodiscard]] Project set_source_in_point(Project p, std::string_view media_id,
                                          std::optional<double> time);
[[nodiscard]] Project set_source_out_point(Project p, std::string_view media_id,
                                           std::optional<double> time);
[[nodiscard]] Project clear_source_marks(Project p, std::string_view media_id);

[[nodiscard]] Project clear_marks(Project p);

// ------------------------------------------------------------------ proxies --

/// Attaches a proxy to a source, or takes one away with an empty path.
///
/// Only a path is kept, because a proxy is the same footage — the same length,
/// the same frames at the same times — so there is nothing else about it a clip
/// could need. Naming a media that is not in the project changes nothing.
[[nodiscard]] Project set_proxy_path(Project p, std::string_view media_id, std::string path);

/// Labels a pool entry, or takes its label off with an empty colour.
///
/// Only what is placed *after* this carries it. Repainting the clips already on
/// the timeline would undo every label somebody set on a clip by hand, and a
/// label is exactly the thing people set by hand.
[[nodiscard]] Project set_media_label(Project p, std::string_view media_id, std::string color);

/// Whether the preview reads from proxies where sources have them.
///
/// A property of the project rather than of the application, so a cut opened on
/// another machine is edited the way it was left rather than the way that
/// machine happens to be set. Export ignores it either way.
[[nodiscard]] Project set_use_proxies(Project p, bool use);

/// How many sources have a proxy attached. What a menu item says, and what
/// tells a switch whether it would do anything at all.
[[nodiscard]] std::size_t proxy_count(const Project& p) noexcept;

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

/// The marked span with a run-up before it and a run-out after it.
///
/// Premiere's preroll and postroll. Watching a cut means seeing what leads into
/// it and what it lands in, and a loop that starts exactly on the in point
/// gives neither — the first thing you see is the frame you were judging.
///
/// Both are clamped to the sequence, so a preroll longer than what precedes the
/// mark starts at zero rather than at a negative time, and neither is allowed
/// to turn a marked span into one that runs off the end. Zero for both gives
/// back exactly `marked_span`, which is what makes this safe to use everywhere
/// the marked span was used before anybody set one.
[[nodiscard]] MarkedSpan playback_span(const Project& p, double preroll,
                                       double postroll) noexcept;

// --------------------------------------------------------------- factories --

/// A project with empty tracks and no media.
[[nodiscard]] Project empty_project(int video_tracks = 1, int audio_tracks = 2);

}  // namespace cutline::core
