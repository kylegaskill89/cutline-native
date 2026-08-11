#include "cutline/ui/timeline.hpp"

#include "cutline/core/keyframe.hpp"
#include "cutline/core/time.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace cutline::ui {
namespace {

/// How much a wheel notch moves the view, as a fraction of what is on screen.
constexpr double kScrollFraction = 0.15;
/// And how much it zooms. A shade under a quarter, which is about two notches
/// to double and feels neither sluggish nor twitchy.
constexpr double kZoomStep = 1.22;

/// The smallest a clip may be drawn. A one-frame clip zoomed out is a fraction
/// of a pixel wide, and drawing it as nothing means it cannot be found or
/// clicked — which is exactly when someone is hunting for it.
constexpr double kMinBlockWidth = 2.0;

/// How far the pointer has to travel before a press on a clip becomes a drag.
/// Without it, selecting a clip nudges it by whatever the hand wobbled.
constexpr double kDragThreshold = 3.0;

/// How close, in pixels, a dragged edge has to come before it sticks.
constexpr double kSnapDistance = 10.0;

/// How near the start of the sequence a drop has to be to land exactly on it.
///
/// Much wider than an ordinary snap, and deliberately. Zero is the one point on
/// a timeline with nothing to its left to be mistaken for, so a generous catch
/// there cannot steal a drop meant for something else — where the ordinary
/// distance exists to pick one edge out of several sitting close together. And
/// nearly every sequence begins with something at zero: "right at the start" is
/// a thing people mean exactly, and a clip landing three frames short of it
/// leaves a gap that has to be noticed and closed later.
constexpr double kStartSnapDistance = 48.0;

/// The square grabbed to pull a fade. Small, because it sits on the top edge
/// over the clip's own body and a large one would swallow the trim handle it
/// shares a corner with.
constexpr double kFadeHandle = 9.0;

/// The grabbable strip at each end of a clip. Never more than a third of it,
/// so a short clip can still be moved rather than being all handle.
constexpr double kTrimHandle = 8.0;

/// How far either side of the line under a header counts as grabbing it.
/// Four is the smallest reach anybody can hit reliably with a mouse, and the
/// strip is the full width of the header so there is plenty of it to aim at.
constexpr double kTrackResizeReach = 4.0;

/// How wide the grips at the ends of the scroll thumb are. Small, because they
/// live inside the thumb and taking too much of it would leave nothing to
/// scroll by on a sequence that is mostly on screen already.
constexpr double kZoomGrip = 7.0;

/// How tall the "fx" badge is. Small enough to sit inside a clip at the
/// shortest track height without covering the label beside it.
constexpr double kEffectBadge = 11.0;

/// How tall the stripe along a labelled clip is. Thin: it is a mark saying
/// which shot this is, not a second border.
constexpr double kLabelStripe = 4.0;

[[nodiscard]] Color fade(const Color& color, float amount) noexcept {
  return Color{color.r, color.g, color.b, color.a * amount};
}

/// How close two times have to be to count as touching. The same tolerance the
/// core's slide uses, and it has to be: a clip the view thinks abuts its
/// neighbour and the core does not would preview a slide and then refuse it.
constexpr double kAbutEps = 1e-3;

/// One header switch, and the gap between them. Small, because there are up to
/// three of them and a track header is a hundred and forty pixels wide with a
/// name to fit as well.
constexpr double kSwitchSize = 15.0;
constexpr double kSwitchGap = 3.0;

/// How wide a marker's tab is drawn. Fixed rather than scaled with the zoom: a
/// marker is a point in time, so its width says nothing and a one-pixel one
/// could not be seen.
constexpr double kMarkerWidth = 9.0;

/// How far in from a block's edges the volume band runs, and how big a grab a
/// point on it gets. The reach is generous against the six pixels a point is
/// drawn at: it is a small target on a short clip, and a band that has to be
/// hit exactly is one nobody uses twice.
constexpr double kGainInset = 5.0;
constexpr double kGainPointRadius = 3.0;
constexpr double kGainPointReach = 6.0;
/// How near the line itself a press has to be to take hold of it.
constexpr double kGainBandReach = 5.0;

/// The switches each kind of track shows, in the order they are drawn.
// Targeting first on both, because it is the one that says where the next edit
// goes rather than what this track is doing, and because a row that begins with
// the same switch on every track reads as a column.
constexpr std::array<TrackControl, 4> kAudioControls{TrackControl::Target, TrackControl::Mute,
                                                     TrackControl::Solo, TrackControl::Lock};
constexpr std::array<TrackControl, 3> kVideoControls{TrackControl::Target, TrackControl::Hide,
                                                     TrackControl::Lock};

}  // namespace

std::string_view to_string(Tool tool) noexcept {
  switch (tool) {
    case Tool::Selection: return "selection";
    case Tool::Razor: return "razor";
    case Tool::Ripple: return "ripple";
    case Tool::Roll: return "roll";
    case Tool::RateStretch: return "rate";
    case Tool::Slip: return "slip";
    case Tool::Slide: return "slide";
  }
  return "selection";
}

std::string_view to_string(TrackControl control) noexcept {
  switch (control) {
    case TrackControl::Target: return "T";
    case TrackControl::Mute: return "M";
    case TrackControl::Solo: return "S";
    case TrackControl::Lock: return "L";
    case TrackControl::Hide: return "H";
  }
  return "?";
}

bool pulls_start(DragMode mode) noexcept {
  return mode == DragMode::TrimStart || mode == DragMode::RateStart ||
         mode == DragMode::RippleStart || mode == DragMode::RollStart;
}

bool pulls_end(DragMode mode) noexcept {
  return mode == DragMode::TrimEnd || mode == DragMode::RateEnd ||
         mode == DragMode::RippleEnd || mode == DragMode::RollEnd;
}

std::vector<double> snap_points(const TimelineModel& model, double playhead,
                                std::optional<BlockRef> exclude) {
  std::vector<double> points{0.0, playhead};

  for (std::size_t track = 0; track < model.tracks.size(); ++track) {
    const std::vector<TimelineBlock>& blocks = model.tracks[track].blocks;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      // A clip must not snap to itself: its own edges are the ones following
      // the pointer, so leaving them in would pin the drag where it started.
      if (exclude.has_value() && exclude->track == track && exclude->block == i) continue;
      points.push_back(blocks[i].start);
      points.push_back(blocks[i].end);
    }
  }
  std::ranges::sort(points);
  return points;
}

std::optional<double> nearest_snap(std::span<const double> points, double time,
                                   double tolerance) {
  if (tolerance <= 0.0) return std::nullopt;

  std::optional<double> best;
  double closest = tolerance;
  for (const double point : points) {
    const double distance = std::abs(point - time);
    // Strictly closer, so with the points in order a tie goes to the earlier
    // one rather than to whichever happened to be visited last.
    if (distance < closest) {
      closest = distance;
      best = point;
    }
  }
  return best;
}

const FilmFrame* Filmstrip::nearest(double t) const noexcept {
  const FilmFrame* best = nullptr;
  double closest = std::numeric_limits<double>::max();
  for (const FilmFrame& frame : frames) {
    if (frame.empty()) continue;
    const double distance = std::abs(frame.t - t);
    if (distance < closest) {
      closest = distance;
      best = &frame;
    }
  }
  return best;
}

double source_time_of(const TimelineBlock& block, double local_t) noexcept {
  const double travelled = local_t * block.speed;
  if (!block.reverse) return block.source_in + travelled;
  // Reverse runs from the far end back, so the source span has to be known
  // before the first pixel can be placed.
  return block.source_in + block.duration() * block.speed - travelled;
}

double gain_to_band(double gain, double maximum) noexcept {
  if (!(maximum > 0.0)) return 0.0;
  // Silence has no logarithm, and everything below the floor is silence as far
  // as the band is concerned, so both land on the foot of it.
  if (!(gain > 0.0)) return 0.0;

  const double top_db = 20.0 * std::log10(maximum);
  const double db = 20.0 * std::log10(gain);
  if (db <= kGainFloorDb) return 0.0;
  return std::clamp((db - kGainFloorDb) / (top_db - kGainFloorDb), 0.0, 1.0);
}

double band_to_gain(double fraction, double maximum) noexcept {
  if (!(maximum > 0.0)) return 0.0;
  // The foot of the band is silence rather than the floor. Otherwise a clip
  // could be dragged very quiet and never actually off, and the one thing a
  // volume control at its bottom stop is expected to do is nothing at all.
  if (fraction <= 0.0) return 0.0;
  if (fraction >= 1.0) return maximum;

  const double top_db = 20.0 * std::log10(maximum);
  const double db = kGainFloorDb + fraction * (top_db - kGainFloorDb);
  return std::clamp(std::pow(10.0, db / 20.0), 0.0, maximum);
}

double gain_to_fader_db(double gain) noexcept {
  if (!(gain > 0.0)) return kGainFloorDb;
  return std::max(20.0 * std::log10(gain), kGainFloorDb);
}

double fader_db_to_gain(double db, double maximum) noexcept {
  if (!(maximum > 0.0)) return 0.0;
  if (db <= kGainFloorDb) return 0.0;
  return std::clamp(std::pow(10.0, db / 20.0), 0.0, maximum);
}

double TimelineModel::content_duration() const noexcept {
  double last = duration;
  for (const TimelineTrack& track : tracks) {
    for (const TimelineBlock& block : track.blocks) last = std::max(last, block.end);
  }
  return std::max(0.0, last);
}

TimelineView::TimelineView() {
  // Blocks run past both edges of the view constantly; without this they would
  // be drawn over the panels either side.
  set_clips_children(true);
  set_focusable(true);
}

void TimelineView::set_model(TimelineModel model) {
  // Not while a gesture is in flight. Every drag here is a *live edit of this
  // model* — that is what makes a trim, a move or a volume band visible while
  // it happens — so a rebuild landing mid-gesture wipes it out between one
  // mouse move and the next.
  //
  // Playback is what made this constant rather than occasional: the loop
  // rebuilds the timeline once per displayed frame, so a drag during playback
  // had the model pulled out from under it sixty times a second. It was
  // reported as the volume band being unadjustable while the preview plays,
  // which is where anybody would notice it; it was every drag on the timeline.
  //
  // Kept rather than discarded, and applied when the gesture ends. Discarding
  // would lose a waveform or a filmstrip that arrived mid-drag, and those
  // arrive on a worker whenever they happen to be ready.
  if (mode_ != DragMode::None) {
    pending_model_ = std::move(model);
    return;
  }
  model_ = std::move(model);
  refresh_bounds();
}

void TimelineView::settle_model() {
  if (!pending_model_.has_value()) return;
  model_ = std::move(*pending_model_);
  pending_model_.reset();
  refresh_bounds();
}

void TimelineView::set_scale(const TimeScale& scale) {
  scale_ = scale;
  scale_.pixels_per_second =
      std::clamp(scale_.pixels_per_second, kMinPixelsPerSecond, kMaxPixelsPerSecond);
  refresh_bounds();
}

void TimelineView::zoom_to_fit() {
  scale_.fit(model_.content_duration(), time_area().width);
}

void TimelineView::zoom_about_playhead(bool closer) {
  const Rect area = time_area();
  if (area.width <= 0.0) return;

  // Where on screen to hold still: the playhead, or the middle of the view when
  // it is not on screen to hold.
  const double at = scale_.to_x(playhead_);
  const double anchor = at >= 0.0 && at <= area.width ? at : area.width * 0.5;

  scale_.zoom_about(anchor, closer ? kZoomStep : 1.0 / kZoomStep);
  scale_.clamp_start(model_.content_duration());
  // Anchored on the playhead, so this is a statement that the playhead is what
  // is being looked at — following it again is what was asked for.
  scrolled_by_hand_ = false;
  refresh_bounds();
}

void TimelineView::set_playhead(double seconds) {
  playhead_ = std::max(0.0, core::snap_to_frame(seconds, model_.fps));
}

bool TimelineView::follow_playhead() {
  const double width = time_area().width;
  if (width <= 0.0) return false;

  const double visible = scale_.visible_duration(width);
  if (visible <= 0.0) return false;

  const double margin = visible * kFollowMargin;
  const double first = scale_.start;
  const double last = first + visible;

  // A view somebody has scrolled is theirs until the playhead reaches it.
  //
  // This runs on every frame of a playback, so without it the timeline simply
  // could not be scrolled while anything was playing: the wheel moved the view
  // and the next frame put it back. Handing it over again when the playhead
  // arrives is what lets somebody look ahead and then stop looking, without a
  // control for it.
  if (scrolled_by_hand_) {
    if (playhead_ < first || playhead_ > last) return false;
    scrolled_by_hand_ = false;
  }

  // Inside the comfortable part, which is where it is nearly all the time.
  if (playhead_ >= first + margin && playhead_ <= last - margin) return false;

  // To the leading margin, so a page of what is coming is on screen. Backwards
  // too: scrubbing to the head of a sequence and pressing play should not leave
  // the playhead off the left edge.
  const double start = playhead_ - margin;
  const double before = scale_.start;
  scale_.start = std::max(0.0, start);
  scale_.clamp_start(model_.content_duration());
  return scale_.start != before;
}

void TimelineView::refresh_bounds() {
  scale_.clamp_start(model_.content_duration());

  double total = 0.0;
  for (std::size_t i = 0; i < model_.tracks.size(); ++i) total += track_height(i);
  vertical_.content = total;
  vertical_.visible = tracks_area().height;
  vertical_.clamp();
}

double TimelineView::track_height(std::size_t track) const noexcept {
  if (track >= model_.tracks.size()) return 0.0;
  const TimelineTrack& row = model_.tracks[track];
  if (row.height.has_value()) {
    return std::clamp(*row.height, kMinTrackHeight, kMaxTrackHeight);
  }
  return row.audio ? metrics_.audio_track_height : metrics_.track_height;
}

// -------------------------------------------------------------- geometry --

Rect TimelineView::header_area() const {
  const double width = std::min(metrics_.track_header_width, bounds().width);
  return Rect{bounds().x, bounds().y, width, bounds().height};
}

Rect TimelineView::time_area() const {
  const Rect headers = header_area();
  return Rect{headers.right(), bounds().y, std::max(0.0, bounds().width - headers.width),
              bounds().height};
}

Rect TimelineView::ruler_area() const {
  const Rect area = time_area();
  return Rect{area.x, area.y, area.width, std::min(metrics_.ruler_height, area.height)};
}

Rect TimelineView::tracks_area() const {
  const Rect area = time_area();
  const double top = ruler_area().height;
  const double bottom = scroll_area().height;
  return Rect{area.x, area.y + top, area.width,
              std::max(0.0, area.height - top - bottom)};
}

Viewport TimelineView::across() const {
  const Rect area = time_area();
  Viewport view;
  view.content = model_.content_duration();
  view.visible = scale_.visible_duration(area.width);
  view.offset = scale_.start;
  return view;
}

Rect TimelineView::scroll_area() const {
  // Nothing when the whole sequence already fits. A bar whose thumb fills its
  // own track says nothing, and it costs a row of pixels the tracks want.
  if (!across().scrollable()) return {};

  const Rect area = time_area();
  const double thickness = std::min(metrics_.scrollbar_width, area.height);
  return Rect{area.x, area.bottom() - thickness, area.width, thickness};
}

Rect TimelineView::scroll_thumb() const {
  const Rect bar = scroll_area();
  if (bar.empty()) return {};

  const Viewport view = across();
  return Rect{bar.x + view.thumb_offset(bar.width), bar.y, view.thumb_size(bar.width),
              bar.height};
}

Rect TimelineView::zoom_grip(bool at_end) const {
  const Rect thumb = scroll_thumb();
  // Three grips' worth at least, so the middle of the thumb is still somewhere
  // to grab for a scroll. Below that the whole thumb scrolls and zooming is the
  // wheel's job, which it is anyway.
  if (thumb.empty() || thumb.width < kZoomGrip * 3.0) return {};
  const double x = at_end ? thumb.right() - kZoomGrip : thumb.x;
  return Rect{x, thumb.y, kZoomGrip, thumb.height};
}

Rect TimelineView::track_scroll_area() const {
  if (!vertical_.scrollable()) return {};
  const Rect area = tracks_area();
  const double thickness = std::min(metrics_.scrollbar_width, area.width);
  return Rect{area.right() - thickness, area.y, thickness, area.height};
}

Rect TimelineView::track_scroll_thumb() const {
  const Rect bar = track_scroll_area();
  if (bar.empty()) return {};
  return Rect{bar.x, bar.y + vertical_.thumb_offset(bar.height), bar.width,
              vertical_.thumb_size(bar.height)};
}

Rect TimelineView::track_rect(std::size_t track) const {
  if (track >= model_.tracks.size()) return {};

  double top = 0.0;
  for (std::size_t i = 0; i < track; ++i) top += track_height(i);

  const Rect area = tracks_area();
  return Rect{area.x, area.y + top - vertical_.offset, area.width, track_height(track)};
}

Rect TimelineView::header_rect(std::size_t track) const {
  const Rect row = track_rect(track);
  if (row.empty()) return {};
  const Rect headers = header_area();
  return Rect{headers.x, row.y, headers.width, row.height};
}

Rect TimelineView::resize_rect(std::size_t track) const {
  const Rect header = header_rect(track);
  if (header.empty()) return {};
  // Straddling the line rather than sitting above it, so the reach is the same
  // whichever side of the boundary the pointer approaches from.
  const double reach = kTrackResizeReach;
  return Rect{header.x, header.bottom() - reach, header.width, reach * 2.0};
}

bool TimelineView::has_control(std::size_t track, TrackControl control) const {
  if (track >= model_.tracks.size()) return false;
  if (model_.tracks[track].audio) {
    return std::ranges::find(kAudioControls, control) != kAudioControls.end();
  }
  return std::ranges::find(kVideoControls, control) != kVideoControls.end();
}

Rect TimelineView::control_rect(std::size_t track, TrackControl control) const {
  if (!has_control(track, control)) return {};

  const Rect header = header_rect(track);
  if (header.empty()) return {};

  const std::span<const TrackControl> controls =
      model_.tracks[track].audio ? std::span<const TrackControl>(kAudioControls)
                                 : std::span<const TrackControl>(kVideoControls);
  const auto found = std::ranges::find(controls, control);
  const auto index = static_cast<double>(std::distance(controls.begin(), found));

  // A second row under the name. Under rather than beside, because a hundred
  // and forty pixels is not enough for a readable label and three switches on
  // one line.
  //
  // Under the *name* rather than at the foot of the header, which is what it
  // was: on a lane dragged twice as tall the switches were stranded at the
  // bottom, a long way from the track they belong to. On a lane at its usual
  // height the two are the same place, so nothing moved for anybody who has not
  // resized one.
  const double x = header.x + metrics_.padding_x + index * (kSwitchSize + kSwitchGap);
  const double under_name = header.y + metrics_.padding_y +
                            metrics_.font_size * metrics_.line_height + kSwitchGap;
  const double y = std::min(under_name, header.bottom() - kSwitchSize - metrics_.padding_y);
  const Rect box{x, y, kSwitchSize, kSwitchSize};

  // Refused rather than clipped. A switch drawn half outside its own header, or
  // overlapping the name, is worse than one that admits there is no room.
  if (box.right() > header.right() - metrics_.padding_x * 0.5 || box.y < header.y) return {};
  return box;
}

std::optional<std::size_t> TimelineView::header_at(double x, double y) const {
  if (!header_area().contains(x, y)) return std::nullopt;
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    if (header_rect(track).contains(x, y)) return track;
  }
  return std::nullopt;
}

std::optional<TrackControlRef> TimelineView::control_at(double x, double y) const {
  if (!header_area().contains(x, y)) return std::nullopt;

  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    for (const TrackControl control : {TrackControl::Target, TrackControl::Mute,
                                      TrackControl::Solo, TrackControl::Lock,
                                      TrackControl::Hide}) {
      if (control_rect(track, control).contains(x, y)) {
        return TrackControlRef{.track = track, .control = control};
      }
    }
  }
  return std::nullopt;
}

Rect TimelineView::block_rect(std::size_t track, std::size_t block) const {
  if (track >= model_.tracks.size()) return {};
  const TimelineTrack& row = model_.tracks[track];
  if (block >= row.blocks.size()) return {};

  const TimelineBlock& clip = row.blocks[block];
  const Rect area = track_rect(track);
  if (area.empty()) return {};

  const double x = area.x + scale_.to_x(clip.start);
  const double width = std::max(kMinBlockWidth, scale_.width_of(clip.duration()));
  return Rect{x, area.y, width, area.height};
}

Rect TimelineView::transition_span(std::size_t track, std::size_t block,
                                   double duration) const {
  if (track >= model_.tracks.size()) return {};
  const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
  if (block >= blocks.size()) return {};
  if (duration <= 0.0) return {};

  const Rect row = track_rect(track);
  if (row.empty()) return {};

  // Centred on the cut, which is where the model puts it: half the transition
  // plays before the join and half after.
  const double centre = row.x + scale_.to_x(blocks[block].end);
  const double half = scale_.width_of(duration) * 0.5;
  return Rect{centre - half, row.y, half * 2.0, row.height};
}

Rect TimelineView::transition_rect(std::size_t track, std::size_t block) const {
  if (track >= model_.tracks.size()) return {};
  const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
  if (block >= blocks.size()) return {};
  return transition_span(track, block, blocks[block].transition.duration);
}

void TimelineView::set_transition_ghost(std::optional<TransitionGhost> ghost) {
  if (transition_ghost_ == ghost) return;
  transition_ghost_ = std::move(ghost);
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

Rect TimelineView::transition_ghost_rect() const {
  if (!transition_ghost_.has_value()) return {};
  return transition_span(transition_ghost_->track, transition_ghost_->block,
                         transition_ghost_->duration);
}

Rect TimelineView::fade_handle_rect(std::size_t track, std::size_t block,
                                    bool out_edge) const {
  if (track >= model_.tracks.size()) return {};
  const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
  if (block >= blocks.size()) return {};

  const Rect box = block_rect(track, block);
  // A clip too small to hold two handles and something between them would be
  // all handle, and could then not be moved at all.
  if (box.width < kFadeHandle * 3.0 || box.height < kFadeHandle * 2.0) return {};

  const TimelineBlock& clip = blocks[block];
  const double length = out_edge ? clip.fade_out : clip.fade_in;
  const double along = std::min(scale_.width_of(length), box.width * 0.5);
  const double x = out_edge ? box.right() - along : box.x + along;
  return Rect{x - kFadeHandle * 0.5, box.y, kFadeHandle, kFadeHandle};
}

Rect TimelineView::filmstrip_area(std::size_t track, std::size_t block) const {
  if (track >= model_.tracks.size()) return {};
  const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
  if (block >= blocks.size()) return {};

  const std::shared_ptr<const Filmstrip>& strip = blocks[block].filmstrip;
  if (strip == nullptr || strip->empty()) return {};

  const Rect box = block_rect(track, block);
  // Below a certain size a thumbnail is a smear and the label is what carries
  // the clip, so there is nothing to gain by drawing one.
  if (box.height < 20.0 || box.width < 8.0) return {};
  return box.inset(2.0);
}

Rect TimelineView::waveform_area(std::size_t track, std::size_t block) const {
  if (track >= model_.tracks.size()) return {};
  const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
  if (block >= blocks.size()) return {};

  const std::shared_ptr<const Waveform>& wave = blocks[block].waveform;
  if (wave == nullptr || wave->empty()) return {};

  const Rect box = block_rect(track, block);
  // Inset by the border the clip's surface draws, so the envelope does not sit
  // on top of its own edge.
  if (box.height < 6.0 || box.width < 2.0) return {};
  return box.inset(2.0);
}

Rect TimelineView::gain_area(std::size_t track, std::size_t block) const {
  if (track >= model_.tracks.size()) return {};
  const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
  if (block >= blocks.size() || !blocks[block].gain.has_value()) return {};

  const Rect box = block_rect(track, block);
  // Too short to hold a band and still be a clip. Below this the line would be
  // the whole block and there would be nothing left to see it against.
  if (box.height < kGainInset * 2.0 + 4.0) return {};
  return Rect{box.x, box.y + kGainInset, box.width,
              std::max(0.0, box.height - kGainInset * 2.0)};
}

double TimelineView::gain_to_y(std::size_t track, std::size_t block, double gain) const {
  const Rect area = gain_area(track, block);
  if (area.empty()) return 0.0;
  // Loud is up, which is the one thing about a fader nobody has to be told.
  return area.bottom() - gain_to_band(gain, model_.max_gain) * area.height;
}

double TimelineView::gain_at_y(std::size_t track, std::size_t block, double y) const {
  const Rect area = gain_area(track, block);
  if (area.empty() || area.height <= 0.0) return 0.0;
  return band_to_gain((area.bottom() - y) / area.height, model_.max_gain);
}

Rect TimelineView::gain_point_rect(std::size_t track, std::size_t block,
                                   std::size_t point) const {
  const Rect area = gain_area(track, block);
  if (area.empty()) return {};

  const GainBand& band = *model_.tracks[track].blocks[block].gain;
  if (point >= band.points.size()) return {};

  const GainPoint& at = band.points[point];
  const double x = area.x + at.t * scale_.pixels_per_second;
  const double y = gain_to_y(track, block, at.v);
  return Rect{x - kGainPointRadius, y - kGainPointRadius, kGainPointRadius * 2.0,
              kGainPointRadius * 2.0};
}

std::optional<GainPointRef> TimelineView::gain_point_at(double x, double y) const {
  const std::optional<BlockRef> hit = block_at(x, y);
  if (!hit.has_value()) return std::nullopt;

  const std::optional<GainBand>& band = model_.tracks[hit->track].blocks[hit->block].gain;
  if (!band.has_value()) return std::nullopt;

  // Nearest rather than first, so two points dragged close together can still
  // be told apart by which one the pointer is actually over.
  std::optional<GainPointRef> best;
  double closest = kGainPointReach;
  for (std::size_t i = 0; i < band->points.size(); ++i) {
    const Rect box = gain_point_rect(hit->track, hit->block, i);
    if (box.empty()) continue;
    const double dx = x - (box.x + box.width * 0.5);
    const double dy = y - (box.y + box.height * 0.5);
    const double distance = std::hypot(dx, dy);
    if (distance < closest) {
      closest = distance;
      best = GainPointRef{*hit, i};
    }
  }
  return best;
}

std::vector<std::size_t> TimelineView::gain_segment_at(std::size_t track, std::size_t block,
                                                       double x) const {
  const Rect area = gain_area(track, block);
  if (area.empty()) return {};

  const GainBand& band = *model_.tracks[track].blocks[block].gain;
  const std::vector<GainPoint>& points = band.points;
  if (points.empty()) return {};

  const double t = (x - area.x) / scale_.pixels_per_second;
  // Outside the points the line is flat, held up by the one end nearest it —
  // so that stretch moves with that end alone.
  if (t <= points.front().t) return {0};
  if (t >= points.back().t) return {points.size() - 1};

  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    if (t >= points[i].t && t <= points[i + 1].t) return {i, i + 1};
  }
  return {points.size() - 1};
}

bool TimelineView::over_gain_band(double x, double y) const {
  const std::optional<BlockRef> hit = block_at(x, y);
  if (!hit.has_value()) return false;

  const std::optional<GainBand>& band = model_.tracks[hit->track].blocks[hit->block].gain;
  if (!band.has_value()) return false;

  const Rect area = gain_area(hit->track, hit->block);
  if (area.empty()) return false;

  // Where the line actually is at this moment, which on an automated band is
  // whatever the points evaluate to rather than the stored level.
  const double t = (x - area.x) / scale_.pixels_per_second;
  const double at = gain_at(*band, t);
  return std::abs(y - gain_to_y(hit->track, hit->block, at)) <= kGainBandReach;
}

double TimelineView::playhead_x() const {
  return time_area().x + scale_.to_x(playhead_);
}

Rect TimelineView::marker_rect(std::size_t index) const {
  if (index >= model_.markers.size()) return {};

  const Rect ruler = ruler_area();
  const double x = ruler.x + scale_.to_x(model_.markers[index].time);
  // In the middle band, between the timecode labels above and the marked span
  // along the foot. The first attempt put it at the top and it sat squarely on
  // a label — over the ticks is fine, since one tick is much like another, but
  // over a number is not.
  const double height = std::max(4.0, ruler.height * 0.3);
  const Rect tab{x - kMarkerWidth * 0.5, ruler.y + ruler.height * 0.45, kMarkerWidth, height};
  if (tab.right() < ruler.x || tab.x > ruler.right()) return {};
  return tab;
}

Rect TimelineView::marker_span(std::size_t index) const {
  if (index >= model_.markers.size()) return {};
  const TimelineMarker& marker = model_.markers[index];
  if (marker.duration <= 0.0) return {};

  const Rect ruler = ruler_area();
  const Rect tab = marker_rect(index);
  // From where the tab sits, so the band and the tab read as one object rather
  // than as a marker with a stripe near it.
  const double x = ruler.x + scale_.to_x(marker.time);
  const double width = std::max(1.0, scale_.width_of(marker.duration));
  const Rect band{x, tab.empty() ? ruler.y + ruler.height * 0.45 : tab.y, width,
                  tab.empty() ? std::max(4.0, ruler.height * 0.3) : tab.height};
  if (band.right() < ruler.x || band.x > ruler.right()) return {};
  return band;
}

std::optional<std::size_t> TimelineView::marker_at(double x, double y) const {
  // Backwards, so the one drawn last — on top — is the one found. Markers can
  // sit on each other, and the answer has to match the picture.
  for (std::size_t i = model_.markers.size(); i-- > 0;) {
    if (marker_rect(i).contains(x, y)) return i;
    if (const Rect band = marker_span(i); !band.empty() && band.contains(x, y)) return i;
  }
  return std::nullopt;
}

std::string TimelineView::tooltip_at(double x, double y) const {
  if (const std::optional<TrackControlRef> control = control_at(x, y)) {
    // Named for what the switch *is*, not for what pressing it would do. A
    // label that flips between "Mute" and "Unmute" makes you read it to find
    // out which state you are in, when the switch already shows that.
    switch (control->control) {
      case TrackControl::Target: return "Target this track for insert and overwrite";
      case TrackControl::Mute: return "Mute this track";
      case TrackControl::Solo: return "Solo: play only the soloed tracks";
      case TrackControl::Lock: return "Lock: nothing on this track can be edited";
      case TrackControl::Hide: return "Hide this track from the picture";
    }
  }

  if (const std::optional<std::size_t> marker = marker_at(x, y)) {
    const TimelineMarker& found = model_.markers[*marker];
    if (!found.label.empty() && !found.comment.empty()) {
      return found.label + " — " + found.comment;
    }
    if (!found.comment.empty()) return found.comment;
    if (!found.label.empty()) return found.label;
    return "Marker (double-click to name it)";
  }

  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    if (resize_rect(track).contains(x, y)) {
      return "Drag to resize the track, double-click to put it back";
    }
  }
  return tooltip();
}

Rect TimelineView::marked_bar() const {
  if (!model_.in_point.has_value() && !model_.out_point.has_value()) return {};

  const double total = model_.content_duration();
  const double from = std::clamp(model_.in_point.value_or(0.0), 0.0, total);
  const double to = std::clamp(model_.out_point.value_or(total), 0.0, total);
  if (to <= from) return {};

  const Rect ruler = ruler_area();
  const double x = ruler.x + scale_.to_x(from);
  const double width = scale_.width_of(to - from);
  // Along the foot of the ruler, under the tick labels. A bar across the whole
  // ruler would fight the ticks for the same pixels and win.
  const double height = std::max(3.0, ruler.height * 0.2);
  return Rect{x, ruler.bottom() - height, width, height};
}

void TimelineView::set_drop_target(std::optional<BlockRef> target) {
  if (drop_target_ == target) return;
  drop_target_ = target;
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

std::optional<BlockRef> TimelineView::block_at(double x, double y) const {
  if (!tracks_area().contains(x, y)) return std::nullopt;

  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const Rect row = track_rect(track);
    if (y < row.y || y >= row.bottom()) continue;

    // Backwards, so an overlapping block drawn on top is the one that answers.
    const std::size_t count = model_.tracks[track].blocks.size();
    for (std::size_t i = count; i > 0; --i) {
      if (block_rect(track, i - 1).contains(x, y)) return BlockRef{track, i - 1};
    }
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<DropPoint> TimelineView::drop_at(double x, double y) const {
  const Rect tracks = tracks_area();
  // The rows and the headers beside them. `tracks_area` is the time side only,
  // and a drop is now allowed over the headers — see below — so the band this
  // gesture may land in is that area widened back to the panel's own edge.
  const Rect landable{bounds().x, tracks.y, tracks.right() - bounds().x, tracks.height};
  if (!landable.contains(x, y)) return std::nullopt;
  const Rect time = time_area();

  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const Rect row = track_rect(track);
    if (row.empty() || y < row.y || y >= row.bottom()) continue;

    // Left of the time area is the header column, and the pointer having gone
    // that far left means the start of the sequence. It used to be refused, on
    // the grounds that a drop over the headers has no time and rounding it to
    // zero would *quietly* put the clip at the start — but the drop ghost says
    // where it will land before the button comes up, so it is not quiet any
    // more, and "further left than the beginning" has one sensible reading.
    //
    // Clamped rather than sent straight to zero, so a view scrolled into the
    // middle of a sequence lands at the earliest time it is showing instead of
    // jumping to a start that is nowhere on screen.
    const double at = std::max(0.0, scale_.to_time(std::max(x, time.x) - time.x));

    // And the start gets a catch of its own, wider than any other — see
    // `kStartSnapDistance`. Off with snapping off, like every other snap.
    const bool near_start = snapping_ && scale_.width_of(at) <= kStartSnapDistance;
    return DropPoint{.track = track, .time = near_start ? 0.0 : at};
  }
  return std::nullopt;
}

void TimelineView::set_drop_ghost(std::optional<DropGhost> ghost) {
  drop_ghost_ = std::move(ghost);
}

Rect TimelineView::drop_ghost_rect() const {
  if (!drop_ghost_.has_value()) return {};
  if (drop_ghost_->track >= model_.tracks.size()) return {};

  const Rect row = track_rect(drop_ghost_->track);
  if (row.empty()) return {};

  // A minimum width for the same reason a block has one: a clip a few frames
  // long is still something you have to be able to see yourself dropping.
  const double width = std::max(kMinBlockWidth, scale_.width_of(drop_ghost_->duration));
  return Rect{row.x + scale_.to_x(drop_ghost_->start), row.y, width, row.height};
}

double TimelineView::trim_handle_width(std::size_t track, std::size_t block) const {
  const Rect box = block_rect(track, block);
  return std::min(kTrimHandle, box.width / 3.0);
}

DragMode TimelineView::zone_at(double x, double y, Modifiers modifiers) const {
  // A transition first, and with the selection tool only. It sits over the join
  // and so over two trim handles; answering with one of those would both give
  // the wrong cursor and make a transition impossible to grab. The other tools
  // each mean something specific about a clip, and none of them mean this.
  if (tool_ == Tool::Selection && transition_at(x, y).has_value()) {
    return DragMode::TransitionLength;
  }

  const std::optional<BlockRef> hit = block_at(x, y);
  if (!hit.has_value()) return DragMode::None;

  const Rect box = block_rect(hit->track, hit->block);

  // What control turns an edge into. Shift with it is the roll, which is the
  // pair Premiere uses and the pair that makes sense together: one moves an
  // edge and takes the sequence with it, the other moves a join and takes
  // nothing.
  const bool ripple = modifiers.control && !modifiers.shift;
  const bool roll = modifiers.control && modifiers.shift;

  // The volume band comes first, and only under the selection tool. It sits on
  // top of the clip body, so a press that found it would otherwise be a move —
  // and the other tools each mean one specific thing everywhere on a clip,
  // which is what makes them usable without hunting for a zone.
  if (tool_ == Tool::Selection && !modifiers.control) {
    // The fade handles first. With no fade set they sit exactly on the clip's
    // corners, which is where the trim handles are, and a corner that trimmed
    // instead of fading would leave the fades unreachable — where the trims are
    // still reachable everywhere below the handle.
    if (fade_handle_rect(hit->track, hit->block, false).contains(x, y)) {
      return DragMode::FadeIn;
    }
    if (fade_handle_rect(hit->track, hit->block, true).contains(x, y)) {
      return DragMode::FadeOut;
    }

    // A point next: it sits on the line, so a press near both means the point,
    // which is the more precise of the two and the one being aimed at.
    if (gain_point_at(x, y).has_value()) return DragMode::GainPointDrag;
    if (over_gain_band(x, y)) {
      const std::optional<GainBand>& band = model_.tracks[hit->track].blocks[hit->block].gain;
      return band->points.empty() ? DragMode::GainLevel : DragMode::GainSegment;
    }
  }

  switch (tool_) {
    case Tool::Razor:
      return DragMode::Razor;
    case Tool::Slip:
      return DragMode::Slip;
    case Tool::Slide:
      return DragMode::Slide;

    // Halves rather than the trim handles. A rate stretch has nothing to do in
    // the middle of a clip, so leaving a dead zone there would mean a tool that
    // ignores most of what it is pointed at; whichever end is nearer is what
    // was meant anyway. The same for the two edge tools below, which also mean
    // one thing and only ever act on an end.
    case Tool::RateStretch:
      return x < box.x + box.width * 0.5 ? DragMode::RateStart : DragMode::RateEnd;

    case Tool::Ripple:
      return x < box.x + box.width * 0.5 ? DragMode::RippleStart : DragMode::RippleEnd;

    case Tool::Roll:
      return x < box.x + box.width * 0.5 ? DragMode::RollStart : DragMode::RollEnd;

    case Tool::Selection:
      break;
  }

  const double handle = trim_handle_width(hit->track, hit->block);
  const bool at_start = x < box.x + handle;
  const bool at_end = x >= box.right() - handle;

  // Held anywhere on the clip rather than only on a handle. A ripple or a roll
  // is aimed at an *edit point*, and asking for the nearer one is what every
  // edge tool here already does — a modifier that worked in a four-pixel strip
  // and nowhere else would be a modifier nobody could find.
  if (ripple || roll) {
    const bool start = at_start || (!at_end && x < box.x + box.width * 0.5);
    if (ripple) return start ? DragMode::RippleStart : DragMode::RippleEnd;
    return start ? DragMode::RollStart : DragMode::RollEnd;
  }

  if (at_start) return DragMode::TrimStart;
  if (at_end) return DragMode::TrimEnd;
  return DragMode::Move;
}

std::optional<BlockRef> TimelineView::transition_at(double x, double y) const {
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
    for (std::size_t block = 0; block < blocks.size(); ++block) {
      const Rect box = transition_rect(track, block);
      if (!box.empty() && box.contains(x, y)) return BlockRef{.track = track, .block = block};
    }
  }
  return std::nullopt;
}

Rect TimelineView::marquee() const {
  if (mode_ != DragMode::Marquee || !moved_) return {};

  const double left = std::min(press_x_, marquee_x_);
  const double top = std::min(press_y_, marquee_y_);
  const Rect swept{left, top, std::abs(marquee_x_ - press_x_), std::abs(marquee_y_ - press_y_)};

  // Never outside the tracks: a sweep that ran up into the ruler would be drawn
  // over the timecodes and would look like it was selecting them.
  const Rect area = tracks_area();
  const double x0 = std::max(swept.x, area.x);
  const double y0 = std::max(swept.y, area.y);
  const double x1 = std::min(swept.right(), area.right());
  const double y1 = std::min(swept.bottom(), area.bottom());
  if (x1 <= x0 || y1 <= y0) return {};
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

std::vector<BlockRef> TimelineView::blocks_touching(const Rect& area) const {
  std::vector<BlockRef> found;
  if (area.empty()) return found;

  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const Rect row = track_rect(track);
    if (row.empty() || row.bottom() <= area.y || row.y >= area.bottom()) continue;

    for (std::size_t i = 0; i < model_.tracks[track].blocks.size(); ++i) {
      const Rect box = block_rect(track, i);
      if (box.empty()) continue;
      // Overlap rather than containment. A clip wider than the window can never
      // be enclosed by a rectangle drawn inside it, and being unable to sweep
      // up the long clip is exactly when somebody would reach for a sweep.
      if (box.right() <= area.x || box.x >= area.right()) continue;
      found.push_back(BlockRef{track, i});
    }
  }
  return found;
}

std::vector<BlockRef> TimelineView::selection() const {
  std::vector<BlockRef> chosen;
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i].selected) chosen.push_back(BlockRef{track, i});
    }
  }
  return chosen;
}

std::optional<BlockRef> TimelineView::first_selected() const {
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i].selected) return BlockRef{track, i};
    }
  }
  return std::nullopt;
}

void TimelineView::select(std::optional<BlockRef> block) {
  if (!block.has_value()) return select(std::span<const BlockRef>{});
  const std::array<BlockRef, 1> one{*block};
  select(std::span<const BlockRef>{one});
}

void TimelineView::select(std::span<const BlockRef> blocks) {
  for (TimelineTrack& track : model_.tracks) {
    for (TimelineBlock& clip : track.blocks) clip.selected = false;
  }
  for (const BlockRef& block : blocks) {
    if (block.track >= model_.tracks.size()) continue;
    std::vector<TimelineBlock>& row = model_.tracks[block.track].blocks;
    if (block.block < row.size()) row[block.block].selected = true;
  }
}

// --------------------------------------------------------------- layout --

void TimelineView::layout(const LayoutContext& context) {
  metrics_ = context.metrics();
  refresh_bounds();
}

// -------------------------------------------------------------- painting --

void TimelineView::paint_content(Painter& painter, const Theme& theme) const {
  const Rect ruler = ruler_area();
  const Rect tracks = tracks_area();
  const Rect headers = header_area();

  // ---- the tracks, clipped so blocks do not run out over the headers
  painter.push_clip(tracks, 0.0);

  // Where an alt-drag's originals will be left, under the clips so a copy that
  // has not travelled far yet still reads as the thing on top.
  //
  // Drawn from the arrangement at the press, which is the only place that still
  // knows: the model shows the blocks already moved, because the preview drags
  // the originals rather than inventing copies to drag. Faint and outlined
  // rather than solid — this is a promise about the release, not a clip that
  // exists yet.
  if (duplicating_ && moved_) {
    for (const Moving& carried : moving_) {
      if (carried.ref.track >= press_model_.tracks.size()) continue;
      const Rect row = track_rect(carried.ref.track);
      if (row.bottom() < tracks.y || row.y > tracks.bottom()) continue;

      const Rect box{row.x + scale_.to_x(carried.origin.start), row.y,
                     std::max(kMinBlockWidth, scale_.width_of(carried.origin.duration())),
                     row.height};
      if (box.right() < tracks.x || box.x > tracks.right()) continue;

      const SurfaceStyle& style = theme.style(Part::Clip, State::Normal);
      painter.fill(box.inset(1.0), style.corner_radius,
                   Fill::solid(fade(style.text, 0.12f)));
      painter.stroke(box.inset(1.0), style.corner_radius, fade(style.text, 0.5f), 1.0);
    }
  }

  // Where a clip dragged in from elsewhere would land, drawn *over* the clips.
  //
  // Under them was the first answer, on the grounds that a promise should not
  // hide what is already there — and driving showed what is wrong with it: a
  // track with a clip across the whole visible span hid the ghost completely,
  // which is exactly the case where somebody most needs to see where a drop
  // will go. Over the top and mostly transparent shows both.
  const auto paint_drop_ghost = [&] {
    const Rect landing = drop_ghost_rect();
    if (landing.empty() || landing.right() < tracks.x || landing.x > tracks.right()) return;

    const SurfaceStyle& style = theme.style(Part::Clip, State::Normal);
    painter.fill(landing.inset(1.0), style.corner_radius, Fill::solid(fade(style.fill.color, 0.55f)));
    painter.stroke(landing.inset(1.0), style.corner_radius, fade(style.text, 0.75f), 1.0);

    // The leading edge picked out, because where it *starts* is the thing being
    // aimed and a soft-edged rectangle does not say precisely where that is.
    painter.line(landing.x, landing.y, landing.x, landing.bottom(),
                 theme.style(Part::Playhead, State::Normal).fill.color, 1.5);

    if (!drop_ghost_->label.empty() && landing.width > metrics_.padding_x * 3.0) {
      const Rect text{landing.x + metrics_.padding_x * 0.5, landing.y,
                      landing.width - metrics_.padding_x, landing.height};
      painter.text(text_run(text, drop_ghost_->label, style, metrics_.small_font_size,
                            TextAlign::Left, false));
    }
  };

  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const Rect row = track_rect(track);
    if (row.bottom() < tracks.y || row.y > tracks.bottom()) continue;

    for (std::size_t i = 0; i < model_.tracks[track].blocks.size(); ++i) {
      const TimelineBlock& clip = model_.tracks[track].blocks[i];
      const Rect box = block_rect(track, i);
      // Off-screen either side. Cheap to skip and worth it: a long project has
      // far more clips out of view than in it.
      if (box.right() < tracks.x || box.x > tracks.right()) continue;

      // Disabled wins over selected: a clip can be both, and which of the two
      // is worth saying is the one that changes what comes out of the render.
      const State state = clip.disabled  ? State::Disabled
                          : clip.selected ? State::Selected
                                          : State::Normal;
      const SurfaceStyle& style = theme.style(Part::Clip, state);

      // A label replaces the fill and nothing else — the border, the bevel and
      // the text stay the theme's, so a labelled clip still reads as selected
      // or disabled rather than becoming a flat rectangle that has lost every
      // other thing it was saying.
      if (clip.color.empty()) {
        paint_surface(painter, box.inset(1.0), style);
      } else {
        SurfaceStyle labelled = style;
        labelled.fill = Fill::solid(parse_color(clip.color, style.text));
        paint_surface(painter, box.inset(1.0), labelled);
      }

      // The clip a drop is about to land on. Outlined rather than filled: the
      // point is to say *which* one receives it, and repainting the block would
      // hide the picture somebody is aiming at.
      if (drop_target_.has_value() && drop_target_->track == track &&
          drop_target_->block == i) {
        painter.stroke(box.inset(1.0), style.corner_radius, theme.accent, 2.0);
      }

      // The filmstrip first, under everything, for the same reason the waveform
      // is: it is the clip's picture and the label reads over the top of it.
      //
      // Tiled left to right, each tile showing the frame nearest the source
      // time under it. Only the tiles on screen are drawn — a ten-minute clip
      // scrolled mostly out of view costs what is visible of it.
      if (const Rect strip_box = filmstrip_area(track, i); !strip_box.empty()) {
        const Filmstrip& strip = *clip.filmstrip;
        painter.push_clip(box, style.corner_radius);

        // Sized by the frames themselves, so a filmstrip of mixed sources still
        // lines up and a vertical video does not come out stretched.
        const double aspect = strip.frames.front().aspect();
        const double tile = std::max(8.0, strip_box.height * (aspect > 0.0 ? aspect : 1.6));

        const double from = std::max(strip_box.x, tracks.x);
        const double to = std::min(strip_box.right(), tracks.right());
        // Started at a whole tile from the block's own start rather than from
        // the edge of the screen, so scrolling slides the strip instead of
        // reshuffling which frame is in which tile.
        const double first = strip_box.x + std::floor((from - strip_box.x) / tile) * tile;

        for (double x = first; x < to; x += tile) {
          const double centre = (x + tile * 0.5 - strip_box.x) / scale_.pixels_per_second;
          const FilmFrame* frame = strip.nearest(source_time_of(clip, centre));
          if (frame == nullptr) continue;
          painter.image(Rect{x, strip_box.y, tile, strip_box.height},
                        ImageView{.pixels = frame->rgba.data(),
                                  .width = frame->width,
                                  .height = frame->height});
        }
        painter.pop_clip();
      }

      // The waveform next, under everything else: it is the clip's picture, and
      // the label and the volume band read over the top of it.
      //
      // A column per pixel rather than a shape through the buckets. What it
      // costs is what is on screen, not how long the clip is — a ten-minute
      // source zoomed out to a centimetre draws a centimetre's worth of
      // columns, and the envelope it draws them from is shared rather than
      // sliced per clip.
      if (const Rect wave_box = waveform_area(track, i); !wave_box.empty()) {
        const Waveform& wave = *clip.waveform;
        painter.push_clip(box, style.corner_radius);

        Color ink = style.text;
        ink.a *= 0.4f;
        const double middle = wave_box.y + wave_box.height * 0.5;
        const double reach = wave_box.height * 0.5;

        // Only the part of the block that is actually on screen.
        const double from = std::max(wave_box.x, tracks.x);
        const double to = std::min(wave_box.right(), tracks.right());

        for (double x = std::floor(from); x < to; x += 1.0) {
          const double local_t = (x - wave_box.x) / scale_.pixels_per_second;
          const double source_t = source_time_of(clip, local_t);
          const auto bucket =
              static_cast<std::ptrdiff_t>(std::floor(source_t * wave.buckets_per_second));
          if (bucket < 0 || static_cast<std::size_t>(bucket) >= wave.size()) continue;

          const double low = std::clamp(static_cast<double>(wave.minimum[bucket]), -1.0, 1.0);
          const double high = std::clamp(static_cast<double>(wave.maximum[bucket]), -1.0, 1.0);
          // Up is positive, which is only a convention but the universal one.
          const double top = middle - high * reach;
          const double bottom = middle - low * reach;
          // A silent bucket is still a clip, so it keeps a centre line rather
          // than a gap that reads as a hole in the audio.
          painter.line(x, top, x, std::max(bottom, top + 1.0), ink, 1.0);
        }
        painter.pop_clip();
      }

      // Only worth a label if there is room for one to be read.
      if (!clip.label.empty() && box.width > metrics_.font_size * 2.0) {
        painter.push_clip(box, style.corner_radius);
        const Rect text = inset(box, Edges::symmetric(metrics_.padding_y * 1.5, 0.0));
        painter.text(text_run(text, clip.label, style, metrics_.small_font_size,
                              TextAlign::Left, false));
        painter.pop_clip();
      }

      // The volume band, over the clip body and under its keyframe marks.
      //
      // Drawn on every audio clip rather than only the selected one: a line
      // that appears when a clip is clicked says nothing while the sequence is
      // being read, and which clips have been rewritten is exactly what somebody
      // scanning a mix wants to see.
      if (const Rect band_area = gain_area(track, i); !band_area.empty()) {
        const GainBand& band = *clip.gain;
        painter.push_clip(box, style.corner_radius);

        const Color line = style.text;
        const std::vector<GainPoint>& points = band.points;

        if (points.empty()) {
          const double y = gain_to_y(track, i, band.level);
          painter.line(band_area.x, y, band_area.right(), y, line, 1.5);
        } else {
          // Flat out to the first point and out from the last, because that is
          // how the value is evaluated: outside the keyframes it holds. Drawing
          // the segments only would leave the ends of the clip looking
          // unautomated when they are the parts that never change.
          const double first_x = band_area.x + points.front().t * scale_.pixels_per_second;
          const double first_y = gain_to_y(track, i, points.front().v);
          if (first_x > band_area.x) {
            painter.line(band_area.x, first_y, first_x, first_y, line, 1.5);
          }

          for (std::size_t p = 0; p + 1 < points.size(); ++p) {
            const double x0 = band_area.x + points[p].t * scale_.pixels_per_second;
            const double x1 = band_area.x + points[p + 1].t * scale_.pixels_per_second;
            if (x1 < band_area.x || x0 > band_area.right()) continue;
            painter.line(x0, gain_to_y(track, i, points[p].v), x1,
                         gain_to_y(track, i, points[p + 1].v), line, 1.5);
          }

          const double last_x = band_area.x + points.back().t * scale_.pixels_per_second;
          const double last_y = gain_to_y(track, i, points.back().v);
          if (last_x < band_area.right()) {
            painter.line(last_x, last_y, band_area.right(), last_y, line, 1.5);
          }

          for (std::size_t p = 0; p < points.size(); ++p) {
            const Rect dot = gain_point_rect(track, i, p);
            if (dot.empty() || dot.right() < box.x || dot.x > box.right()) continue;
            painter.fill(dot, kGainPointRadius, Fill::solid(line));
          }
        }
        painter.pop_clip();
      }

      // A stripe along the top for a labelled clip, over the filmstrip rather
      // than under it. The fill alone is invisible on a video clip: the
      // filmstrip tiles the whole body and covers it, so a label put on to find
      // a shot again could not be seen on exactly the clips most worth
      // labelling.
      if (!clip.color.empty()) {
        const double stripe = std::min(kLabelStripe, box.height * 0.25);
        painter.fill(Rect{box.x + 1.0, box.y + 1.0, std::max(0.0, box.width - 2.0), stripe},
                     0.0, Fill::solid(parse_color(clip.color, style.text)));
      }

      // The effects badge, at the top left where Premiere puts it. Two letters
      // rather than a mark: "fx" is what the panel is called and what everybody
      // already reads it as, and no drawing at eight pixels says "there is a
      // stack on this" as plainly as the word does.
      if (clip.has_effects) {
        const double badge = std::min(kEffectBadge, box.height - 2.0);
        const Rect at{box.x + 2.0, box.y + 2.0, badge * 1.4, badge};
        // Only where it fits beside the label rather than over it. A clip too
        // short to hold both keeps the badge, which is the smaller of the two
        // and the one that cannot be worked out from anything else on screen.
        if (at.right() < box.right() - 1.0 && badge > 4.0) {
          painter.fill(at, 2.0, Fill::solid(fade(style.text, 0.22f)));
          painter.text(text_run(at, "fx", style, metrics_.small_font_size,
                                TextAlign::Center, false));
        }
      }

      // The fades, as a ramp down to the corner the clip arrives or leaves at.
      //
      // Drawn even when the fade is zero, because the handle is what says the
      // gesture exists — a control that only appears once you have used it is
      // one nobody finds. At zero the ramp has no width and only the handle
      // shows.
      for (const bool out_edge : {false, true}) {
        const Rect grip = fade_handle_rect(track, i, out_edge);
        if (grip.empty()) continue;

        const double length = out_edge ? clip.fade_out : clip.fade_in;
        painter.push_clip(box, style.corner_radius);
        if (length > 0.0) {
          // From the far corner up to where the fade finishes: the shape of the
          // level coming up or going down, which is what a fade is.
          const double tip = grip.x + grip.width * 0.5;
          const double corner = out_edge ? box.right() : box.x;
          painter.line(corner, box.bottom() - 1.0, tip, box.y + 1.0, style.text, 1.0);
        }
        painter.fill(grip.inset(1.0), 2.0, Fill::solid(style.text));
        painter.pop_clip();
      }

      // Keyframes, as small diamonds along the foot of the block, so an
      // animation is visible where the clip is rather than only in the
      // inspector one parameter at a time. Clipped to the block, or one at the
      // very end spills onto the clip after it.
      if (!clip.keyframes.empty()) {
        painter.push_clip(box, style.corner_radius);
        constexpr double reach = 3.0;
        const double y = box.bottom() - reach - 2.0;
        for (const double at : clip.keyframes) {
          const double x = box.x + at * scale_.pixels_per_second;
          if (x < box.x - reach || x > box.right() + reach) continue;
          painter.line(x, y - reach, x + reach, y, style.text, 1.0);
          painter.line(x + reach, y, x, y + reach, style.text, 1.0);
          painter.line(x, y + reach, x - reach, y, style.text, 1.0);
          painter.line(x - reach, y, x, y - reach, style.text, 1.0);
        }
        painter.pop_clip();
      }
    }

    // Transitions last on this track, so one always draws over both the clips
    // it joins rather than half of it disappearing under the next block.
    for (std::size_t i = 0; i < model_.tracks[track].blocks.size(); ++i) {
      const Rect box = transition_rect(track, i);
      if (box.empty() || box.right() < tracks.x || box.x > tracks.right()) continue;

      const SurfaceStyle& style = theme.style(Part::Clip, State::Selected);
      Fill wash = style.fill;
      wash.color.a *= 0.75f;
      painter.fill(box.inset(1.0), style.corner_radius, wash);
      // A diagonal, which is what every editor draws a transition as: it says
      // "one becomes the other across here" in a way no colour does.
      painter.line(box.x + 1.0, box.bottom() - 1.0, box.right() - 1.0, box.y + 1.0,
                   style.text, 1.0);
      painter.stroke(box.inset(1.0), style.corner_radius, style.text, 1.0);

      // Measured rather than guessed at. A name centred in a box too small for
      // it overflows both ends, and what is left after clipping is a word with
      // its first and last letters missing sitting on top of the clip's own
      // label. Nothing at all reads better than that.
      const TimelineBlock& clip = model_.tracks[track].blocks[i];
      if (!clip.transition.label.empty() &&
          painter.measure(clip.transition.label, metrics_.small_font_size, false) <=
              box.width - 4.0) {
        painter.text(text_run(box, clip.transition.label, style, metrics_.small_font_size,
                              TextAlign::Center, false));
      }
    }

    // And the one being held over a cut, drawn where it would land and at the
    // length it would be. Outlined rather than filled, and after the real ones,
    // so it reads as a promise rather than as something already there.
    if (transition_ghost_.has_value() && transition_ghost_->track == track) {
      const Rect box = transition_ghost_rect();
      if (!box.empty() && box.right() >= tracks.x && box.x <= tracks.right()) {
        const SurfaceStyle& style = theme.style(Part::Clip, State::Selected);
        const Color ink = fade(style.text, 0.85f);
        painter.fill(box.inset(1.0), style.corner_radius,
                     Fill::solid(fade(style.fill.color, 0.35f)));
        painter.line(box.x + 1.0, box.bottom() - 1.0, box.right() - 1.0, box.y + 1.0, ink,
                     1.0);
        painter.stroke(box.inset(1.0), style.corner_radius, ink, 1.0);

        if (!transition_ghost_->label.empty() &&
            painter.measure(transition_ghost_->label, metrics_.small_font_size, false) <=
                box.width - 4.0) {
          painter.text(text_run(box, transition_ghost_->label, style,
                                metrics_.small_font_size, TextAlign::Center, false));
        }
      }
    }
  }

  // Last, and inside the tracks' clip, so it lies over whatever is already
  // there and stops at the track headers like everything else.
  paint_drop_ghost();
  painter.pop_clip();

  // ---- what the drag has snapped to, over the clips so it can be seen on one
  //
  // A clip that clicked into place against its neighbour and one that happened
  // to land a pixel away looked identical, which made snapping something to be
  // trusted rather than seen.
  if (snapped_.has_value() && mode_ != DragMode::None) {
    const double at = time_area().x + scale_.to_x(*snapped_);
    painter.push_clip(tracks, 0.0);
    painter.line(at, tracks.y, at, tracks.bottom(), theme.accent, 1.0);
    painter.pop_clip();
  }

  // ---- the zone under the pointer, over the clips and under everything else
  //
  // What a press would do, said before it is done. The timeline was the only
  // interactive surface in the application with no hover feedback at all: every
  // button, menu row, splitter and tab lights up, and the one place where a
  // single pixel decides between moving a clip and trimming it said nothing.
  if (pointer_.has_value() && mode_ == DragMode::None) {
    // Clipped like the blocks it sits on. A clip scrolled half under the header
    // column has half a handle, and a highlight drawn over the headers would be
    // pointing at something that is not there.
    painter.push_clip(tracks, 0.0);
    const auto [px, py] = *pointer_;
    if (const std::optional<BlockRef> hit = block_at(px, py); hit.has_value()) {
      const Rect box = block_rect(hit->track, hit->block);
      const DragMode zone = zone_at(px, py, hover_modifiers_);
      const SurfaceStyle& style = theme.style(Part::Clip, State::Hover);

      if (zone == DragMode::Razor) {
        // Where the cut would land. A razor is aimed at a *place* rather than
        // at a clip, and a tool whose whole gesture is one click is the one
        // that most needs to say where it would take effect.
        painter.line(px, box.y, px, box.bottom(), style.text, 1.0);
      } else if (pulls_start(zone) || pulls_end(zone)) {
        const double handle = trim_handle_width(hit->track, hit->block);
        const double x = pulls_start(zone) ? box.x : box.right() - handle;
        painter.fill(Rect{x, box.y + 1.0, handle, std::max(0.0, box.height - 2.0)},
                     style.corner_radius, Fill::solid(fade(style.text, 0.35f)));
      }
    }
    painter.pop_clip();
  }

  // ---- the ruler
  const SurfaceStyle& ruler_style = theme.style(Part::Ruler, State::Normal);
  paint_surface(painter, ruler, ruler_style);

  painter.push_clip(ruler, 0.0);
  const double from = scale_.start;
  const double to = scale_.to_time(ruler.width);
  for (const Tick& tick : ruler_ticks(scale_, from, to, model_.fps)) {
    const double x = ruler.x + scale_.to_x(tick.time);
    // Major ticks reach further down, so the eye can find the labelled ones
    // without reading any of them.
    const double height = tick.major ? ruler.height * 0.5 : ruler.height * 0.25;
    painter.line(x, ruler.bottom() - height, x, ruler.bottom(),
                 tick.major ? ruler_style.text : fade(ruler_style.text, 0.45f), 1.0);

    if (!tick.major) continue;
    const Rect label{x + 3.0, ruler.y, 100.0, ruler.height * 0.6};
    painter.text(text_run(label, core::seconds_to_timecode(tick.time, model_.fps, model_.drop_frame), ruler_style,
                          metrics_.small_font_size, TextAlign::Left, false));
  }

  // Inside the ruler's clip, so a span running off either end is cut at the
  // edge of the ruler rather than drawn across the track headers.
  if (const Rect marked = marked_bar(); !marked.empty()) {
    // The playhead's colour: this is the other thing on the ruler that says
    // where something is, and a second accent would be one too many.
    painter.fill(marked, 1.0, theme.style(Part::Playhead, State::Normal).fill);
  }

  for (std::size_t i = 0; i < model_.markers.size(); ++i) {
    const Rect tab = marker_rect(i);
    const Rect band = marker_span(i);
    if (tab.empty() && band.empty()) continue;

    // Its own colour when it has one. That is what a marker's colour is for —
    // somebody has said this one means something the others do not.
    const Color color = model_.markers[i].color.empty()
                            ? ruler_style.text
                            : parse_color(model_.markers[i].color, ruler_style.text);

    // The span first and faded, so the tab that names it still stands out
    // against it — a band at full strength would swallow its own head.
    if (!band.empty()) painter.fill(band, 2.0, Fill::solid(fade(color, 0.35f)));
    if (tab.empty()) continue;
    painter.fill(tab, 2.0, Fill::solid(color));
    // A stem down to the ticks, so a marker can be lined up against a time
    // rather than only noticed.
    const double centre = tab.x + tab.width * 0.5;
    painter.line(centre, tab.bottom(), centre, ruler.bottom(), fade(color, 0.6f), 1.0);
  }
  painter.pop_clip();

  // ---- the track headers, over the tracks so nothing scrolls out from under
  painter.push_clip(Rect{headers.x, tracks.y, headers.width, tracks.height}, 0.0);
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const Rect box = header_rect(track);
    if (box.empty() || box.bottom() < tracks.y || box.y > tracks.bottom()) continue;

    const SurfaceStyle& style =
        theme.style(Part::TrackHeader, model_.tracks[track].muted ? State::Disabled
                                                                  : State::Normal);
    paint_surface(painter, box, style);

    // The name on the first line rather than centred, so it does not collide
    // with the row of switches beneath it.
    const Rect text{box.x + metrics_.padding_x, box.y + metrics_.padding_y,
                    std::max(0.0, box.width - 2.0 * metrics_.padding_x),
                    metrics_.font_size * metrics_.line_height};
    painter.text(text_run(text, model_.tracks[track].name, style, metrics_.font_size,
                          TextAlign::Left, true));

    const TrackSwitches& on = model_.tracks[track].switches;
    for (const TrackControl control : {TrackControl::Target, TrackControl::Mute,
                                      TrackControl::Solo, TrackControl::Lock,
                                      TrackControl::Hide}) {
      const Rect box_of = control_rect(track, control);
      if (box_of.empty()) continue;

      const bool lit = control == TrackControl::Target ? on.target
                       : control == TrackControl::Mute  ? on.mute
                       : control == TrackControl::Solo  ? on.solo
                       : control == TrackControl::Lock  ? on.lock
                                                        : on.hide;
      const SurfaceStyle& switch_style =
          theme.style(Part::ToolButton, lit ? State::Selected : State::Normal);
      paint_surface(painter, box_of, switch_style);
      if (!lit) {
        // An outline of its own when it is off. A tool button's resting state is
        // transparent in every theme — which is right in a toolbar, where the
        // row itself frames the buttons, and wrong here, where it leaves three
        // letters floating in a header looking like a label rather than three
        // things to press.
        Color edge = switch_style.text;
        edge.a *= 0.35f;
        painter.stroke(box_of, switch_style.corner_radius, edge, 1.0);
      }
      painter.text(text_run(box_of, std::string(to_string(control)), switch_style,
                            metrics_.small_font_size, TextAlign::Center, true));
    }
  }
  painter.pop_clip();

  // ---- the sweep, over the clips it is gathering and under the playhead
  if (const Rect swept = marquee(); !swept.empty()) {
    const SurfaceStyle& style = theme.style(Part::Clip, State::Selected);
    Fill wash = style.fill;
    wash.color.a *= 0.25f;
    painter.push_clip(tracks, 0.0);
    painter.fill(swept, 0.0, wash);
    painter.stroke(swept, 0.0, style.text, 1.0);
    painter.pop_clip();
  }

  // ---- the scrollbars, which say where in the sequence this is
  //
  // The horizontal one is the zoom control as well. Premiere's is, and the two
  // questions are really one: how much of the sequence am I looking at, and
  // which part. Without either of them a long sequence gives no clue where the
  // view sits in it — the wheel moved, and that was the whole of the answer.
  if (const Rect bar = scroll_area(); !bar.empty()) {
    paint_surface(painter, bar, theme.style(Part::Scrollbar, State::Normal));

    const Rect thumb = scroll_thumb();
    const State state = mode_ == DragMode::ScrollTime || mode_ == DragMode::ZoomStart ||
                                mode_ == DragMode::ZoomEnd
                            ? State::Pressed
                            : State::Normal;
    const SurfaceStyle& style = theme.style(Part::ScrollThumb, state);
    paint_surface(painter, thumb.inset(1.0), style);

    // A notch on each grip, so the ends read as ends rather than as the edge of
    // the thumb. Two lines is what every resize grip in the system is.
    for (const bool at_end : {false, true}) {
      const Rect grip = zoom_grip(at_end);
      if (grip.empty()) continue;
      const double cx = grip.x + grip.width * 0.5;
      const double inset = grip.height * 0.25;
      painter.line(cx - 1.0, grip.y + inset, cx - 1.0, grip.bottom() - inset, style.text, 1.0);
      painter.line(cx + 1.0, grip.y + inset, cx + 1.0, grip.bottom() - inset, style.text, 1.0);
    }
  }

  if (const Rect bar = track_scroll_area(); !bar.empty()) {
    paint_surface(painter, bar, theme.style(Part::Scrollbar, State::Normal));
    paint_surface(painter, track_scroll_thumb().inset(1.0),
                  theme.style(Part::ScrollThumb, mode_ == DragMode::ScrollTracks
                                                     ? State::Pressed
                                                     : State::Normal));
  }

  // ---- the playhead, last and over everything, clipped to the time area so
  // it never draws across the headers
  const double x = playhead_x();
  const Rect area = time_area();
  if (x >= area.x && x <= area.right()) {
    const SurfaceStyle& style = theme.style(Part::Playhead, State::Normal);
    painter.push_clip(area, 0.0);
    painter.line(x, area.y, x, area.bottom(), style.fill.color, 1.0);
    // A head on it, so it can be grabbed at any zoom rather than hunted for.
    painter.fill(Rect{x - 5.0, ruler.y, 10.0, ruler.height * 0.4}, 2.0, style.fill);
    painter.pop_clip();
  }
}

// -------------------------------------------------------------- behaviour --

void TimelineView::scrub_to(double x) {
  const Rect area = time_area();
  set_playhead(scale_.to_time(x - area.x));
  if (on_scrub_) on_scrub_(playhead_);
}

void TimelineView::capture_neighbours() {
  before_.reset();
  after_.reset();
  if (!drag_.has_value()) return;

  const std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (i == drag_->block) continue;
    // Abutting only. A clip with a gap beside it has nothing to take the length
    // out of, which is what the core says too — and if the view disagreed it
    // would preview a slide the project then refused.
    if (std::abs(blocks[i].end - origin_.start) < kAbutEps) {
      before_ = Neighbour{.index = i, .origin = blocks[i]};
    } else if (std::abs(origin_.end - blocks[i].start) < kAbutEps) {
      after_ = Neighbour{.index = i, .origin = blocks[i]};
    }
  }
}

void TimelineView::sync_group(double start_delta, double end_delta) {
  if (!drag_.has_value() || origin_.group.empty()) return;

  // From the arrangement at the press rather than from where the blocks are
  // now, so a long drag cannot accumulate — the rule every other drag here
  // follows, applied to the clips travelling with the one being held.
  for (std::size_t track = 0; track < model_.tracks.size() && track < press_model_.tracks.size();
       ++track) {
    std::vector<TimelineBlock>& row = model_.tracks[track].blocks;
    const std::vector<TimelineBlock>& was = press_model_.tracks[track].blocks;
    for (std::size_t i = 0; i < row.size() && i < was.size(); ++i) {
      if (track == drag_->track && i == drag_->block) continue;
      if (was[i].group != origin_.group) continue;
      row[i].start = was[i].start + start_delta;
      row[i].end = was[i].end + end_delta;
    }
  }
}

void TimelineView::capture_downstream(double from) {
  moving_.clear();
  if (!drag_.has_value()) return;

  // Every track, not only the one being dragged: a ripple that moved the
  // picture and left the sound where it was would put the whole sequence out
  // of sync from that point on, which is the fault the core avoids too.
  //
  // The dragged clip itself is left out. What happens to it is the trim, and
  // the ripple is what happens to everything else.
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const std::vector<TimelineBlock>& row = model_.tracks[track].blocks;
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (track == drag_->track && i == drag_->block) continue;
      // The linked group is not traffic to be pushed along: it is being
      // trimmed too, and `sync_group` is what moves it. Carrying it here as
      // well would move it twice.
      if (!origin_.group.empty() && row[i].group == origin_.group) continue;
      if (row[i].start < from - kAbutEps) continue;
      moving_.push_back(Moving{.ref = BlockRef{.track = track, .block = i}, .origin = row[i]});
    }
  }
}

std::optional<TimelineView::Neighbour> TimelineView::capture_join(bool at_start) {
  if (!drag_.has_value()) return std::nullopt;

  const std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (i == drag_->block) continue;
    // Abutting only. An edge with a gap beside it, or the end of the track, is
    // not a join and there is nothing on the other side of it to roll into —
    // which is what the core says as well.
    const double meets = at_start ? blocks[i].end : blocks[i].start;
    const double edge = at_start ? origin_.start : origin_.end;
    if (std::abs(meets - edge) < kAbutEps) {
      return Neighbour{.index = i, .origin = blocks[i]};
    }
  }
  return std::nullopt;
}

void TimelineView::slide_to(double moved, double frame) {
  // Nothing abutting means nothing to slide against, which is what the core
  // says too. Moving the clip anyway would preview an edit the project then
  // refuses, and the view would visibly snap back on release.
  if (!before_.has_value() && !after_.has_value()) return;

  std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;

  // Bounded by what the neighbours can give up. The clip that grows has no
  // limit here — the core knows how much source is left, and this does not —
  // but the one that shrinks must keep a frame, or it disappears and there is
  // nothing left to slide back into.
  double lo = -origin_.start;
  double hi = std::numeric_limits<double>::infinity();
  if (before_.has_value()) lo = std::max(lo, frame - before_->origin.duration());
  if (after_.has_value()) hi = std::min(hi, after_->origin.duration() - frame);
  if (lo > hi) return;

  const double delta = core::snap_to_frame(std::clamp(moved, lo, hi), model_.fps);

  blocks[drag_->block].start = origin_.start + delta;
  blocks[drag_->block].end = origin_.end + delta;
  // The one before grows into the gap and the one after gives it up, so the
  // three edges move together and the sequence keeps its length.
  if (before_.has_value()) blocks[before_->index].end = before_->origin.end + delta;
  if (after_.has_value()) blocks[after_->index].start = after_->origin.start + delta;
}

double gain_at(const GainBand& band, double t) noexcept {
  if (band.points.empty()) return band.level;
  if (t <= band.points.front().t) return band.points.front().v;
  if (t >= band.points.back().t) return band.points.back().v;

  for (std::size_t i = 0; i + 1 < band.points.size(); ++i) {
    const GainPoint& a = band.points[i];
    const GainPoint& b = band.points[i + 1];
    if (t < a.t || t > b.t) continue;
    const double span = b.t - a.t;
    if (!(span > 0.0)) return b.v;
    return a.v + (b.v - a.v) * ((t - a.t) / span);
  }
  return band.points.back().v;
}

void TimelineView::ensure_gain_anchors(BlockRef block,
                                       const std::vector<std::size_t>& segment) {
  gain_anchors_.clear();
  if (block.track >= model_.tracks.size() || segment.empty()) return;
  std::vector<TimelineBlock>& blocks = model_.tracks[block.track].blocks;
  if (block.block >= blocks.size() || !blocks[block.block].gain.has_value()) return;

  GainBand& band = *blocks[block.block].gain;
  if (band.points.empty()) return;

  const std::size_t first = segment.front();
  const std::size_t last = segment.back();
  if (first >= band.points.size() || last >= band.points.size()) return;

  // Only where there is something outside the stretch to protect. A run held by
  // a single end is the head or the tail of the clip, and dragging it is meant
  // to move that end.
  const bool guard_before = segment.size() > 1 || last + 1 == band.points.size();
  const bool guard_after = segment.size() > 1 || first == 0;

  const double frame = core::frame_duration(model_.fps);
  const double duration = blocks[block.block].duration();

  // Both values read before either point is added. `gain_value_at` walks the
  // list in time order, so adding one anchor and only then asking about the
  // other reads an unsorted band and answers with whatever is last in it.
  std::vector<GainPoint> wanted;
  const auto want = [&](double at) {
    if (at < 0.0 || at > duration) return;
    const bool there = std::ranges::any_of(band.points, [&](const GainPoint& point) {
      return std::abs(point.t - at) <= core::kKeyframeMatchEps;
    });
    if (!there) wanted.push_back(GainPoint{at, gain_at(band, at)});
  };
  if (guard_before) want(band.points[first].t - frame);
  if (guard_after) want(band.points[last].t + frame);

  for (const GainPoint& anchor : wanted) {
    band.points.push_back(anchor);
    gain_anchors_.push_back(anchor);
  }
  std::ranges::sort(band.points, {}, &GainPoint::t);
}

std::optional<std::size_t> TimelineView::add_gain_point(BlockRef block, double x) {
  if (block.track >= model_.tracks.size()) return std::nullopt;
  std::vector<TimelineBlock>& blocks = model_.tracks[block.track].blocks;
  if (block.block >= blocks.size() || !blocks[block.block].gain.has_value()) {
    return std::nullopt;
  }

  const Rect area = gain_area(block.track, block.block);
  if (area.empty()) return std::nullopt;

  GainBand& band = *blocks[block.block].gain;
  const double t = std::clamp(
      core::snap_to_frame((x - area.x) / scale_.pixels_per_second, model_.fps), 0.0,
      blocks[block.block].duration());

  // One already here. Adding a second at the same instant would give the
  // evaluator two answers for one moment, and the drag that follows would pick
  // between them by iteration order.
  for (const GainPoint& point : band.points) {
    if (std::abs(point.t - t) <= core::kKeyframeMatchEps) return std::nullopt;
  }

  const GainPoint added{t, gain_at(band, t)};
  band.points.push_back(added);
  std::ranges::sort(band.points, {}, &GainPoint::t);
  for (std::size_t i = 0; i < band.points.size(); ++i) {
    if (band.points[i] == added) return i;
  }
  return std::nullopt;
}

void TimelineView::gain_to(double x, double y) {
  if (!drag_.has_value()) return;
  std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
  if (drag_->block >= blocks.size() || !blocks[drag_->block].gain.has_value()) return;

  GainBand& band = *blocks[drag_->block].gain;

  if (mode_ == DragMode::GainLevel) {
    band.level = gain_at_y(drag_->track, drag_->block, y);
    return;
  }

  if (mode_ == DragMode::GainSegment) {
    // By how far the pointer has moved rather than to where it is: the stretch
    // being dragged has two ends at different levels, and setting both to the
    // pointer's height would flatten the ramp instead of moving it.
    //
    // The shift is in band fractions, which is decibels — so a ramp keeps its
    // shape and every point in it moves by the same number of decibels, which
    // is what "this bit is too loud" means.
    const Rect area = gain_area(drag_->track, drag_->block);
    if (area.empty() || area.height <= 0.0) return;
    const double shift = (press_y_ - y) / area.height;

    for (std::size_t i = 0; i < gain_segment_.size(); ++i) {
      const std::size_t at = gain_segment_[i];
      if (at >= band.points.size() || i >= gain_segment_origin_.size()) continue;
      const double from = gain_to_band(gain_segment_origin_[i], model_.max_gain);
      band.points[at].v = band_to_gain(std::clamp(from + shift, 0.0, 1.0), model_.max_gain);
    }
    return;
  }

  if (gain_point_ >= band.points.size()) return;

  // In time as well as in level. A point that could only move up and down would
  // have to be deleted and re-added to be a moment earlier, which is the sort of
  // thing that makes people leave the automation alone.
  const Rect area = gain_area(drag_->track, drag_->block);
  const double t = core::snap_to_frame((x - area.x) / scale_.pixels_per_second, model_.fps);

  GainPoint moved = band.points[gain_point_];
  moved.v = gain_at_y(drag_->track, drag_->block, y);
  // Never off its own clip: a keyframe outside the clip's length is one the
  // evaluator clamps to and nothing can reach again.
  moved.t = std::clamp(t, 0.0, blocks[drag_->block].duration());
  band.points[gain_point_] = moved;

  // Kept in order, because everything that reads a keyframe list — the drawing
  // above, the evaluator in the core — assumes it is sorted, and dragging one
  // point past another is the one gesture that can break that. Held by value
  // across the sort, since the slot it was in is not the slot it ends up in.
  std::ranges::sort(band.points, {}, &GainPoint::t);
  for (std::size_t i = 0; i < band.points.size(); ++i) {
    if (band.points[i] == moved) {
      gain_point_ = i;
      break;
    }
  }
}

std::optional<std::size_t> TimelineView::track_at(double y) const {
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const Rect row = track_rect(track);
    if (!row.empty() && y >= row.y && y < row.bottom()) return track;
  }
  return std::nullopt;
}

void TimelineView::relocate_carried(int lanes, double shift) {
  model_ = press_model_;
  if (moving_.empty() || !drag_.has_value()) return;

  // By position rather than by id. A block's id is opaque to the timeline and
  // may be anything at all — the sample models in the tests leave it empty —
  // so identifying the carried blocks by it removed every block that shared a
  // blank id, which was all of them.
  std::vector<Moving> carried = moving_;
  std::ranges::stable_sort(carried, [](const Moving& a, const Moving& b) {
    return a.ref.track != b.ref.track ? a.ref.track < b.ref.track
                                      : a.ref.block > b.ref.block;
  });

  // Taken out from the back of each track forwards, so an index still names
  // what it named before the one after it was removed. And out of all of them
  // before any goes back in, or the second would land in a track the first has
  // already changed the shape of.
  for (const Moving& one : carried) {
    if (one.ref.track >= model_.tracks.size()) continue;
    std::vector<TimelineBlock>& row = model_.tracks[one.ref.track].blocks;
    if (one.ref.block >= row.size()) continue;
    row.erase(row.begin() + static_cast<std::ptrdiff_t>(one.ref.block));
  }

  /// The tracks of one kind, in the order they are stored.
  const auto lanes_of = [this](bool audio) {
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < model_.tracks.size(); ++i) {
      if (model_.tracks[i].audio == audio) found.push_back(i);
    }
    return found;
  };

  // Where the dragged one lands, so it can be found again afterwards: it very
  // likely changed index, and the release reports whatever `drag_` names.
  std::size_t landed_track = drag_->track;
  std::size_t landed_at = 0;

  for (const Moving& one : carried) {
    if (one.ref.track >= model_.tracks.size()) continue;
    const bool audio = model_.tracks[one.ref.track].audio;
    const std::vector<std::size_t> same = lanes_of(audio);
    const auto here = std::ranges::find(same, one.ref.track);
    if (here == same.end()) continue;

    // A video clip dragged past the last video track stops there rather than
    // landing in the audio, which is not somewhere a picture can go.
    const auto index = static_cast<int>(std::distance(same.begin(), here));
    const auto wanted = static_cast<std::size_t>(
        std::clamp(index + lanes, 0, static_cast<int>(same.size()) - 1));

    TimelineBlock block = one.origin;
    block.start += shift;
    block.end += shift;

    std::vector<TimelineBlock>& row = model_.tracks[same[wanted]].blocks;
    if (one.ref == *drag_) {
      landed_track = same[wanted];
      landed_at = row.size();
    }
    row.push_back(std::move(block));
  }

  // Sorted by an index permutation rather than in place, so the dragged block
  // can be followed through the sort. Nothing else here can identify it: two
  // blocks may share a start, a label and an empty id.
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    std::vector<TimelineBlock>& row = model_.tracks[track].blocks;
    std::vector<std::size_t> order(row.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, {}, [&row](std::size_t i) { return row[i].start; });

    std::vector<TimelineBlock> sorted;
    sorted.reserve(row.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (track == landed_track && order[i] == landed_at) {
        drag_ = BlockRef{.track = track, .block = i};
      }
      sorted.push_back(std::move(row[order[i]]));
    }
    row = std::move(sorted);
  }
  refresh_bounds();
}

void TimelineView::drag_to(double x, double y) {
  if (!drag_.has_value()) return;
  std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
  if (drag_->block >= blocks.size()) return;

  // Always from where the press was, never from the last position, so rounding
  // cannot accumulate over a long drag and leave the clip a frame adrift.
  const double moved = (x - press_x_) / scale_.pixels_per_second;
  const double frame = core::frame_duration(model_.fps);
  const double tolerance = snapping_ ? kSnapDistance / scale_.pixels_per_second : 0.0;
  const std::vector<double> points = snap_points(model_, playhead_, drag_);

  // What the drag stuck to, for the line that says it stuck. Cleared each time
  // round, so letting go of an edge takes the line away rather than leaving one
  // at the last place it happened to catch.
  snapped_.reset();
  const auto snap_to = [&](double value) -> std::optional<double> {
    const std::optional<double> found = nearest_snap(points, value, tolerance);
    if (found.has_value()) snapped_ = *found;
    return found;
  };

  TimelineBlock next = origin_;

  /// Puts everything the ripple is carrying at its own start plus `delta`.
  /// From the captured origins rather than from where the blocks are now, for
  /// the same reason the dragged clip is: a long drag must not accumulate.
  const auto shift_downstream = [&](double delta) {
    for (const Moving& carried : moving_) {
      if (carried.ref.track >= model_.tracks.size()) continue;
      std::vector<TimelineBlock>& row = model_.tracks[carried.ref.track].blocks;
      if (carried.ref.block >= row.size()) continue;
      row[carried.ref.block].start = carried.origin.start + delta;
      row[carried.ref.block].end = carried.origin.end + delta;
    }
  };

  switch (mode_) {
    case DragMode::Move: {
      double start = std::max(0.0, origin_.start + moved);
      // Both edges are offered to the snapper and the nearer pull wins, which
      // is what makes a clip click into place against the one before it as
      // readily as against the one after.
      const double length = origin_.duration();
      const auto to_start = nearest_snap(points, start, tolerance);
      const auto to_end = nearest_snap(points, start + length, tolerance);

      if (to_start && (!to_end || std::abs(*to_start - start) <=
                                      std::abs(*to_end - (start + length)))) {
        start = *to_start;
        snapped_ = *to_start;
      } else if (to_end) {
        start = *to_end - length;
        snapped_ = *to_end;
      }
      start = std::max(0.0, core::snap_to_frame(start, model_.fps));

      // Everything the move is carrying, shifted by what the dragged clip
      // worked out. Previewing only the one under the pointer left the rest of
      // the selection sitting still until the mouse came up, and then jumping —
      // so the drag showed something the release did not do.
      double shift = start - origin_.start;
      // Clamped for the set rather than per clip, so the earliest one stopping
      // at the start of the timeline stops them all and the shape of the
      // selection survives the edge. This is the same rule `move_clips` keeps.
      double earliest = origin_.start;
      for (const Moving& carried : moving_) earliest = std::min(earliest, carried.origin.start);
      shift = std::max(shift, -earliest);

      // And how many lanes of its own kind it has travelled, from where the
      // pointer is now.
      //
      // The nearest lane of its own kind *in the direction it went*, rather
      // than only counting a pointer that is exactly over one. Video and audio
      // are different kinds of place, so a picture dragged down towards the
      // sound has to stop on the lowest video lane — stopping it where it
      // started instead would mean the drag simply failed once the pointer got
      // that far, which is not what it looks like it is doing.
      int lanes = 0;
      if (const std::optional<std::size_t> over = track_at(y);
          over.has_value() && press_track_ < model_.tracks.size()) {
        const bool audio = model_.tracks[press_track_].audio;
        std::vector<std::size_t> same;
        for (std::size_t i = 0; i < model_.tracks.size(); ++i) {
          if (model_.tracks[i].audio == audio) same.push_back(i);
        }

        int from = 0;
        int to = 0;
        for (std::size_t k = 0; k < same.size(); ++k) {
          if (same[k] == press_track_) from = static_cast<int>(k);
          // The last one at or above the pointer. Left at zero when the pointer
          // is above every lane of this kind, which is the right answer there.
          if (same[k] <= *over) to = static_cast<int>(k);
        }
        lanes = to - from;
      }

      carried_lanes_ = lanes;
      relocate_carried(lanes, shift);
      return;
    }

    case DragMode::TrimStart: {
      double start = origin_.start + moved;
      if (const auto snapped = snap_to(start)) start = *snapped;
      // Never past the far edge: a clip has to keep at least one frame, or it
      // vanishes and there is nothing left to drag back.
      next.start = std::clamp(core::snap_to_frame(start, model_.fps), 0.0,
                              origin_.end - frame);
      break;
    }

    case DragMode::TrimEnd: {
      double end = origin_.end + moved;
      if (const auto snapped = snap_to(end)) end = *snapped;
      next.end = std::max(origin_.start + frame, core::snap_to_frame(end, model_.fps));
      break;
    }

    // A ripple shows what the sequence will look like once it has closed up,
    // rather than showing the trim and then rearranging on release. Dragging a
    // head therefore leaves the clip where it is and takes the length off the
    // front, because that is the net of trimming it and closing the gap.
    case DragMode::RippleStart: {
      double start = origin_.start + moved;
      if (const auto snapped = snap_to(start)) start = *snapped;
      start = std::clamp(core::snap_to_frame(start, model_.fps), 0.0, origin_.end - frame);

      const double delta = start - origin_.start;
      next.end = origin_.end - delta;
      shift_downstream(-delta);
      break;
    }

    case DragMode::RippleEnd: {
      double end = origin_.end + moved;
      if (const auto snapped = snap_to(end)) end = *snapped;
      next.end = std::max(origin_.start + frame, core::snap_to_frame(end, model_.fps));
      shift_downstream(next.end - origin_.end);
      break;
    }

    // A roll moves two edges and nothing else, so the neighbour is written here
    // and the sequence keeps its length. Bounded by both clips keeping a frame:
    // either of them disappearing would leave a join with one side.
    case DragMode::RollStart: {
      if (!before_.has_value()) return;
      double start = origin_.start + moved;
      if (const auto snapped = snap_to(start)) start = *snapped;
      next.start = std::clamp(core::snap_to_frame(start, model_.fps),
                              before_->origin.start + frame, origin_.end - frame);
      model_.tracks[drag_->track].blocks[before_->index].end = next.start;
      break;
    }

    case DragMode::RollEnd: {
      if (!after_.has_value()) return;
      double end = origin_.end + moved;
      if (const auto snapped = snap_to(end)) end = *snapped;
      next.end = std::clamp(core::snap_to_frame(end, model_.fps), origin_.start + frame,
                            after_->origin.end - frame);
      model_.tracks[drag_->track].blocks[after_->index].start = next.end;
      break;
    }

    // The rate stretches are the trims without the source as a limit: what
    // changes is the speed, so the clip can be pulled longer than the footage
    // it came from. Only the one-frame floor is left.
    case DragMode::RateStart: {
      double start = origin_.start + moved;
      if (const auto snapped = snap_to(start)) start = *snapped;
      next.start = std::clamp(core::snap_to_frame(start, model_.fps), 0.0, origin_.end - frame);
      break;
    }

    case DragMode::RateEnd: {
      double end = origin_.end + moved;
      if (const auto snapped = snap_to(end)) end = *snapped;
      next.end = std::max(origin_.start + frame, core::snap_to_frame(end, model_.fps));
      break;
    }

    case DragMode::Slide:
      // Three edges at once, so this writes them itself rather than going
      // through the single-block path below.
      slide_to(moved, frame);
      refresh_bounds();
      return;

    case DragMode::FadeIn:
    case DragMode::FadeOut: {
      // How far the handle is from the edge it belongs to. Not snapped to other
      // clips: a fade is a length rather than a position, and sticking it to a
      // neighbour's edge would mean nothing.
      const Rect box = block_rect(drag_->track, drag_->block);
      const double from_edge =
          mode_ == DragMode::FadeIn ? x - box.x : box.right() - x;
      const double seconds = core::snap_to_frame(
          std::max(0.0, from_edge / scale_.pixels_per_second), model_.fps);
      // Never more than the clip: the model clamps the pair together anyway, and
      // a handle that could be dragged past the far end would spring back.
      const double longest = origin_.duration();
      if (mode_ == DragMode::FadeIn) {
        blocks[drag_->block].fade_in = std::clamp(seconds, 0.0, longest);
      } else {
        blocks[drag_->block].fade_out = std::clamp(seconds, 0.0, longest);
      }
      return;
    }

    case DragMode::TransitionLength: {
      // Half either side of the cut, so the pointer's distance from the join is
      // half the duration. Measured from the cut rather than accumulated from
      // the press, like every other drag here: a long one must not gather a
      // rounding error, and the edge should sit under the hand exactly.
      const double cut = origin_.end;
      const double where = scale_.to_time(x - time_area().x);
      const double half = transition_from_end_ ? where - cut : cut - where;
      // Pulled through the cut and out the other side is a transition of no
      // length, rather than one growing again backwards.
      blocks[drag_->block].transition.duration =
          std::max(0.0, core::snap_to_frame(half * 2.0, model_.fps));
      return;
    }

    case DragMode::Slip:
      // Nothing moves. The clip stays exactly where it is — that is what a slip
      // means — and the distance dragged is reported at the end. There is
      // deliberately no preview here: the change is which frames the clip
      // shows, and the timeline does not draw frames.
      return;

    case DragMode::None:
    case DragMode::Scrub:
    case DragMode::Razor:
      return;
  }

  blocks[drag_->block].start = next.start;
  blocks[drag_->block].end = next.end;
  // And whatever is linked to it, by the same amounts. Every edge gesture that
  // reaches here goes through an operation that acts on the whole group, so a
  // preview of one block was showing something the release did not do.
  sync_group(next.start - origin_.start, next.end - origin_.end);
  refresh_bounds();
}

Cursor TimelineView::cursor_at(double x, double y) const {
  // While a gesture is running the cursor describes the gesture, not the
  // pointer. Otherwise a trim dragged past the end of its clip — where there is
  // no clip under the pointer at all — would go back to an arrow mid-drag.
  const DragMode mode = mode_ != DragMode::None ? mode_ : zone_at(x, y, hover_modifiers_);

  switch (mode) {
    case DragMode::TrimStart:
    case DragMode::TrimEnd:
      // What this has to say is the thing nothing else was saying at all: that
      // this pixel takes hold of an edge rather than of the clip.
      return Cursor::ResizeWE;

    // Their own, rather than sharing the plain trim's. An earlier version said
    // one cursor was enough because the *tool* names which of the three you are
    // holding — and then control started making a ripple out of a trim, with
    // nothing anywhere to say so. Drawn from the same art as the palette
    // buttons, so a ripple looks like a ripple however it was reached.
    case DragMode::RippleStart:
    case DragMode::RippleEnd: return Cursor::Ripple;
    case DragMode::RollStart:
    case DragMode::RollEnd: return Cursor::Roll;

    case DragMode::RateStart:
    case DragMode::RateEnd: return Cursor::RateStretch;
    case DragMode::Slip: return Cursor::Slip;
    case DragMode::Slide: return Cursor::Slide;
    case DragMode::Razor: return Cursor::Razor;
    case DragMode::Move: return Cursor::Move;

    case DragMode::TrackHeight: return Cursor::ResizeNS;
    case DragMode::GainLevel:
    case DragMode::GainSegment:
    case DragMode::GainPointDrag: return Cursor::ResizeNS;
    case DragMode::FadeIn:
    case DragMode::FadeOut: return Cursor::ResizeWE;

    // The same as a trim's, and for the same reason: what it has to say is that
    // this pixel takes hold of an edge rather than of what is under it.
    case DragMode::TransitionLength: return Cursor::ResizeWE;

    // The ends of the scroll thumb zoom, which is a horizontal resize of what
    // is on screen; the thumb itself is picked up and moved.
    case DragMode::ZoomStart:
    case DragMode::ZoomEnd: return Cursor::ResizeWE;
    case DragMode::ScrollTime:
    case DragMode::ScrollTracks: return Cursor::Move;

    case DragMode::None:
    case DragMode::Scrub:
    case DragMode::Marquee:
    case DragMode::GainPointRemove: break;
  }

  // The scrollbars, which `zone_at` knows nothing about: they are outside the
  // tracks, and it only answers about clips.
  if (zoom_grip(false).contains(x, y) || zoom_grip(true).contains(x, y)) {
    return Cursor::ResizeWE;
  }
  if (scroll_thumb().contains(x, y) || track_scroll_thumb().contains(x, y)) {
    return Cursor::Move;
  }

  // The grip under a header, which `zone_at` knows nothing about because it is
  // not on a clip. Without this the one gesture with no visible affordance at
  // all — a line between two lanes — stays unguessable.
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    if (resize_rect(track).contains(x, y)) return Cursor::ResizeNS;
  }

  // Over a clip with a tool that means something everywhere on it, the tool's
  // own cursor. `zone_at` answered `None` only because the point missed every
  // clip, so this is the empty track, the ruler and the headers.
  return Cursor::Arrow;
}

void TimelineView::on_mouse_enter() { }

void TimelineView::on_mouse_leave() {
  pointer_.reset();
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

bool TimelineView::on_mouse_down(const MouseEvent& event) {
  if (event.button == MouseButton::Right) {
    // Over a clip that is not already selected, the right-click selects it
    // first: a menu that acted on something other than what was clicked would
    // be a trap. One that *is* selected is left alone, so right-clicking one of
    // several keeps all of them.
    if (const std::optional<BlockRef> hit = block_at(event.x, event.y);
        hit.has_value() && !model_.tracks[hit->track].blocks[hit->block].selected) {
      select(hit);
      if (on_select_) {
        const std::vector<BlockRef> chosen = selection();
        on_select_(chosen);
      }
    }
    if (on_context_menu_) on_context_menu_(event.x, event.y);
    return true;
  }

  if (event.button != MouseButton::Left) return false;

  // The scrollbars, before anything else in the time area: they sit outside the
  // tracks and the ruler, so nothing else can want this point.
  if (scroll_area().contains(event.x, event.y)) {
    zoom_origin_ = scale_;
    scroll_origin_ = event.x;
    press_x_ = event.x;

    if (zoom_grip(false).contains(event.x, event.y)) {
      mode_ = DragMode::ZoomStart;
    } else if (zoom_grip(true).contains(event.x, event.y)) {
      mode_ = DragMode::ZoomEnd;
    } else if (scroll_thumb().contains(event.x, event.y)) {
      mode_ = DragMode::ScrollTime;
    } else {
      // A press on the empty part of the bar jumps the view there and then
      // drags from it, rather than paging: the pointer is already where the
      // answer is, which is the same rule the ruler follows.
      const Rect bar = scroll_area();
      Viewport view = across();
      view.drag_thumb(bar.width, event.x - bar.x - view.thumb_size(bar.width) * 0.5);
      scale_.start = view.offset;
      scale_.clamp_start(model_.content_duration());
      zoom_origin_ = scale_;
      scrolled_by_hand_ = true;
      mode_ = DragMode::ScrollTime;
    }
    return true;
  }

  if (track_scroll_area().contains(event.x, event.y)) {
    mode_ = DragMode::ScrollTracks;
    press_y_ = event.y;
    scroll_origin_ = vertical_.offset;
    if (!track_scroll_thumb().contains(event.x, event.y)) {
      const Rect bar = track_scroll_area();
      vertical_.drag_thumb(bar.height,
                           event.y - bar.y - vertical_.thumb_size(bar.height) * 0.5);
      scroll_origin_ = vertical_.offset;
      press_y_ = event.y;
    }
    return true;
  }

  // The line under a header, before the switches: it is at the bottom edge and
  // they are in the middle, so the two never overlap, and taking this first
  // means the boundary between two lanes belongs to the one above it rather
  // than to whichever happens to be tested first.
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    if (!resize_rect(track).contains(event.x, event.y)) continue;

    // A double-click gives the lane back to the theme, which is the only way
    // back to the default height once one has been dragged.
    if (event.click_count >= 2) {
      model_.tracks[track].height.reset();
      refresh_bounds();
      if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
      if (on_track_resize_) on_track_resize_(track, std::nullopt);
      return true;
    }
    mode_ = DragMode::TrackHeight;
    sizing_ = track;
    sizing_origin_ = track_height(track);
    press_y_ = event.y;
    return true;
  }

  // A header switch, before anything else. Flipped here as well as reported, so
  // the press is visible on the frame it happened rather than only once the
  // document has come back round — the same reason a dragged clip moves in the
  // view before the model has agreed to it.
  if (const auto hit = control_at(event.x, event.y)) {
    TrackSwitches& on = model_.tracks[hit->track].switches;
    switch (hit->control) {
      case TrackControl::Target: on.target = !on.target; break;
      case TrackControl::Mute: on.mute = !on.mute; break;
      case TrackControl::Solo: on.solo = !on.solo; break;
      case TrackControl::Lock: on.lock = !on.lock; break;
      case TrackControl::Hide: on.hide = !on.hide; break;
    }
    if (on_track_toggle_) on_track_toggle_(*hit);
    return true;
  }

  // A marker before the ruler swallows the press, and only on a double-click:
  // a single click on the ruler scrubs, including over a marker, because
  // hunting for the gaps between markers to move the playhead would be worse
  // than not being able to open one.
  if (event.click_count >= 2) {
    if (const auto marker = marker_at(event.x, event.y)) {
      if (on_marker_activate_) on_marker_activate_(*marker);
      return true;
    }
  }

  // A double-click anywhere else on a header renames the track. After the
  // switches, so double-clicking mute twice is two mutes rather than a rename
  // — which is what it looks like it should be.
  if (event.click_count >= 2) {
    if (const auto track = header_at(event.x, event.y)) {
      if (on_track_rename_) on_track_rename_(*track);
      return true;
    }
  }

  // Anywhere on the ruler scrubs, not just on the playhead itself. Hunting for
  // a one-pixel line is not an interaction.
  if (ruler_area().contains(event.x, event.y)) {
    mode_ = DragMode::Scrub;
    scrub_to(event.x);
    return true;
  }

  if (!tracks_area().contains(event.x, event.y)) return false;

  const std::optional<BlockRef> hit = block_at(event.x, event.y);

  // The razor cuts and nothing else happens: no selection change, no drag. A
  // cut that also selected would leave one of the two halves highlighted for no
  // reason anybody asked for, and the tool exists to be used repeatedly.
  if (hit.has_value() && tool_ == Tool::Razor) {
    if (on_edit_) {
      on_edit_(TimelineEdit{.block = *hit,
                            .mode = DragMode::Razor,
                            .result = model_.tracks[hit->track].blocks[hit->block],
                            .at = scale_.to_time(event.x - time_area().x),
                            .all_tracks = event.modifiers.shift});
    }
    return true;
  }

  // Shift on a clip adds it to the selection, or takes it out again if it was
  // already in. Toggling rather than only adding, because otherwise there is no
  // way to correct a sweep that caught one clip too many except by starting
  // over.
  //
  // Not with control, which is the roll: control and shift together already
  // mean one thing, and a chord that both toggled the selection and started an
  // edit would do two things nobody asked for at once.
  if (hit.has_value() && event.modifiers.shift && !event.modifiers.control &&
      tool_ == Tool::Selection) {
    std::vector<BlockRef> chosen = selection();
    const auto already = std::ranges::find(chosen, *hit);
    if (already != chosen.end()) {
      chosen.erase(already);
    } else {
      chosen.push_back(*hit);
    }
    select(chosen);
    if (on_select_) on_select_(chosen);
    // No drag. A shift-click is about what is selected, and moving the clip as
    // well would make gathering a selection up shove it around.
    return true;
  }

  // A press on empty track starts a sweep. Nothing was taken away to make room
  // for it: this was already the gesture that deselected, and a press that goes
  // nowhere still does exactly that.
  if (!hit.has_value() && tool_ == Tool::Selection) {
    mode_ = DragMode::Marquee;
    press_x_ = event.x;
    press_y_ = event.y;
    marquee_x_ = event.x;
    marquee_y_ = event.y;
    moved_ = false;
    // Kept so a shift-sweep adds to what was there, and so dragging back over
    // the start does not leave the earlier selection half rubbed out.
    marquee_from_ = event.modifiers.shift ? selection() : std::vector<BlockRef>{};
    select(marquee_from_);
    if (on_select_) on_select_(marquee_from_);
    return true;
  }

  // A transition, before the clips it straddles. It covers the out-edge of the
  // clip before the cut and the in-edge of the one after — both trim handles —
  // so testing the clips first would make a transition impossible to take hold
  // of at all.
  //
  // The selection is left exactly as it was: pulling a transition longer is not
  // a statement about which clips are being worked on, and throwing away a
  // selection to do it would be a surprise.
  //
  // Asked of `zone_at` rather than tested again here, so the cursor and the
  // press cannot disagree about what a pixel does. They were two tests to begin
  // with, and breaking one of them on purpose showed the other still working —
  // which is the shape of a hover that says one thing and a press that does
  // another.
  if (zone_at(event.x, event.y, event.modifiers) == DragMode::TransitionLength) {
    if (const std::optional<BlockRef> over = transition_at(event.x, event.y);
        over.has_value()) {
      mode_ = DragMode::TransitionLength;
      drag_ = over;
      origin_ = model_.tracks[over->track].blocks[over->block];
      press_model_ = model_;
      press_track_ = over->track;
      press_x_ = event.x;
      press_y_ = event.y;
      moved_ = false;
      // Which edge is being held, read once. See `transition_from_end_`.
      const Rect box = transition_rect(over->track, over->block);
      transition_from_end_ = event.x >= box.x + box.width * 0.5;
      return true;
    }
  }

  // A press on a clip that is *already* selected leaves the selection alone.
  //
  // Without this, taking hold of one clip of a selection to drag it threw the
  // rest away before the drag began — so a multiple selection could be made and
  // never moved, which is most of what one is for. Pressing something outside
  // the selection still replaces it, which is how a selection is abandoned.
  const bool already_selected =
      hit.has_value() && model_.tracks[hit->track].blocks[hit->block].selected;
  if (!already_selected) {
    select(hit);
    if (on_select_) {
      const std::vector<BlockRef> chosen =
          hit.has_value() ? std::vector<BlockRef>{*hit} : std::vector<BlockRef>{};
      on_select_(chosen);
    }
  }

  if (hit.has_value()) {
    mode_ = zone_at(event.x, event.y, event.modifiers);
    drag_ = hit;
    origin_ = model_.tracks[hit->track].blocks[hit->block];
    press_x_ = event.x;
    press_y_ = event.y;
    moved_ = false;

    // Alt over an audio clip adds a point where the band already is and drags
    // it from there, so a new point and an old one are the same gesture from
    // here on. Anywhere on the clip, not only on the line: the band is a couple
    // of pixels tall and asking someone to hit it before they can put a point on
    // it is asking twice.
    if (event.modifiers.alt && tool_ == Tool::Selection) {
      // Alt on a point that is already there takes it away instead. The same
      // key both ways, so nothing exists only to undo the other.
      if (const auto point = gain_point_at(event.x, event.y)) {
        std::vector<GainPoint>& points =
            model_.tracks[hit->track].blocks[hit->block].gain->points;
        const GainPoint removed = points[point->point];
        points.erase(points.begin() + static_cast<std::ptrdiff_t>(point->point));

        if (on_edit_) {
          on_edit_(TimelineEdit{.block = *hit,
                                .mode = DragMode::GainPointRemove,
                                .result = model_.tracks[hit->track].blocks[hit->block],
                                .gain_from = removed,
                                .gain_to = removed});
        }
        mode_ = DragMode::None;
        settle_model();
        drag_.reset();
        duplicating_ = false;
        return true;
      }

      if (const auto added = add_gain_point(*hit, event.x)) {
        mode_ = DragMode::GainPointDrag;
        gain_point_ = *added;
        gain_origin_ = model_.tracks[hit->track].blocks[hit->block].gain->points[*added];
        // A point that is only clicked is still a point. Without this, adding
        // one and letting go without moving would leave it in the view and
        // never report it, and it would vanish at the next rebuild.
        moved_ = true;
        return true;
      }
    }

    if (mode_ == DragMode::GainPointDrag) {
      const auto point = gain_point_at(event.x, event.y);
      if (point.has_value()) {
        gain_point_ = point->point;
        gain_origin_ =
            model_.tracks[hit->track].blocks[hit->block].gain->points[point->point];
      }
    } else if (mode_ == DragMode::GainLevel) {
      gain_level_origin_ = model_.tracks[hit->track].blocks[hit->block].gain->level;
    } else if (mode_ == DragMode::GainSegment) {
      // Nothing captured here on purpose. Anchoring adds points to the band, and
      // a press that turns out not to be a drag must leave the clip exactly as
      // it found it — so both wait until the pointer has actually travelled.
      gain_segment_.clear();
      gain_segment_origin_.clear();
      gain_anchors_.clear();
    }

    // The arrangement as it stands. A move needs it because it can change which
    // track a block is on; every other drag needs it because the clips linked
    // to the one being held are moved from where *they* were at the press.
    press_model_ = model_;
    press_track_ = hit->track;
    if (mode_ == DragMode::Move) {
      capture_moving();
      carried_lanes_ = 0;
      duplicating_ = event.modifiers.alt;
    }
    if (mode_ == DragMode::Slide) capture_neighbours();
    if (mode_ == DragMode::RippleStart || mode_ == DragMode::RippleEnd) {
      capture_downstream(mode_ == DragMode::RippleStart ? origin_.start : origin_.end);
    }
    if (mode_ == DragMode::RollStart) before_ = capture_join(true);
    if (mode_ == DragMode::RollEnd) after_ = capture_join(false);
  }
  return true;
}

void TimelineView::capture_moving() {
  moving_.clear();
  if (!drag_.has_value()) return;

  // The whole selection when the clip under the pointer is part of it, and that
  // clip alone otherwise — dragging something outside the selection must not
  // sweep up whatever happens to be highlighted elsewhere.
  const std::vector<BlockRef> chosen = selection();
  const bool carries = std::ranges::find(chosen, *drag_) != chosen.end();

  for (const BlockRef& ref : carries ? chosen : std::vector<BlockRef>{*drag_}) {
    if (ref.track >= model_.tracks.size()) continue;
    const std::vector<TimelineBlock>& row = model_.tracks[ref.track].blocks;
    if (ref.block >= row.size()) continue;
    moving_.push_back(Moving{.ref = ref, .origin = row[ref.block]});
  }
}

bool TimelineView::on_mouse_move(const MouseEvent& event) {
  // Kept whether or not anything is being dragged: this is what the highlight
  // on the zone under the pointer is drawn from, and hovering is when it
  // matters most — the point of it is to say what a press *would* do.
  const std::optional<std::pair<double, double>> was = pointer_;
  const Modifiers held = hover_modifiers_;
  pointer_ = std::pair{event.x, event.y};
  // Kept for the same reason the position is. Control turns a trim into a
  // ripple, so the highlight and the cursor have to change when it goes down
  // even though the pointer has not moved — and a move arrives whenever a
  // modifier changes under a stationary hand.
  hover_modifiers_ = event.modifiers;
  if (mode_ == DragMode::None && (was != pointer_ || held.control != event.modifiers.control ||
                                  held.shift != event.modifiers.shift)) {
    if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
  }

  // Capture means these arrive with the pointer far outside the widget, which
  // is where a drag spends most of its time.
  if (mode_ == DragMode::Scrub) {
    scrub_to(event.x);
    return true;
  }
  if (mode_ == DragMode::ScrollTime) {
    const Rect bar = scroll_area();
    Viewport view = across();
    view.offset = zoom_origin_.start;
    view.drag_thumb(bar.width,
                    view.thumb_offset(bar.width) + (event.x - scroll_origin_));
    scale_.start = view.offset;
    scale_.clamp_start(model_.content_duration());
    scrolled_by_hand_ = true;
    if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
    return true;
  }

  if (mode_ == DragMode::ZoomStart || mode_ == DragMode::ZoomEnd) {
    // The far end holds still and the span between the two becomes what is on
    // screen, which is what dragging the end of a scrollbar looks like it will
    // do. Worked from the view at the press, so a long drag cannot accumulate.
    const Rect area = time_area();
    const double was_visible = zoom_origin_.visible_duration(area.width);
    const double moved = (event.x - scroll_origin_) / std::max(1.0, area.width) *
                         model_.content_duration();

    const bool from_start = mode_ == DragMode::ZoomStart;
    const double span = std::max(core::frame_duration(model_.fps) * 2.0,
                                 from_start ? was_visible - moved : was_visible + moved);
    scale_.pixels_per_second =
        std::clamp(area.width / span, kMinPixelsPerSecond, kMaxPixelsPerSecond);
    // The end that was not grabbed stays where it was.
    scale_.start = from_start ? zoom_origin_.start +
                                    (was_visible - scale_.visible_duration(area.width))
                              : zoom_origin_.start;
    scale_.start = std::max(0.0, scale_.start);
    scale_.clamp_start(model_.content_duration());
    scrolled_by_hand_ = true;
    refresh_bounds();
    if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
    return true;
  }

  if (mode_ == DragMode::ScrollTracks) {
    const Rect bar = track_scroll_area();
    Viewport view = vertical_;
    view.offset = scroll_origin_;
    view.drag_thumb(bar.height,
                    view.thumb_offset(bar.height) + (event.y - press_y_));
    vertical_.offset = view.offset;
    vertical_.clamp();
    if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
    return true;
  }

  if (mode_ == DragMode::TrackHeight) {
    // Live, because a lane that only resized on release would be a drag with
    // nothing to aim by.
    if (sizing_ < model_.tracks.size()) {
      model_.tracks[sizing_].height =
          std::clamp(sizing_origin_ + (event.y - press_y_), kMinTrackHeight, kMaxTrackHeight);
      refresh_bounds();
      if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
    }
    return true;
  }

  if (mode_ == DragMode::Marquee) {
    marquee_x_ = event.x;
    marquee_y_ = event.y;
    // Both axes, like the volume drags: a sweep straight down a stack of tracks
    // covers no distance in x at all.
    if (!moved_ && std::hypot(event.x - press_x_, event.y - press_y_) < kDragThreshold) {
      return true;
    }
    moved_ = true;

    // Live, so the sweep can be seen gathering clips up rather than only
    // reporting what it caught once it is too late to adjust.
    std::vector<BlockRef> chosen = marquee_from_;
    for (const BlockRef& block : blocks_touching(marquee())) {
      if (std::ranges::find(chosen, block) == chosen.end()) chosen.push_back(block);
    }
    select(chosen);
    if (on_select_) on_select_(chosen);
    return true;
  }

  if (!drag_.has_value()) return false;

  const bool volume = mode_ == DragMode::GainLevel || mode_ == DragMode::GainPointDrag ||
                      mode_ == DragMode::GainSegment;

  // A press that has not travelled yet is still a click. Without this,
  // selecting a clip nudges it by however much the hand wobbled.
  //
  // Measured across both axes for anything that can move in both. The volume
  // gestures always could — a band pulled straight down covers no distance in
  // x at all — and a **move** can now that a clip can change track, which is
  // the reason dragging one straight up did nothing at all: the threshold sat
  // there refusing to start a gesture that had travelled the whole height of
  // the panel.
  const bool two_axis = volume || mode_ == DragMode::Move;
  const double travelled = two_axis ? std::hypot(event.x - press_x_, event.y - press_y_)
                                    : std::abs(event.x - press_x_);
  if (!moved_ && travelled < kDragThreshold) return true;
  moved_ = true;

  if (volume) {
    // The first move of a stretch drag is where the band gains its anchors and
    // the drag learns which points it is carrying — in that order, since
    // anchoring adds points and indices found beforehand would name the wrong
    // ones.
    if (mode_ == DragMode::GainSegment && gain_segment_.empty() && drag_.has_value()) {
      // Which points the stretch is held by, on the band as it stands. The
      // anchors go just outside *those*, so they have to be known first.
      const std::vector<std::size_t> grabbed =
          gain_segment_at(drag_->track, drag_->block, press_x_);
      const std::vector<GainPoint>& before =
          model_.tracks[drag_->track].blocks[drag_->block].gain->points;

      // Held by time rather than by index: anchoring inserts points, and every
      // index after an insertion means a different point afterwards.
      std::vector<double> times;
      for (const std::size_t at : grabbed) {
        if (at < before.size()) times.push_back(before[at].t);
      }

      ensure_gain_anchors(*drag_, grabbed);

      const std::vector<GainPoint>& after =
          model_.tracks[drag_->track].blocks[drag_->block].gain->points;
      for (const double when : times) {
        for (std::size_t i = 0; i < after.size(); ++i) {
          if (std::abs(after[i].t - when) > core::kKeyframeMatchEps) continue;
          gain_segment_.push_back(i);
          gain_segment_origin_.push_back(after[i].v);
          break;
        }
      }
    }
    gain_to(event.x, event.y);
    return true;
  }

  drag_to(event.x, event.y);
  return true;
}

bool TimelineView::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || mode_ == DragMode::None) return false;

  // A press on an already-selected clip that turned out not to be a drag
  // collapses the selection onto it.
  //
  // The press had to leave the selection alone in case a drag was coming; this
  // is where it turns out none was. Without it there is no way back from a
  // selection of several to one of them without clicking away first.
  if (!moved_ && drag_.has_value() && mode_ == DragMode::Move &&
      !event.modifiers.shift && selection().size() > 1) {
    select(drag_);
    if (on_select_) {
      const std::vector<BlockRef> chosen{*drag_};
      on_select_(chosen);
    }
  }

  if (mode_ == DragMode::ScrollTime || mode_ == DragMode::ZoomStart ||
      mode_ == DragMode::ZoomEnd || mode_ == DragMode::ScrollTracks) {
    // Nothing to report. These move the view rather than the document, and
    // undoing a scroll is not a thing anybody wants.
    mode_ = DragMode::None;
    settle_model();
    return true;
  }

  if (mode_ == DragMode::TrackHeight) {
    mode_ = DragMode::None;
    // After the report, not before: what it reports is the height that was
    // dragged to, and that lives in the model a pending rebuild would replace.
    if (on_track_resize_ && sizing_ < model_.tracks.size()) {
      on_track_resize_(sizing_, model_.tracks[sizing_].height);
    }
    settle_model();
    return true;
  }

  // A sweep has already reported everything it gathered, on every move. There
  // is no edit at the end of it — nothing about the project changed.
  if (mode_ == DragMode::Marquee) {
    mode_ = DragMode::None;
    settle_model();
    moved_ = false;
    marquee_from_.clear();
    return true;
  }

  // Reported once, at the end. The model has been updated all along so the
  // drag can be seen; firing on every move would put a hundred entries in the
  // undo stack for one gesture.
  if (moved_ && drag_.has_value() && on_edit_ &&
      (mode_ == DragMode::GainLevel || mode_ == DragMode::GainPointDrag ||
       mode_ == DragMode::GainSegment)) {
    const std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
    if (drag_->block < blocks.size() && blocks[drag_->block].gain.has_value()) {
      const GainBand& band = *blocks[drag_->block].gain;
      TimelineEdit edit{.block = *drag_, .mode = mode_, .result = blocks[drag_->block]};
      if (mode_ == DragMode::GainLevel) {
        edit.gain = band.level;
      } else if (mode_ == DragMode::GainSegment) {
        // The anchors first, at the values they were given. They did not move,
        // but the project has never heard of them — without these the band the
        // view is showing is not the one that would be played back.
        for (const GainPoint& anchor : gain_anchors_) edit.gain_moved.push_back(anchor);
        for (const std::size_t at : gain_segment_) {
          if (at < band.points.size()) edit.gain_moved.push_back(band.points[at]);
        }
      } else if (gain_point_ < band.points.size()) {
        edit.gain_from = gain_origin_;
        edit.gain_to = band.points[gain_point_];
      }
      on_edit_(edit);
    }

    mode_ = DragMode::None;
    settle_model();
    drag_.reset();
    duplicating_ = false;
    moved_ = false;
    gain_segment_.clear();
    gain_segment_origin_.clear();
    gain_anchors_.clear();
    return true;
  }

  if (moved_ && drag_.has_value() && on_edit_ &&
      (mode_ == DragMode::FadeIn || mode_ == DragMode::FadeOut)) {
    const std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
    if (drag_->block < blocks.size()) {
      const TimelineBlock& clip = blocks[drag_->block];
      on_edit_(TimelineEdit{.block = *drag_,
                            .mode = mode_,
                            .result = clip,
                            .fade = mode_ == DragMode::FadeIn ? clip.fade_in : clip.fade_out});
    }
    mode_ = DragMode::None;
    settle_model();
    drag_.reset();
    duplicating_ = false;
    moved_ = false;
    return true;
  }

  if (moved_ && drag_.has_value() && on_edit_) {
    const std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
    if (drag_->block < blocks.size()) {
      // Where the edge ended up, for the modes whose `result` cannot say. A
      // rippled head leaves the clip's start exactly where it was — the gap
      // closed behind it — so the trimmed edge has to be reported separately or
      // the edit would look like nothing happened.
      const TimelineBlock& shown = blocks[drag_->block];
      double edge = 0.0;
      switch (mode_) {
        case DragMode::RippleStart:
          // The head, in the clip's own terms: it lost `duration` from the
          // front, and the edge it was dragged to is that far past its start.
          edge = shown.start + (origin_.duration() - shown.duration());
          break;
        case DragMode::RippleEnd:
        case DragMode::RollEnd: edge = shown.end; break;
        case DragMode::RollStart: edge = shown.start; break;
        default: break;
      }

      on_edit_(TimelineEdit{
          .block = *drag_,
          .mode = mode_,
          .result = blocks[drag_->block],
          // Frame-snapped, so a slip moves the source by whole frames like
          // every other edit rather than by however many pixels the hand
          // travelled.
          .lanes = mode_ == DragMode::Move ? carried_lanes_ : 0,
          .delta = core::snap_to_frame((event.x - press_x_) / scale_.pixels_per_second,
                                       model_.fps),
          .at = edge,
          .copy = mode_ == DragMode::Move && duplicating_});
    }
  }

  mode_ = DragMode::None;
  settle_model();
  drag_.reset();
  duplicating_ = false;
  before_.reset();
  after_.reset();
  moving_.clear();
  moved_ = false;
  return true;
}

bool TimelineView::on_wheel(const WheelEvent& event) {
  const Rect area = time_area();

  if (event.modifiers.control) {
    // Zoom about the pointer. Anywhere else and every notch has to be undone
    // by scrolling back to what you were looking at.
    const double factor = event.delta_y > 0.0 ? 1.0 / kZoomStep : kZoomStep;
    scale_.zoom_about(event.x - area.x, factor);
    scale_.clamp_start(model_.content_duration());
    scrolled_by_hand_ = true;
    return true;
  }

  // A plain wheel scrolls the tracks where there are more than fit, and moves
  // through time where there are not — so the gesture always does something.
  if (!event.modifiers.shift && vertical_.scrollable()) {
    const double before = vertical_.offset;
    vertical_.scroll_by(event.delta_y * metrics_.track_height);
    if (vertical_.offset != before) return true;
  }

  const double before = scale_.start;
  scale_.start += event.delta_y * scale_.visible_duration(area.width) * kScrollFraction;
  scale_.clamp_start(model_.content_duration());
  if (scale_.start != before) scrolled_by_hand_ = true;
  // Unhandled when it could not move, so the wheel bubbles to whatever is
  // outside rather than dying against a timeline already at its end.
  return scale_.start != before;
}

}  // namespace cutline::ui
