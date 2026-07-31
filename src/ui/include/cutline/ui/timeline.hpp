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

#include <cstddef>
#include <functional>
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

/// One clip, as far as drawing is concerned.
struct TimelineBlock {
  /// Opaque to the timeline, which never looks inside it. Whoever built the
  /// model decides what it means — for the editor it is a clip id, which is
  /// how a drag finds its way back to the project without the timeline
  /// knowing a project exists.
  std::string id;

  double start = 0.0;
  double end = 0.0;
  std::string label;
  bool selected = false;

  /// Where this clip is animated, in seconds from its own start.
  ///
  /// Drawn as marks along the block. Keyframes are otherwise visible only in
  /// the inspector, one parameter at a time, which makes an animation
  /// something to be remembered rather than something that can be seen.
  std::vector<double> keyframes;

  BlockTransition transition;

  [[nodiscard]] double duration() const noexcept { return end - start; }

  friend bool operator==(const TimelineBlock&, const TimelineBlock&) = default;
};

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

  friend bool operator==(const TrackSwitches&, const TrackSwitches&) = default;
};

struct TimelineTrack {
  /// Opaque, like a block's.
  std::string id;
  std::string name;
  /// Audio tracks are shorter, because the theme says so.
  bool audio = false;
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
enum class TrackControl { Mute, Solo, Lock, Hide };

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
};

/// Which edge a mode pulls, whichever tool is pulling it. Trim and rate stretch
/// differ in what they do to the clip, not in which end is being held.
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

  /// How far the gesture travelled, in seconds. `Slip` has nothing else to say:
  /// what moved is the source, which the timeline does not model.
  double delta = 0.0;

  /// `Razor` only: where the cut goes.
  double at = 0.0;
  /// `Razor` only: cut every track at `at` rather than only the clip clicked.
  bool all_tracks = false;

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

  [[nodiscard]] double playhead() const noexcept { return playhead_; }
  /// Snapped to a frame, and never negative.
  void set_playhead(double seconds);

  /// Called while the playhead is dragged, and once when it is clicked.
  void set_on_scrub(std::function<void(double)> on_scrub) { on_scrub_ = std::move(on_scrub); }
  /// Called when the selection changes. Null means everything was deselected.
  void set_on_select(std::function<void(std::optional<BlockRef>)> on_select) {
    on_select_ = std::move(on_select);
  }

  [[nodiscard]] std::optional<BlockRef> selection() const;
  void select(std::optional<BlockRef> block);

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
  /// Below the ruler, where the tracks are.
  [[nodiscard]] Rect tracks_area() const;

  /// A track's row across the time area, or empty if it is scrolled out of
  /// sight. Includes the part hidden behind the header column, so a block's
  /// position does not depend on how far the view is scrolled.
  [[nodiscard]] Rect track_rect(std::size_t track) const;
  [[nodiscard]] Rect header_rect(std::size_t track) const;
  [[nodiscard]] Rect block_rect(std::size_t track, std::size_t block) const;

  /// Where a block's transition is drawn: centred on its out-edge, half either
  /// side. Empty when it has none.
  [[nodiscard]] Rect transition_rect(std::size_t track, std::size_t block) const;

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

  /// How tall the tracks are altogether, and how far they are scrolled.
  [[nodiscard]] const Viewport& vertical() const noexcept { return vertical_; }

  // ------------------------------------------------------------ behaviour --

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_wheel(const WheelEvent& event) override;

 private:
  [[nodiscard]] double track_height(std::size_t track) const noexcept;
  [[nodiscard]] double trim_handle_width(std::size_t track, std::size_t block) const;
  void scrub_to(double x);
  void drag_to(double x);
  void refresh_bounds();

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

  /// Where a slide's neighbours are, and the room they leave.
  void capture_neighbours();
  void slide_to(double moved, double frame);

  TimelineModel model_;
  TimeScale scale_;
  Viewport vertical_;

  double playhead_ = 0.0;
  bool snapping_ = true;
  Tool tool_ = Tool::Selection;

  DragMode mode_ = DragMode::None;
  std::optional<BlockRef> drag_;
  /// The clip as it was when the drag began. Every position is computed from
  /// this rather than from the last one, so rounding cannot accumulate over a
  /// long drag and leave the clip a frame off where the pointer is.
  TimelineBlock origin_;
  std::optional<Neighbour> before_;
  std::optional<Neighbour> after_;
  double press_x_ = 0.0;
  /// Whether the pointer has moved far enough for this to be a drag at all.
  bool moved_ = false;

  /// Taken from the theme at layout, because input arrives without one.
  Metrics metrics_;

  std::function<void(double)> on_scrub_;
  std::function<void(std::optional<BlockRef>)> on_select_;
  std::function<void(const TimelineEdit&)> on_edit_;
  std::function<void(TrackControlRef)> on_track_toggle_;
};

}  // namespace cutline::ui
