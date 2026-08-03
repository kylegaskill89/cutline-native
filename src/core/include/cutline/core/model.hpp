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
/// New enumerators go on the *end*: a saved file names its keyframe lists
/// rather than numbering them, but everything in memory indexes by position.
enum class AnimProp {
  X,
  Y,
  ScaleX,
  ScaleY,
  Rotation,
  Opacity,
  AnchorX,
  AnchorY,
  Pan,
  /// Playback rate, and the odd one out: every other property here is *the*
  /// value at a moment, and this one is a rate whose effect accumulates. Which
  /// source frame is shown at a moment is the integral of it — see
  /// `source_offset_at`. Premiere calls animating it Time Remapping.
  Speed,
};

inline constexpr std::size_t kAnimPropCount = 10;

inline constexpr std::array<AnimProp, kAnimPropCount> kAnimProps{
    AnimProp::X,       AnimProp::Y,       AnimProp::ScaleX, AnimProp::ScaleY,
    AnimProp::Rotation, AnimProp::Opacity, AnimProp::AnchorX, AnimProp::AnchorY,
    AnimProp::Pan,     AnimProp::Speed,
};

[[nodiscard]] constexpr std::size_t anim_prop_index(AnimProp prop) noexcept {
  return static_cast<std::size_t>(prop);
}

// ------------------------------------------------------------------ limits --

/// Allowed per-clip linear gain: 0 (silent) to 2 (about +6 dB).
inline constexpr double kMaxGain = 2.0;

/// Allowed master gain, the same range as a clip's. The master fader is not
/// given more reach than a clip fader on purpose: mixing is a plain sum, so a
/// master pushed further would only feed the limiter something it has to pull
/// straight back down, which sounds like the mix being squashed rather than
/// like it being loud.
inline constexpr double kMaxMasterGain = 2.0;

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

/// Visual transform for a clip on a video track. Position is where the layer's
/// *anchor point* lands, as a fraction of the output canvas, so (0.5, 0.5) is
/// the middle of the frame. `scale_x`/`scale_y` are relative to the
/// aspect-preserving fit-to-canvas size, where 1 fills that axis, and may differ
/// for non-proportional scaling. Rotation is in degrees, clockwise. Expressing
/// this in canvas fractions keeps it independent of the export resolution.
///
/// The anchor is the point of the layer that position places and that scale and
/// rotation happen about, as a fraction of the layer itself: (0.5, 0.5) is its
/// middle, (0, 0) its top left corner. A layer left at the default therefore
/// behaves exactly as it did before there was an anchor at all, which is what
/// keeps every project written without one reading the same.
struct Transform {
  double x = 0.5;
  double y = 0.5;
  double scale_x = 1.0;
  double scale_y = 1.0;
  double rotation = 0.0;
  double anchor_x = 0.5;
  double anchor_y = 0.5;

  /// How much to soften the layer vertically before it is scaled, from 0 to 1.
  ///
  /// Premiere's Anti-flicker Filter. A still or a graphic full of one-pixel
  /// detail shimmers when it is scaled down or moved slowly, because which
  /// source rows survive the resampling changes from frame to frame. Softening
  /// vertically first takes the shimmer out, at the cost of a little sharpness
  /// — which is why it is a slider rather than something always on.
  double anti_flicker = 0.0;

  friend bool operator==(const Transform&, const Transform&) = default;
};

/// A transition at a clip's out-edge, into the next abutting clip on its track.
struct Transition {
  TransitionKind kind = TransitionKind::Dissolve;
  double duration = 0.0;  ///< total seconds, centred on the cut

  friend bool operator==(const Transition&, const Transition&) = default;
};

/// The shape a mask cuts. `None` is an effect that applies everywhere, which is
/// what every effect did before there were masks.
enum class MaskShape { None, Ellipse, Rectangle, Path };

/// One corner of a free-drawn mask, in fractions of the layer and measured from
/// the mask's own centre — so the position and rotation above move and turn the
/// whole path, through exactly the transform the other two shapes go through.
///
/// Straight lines between them: a curve is a different feature, and one that
/// would want a handle either side of every point.
struct MaskPoint {
  double x = 0.0;
  double y = 0.0;

  friend bool operator==(const MaskPoint&, const MaskPoint&) = default;
};

/// The most points one path may carry.
///
/// A cap rather than no cap, because the shape has to reach the shader and the
/// shader reads it per pixel. Sixty-four is more corners than anybody draws by
/// hand around a face or a sign, and it keeps the buffer a fixed size.
inline constexpr std::size_t kMaxMaskPoints = 64;

/// Where one effect applies.
///
/// A mask belongs to a **single effect**, not to the clip and not to the stack.
/// That is what Premiere means by one, and it is the thing the old flat effect
/// struct could not express at all: once a stack has been folded into one set
/// of numbers there is nothing left for a mask to belong to.
///
/// Everything is a fraction of the layer rather than a pixel count, so a mask
/// keeps its place when the clip is scaled, and means the same thing whatever
/// resolution the project is exported at.
struct Mask {
  MaskShape shape = MaskShape::None;

  /// Centre, where (0.5, 0.5) is the middle of the layer.
  double x = 0.5;
  double y = 0.5;
  /// Half-extents: the ellipse's radii, or half the rectangle's sides.
  double width = 0.25;
  double height = 0.25;
  /// Clockwise, in degrees, about the mask's own centre.
  double rotation = 0.0;

  /// How far the edge is softened, as a fraction of the layer. Zero is a hard
  /// edge, which is almost never what anybody wants and is still the honest
  /// default: a feather nobody asked for is a mask that does not go where it
  /// was put.
  double feather = 0.0;

  /// How strongly the effect applies inside the shape. This is the effect's
  /// strength, not the layer's transparency — a mask at half opacity is half
  /// the blur, not a half-visible clip.
  double opacity = 1.0;

  /// The effect applies *outside* the shape instead. Premiere's Inverted.
  bool inverted = false;

  /// The corners, for `MaskShape::Path` and ignored otherwise. Closed
  /// implicitly: the last point joins the first, because an open mask is not a
  /// region and there would be nothing to fill.
  ///
  /// Not keyframed. The numbers above are, and a path that animated would want
  /// a keyframe per point with the points themselves able to come and go — a
  /// different feature wearing the same word.
  std::vector<MaskPoint> points;

  [[nodiscard]] bool active() const noexcept { return shape != MaskShape::None; }

  /// A path needs three points to enclose anything. Fewer is a shape nobody can
  /// see, which reads as a mask that has stopped working.
  [[nodiscard]] bool usable() const noexcept {
    if (shape != MaskShape::Path) return active();
    return points.size() >= 3;
  }

  friend bool operator==(const Mask&, const Mask&) = default;
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

  /// Where this effect applies. `MaskShape::None` is everywhere.
  Mask mask;

  friend bool operator==(const ClipEffect&, const ClipEffect&) = default;
};

/// One entry in a clip's audio-effect stack. `type` keys the audio registry.
struct AudioClipEffect {
  std::string type;
  bool enabled = true;
  std::map<std::string, double> params;
  /// Per-parameter animation, exactly as `ClipEffect` has it. An animated
  /// parameter's `params` entry is ignored in favour of its keyframes.
  ///
  /// The sound is retuned at block boundaries rather than per sample: a filter
  /// carries state that assumes its own coefficients, and moving them under it
  /// every sample is how an IIR filter is made to ring.
  std::map<std::string, std::vector<Keyframe>> keyframes;

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

  /// Where the clip sits across the stereo image: -1 hard left, 0 centred,
  /// 1 hard right. Automated through `AnimProp::Pan` rather than a list of its
  /// own, unlike gain — gain predates the keyframe array and keeps its own
  /// list for that reason alone.
  double pan = 0.0;

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

  /// What colour this clip is drawn, as a hex string. Empty is the theme's.
  ///
  /// A colour rather than a name, the way a marker's is, so the two things that
  /// carry a colour in this model carry it the same way and neither needs a
  /// table to be read. Which *names* are offered is a question for the layer
  /// that puts up the menu.
  ///
  /// It changes nothing about what is rendered. A label is for the person
  /// reading the timeline — this is the interview, that is the b-roll — and a
  /// colour that also did something would be a colour nobody dared use.
  std::string label_color;

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

  /// Whether a keyboard edit lands here.
  ///
  /// Premiere's track targeting, and the thing insert and overwrite are aimed
  /// with: they take a source and put it at the playhead, and *where* is not a
  /// question the keyboard can answer any other way. Selecting a clip says what
  /// to edit; targeting a track says where the next one goes.
  bool targeted = false;

  /// Whether this track moves when something *else* is rippled.
  ///
  /// Premiere's sync lock, and on by default there and here: the ordinary case
  /// is that an insert opens the whole sequence up and nothing goes out of step
  /// with anything. Turning it off is what pins a track down — a music bed, a
  /// title card at a fixed time, a bar of tone at the head — so an edit
  /// elsewhere leaves it exactly where it is.
  ///
  /// Different from `locked`, which stops the track being *edited* at all. A
  /// track can be freely editable and pinned, or locked and still rippled by
  /// its neighbours, and conflating the two is how a lock nobody set appears to
  /// move things.
  bool sync_locked = true;

  /// The track's own fader and panner, applied to everything on it after each
  /// clip's own gain and pan.
  ///
  /// A mix has two levels for a reason: a clip's gain is what that take needed,
  /// and a track's is what the whole stem needs against the others. Riding one
  /// to fix the other is how a mix stops being reversible.
  double gain = 1.0;
  double pan = 0.0;

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

  /// Linear gain applied to the whole mix, after the tracks are summed and
  /// before the limiter. Ahead of the limiter so that turning the master down
  /// gets a project *out* of limiting rather than quietly leaving it in.
  double master_gain = 1.0;

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
