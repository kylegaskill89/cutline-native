#pragma once

/// The editor data model: Project -> Tracks -> Clips, over a pool of Media.
///
/// This is the backbone of the NLE. Every other subsystem — compositor, audio
/// engine, exporter, UI — is a function of this model, so it stays pure: editing
/// operations take a Project and return a new one, never mutating in place.
///
/// Ported from the TypeScript reference (`src/core/project.ts`). Two deliberate
/// deviations, both enabled by the fresh file format:
///
///  * Optional-with-a-default fields become plain fields carrying that default.
///    TypeScript expressed `gain?: number` plus a `clipGain()` reader defaulting
///    to 1; here the field simply defaults to 1. Accessors survive only where
///    they apply real logic (clamping, guarding), not where they were spelling
///    out a default.
///  * `canvas_w`, `canvas_h`, and `fps` live on Project. The reference kept them
///    as module-level UI state and persisted them in a wrapper around the
///    project, which meant the model could not answer what it was authored
///    against. The specification always described them as project fields.

#include "cutline/core/keyframe.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cutline::core {

enum class TrackKind { Video, Audio };

enum class BlendMode {
  Normal,
  Add,
  Screen,
  Multiply,
  Overlay,
  Darken,
  Lighten,
  Difference,
};

enum class TransitionKind { Dissolve, DipBlack, Push, Slide };

enum class TextAlign { Left, Center, Right };

/// Which end of a clip an edit addresses — the head or the tail.
enum class ClipEdge { In, Out };

/// Properties that can be animated with keyframes over a clip's duration.
/// The enumerators double as indices into `Clip::keyframes`.
enum class AnimProp { X, Y, ScaleX, ScaleY, Rotation, Opacity };

inline constexpr std::size_t kAnimPropCount = 6;

inline constexpr std::array<AnimProp, kAnimPropCount> kAnimProps{
    AnimProp::X,        AnimProp::Y,        AnimProp::ScaleX,
    AnimProp::ScaleY,   AnimProp::Rotation, AnimProp::Opacity,
};

[[nodiscard]] constexpr std::size_t anim_prop_index(AnimProp prop) noexcept {
  return static_cast<std::size_t>(prop);
}

// ------------------------------------------------------------------ limits --

/// Allowed per-clip linear gain: 0 (silent) to 2 (about +6 dB).
inline constexpr double kMaxGain = 2.0;

/// Shortest clip a trim may produce, in seconds.
inline constexpr double kMinClip = 0.05;

inline constexpr double kMinSpeed = 0.05;
inline constexpr double kMaxSpeed = 100.0;

/// Default on-timeline length for a freshly placed still image, in seconds.
inline constexpr double kDefaultImageDuration = 5.0;

inline constexpr const char* kDefaultMatteColor = "#1a1a1a";

// ------------------------------------------------------------------- media --

/// A colour matte's linear-gradient fill, from `Media::color` to `color2`.
struct MatteGradient {
  std::string color2;
  double angle = 0.0;  ///< degrees; 0 = left to right, 90 = top to bottom

  friend bool operator==(const MatteGradient&, const MatteGradient&) = default;
};

/// Styling for a text/title clip. Sizes are in canvas pixels.
struct TextSpec {
  std::string content = "Text";
  double font_size = 96.0;
  std::string color = "#ffffff";
  std::string font_family = "system-ui, sans-serif";
  bool bold = true;
  bool italic = false;
  TextAlign align = TextAlign::Center;
  std::optional<std::string> background;    ///< unset = transparent
  std::optional<std::string> stroke_color;  ///< unset = no outline
  double stroke_width = 0.0;
  bool shadow = false;

  friend bool operator==(const TextSpec&, const TextSpec&) = default;
};

/// A source asset: a file, a still, or a generated source.
struct Media {
  std::string id;
  std::string path;  ///< absolute; empty for generated media
  std::string name;

  /// Source duration in seconds. For images, the default placement length.
  double duration = 0.0;
  bool has_video = false;
  int audio_stream_count = 0;

  /// A still image (PNG/JPG/...) or GIF: looped on export, no inherent duration.
  bool is_image = false;
  /// An animated source (GIF): loops its animation rather than holding a frame.
  bool is_animated = false;
  /// A generated title; `text` holds its content and styling.
  bool is_text = false;
  /// A generated colour matte; `color` is its fill, `gradient` optional.
  bool is_color = false;
  /// An adjustment layer: draws nothing itself, but its clip's effect stack
  /// applies to everything composited on the tracks below within its span.
  bool is_adjustment = false;

  std::optional<TextSpec> text;
  std::string color;
  std::optional<MatteGradient> gradient;

  std::optional<int> width;
  std::optional<int> height;
  std::optional<double> fps;

  friend bool operator==(const Media&, const Media&) = default;
};

// -------------------------------------------------------------------- clip --

/// Visual transform for a clip on a video track. Position is the clip centre as
/// a fraction of the output canvas, so (0.5, 0.5) is centred. `scale_x`/`scale_y`
/// are relative to the aspect-preserving fit-to-canvas size, where 1 fills that
/// axis, and may differ for non-proportional scaling. Rotation is in degrees,
/// clockwise. Expressing this in canvas fractions keeps it independent of the
/// export resolution.
struct Transform {
  double x = 0.5;
  double y = 0.5;
  double scale_x = 1.0;
  double scale_y = 1.0;
  double rotation = 0.0;

  friend bool operator==(const Transform&, const Transform&) = default;
};

/// A transition at a clip's out-edge, into the next abutting clip on its track.
struct Transition {
  TransitionKind kind = TransitionKind::Dissolve;
  double duration = 0.0;  ///< total seconds, centred on the cut

  friend bool operator==(const Transition&, const Transition&) = default;
};

/// One entry in a clip's visual effect stack. `type` keys the effect registry.
struct ClipEffect {
  std::string type;
  /// Disabled entries stay in the stack but contribute nothing.
  bool enabled = true;
  std::map<std::string, double> params;
  /// Colour parameters (hex), such as the chroma-key colour. Not keyframeable.
  std::map<std::string, std::string> colors;
  /// Per-parameter animation. An animated parameter's `params` entry is ignored
  /// in favour of its keyframes.
  std::map<std::string, std::vector<Keyframe>> keyframes;

  friend bool operator==(const ClipEffect&, const ClipEffect&) = default;
};

/// One entry in a clip's audio-effect stack. `type` keys the audio registry.
struct AudioClipEffect {
  std::string type;
  bool enabled = true;
  std::map<std::string, double> params;

  friend bool operator==(const AudioClipEffect&, const AudioClipEffect&) = default;
};

/// A placement of a media on a track over a span of timeline time.
struct Clip {
  std::string id;
  std::string media_id;
  TrackKind kind = TrackKind::Video;

  /// For audio clips: which source audio stream (the N in `0:a:N`).
  int audio_stream = 0;

  /// In/out points within the source media, in seconds.
  double source_in = 0.0;
  double source_out = 0.0;

  /// Position on the timeline, in seconds.
  double start = 0.0;

  /// Clips sharing a group move and cut together (Premiere-style A/V link).
  std::optional<std::string> group_id;

  /// Per-clip linear audio gain; 1 is unity.
  double gain = 1.0;
  /// Volume automation, clip-local. Non-empty overrides `gain`.
  std::vector<Keyframe> gain_keyframes;

  double opacity = 1.0;
  /// Fade durations in seconds — alpha for video, gain for audio.
  double fade_in = 0.0;
  double fade_out = 0.0;

  Transform transform;

  /// Playback rate; 2 is twice as fast, 0.5 is slow motion.
  double speed = 1.0;
  bool reverse = false;

  std::optional<Transition> transition_out;
  BlendMode blend = BlendMode::Normal;

  /// Disabled clips keep their place on the timeline but are not rendered.
  bool disabled = false;

  /// Per-property animation, indexed by `anim_prop_index`. Empty means static.
  std::array<std::vector<Keyframe>, kAnimPropCount> keyframes;

  /// Ordered effect stacks; order is apply order.
  std::vector<ClipEffect> effects;
  std::vector<AudioClipEffect> audio_effects;

  friend bool operator==(const Clip&, const Clip&) = default;
};

// ------------------------------------------------------------------- track --

/// An ordered lane of clips. Display order is the array order, and clips within
/// a track are kept sorted by start time.
struct Track {
  std::string id;
  TrackKind kind = TrackKind::Video;
  std::string label;  ///< empty falls back to V1/A1/... in the UI

  bool muted = false;   ///< audio: silent
  bool solo = false;    ///< audio: see `is_track_audible`
  bool locked = false;  ///< clips cannot be selected, moved, or trimmed
  bool hidden = false;  ///< video: excluded from the render (the "eye")

  /// Custom lane height in pixels, overriding the default for its kind.
  std::optional<double> height;

  std::vector<Clip> clips;

  friend bool operator==(const Track&, const Track&) = default;
};

/// A named point on the timeline ruler (a Premiere-style sequence marker).
struct Marker {
  std::string id;
  double time = 0.0;
  std::string label;
  std::string color;

  friend bool operator==(const Marker&, const Marker&) = default;
};

// ----------------------------------------------------------------- project --

/// The whole editing session.
struct Project {
  int canvas_w = 1920;
  int canvas_h = 1080;
  double fps = 30.0;

  std::vector<Media> media;
  /// Video tracks first (topmost), then audio. Note that video tracks composite
  /// bottom-first, so the render order is the reverse of the storage order.
  std::vector<Track> tracks;
  std::vector<Marker> markers;

  /// The span the sequence is marked out for — Premiere's in and out points.
  /// What export offers to render instead of the whole timeline.
  ///
  /// Either may be set without the other, so these are two optionals rather
  /// than one optional range. They can never cross: setting one past the other
  /// clears that other, which makes an inverted pair unrepresentable instead of
  /// something every reader has to remember to check for. See `marked_span`.
  std::optional<double> in_point;
  std::optional<double> out_point;

  friend bool operator==(const Project&, const Project&) = default;
};

}  // namespace cutline::core
