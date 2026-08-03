#pragma once

/// The timeline.
///
/// What it draws is passed in as plain data rather than read from the project
/// model. That keeps the whole of `cutline::ui` free of the editor's model —
/// the widget can be laid out, hit tested and painted from a handful of
/// structs in a test, with no project, no media and no decoding — and it means
/// the same view can later show something that is not a project at all, like a
/// nested sequence or a range being previewed.
///
/// Building that data from a project belongs to the editor, above both.

#include "cutline/ui/layout.hpp"
#include "cutline/ui/timescale.hpp"
#include "cutline/ui/widget.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cutline::ui {

/// A transition at a block's out-edge, straddling the cut into the next one.
///
/// Only the length and a name, because that is all a timeline can usefully say
/// about one — which kind it is shows in the picture, not in a few pixels of
/// track. Zero duration means there is none.
struct BlockTransition {
  double duration = 0.0;
  std::string label;

  friend bool operator==(const BlockTransition&, const BlockTransition&) = default;
};

/// The min/max envelope of a source's audio, in buckets of equal time.
///
/// Both bounds rather than an average, which is what makes a transient visible
/// instead of averaging it away. Values are the sample range, so -1 to 1.
///
/// This describes a *source*, not a clip of one: the envelope of a file does
/// not change when somebody trims a clip that shows part of it. Where a clip
/// sits in it is on the block, which is what lets one envelope serve a source
/// used a dozen times over.
struct Waveform {
  double buckets_per_second = 100.0;
  std::vector<float> minimum;
  std::vector<float> maximum;

  [[nodiscard]] bool empty() const noexcept { return minimum.empty(); }
  [[nodiscard]] std::size_t size() const noexcept {
    return std::min(minimum.size(), maximum.size());
  }
  /// How much of the source this covers, in seconds.
  [[nodiscard]] double duration() const noexcept {
    return buckets_per_second > 0.0 ? static_cast<double>(size()) / buckets_per_second : 0.0;
  }

  friend bool operator==(const Waveform&, const Waveform&) = default;
};

/// One frame of a source, scaled small enough to draw on a clip.
///
/// Tightly packed 8-bit RGBA, owned rather than borrowed: a filmstrip outlives
/// the draw that shows it, unlike the decoded frame the monitor displays.
struct FilmFrame {
  double t = 0.0;  ///< source seconds this was taken from
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba;

  [[nodiscard]] bool empty() const noexcept {
    return width <= 0 || height <= 0 || rgba.empty();
  }
  [[nodiscard]] double aspect() const noexcept {
    return height > 0 ? static_cast<double>(width) / height : 0.0;
  }

  friend bool operator==(const FilmFrame&, const FilmFrame&) = default;
};

/// Frames sampled across a source, in time order.
///
/// Like `Waveform`, this describes a *source* rather than a clip of one, so
/// trimming costs nothing and a source used a dozen times is decoded once. The
/// consequence is that a short clip of a long source may show the same frame in
/// every tile — the strip says what the footage is, not what each instant of it
/// looks like, and sampling densely enough for the latter would mean holding a
/// decoded film in memory.
struct Filmstrip {
  std::vector<FilmFrame> frames;

  [[nodiscard]] bool empty() const noexcept { return frames.empty(); }

  /// The frame nearest a source time, or null when there are none.
  [[nodiscard]] const FilmFrame* nearest(double t) const noexcept;

  friend bool operator==(const Filmstrip&, const Filmstrip&) = default;
};

/// One point on a clip's volume rubber band.
///
/// A gain keyframe, in the terms the timeline works in: seconds from the
/// block's own start, and a linear multiplier. The band converts to decibels
/// only to decide how high up the clip to draw it.
struct GainPoint {
  double t = 0.0;
  double v = 1.0;

  friend bool operator==(const GainPoint&, const GainPoint&) = default;
};

/// A clip's volume, as a line across its block.
///
/// Flat at `level` until there are points, at which point `level` stops being
/// what plays — automation overrides a constant gain — and the line runs
/// through the points instead. Both are carried because that is what the model
/// holds, and because clearing the automation has to reveal something.
struct GainBand {
  double level = 1.0;
  std::vector<GainPoint> points;

  friend bool operator==(const GainBand&, const GainBand&) = default;
};

/// What a band is worth at a clip-local time, points and all.
///
/// The same clamp-and-interpolate the core evaluates a keyframe list with, in
/// the one shape the timeline needs it: linearly, because that is what the line
/// drawn between two points says is happening. A free function because it needs
/// nothing but the band.
[[nodiscard]] double gain_at(const GainBand& band, double t) noexcept;

/// The band's floor, in decibels. Below this a clip is drawn, and set, silent.
///
/// A band has to be in decibels or it is unusable: gain is stored as a linear
/// multiplier, and on a linear scale every trim anyone actually makes — the
/// couple of decibels either side of unity that is most of mixing — lands
/// within a few pixels of the top of a forty-pixel clip, while the bottom half
/// of the band spans -6 dB to silence and is worth nothing.
///
/// The floor is where the useful range stops rather than where audio does.
/// Thirty-six decibels of travel holds both a fine trim and ducking a bed under
/// a voice, which is what the band is for; anything quieter is a fade or a mute,
/// and both have their own control.
inline constexpr double kGainFloorDb = -36.0;

/// Where a gain sits in the band: 0 at the foot, 1 at the top.
///
/// `maximum` is what the top means, so the band reaches exactly the loudest
/// gain the model allows rather than stopping just short of it at a round
/// number of decibels.
[[nodiscard]] double gain_to_band(double gain, double maximum) noexcept;

/// And back. The foot of the band is silence rather than the floor: a band that
/// bottomed out at -36 dB could not silence a clip by dragging, and one that
/// ran to true -infinity has no bottom to draw.
[[nodiscard]] double band_to_gain(double fraction, double maximum) noexcept;

/// The same scale expressed as a fader reading rather than a fraction, for the
/// controls that show a number: the inspector's Volume row and the master
/// fader. `gain_to_fader_db` floors at `kGainFloorDb` and `fader_db_to_gain`
/// treats that floor as silence, so all three controls agree by construction
/// about what the bottom of the range means.
///
/// Three views of one scale is the reason these live here rather than beside
/// any one of them. The first version had the inspector on a percentage of its
/// own, and a clip pulled down on the band read as pinned to the left of a
/// slider that claimed to show the same value.
[[nodiscard]] double gain_to_fader_db(double gain) noexcept;
[[nodiscard]] double fader_db_to_gain(double db, double maximum) noexcept;

/// One clip, as far as drawing is concerned.
struct TimelineBlock {
  /// Opaque to the timeline, which never looks inside it. Whoever built the
  /// model decides what it means — for the editor it is a clip id, which is
  /// how a drag finds its way back to the project without the timeline
  /// knowing a project exists.
  std::string id;

  /// Which clips this one is tied to, opaque like `id`. Empty means none.
  ///
  /// The timeline never looks inside it; what it is for is knowing that these
  /// blocks move together, so a trim on a picture can show the sound being
  /// trimmed with it. Every edit that crosses a link is the model's business —
  /// the view only has to *draw* the answer before the model gives it, which is
  /// what a drag is.
  std::string group;

  double start = 0.0;
  double end = 0.0;
  std::string label;
  bool selected = false;
  /// Kept on the timeline but not rendered. Drawn faded, because a clip that
  /// is not playing and looks exactly like one that is, is a bug report.
  bool disabled = false;
  /// Whether anything is in this clip's effect stack.
  ///
  /// Drawn as a badge, which is Premiere's. Otherwise a graded shot and an
  /// untouched one are the same rectangle, and the only way to find the one
  /// carrying a look is to click every clip in turn and read the panel.
  bool has_effects = false;

  /// Where this clip is animated, in seconds from its own start.
  ///
  /// Drawn as marks along the block. Keyframes are otherwise visible only in
  /// the inspector, one parameter at a time, which makes an animation
  /// something to be remembered rather than something that can be seen.
  std::vector<double> keyframes;

  BlockTransition transition;

  /// The volume rubber band, on audio clips only. A video clip has no gain to
  /// draw, and an empty optional is what says so — rather than a band at unity
  /// that would look like a control and answer no drag.
  std::optional<GainBand> gain;

  /// The source's audio envelope, if it has been computed yet.
  ///
  /// Shared rather than held: the model is rebuilt after every gesture, and a
  /// ten-minute source's envelope is half a megabyte. Copying a pointer per
  /// rebuild is the difference between dragging a clip being free and being
  /// something you can feel.
  ///
  /// Null while it is still being decoded, which is what an audio clip looks
  /// like for the first moment after it is imported.
  std::shared_ptr<const Waveform> waveform;

  /// The source's filmstrip, if it has been extracted yet. Shared for the same
  /// reason the envelope is, and more so: these are pixels.
  std::shared_ptr<const Filmstrip> filmstrip;

  /// How long the clip fades up at its head and down at its tail, in seconds.
  ///
  /// Alpha on a video clip and gain on an audio one, which is the model's rule
  /// and not something the drawing has to know: either way it is the clip
  /// arriving or leaving over that long.
  double fade_in = 0.0;
  double fade_out = 0.0;

  /// Where this block starts in its source, and how fast it runs through it.
  ///
  /// The envelope is the whole source, so drawing needs to know which part of
  /// it this block shows. These are also what make a retimed or reversed clip
  /// draw the footage it actually plays rather than the footage at the same
  /// offset — which is the sort of thing that looks right until the one clip
  /// somebody slowed down.
  double source_in = 0.0;
  double speed = 1.0;
  bool reverse = false;

  [[nodiscard]] double duration() const noexcept { return end - start; }

  friend bool operator==(const TimelineBlock&, const TimelineBlock&) = default;
};

/// The source time a block reads from at `local_t` seconds along itself.
///
/// The same mapping the core does for a clip, over the three numbers the
/// timeline carries rather than over a project. Reverse runs from the far end,
/// so a reversed clip's first pixel is its source's last.
[[nodiscard]] double source_time_of(const TimelineBlock& block, double local_t) noexcept;

/// The switches in a track's header, as the project holds them.
///
/// Separate from `TimelineTrack::muted`, which is whether the track is
/// contributing *now* — and those are not the same question. A track silenced
/// because something else is soloed is not muted, and lighting its M would say
/// it was, leaving somebody pressing a button that is already off.
struct TrackSwitches {
  bool mute = false;
  bool solo = false;
  bool lock = false;
  bool hide = false;
  /// Whether a keyboard edit lands on this track. Premiere's targeting, and
  /// the only thing that says *where* an insert or an overwrite goes.
  bool target = false;

  friend bool operator==(const TrackSwitches&, const TrackSwitches&) = default;
};

struct TimelineTrack {
  /// Opaque, like a block's.
  std::string id;
  std::string name;
  /// Audio tracks are shorter, because the theme says so.
  bool audio = false;
  /// A height somebody dragged this lane to, overriding the theme's. Empty
  /// means the theme decides, which is what every track starts as.
  ///
  /// Per track rather than per kind because that is the whole point of the
  /// gesture: the one lane being worked on gets the room, and the rest stay out
  /// of the way. Premiere's tall video track over a row of short audio ones is
  /// the arrangement anybody grading or keyframing ends up in.
  std::optional<double> height;
  /// Whether the track contributes nothing right now, however that came about.
  /// Drawn as a disabled header. See `TrackSwitches` for the difference.
  bool muted = false;
  TrackSwitches switches;
  std::vector<TimelineBlock> blocks;

  friend bool operator==(const TimelineTrack&, const TimelineTrack&) = default;
};

/// One switch in a track header.
///
/// Audio tracks show mute, solo and lock; video tracks show hide and lock. A
/// solo on a video track would mean nothing, and a mute on one even less.
enum class TrackControl { Target, Mute, Solo, Lock, Hide };

/// The letter a switch is drawn with.
///
/// Letters rather than pictograms, and deliberately: a padlock and an eye both
/// need arcs the painter has no other use for, and at twelve pixels a drawn
/// padlock is a grey smudge. M and S are what every mixer in the world uses
/// anyway, and a letter that *is* the word needs no learning.
[[nodiscard]] std::string_view to_string(TrackControl control) noexcept;

struct TrackControlRef {
  std::size_t track = 0;
  TrackControl control = TrackControl::Mute;

  friend bool operator==(const TrackControlRef&, const TrackControlRef&) = default;
};

/// A named point on the ruler.
///
/// Carries its own colour rather than taking the theme's, because that is what
/// a marker's colour is *for*: somebody has said this one means something
/// different from that one. Empty falls back to the theme.
struct TimelineMarker {
  double time = 0.0;
  std::string label;
  std::string color;

  friend bool operator==(const TimelineMarker&, const TimelineMarker&) = default;
};

struct TimelineModel {
  std::vector<TimelineTrack> tracks;
  std::vector<TimelineMarker> markers;
  /// How long the project is, which is what bounds scrolling. Zero means it is
  /// worked out from the blocks.
  double duration = 0.0;
  double fps = 30.0;

  /// What the top of a volume band means. The model's ceiling, carried here so
  /// the timeline does not have to know the model to draw one.
  double max_gain = 2.0;

  /// The marked span, drawn as a bar along the ruler. Either may be set alone,
  /// and a missing one reaches to that end of the sequence.
  std::optional<double> in_point;
  std::optional<double> out_point;

  /// The end of the last block, or `duration` where that is longer.
  [[nodiscard]] double content_duration() const noexcept;

  friend bool operator==(const TimelineModel&, const TimelineModel&) = default;
};

/// Which block a point landed on.
struct BlockRef {
  std::size_t track = 0;
  std::size_t block = 0;

  friend bool operator==(const BlockRef&, const BlockRef&) = default;
};

/// Which point of which block's volume band a press landed on.
struct GainPointRef {
  BlockRef block;
  std::size_t point = 0;

  friend bool operator==(const GainPointRef&, const GainPointRef&) = default;
};

/// Where something dragged from elsewhere would land.
struct DropPoint {
  std::size_t track = 0;
  double time = 0.0;

  friend bool operator==(const DropPoint&, const DropPoint&) = default;
};

/// What a press on a clip means. Premiere's tool palette.
///
/// The tool is state rather than a held modifier, because that is what makes
/// these edits reachable at all: slip and slide become two-handed gestures if a
/// key has to be held, and a razor that needs one cannot cut a dozen clips in a
/// row. All four are a drag over the body of a clip, so without a tool to say
/// which is meant they would every one of them be the same drag.
enum class Tool {
  /// Move and trim. What every other tool is a variation on.
  Selection,
  /// A click cuts. The one tool whose gesture is not a drag.
  Razor,
  /// An edge trims and everything after it follows, so no gap opens.
  Ripple,
  /// A join moves: one clip's out-edge and its neighbour's in-edge together,
  /// leaving the sequence exactly as long as it was.
  Roll,
  /// An edge changes the clip's speed instead of its source.
  RateStretch,
  /// The body moves the source without moving the clip.
  Slip,
  /// The body moves the clip, taking the length out of its neighbours.
  Slide,
};

[[nodiscard]] std::string_view to_string(Tool tool) noexcept;

/// What a gesture is doing.
enum class DragMode {
  None,
  /// Moving the playhead.
  Scrub,
  /// Sliding a whole clip along its track.
  Move,
  /// Pulling one edge of a clip, leaving the other where it is.
  TrimStart,
  TrimEnd,
  /// Pulling an edge to change the speed. The source in and out stay put, so
  /// the clip can be dragged longer than the footage it came from.
  RateStart,
  RateEnd,
  /// Trimming an edge with everything downstream following it, so the sequence
  /// grows or shrinks by what was trimmed and nothing goes out of sync.
  ///
  /// The head case is the one worth stating: the clip keeps its *place* and
  /// loses length from the front, because the ripple closes the gap the trim
  /// would have opened in front of it.
  RippleStart,
  RippleEnd,
  /// Moving a join. Two edges travel together and everything else stays put.
  RollStart,
  RollEnd,
  /// Moving which part of the source the clip shows. The clip itself does not
  /// move, which makes this the one mode with nothing to watch on the timeline:
  /// the change is inside the block, and staying put is what it means.
  Slip,
  /// Moving the clip into its neighbours: the one before grows, the one after
  /// shrinks, and the sequence keeps its length.
  Slide,
  /// A cut. Not a drag at all — it happens on the press and there is nothing to
  /// follow — but it is reported the same way, because "the timeline was used"
  /// is better as one thing for a caller to handle than as two.
  Razor,
  /// Pulling a fade longer or shorter from the handle at a clip's top corner.
  ///
  /// The handle sits at the *end* of the fade rather than at the corner, so it
  /// slides along the top edge as the fade grows — which is what makes the
  /// gesture readable: the thing being dragged is where the fade finishes.
  FadeIn,
  FadeOut,
  /// Dragging the line under a track header, which makes that lane taller or
  /// shorter. In the header column only: over the tracks the same downward drag
  /// is a marquee, and there is no room for both.
  TrackHeight,
  /// Dragging the bar along the bottom, which moves the view through time.
  ScrollTime,
  /// Dragging one end of that bar, which zooms: the far end stays where it is
  /// and the span between them becomes what is on screen. Premiere's, and the
  /// reason its scrollbar is the zoom control as well as the scroll control —
  /// the two questions are one question, and answering them with one gesture
  /// is what makes navigating a long sequence quick.
  ZoomStart,
  ZoomEnd,
  /// Dragging the bar down the right, which moves the view through the tracks.
  ScrollTracks,
  /// Sweeping a rectangle over empty track to gather up everything it touches.
  ///
  /// It starts on a press that hit no clip, which was previously the gesture
  /// for "deselect and do nothing" — so nothing has been taken away to make room
  /// for it, and a press with no drag still clears the selection.
  Marquee,
  /// Moving a clip's whole volume band, which sets its constant gain. Only
  /// while it has no automation — with points on it there is no one level to
  /// set, and `GainSegment` is what a drag means instead.
  GainLevel,
  /// Moving one stretch of an automated band up or down, carrying the points at
  /// each end of it with it.
  ///
  /// This is how a level is ridden once there is automation on the clip: grab
  /// the line between two points and move it. Offering only the points meant
  /// every adjustment was two drags that had to match, which is a fiddly way to
  /// say "this bit is too loud".
  ///
  /// The two ends move by the same number of decibels rather than to the same
  /// value, so a ramp stays the ramp it was and only its level changes.
  ///
  /// **The clip's own edges count as points.** Without that, dragging the
  /// stretch between the only two points on a clip moved the whole band, since
  /// outside the outermost points the line is flat *because* those points define
  /// it. Anchoring the edges is what makes two points enough to duck a region:
  /// see `ensure_gain_anchors`.
  GainSegment,
  /// Moving one point of the automation, in time as well as level. Adding a
  /// point is this mode too — it is created under the pointer on the press and
  /// dragged from there, so nothing has to distinguish a new point from an old
  /// one afterwards.
  GainPointDrag,
  /// Taking a point off again. Like the razor it happens on the press and has
  /// no drag to follow, and like the markers it is the *same* gesture that adds
  /// one: alt where there is no point puts one there, alt where there is one
  /// takes it away. Nothing then exists only to undo something else.
  GainPointRemove,
};

/// How short and how tall a lane may be dragged.
///
/// The floor is where a clip stops being a clip: below about a dozen pixels
/// there is no room for a label, a waveform or a fade handle, and a lane
/// dragged to nothing is one that cannot be dragged back because there is
/// nothing left to grab. The ceiling stops one lane from swallowing the panel.
inline constexpr double kMinTrackHeight = 18.0;
inline constexpr double kMaxTrackHeight = 400.0;

/// Which edge a mode pulls, whichever tool is pulling it. Trim, ripple, roll
/// and rate stretch differ in what they do to the clip, not in which end is
/// being held.
[[nodiscard]] bool pulls_start(DragMode mode) noexcept;
[[nodiscard]] bool pulls_end(DragMode mode) noexcept;

/// One finished gesture, as the timeline saw it.
///
/// A struct rather than three arguments because the modes do not all have the
/// same thing to report, and a signature that grew a parameter per mode would
/// end up with every caller passing values it does not use.
struct TimelineEdit {
  BlockRef block;
  DragMode mode = DragMode::None;

  /// The block as it ended up. What `Move`, the trims, the rate stretches and
  /// `Slide` are asking for. Left as it was for `Slip` and `Razor`, neither of
  /// which changes where the clip is.
  TimelineBlock result;

  /// `Move` only: how many lanes of its own kind the clip travelled, positive
  /// downwards in the order the tracks are stored.
  ///
  /// Its own number rather than something read off `result`, because a block
  /// carries no idea of which track it is on — `block` says where it *ended*,
  /// and the project needs to be told the difference.
  int lanes = 0;

  /// How far the gesture travelled, in seconds. `Slip` has nothing else to say:
  /// what moved is the source, which the timeline does not model.
  double delta = 0.0;

  /// Where a gesture landed, in timeline seconds. `Razor` puts the cut here;
  /// the ripples and rolls put the edge they ended on here, because `result`
  /// shows the clip after the ripple closed up behind it and no longer says
  /// where the edge was dragged to.
  double at = 0.0;
  /// `Razor` only: cut every track at `at` rather than only the clip clicked.
  bool all_tracks = false;

  /// `GainLevel` only: the level the band was dragged to, as a linear
  /// multiplier.
  double gain = 1.0;

  /// `FadeIn` and `FadeOut` only: how long the fade ended up, in seconds.
  double fade = 0.0;

  /// `GainPointDrag` only: where the point started and where it ended up.
  ///
  /// Both, because a point moves in time as well as in level, and moving one is
  /// not the upsert that setting one is: the keyframe at the old time has to go
  /// or the drag leaves a copy behind. A point that was just created reports the
  /// same value in each, which makes adding one and moving one the same edit.
  GainPoint gain_from;
  GainPoint gain_to;

  /// `GainSegment` only: the points whose level changed, at the times they
  /// already had.
  ///
  /// A list because a stretch in the middle of a band moves the point at each
  /// end of it, and only times that do not change — which is what lets these be
  /// applied as ordinary upserts, keeping each keyframe's interpolation instead
  /// of rebuilding it.
  std::vector<GainPoint> gain_moved;

  friend bool operator==(const TimelineEdit&, const TimelineEdit&) = default;
};

/// The times a dragged edge should stick to: the start, the playhead, and the
/// edges of every other clip.
///
/// `exclude` is the clip being dragged, which must not snap to itself — its own
/// edges follow the pointer, so leaving them in pins the drag in place.
[[nodiscard]] std::vector<double> snap_points(const TimelineModel& model, double playhead,
                                              std::optional<BlockRef> exclude);

/// The nearest point within `tolerance`, or nothing. Ties go to the earlier
/// one, so a clip dropped exactly between two edges lands somewhere
/// predictable rather than somewhere that depends on iteration order.
[[nodiscard]] std::optional<double> nearest_snap(std::span<const double> points, double time,
                                                 double tolerance);

class TimelineView : public Widget {
 public:
  TimelineView();

  [[nodiscard]] const TimelineModel& model() const noexcept { return model_; }
  void set_model(TimelineModel model);

  [[nodiscard]] const TimeScale& scale() const noexcept { return scale_; }
  void set_scale(const TimeScale& scale);
  /// Zooms so the whole project fills the time area.
  void zoom_to_fit();

  /// One step in or out, keeping the playhead where it is on screen.
  ///
  /// About the playhead rather than the middle of the view, because the
  /// playhead is what somebody zooming in is looking at — the wheel zooms about
  /// the pointer for the same reason, and a key has no pointer to use.
  /// A playhead off screen falls back to the middle, which is the only honest
  /// answer when the thing to keep still is not visible.
  void zoom_about_playhead(bool closer);

  [[nodiscard]] double playhead() const noexcept { return playhead_; }
  /// Snapped to a frame, and never negative.
  void set_playhead(double seconds);

  /// How much of the view the playhead may cross before it is followed, as a
  /// fraction of the visible span either side. A tenth in from each edge, so
  /// there is somewhere to look ahead to rather than a playhead pinned to the
  /// middle of the screen.
  static constexpr double kFollowMargin = 0.1;

  /// Scrolls, if the playhead has left the comfortable part of the view.
  ///
  /// Reports whether it moved, so a caller can skip a repaint it does not need
  /// — which is most frames of a playback, since the playhead spends nearly all
  /// of its time somewhere already visible.
  ///
  /// Pages rather than creeps: once it has gone past the trailing margin the
  /// view jumps so the playhead sits at the *leading* one, giving a screen's
  /// worth of what is coming. Scrolling by the frame instead would mean the
  /// picture never stops moving, which is far harder to read than a jump.
  ///
  /// Not called from `set_playhead`. Scrubbing deliberately does not scroll —
  /// the pointer is already where the answer is, and moving the view out from
  /// under a drag makes it impossible to aim.
  bool follow_playhead();

  /// Called while the playhead is dragged, and once when it is clicked.
  void set_on_scrub(std::function<void(double)> on_scrub) { on_scrub_ = std::move(on_scrub); }
  /// Called when the selection changes. Empty means everything was deselected.
  void set_on_select(std::function<void(std::span<const BlockRef>)> on_select) {
    on_select_ = std::move(on_select);
  }

  /// Every selected block, in track then block order.
  [[nodiscard]] std::vector<BlockRef> selection() const;
  /// The first of them, which is all a caller wanting "the" selected clip needs
  /// and what most of them want.
  [[nodiscard]] std::optional<BlockRef> first_selected() const;

  void select(std::optional<BlockRef> block);
  void select(std::span<const BlockRef> blocks);

  /// Called once, at the end of a gesture. Not on every mouse move: the model
  /// is already updated live so the drag can be seen, and an edit that fired
  /// continuously would put a hundred entries in the undo stack for one drag.
  ///
  /// The mode is passed rather than inferred from what changed, because moving
  /// a clip and trimming both its edges by the same amount are different edits
  /// that leave the same numbers behind — and because a slip changes no numbers
  /// the timeline can see at all.
  ///
  /// A razor cut arrives here too, on the press, since it has no end to wait
  /// for.
  void set_on_edit(std::function<void(const TimelineEdit&)> on_edit) {
    on_edit_ = std::move(on_edit);
  }

  /// Which tool a press over a clip is using.
  [[nodiscard]] Tool tool() const noexcept { return tool_; }
  void set_tool(Tool tool) noexcept { tool_ = tool; }

  /// Called when a header switch is pressed. The view flips it in its own model
  /// so the press is visible at once; whoever handles this writes it down.
  void set_on_track_toggle(std::function<void(TrackControlRef)> on_toggle) {
    on_track_toggle_ = std::move(on_toggle);
  }

  /// Called on a right-click, with where it landed.
  ///
  /// The view selects whatever was under it first, so a menu built from the
  /// selection is built from what was clicked — and a right-click on a clip
  /// already in a multiple selection leaves that selection alone, because a
  /// menu about six clips is what right-clicking one of six means.
  ///
  /// Reported rather than handled: the timeline knows blocks and the menu is
  /// about clips, which is a word from a layer above this one.
  void set_on_context_menu(std::function<void(double, double)> on_context_menu) {
    on_context_menu_ = std::move(on_context_menu);
  }

  /// Called when a lane has been dragged to a new height, once, at the end.
  ///
  /// The view resizes its own model as the drag goes, so the lane follows the
  /// pointer; this is what writes it down. Nothing means the lane was put back
  /// to the theme's height, which is what a double-click on the line does.
  void set_on_track_resize(std::function<void(std::size_t, std::optional<double>)> on_resize) {
    on_track_resize_ = std::move(on_resize);
  }

  /// The strip along the bottom of a header that resizes the lane, or empty
  /// when the header is not on screen.
  [[nodiscard]] Rect resize_rect(std::size_t track) const;

  /// Called when a header is double-clicked anywhere but on a switch, which is
  /// how a track gets renamed.
  ///
  /// Reported rather than handled: renaming needs somewhere to type, and the
  /// timeline draws its headers rather than building widgets in them. Whoever
  /// handles this owns the field.
  void set_on_track_rename(std::function<void(std::size_t)> on_rename) {
    on_track_rename_ = std::move(on_rename);
  }

  /// Which track's header a point is in, if any. `header_rect` says where that
  /// header is, which is what a rename field hangs under.
  [[nodiscard]] std::optional<std::size_t> header_at(double x, double y) const;

  /// Whether a track shows a given switch at all.
  [[nodiscard]] bool has_control(std::size_t track, TrackControl control) const;
  /// Where a switch is drawn. Empty when the track does not have one, or when
  /// the header is too small for the row of them.
  [[nodiscard]] Rect control_rect(std::size_t track, TrackControl control) const;
  /// The switch under a point, if any.
  [[nodiscard]] std::optional<TrackControlRef> control_at(double x, double y) const;

  [[nodiscard]] bool snapping() const noexcept { return snapping_; }
  void set_snapping(bool snapping) noexcept { snapping_ = snapping; }

  [[nodiscard]] DragMode drag_mode() const noexcept { return mode_; }
  [[nodiscard]] std::optional<BlockRef> dragging() const noexcept { return drag_; }

  /// What a gesture starting at this point would do, under the current tool.
  ///
  /// With the selection tool the outer edges of a clip trim it and the middle
  /// moves it, which is how every editor behaves and what makes a trim
  /// reachable without a modifier. The other tools each mean one thing
  /// everywhere on a clip, apart from rate stretch, which takes whichever end
  /// is nearer.
  [[nodiscard]] DragMode zone_at(double x, double y) const;

  // -------------------------------------------------------------- geometry --
  //
  // Exposed because it is the whole of what the timeline is, and every one of
  // these is something to assert rather than measure off a screenshot.

  /// The column of track headers down the left.
  [[nodiscard]] Rect header_area() const;
  /// Everything to the right of it, where time is drawn.
  [[nodiscard]] Rect time_area() const;
  /// The strip of ticks along the top of the time area.
  [[nodiscard]] Rect ruler_area() const;
  /// Below the ruler, where the tracks are. Above the scrollbar, which is why
  /// this is not simply everything under the ruler.
  [[nodiscard]] Rect tracks_area() const;

  /// The bar along the bottom, and the part of it that stands for what is on
  /// screen. Empty when the whole sequence already fits, because a scrollbar
  /// that fills its own track says nothing and costs a row of pixels.
  [[nodiscard]] Rect scroll_area() const;
  [[nodiscard]] Rect scroll_thumb() const;
  /// The two grips at the ends of that thumb, which zoom rather than scroll.
  /// Empty when the thumb is too short to hold one.
  [[nodiscard]] Rect zoom_grip(bool at_end) const;

  /// The bar down the right of the tracks, and its thumb. Empty when the tracks
  /// all fit.
  [[nodiscard]] Rect track_scroll_area() const;
  [[nodiscard]] Rect track_scroll_thumb() const;

  /// The view through the sequence, in seconds: how long it is, how much of it
  /// is on screen, and where that window starts.
  ///
  /// The same shape as the vertical one, so the same tested arithmetic answers
  /// where a thumb goes and what dragging it means.
  [[nodiscard]] Viewport across() const;

  /// A track's row across the time area, or empty if it is scrolled out of
  /// sight. Includes the part hidden behind the header column, so a block's
  /// position does not depend on how far the view is scrolled.
  [[nodiscard]] Rect track_rect(std::size_t track) const;
  [[nodiscard]] Rect header_rect(std::size_t track) const;
  [[nodiscard]] Rect block_rect(std::size_t track, std::size_t block) const;

  /// Where a block's transition is drawn: centred on its out-edge, half either
  /// side. Empty when it has none.
  [[nodiscard]] Rect transition_rect(std::size_t track, std::size_t block) const;

  /// The rectangle being swept, or empty when nothing is. Between the press and
  /// wherever the pointer has got to, in either direction — a marquee dragged
  /// up and to the left is the same marquee.
  [[nodiscard]] Rect marquee() const;

  /// Every block a rectangle touches. Touching rather than containing: a sweep
  /// has to be able to catch a clip wider than the screen, which no rectangle
  /// could ever enclose.
  [[nodiscard]] std::vector<BlockRef> blocks_touching(const Rect& area) const;

  /// The grab handle for one of a clip's fades, sitting on the top edge at the
  /// point the fade finishes. Empty when the block is too small to hold one.
  [[nodiscard]] Rect fade_handle_rect(std::size_t track, std::size_t block,
                                      bool out_edge) const;

  /// The strip of a block its filmstrip is drawn in. Empty when the block has
  /// no frames yet, or is too small to show one.
  [[nodiscard]] Rect filmstrip_area(std::size_t track, std::size_t block) const;

  /// The strip of a block its waveform is drawn in. Empty when the block has
  /// no envelope yet, or is too short to hold one.
  ///
  /// The whole body rather than a lane of its own: the envelope is the clip's
  /// picture, the way thumbnails are a video clip's, and the volume band and
  /// the label sit over the top of it.
  [[nodiscard]] Rect waveform_area(std::size_t track, std::size_t block) const;

  /// The strip of a block the volume band lives in. Empty for a clip with no
  /// gain to draw.
  ///
  /// Inset from the block's own edges so the loudest and quietest settings are
  /// still a line on a clip rather than one merged into its border, and so a
  /// point at either extreme has room to be drawn round.
  [[nodiscard]] Rect gain_area(std::size_t track, std::size_t block) const;

  /// Where a gain is drawn inside that strip, and what a height in it means.
  /// Each is the other's inverse within the strip; outside it they clamp.
  [[nodiscard]] double gain_to_y(std::size_t track, std::size_t block, double gain) const;
  [[nodiscard]] double gain_at_y(std::size_t track, std::size_t block, double y) const;

  /// Where an automation point is drawn. Empty when there is no such point.
  [[nodiscard]] Rect gain_point_rect(std::size_t track, std::size_t block,
                                     std::size_t point) const;

  /// The automation point under a pointer, if any.
  [[nodiscard]] std::optional<GainPointRef> gain_point_at(double x, double y) const;

  /// Whether a press here would take hold of a clip's volume band — the line
  /// itself, rather than one of its points. True whether or not the clip is
  /// automated; what a drag then means is `GainLevel` on a flat band and
  /// `GainSegment` on one with points.
  [[nodiscard]] bool over_gain_band(double x, double y) const;

  /// Which points a drag of the band under `x` would carry.
  ///
  /// Empty on a flat band, since that has no points and moves as a whole. One
  /// index before the first point or after the last, where the line is flat and
  /// held by a single end. Two in between.
  [[nodiscard]] std::vector<std::size_t> gain_segment_at(std::size_t track, std::size_t block,
                                                         double x) const;

  [[nodiscard]] double playhead_x() const;

  /// The bar along the ruler showing what is marked. Empty when neither mark is
  /// set — an unmarked sequence is the whole sequence, and drawing a bar across
  /// all of it would say something was chosen when nothing was.
  [[nodiscard]] Rect marked_bar() const;

  /// Where a marker's tab is drawn on the ruler. Empty when there is no such
  /// marker, or when it is scrolled out of sight.
  [[nodiscard]] Rect marker_rect(std::size_t index) const;

  [[nodiscard]] std::optional<BlockRef> block_at(double x, double y) const;

  /// Which track and what time a point over the tracks falls on, or nothing
  /// when it is over the ruler, the headers, or empty space below the last
  /// track.
  ///
  /// What a drag from somewhere else — the browser, most obviously — asks in
  /// order to know where it was dropped. The timeline answers in its own terms
  /// and stays ignorant of what was being dragged.
  [[nodiscard]] std::optional<DropPoint> drop_at(double x, double y) const;

  /// The block a drop from somewhere else would land on, outlined so it is
  /// obvious *which* clip is about to receive it.
  ///
  /// Set by whoever is running the drag rather than worked out here: the
  /// timeline does not know that a drag is happening, only that something has
  /// asked it to point at a clip. Cleared with `std::nullopt` when the pointer
  /// is over nothing, which is also how a drag that ends elsewhere tidies up.
  void set_drop_target(std::optional<BlockRef> target);
  [[nodiscard]] const std::optional<BlockRef>& drop_target() const noexcept {
    return drop_target_;
  }

  /// How tall the tracks are altogether, and how far they are scrolled.
  [[nodiscard]] const Viewport& vertical() const noexcept { return vertical_; }

  // ------------------------------------------------------------ behaviour --

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  /// The pointer says what a press would do here, which is the whole of what
  /// `zone_at` already works out and which nothing was previously showing.
  ///
  /// While a drag is running it says what the drag *is* instead: a trim pulled
  /// past the end of its clip is still a trim, and a cursor that changed under
  /// a held button would be reporting on the pointer rather than on the
  /// gesture.
  [[nodiscard]] Cursor cursor_at(double x, double y) const override;

  /// The time the drag has stuck to, while it is stuck to one.
  ///
  /// Snapping was silent: a clip that clicked into place against its neighbour
  /// and one that happened to land a pixel away looked exactly the same, so the
  /// only way to know whether an edit was frame-accurate was to zoom in and
  /// check afterwards.
  [[nodiscard]] const std::optional<double>& snapped() const noexcept { return snapped_; }

  /// Where the pointer is, so the zone under it can be drawn as well as
  /// answered. Off the widget entirely is `std::nullopt`, which is what stops
  /// a highlight being left behind when the pointer leaves.
  [[nodiscard]] const std::optional<std::pair<double, double>>& pointer() const noexcept {
    return pointer_;
  }

  bool on_mouse_down(const MouseEvent& event) override;
  void on_mouse_enter() override;
  void on_mouse_leave() override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_wheel(const WheelEvent& event) override;

 private:
  [[nodiscard]] double track_height(std::size_t track) const noexcept;
  [[nodiscard]] double trim_handle_width(std::size_t track, std::size_t block) const;
  void scrub_to(double x);
  void drag_to(double x, double y);

  /// The track a point is over, or nothing when it is over none.
  ///
  /// Unlike `drop_at` this does not care about the ruler or the empty space
  /// under the last lane: a move that wanders off the bottom should hold at the
  /// last track rather than losing its grip.
  [[nodiscard]] std::optional<std::size_t> track_at(double y) const;

  /// Moves the carried blocks `lanes` tracks of their own kind and `shift`
  /// seconds, rebuilding the arrangement from what it was when the drag began.
  ///
  /// Rebuilt rather than nudged because a block that changes track changes
  /// *index*, and every index after it in both tracks changes with it. Working
  /// from the arrangement at the press is the only version of this that does
  /// not accumulate — the same rule every other drag here follows, applied to
  /// the structure rather than to a number.
  void relocate_carried(int lanes, double shift);
  void refresh_bounds();

  /// One block a move is carrying, as it was when the drag began.
  ///
  /// Every position is computed from these rather than from the last frame, so
  /// a long drag cannot accumulate rounding — the same reason `origin_` exists,
  /// applied to the rest of the selection.
  struct Moving {
    BlockRef ref;
    TimelineBlock origin;
  };
  /// What a move is carrying: the whole selection when the clip under the
  /// pointer is part of it, and that clip alone otherwise. Captured on the
  /// press, because the selection is what it was when the drag started.
  std::vector<Moving> moving_;

  /// A clip's neighbour on the same track, as it was when a slide began.
  ///
  /// Kept because a slide moves three edges at once and every one of them has
  /// to be computed from where it started, for the same reason the dragged clip
  /// is. Absent when there is nothing abutting on that side, which is also what
  /// bounds the slide.
  struct Neighbour {
    std::size_t index = 0;
    TimelineBlock origin;
  };

  /// What a move should carry, taken at the press.
  void capture_moving();

  /// Moves the rest of the dragged block's linked group by the same amounts.
  ///
  /// Every edge gesture here goes through an operation that acts on the whole
  /// group — a trim, a rate stretch, a ripple and a roll all do — so a preview
  /// that moved one block was showing something the release did not do. The
  /// sound jumped into place a moment after the picture, which reads as the
  /// application correcting a mistake.
  ///
  /// Worked from the arrangement at the press, like everything else here.
  void sync_group(double start_delta, double end_delta);

  /// The arrangement as it was when a move began, so each frame of the drag is
  /// computed from it rather than from the frame before.
  TimelineModel press_model_;
  /// How many lanes of its own kind the move has travelled.
  int carried_lanes_ = 0;
  /// The track the press landed on. `drag_` follows the block as it moves
  /// between lanes, so it cannot answer where the gesture started from.
  std::size_t press_track_ = 0;

  /// What a ripple should carry: every block that starts at or after the edge
  /// being dragged, on every track, so the whole sequence downstream follows
  /// and nothing goes out of sync. Held in `moving_`, like a move's set.
  void capture_downstream(double from);

  /// The block on the other side of a join, and what it was when the roll
  /// began. Absent when the edge is not a join, which is what makes a roll
  /// there do nothing.
  std::optional<Neighbour> capture_join(bool at_start);

  /// Where a slide's neighbours are, and the room they leave.
  void capture_neighbours();
  void slide_to(double moved, double frame);

  /// The volume gestures, which move a line rather than a block and so need
  /// the pointer's height as well as its position along the track.
  void gain_to(double x, double y);

  /// Puts a point on a band at the time under `x`, taking the value the band
  /// already has there, and answers where it went in the list.
  ///
  /// The value it already has, so that adding a point changes nothing about what
  /// plays — the same bargain the inspector's stopwatch makes. A point that
  /// arrived at unity, or at the pointer's height, would mean every attempt to
  /// automate a clip started by altering it.
  ///
  /// Nothing when the block has no band, or when a point is already there.
  [[nodiscard]] std::optional<std::size_t> add_gain_point(BlockRef block, double x);

  /// Pins the automation immediately outside the stretch about to be dragged.
  ///
  /// A point one frame beyond each end of it, holding the level the band
  /// already has there, so that dragging the stretch changes the stretch and
  /// nothing else. Without it the neighbouring runs are defined *by* the points
  /// being dragged, and moving them drags the rest of the clip along: two points
  /// in the middle and a pull downwards turned the whole band into a long V,
  /// which is not what grabbing a section looks like it will do.
  ///
  /// One frame out rather than at the clip's edges, which was the first attempt.
  /// The edges hold the very ends but leave everything between them ramping, so
  /// the dip still reached the whole clip. A frame is the project's own quantum
  /// and the band's times are already snapped to it, so the ramp into the dip is
  /// the shortest one the model can represent.
  ///
  /// Only on sides that have something to protect. The run before the first
  /// point *is* the head of the clip, so dragging it is meant to move the head
  /// and there is nothing outside it to hold.
  ///
  /// The anchors take the value the band already had, so materialising them
  /// changes nothing about what plays — the same bargain adding any other point
  /// makes. They are ordinary points afterwards, visible and draggable, because
  /// a control that holds the band but cannot be seen or moved would be worse
  /// than the problem.
  void ensure_gain_anchors(BlockRef block, const std::vector<std::size_t>& segment);

  TimelineModel model_;
  TimeScale scale_;
  Viewport vertical_;

  /// What a scroll or zoom drag started from: where along the bar the press
  /// landed, and the view as it was. Worked from these rather than from the
  /// last position, like every other drag here.
  double scroll_origin_ = 0.0;
  TimeScale zoom_origin_;

  double playhead_ = 0.0;
  bool snapping_ = true;
  Tool tool_ = Tool::Selection;

  DragMode mode_ = DragMode::None;
  std::optional<BlockRef> drag_;
  /// The block a drop from another panel would land on. See `set_drop_target`.
  std::optional<BlockRef> drop_target_;
  /// The clip as it was when the drag began. Every position is computed from
  /// this rather than from the last one, so rounding cannot accumulate over a
  /// long drag and leave the clip a frame off where the pointer is.
  TimelineBlock origin_;
  std::optional<Neighbour> before_;
  std::optional<Neighbour> after_;
  double press_x_ = 0.0;
  /// Where the press was vertically. Only the volume gestures need it, and they
  /// need it for the threshold as much as the position: a band dragged straight
  /// down travels no distance in x, and a drag measured along one axis would
  /// never start.
  double press_y_ = 0.0;
  /// Whether the pointer has moved far enough for this to be a drag at all.
  bool moved_ = false;

  /// Which automation point is being dragged, and where it was when the drag
  /// began. Kept for the same reason `origin_` is: every position is worked out
  /// from the press rather than from the last move, so a long drag cannot
  /// accumulate rounding.
  std::size_t gain_point_ = 0;
  GainPoint gain_origin_;
  /// The level the band was at when a `GainLevel` drag started.
  double gain_level_origin_ = 1.0;

  /// Which points a `GainSegment` drag is carrying, and what they were worth
  /// when it began. Held from the press for the same reason `origin_` is:
  /// working from where the drag started rather than from the last move is what
  /// stops a long one accumulating rounding.
  std::vector<std::size_t> gain_segment_;
  std::vector<double> gain_segment_origin_;
  /// Anchors materialised at the start of this drag. Reported along with the
  /// points that moved, so the model gets the ones holding the ends up as well
  /// — otherwise the view would show a band the project does not have.
  std::vector<GainPoint> gain_anchors_;

  /// Taken from the theme at layout, because input arrives without one.
  Metrics metrics_;

  /// Where the pointer is while it is over the view, for the highlight on the
  /// zone under it. Empty when it is somewhere else, which is what takes the
  /// highlight away rather than leaving one stuck at the edge.
  std::optional<std::pair<double, double>> pointer_;

  /// The time the current drag has snapped to, if any. Recomputed on every
  /// move, so it goes as soon as the pointer pulls away from it.
  std::optional<double> snapped_;

  /// Which lane a height drag is carrying, and what it was when it began —
  /// worked from the press rather than from the last move, like every other
  /// drag here, so a long one cannot accumulate rounding.
  std::size_t sizing_ = 0;
  double sizing_origin_ = 0.0;

  /// Where a marquee is being swept from and to, in the view's own
  /// coordinates. Only meaningful while the mode is `Marquee`.
  double marquee_x_ = 0.0;
  double marquee_y_ = 0.0;
  /// What was selected when the sweep began, so a shift-sweep can add to it
  /// rather than replace it — and so dragging back over the start does not
  /// leave the earlier selection half rubbed out.
  std::vector<BlockRef> marquee_from_;

  std::function<void(double)> on_scrub_;
  std::function<void(std::span<const BlockRef>)> on_select_;
  std::function<void(const TimelineEdit&)> on_edit_;
  std::function<void(TrackControlRef)> on_track_toggle_;
  std::function<void(std::size_t, std::optional<double>)> on_track_resize_;
  std::function<void(double, double)> on_context_menu_;
  std::function<void(std::size_t)> on_track_rename_;
};

}  // namespace cutline::ui
