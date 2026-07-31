#include "cutline/ui/timeline.hpp"

#include "cutline/core/time.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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

/// The grabbable strip at each end of a clip. Never more than a third of it,
/// so a short clip can still be moved rather than being all handle.
constexpr double kTrimHandle = 8.0;

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

/// The switches each kind of track shows, in the order they are drawn.
constexpr std::array<TrackControl, 3> kAudioControls{TrackControl::Mute, TrackControl::Solo,
                                                     TrackControl::Lock};
constexpr std::array<TrackControl, 2> kVideoControls{TrackControl::Hide, TrackControl::Lock};

}  // namespace

std::string_view to_string(Tool tool) noexcept {
  switch (tool) {
    case Tool::Selection: return "selection";
    case Tool::Razor: return "razor";
    case Tool::RateStretch: return "rate";
    case Tool::Slip: return "slip";
    case Tool::Slide: return "slide";
  }
  return "selection";
}

std::string_view to_string(TrackControl control) noexcept {
  switch (control) {
    case TrackControl::Mute: return "M";
    case TrackControl::Solo: return "S";
    case TrackControl::Lock: return "L";
    case TrackControl::Hide: return "H";
  }
  return "?";
}

bool pulls_start(DragMode mode) noexcept {
  return mode == DragMode::TrimStart || mode == DragMode::RateStart;
}

bool pulls_end(DragMode mode) noexcept {
  return mode == DragMode::TrimEnd || mode == DragMode::RateEnd;
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
  model_ = std::move(model);
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

void TimelineView::set_playhead(double seconds) {
  playhead_ = std::max(0.0, core::snap_to_frame(seconds, model_.fps));
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
  return model_.tracks[track].audio ? metrics_.audio_track_height : metrics_.track_height;
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
  return Rect{area.x, area.y + top, area.width, std::max(0.0, area.height - top)};
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

  // A second row under the name, aligned to the bottom of the header. Under
  // rather than beside, because a hundred and forty pixels is not enough for a
  // readable label and three switches on one line.
  const double x = header.x + metrics_.padding_x + index * (kSwitchSize + kSwitchGap);
  const double y = header.bottom() - kSwitchSize - metrics_.padding_y;
  const Rect box{x, y, kSwitchSize, kSwitchSize};

  // Refused rather than clipped. A switch drawn half outside its own header, or
  // overlapping the name, is worse than one that admits there is no room.
  if (box.right() > header.right() - metrics_.padding_x * 0.5 || box.y < header.y) return {};
  return box;
}

std::optional<TrackControlRef> TimelineView::control_at(double x, double y) const {
  if (!header_area().contains(x, y)) return std::nullopt;

  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    for (const TrackControl control :
         {TrackControl::Mute, TrackControl::Solo, TrackControl::Lock, TrackControl::Hide}) {
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

Rect TimelineView::transition_rect(std::size_t track, std::size_t block) const {
  if (track >= model_.tracks.size()) return {};
  const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
  if (block >= blocks.size()) return {};

  const double duration = blocks[block].transition.duration;
  if (duration <= 0.0) return {};

  const Rect row = track_rect(track);
  if (row.empty()) return {};

  // Centred on the cut, which is where the model puts it: half the transition
  // plays before the join and half after.
  const double centre = row.x + scale_.to_x(blocks[block].end);
  const double half = scale_.width_of(duration) * 0.5;
  return Rect{centre - half, row.y, half * 2.0, row.height};
}

double TimelineView::playhead_x() const {
  return time_area().x + scale_.to_x(playhead_);
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
  const Rect time = time_area();
  // The header column sits alongside the tracks but is not part of them: a drop
  // there has no time, and letting it round to zero would quietly put the clip
  // at the start of the sequence instead of refusing.
  if (!tracks_area().contains(x, y) || x < time.x) return std::nullopt;

  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const Rect row = track_rect(track);
    if (row.empty() || y < row.y || y >= row.bottom()) continue;
    return DropPoint{.track = track, .time = std::max(0.0, scale_.to_time(x - time.x))};
  }
  return std::nullopt;
}

double TimelineView::trim_handle_width(std::size_t track, std::size_t block) const {
  const Rect box = block_rect(track, block);
  return std::min(kTrimHandle, box.width / 3.0);
}

DragMode TimelineView::zone_at(double x, double y) const {
  const std::optional<BlockRef> hit = block_at(x, y);
  if (!hit.has_value()) return DragMode::None;

  const Rect box = block_rect(hit->track, hit->block);

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
    // was meant anyway.
    case Tool::RateStretch:
      return x < box.x + box.width * 0.5 ? DragMode::RateStart : DragMode::RateEnd;

    case Tool::Selection:
      break;
  }

  const double handle = trim_handle_width(hit->track, hit->block);
  if (x < box.x + handle) return DragMode::TrimStart;
  if (x >= box.right() - handle) return DragMode::TrimEnd;
  return DragMode::Move;
}

std::optional<BlockRef> TimelineView::selection() const {
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const std::vector<TimelineBlock>& blocks = model_.tracks[track].blocks;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i].selected) return BlockRef{track, i};
    }
  }
  return std::nullopt;
}

void TimelineView::select(std::optional<BlockRef> block) {
  for (TimelineTrack& track : model_.tracks) {
    for (TimelineBlock& clip : track.blocks) clip.selected = false;
  }
  if (block.has_value() && block->track < model_.tracks.size()) {
    std::vector<TimelineBlock>& blocks = model_.tracks[block->track].blocks;
    if (block->block < blocks.size()) blocks[block->block].selected = true;
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
  for (std::size_t track = 0; track < model_.tracks.size(); ++track) {
    const Rect row = track_rect(track);
    if (row.bottom() < tracks.y || row.y > tracks.bottom()) continue;

    for (std::size_t i = 0; i < model_.tracks[track].blocks.size(); ++i) {
      const TimelineBlock& clip = model_.tracks[track].blocks[i];
      const Rect box = block_rect(track, i);
      // Off-screen either side. Cheap to skip and worth it: a long project has
      // far more clips out of view than in it.
      if (box.right() < tracks.x || box.x > tracks.right()) continue;

      const State state = clip.selected ? State::Selected : State::Normal;
      const SurfaceStyle& style = theme.style(Part::Clip, state);
      paint_surface(painter, box.inset(1.0), style);

      // Only worth a label if there is room for one to be read.
      if (!clip.label.empty() && box.width > metrics_.font_size * 2.0) {
        painter.push_clip(box, style.corner_radius);
        const Rect text = inset(box, Edges::symmetric(metrics_.padding_y * 1.5, 0.0));
        painter.text(text_run(text, clip.label, style, metrics_.small_font_size,
                              TextAlign::Left, false));
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
  }
  painter.pop_clip();

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
    painter.text(text_run(label, core::seconds_to_timecode(tick.time, model_.fps), ruler_style,
                          metrics_.small_font_size, TextAlign::Left, false));
  }

  // Inside the ruler's clip, so a span running off either end is cut at the
  // edge of the ruler rather than drawn across the track headers.
  if (const Rect marked = marked_bar(); !marked.empty()) {
    // The playhead's colour: this is the other thing on the ruler that says
    // where something is, and a second accent would be one too many.
    painter.fill(marked, 1.0, theme.style(Part::Playhead, State::Normal).fill);
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
    for (const TrackControl control :
         {TrackControl::Mute, TrackControl::Solo, TrackControl::Lock, TrackControl::Hide}) {
      const Rect box_of = control_rect(track, control);
      if (box_of.empty()) continue;

      const bool lit = control == TrackControl::Mute   ? on.mute
                       : control == TrackControl::Solo ? on.solo
                       : control == TrackControl::Lock ? on.lock
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

void TimelineView::drag_to(double x) {
  if (!drag_.has_value()) return;
  std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
  if (drag_->block >= blocks.size()) return;

  // Always from where the press was, never from the last position, so rounding
  // cannot accumulate over a long drag and leave the clip a frame adrift.
  const double moved = (x - press_x_) / scale_.pixels_per_second;
  const double frame = core::frame_duration(model_.fps);
  const double tolerance = snapping_ ? kSnapDistance / scale_.pixels_per_second : 0.0;
  const std::vector<double> points = snap_points(model_, playhead_, drag_);

  TimelineBlock next = origin_;

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
      } else if (to_end) {
        start = *to_end - length;
      }
      start = std::max(0.0, core::snap_to_frame(start, model_.fps));
      next.start = start;
      next.end = start + length;
      break;
    }

    case DragMode::TrimStart: {
      double start = origin_.start + moved;
      if (const auto snapped = nearest_snap(points, start, tolerance)) start = *snapped;
      // Never past the far edge: a clip has to keep at least one frame, or it
      // vanishes and there is nothing left to drag back.
      next.start = std::clamp(core::snap_to_frame(start, model_.fps), 0.0,
                              origin_.end - frame);
      break;
    }

    case DragMode::TrimEnd: {
      double end = origin_.end + moved;
      if (const auto snapped = nearest_snap(points, end, tolerance)) end = *snapped;
      next.end = std::max(origin_.start + frame, core::snap_to_frame(end, model_.fps));
      break;
    }

    // The rate stretches are the trims without the source as a limit: what
    // changes is the speed, so the clip can be pulled longer than the footage
    // it came from. Only the one-frame floor is left.
    case DragMode::RateStart: {
      double start = origin_.start + moved;
      if (const auto snapped = nearest_snap(points, start, tolerance)) start = *snapped;
      next.start = std::clamp(core::snap_to_frame(start, model_.fps), 0.0, origin_.end - frame);
      break;
    }

    case DragMode::RateEnd: {
      double end = origin_.end + moved;
      if (const auto snapped = nearest_snap(points, end, tolerance)) end = *snapped;
      next.end = std::max(origin_.start + frame, core::snap_to_frame(end, model_.fps));
      break;
    }

    case DragMode::Slide:
      // Three edges at once, so this writes them itself rather than going
      // through the single-block path below.
      slide_to(moved, frame);
      refresh_bounds();
      return;

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
  refresh_bounds();
}

bool TimelineView::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  // A header switch, before anything else. Flipped here as well as reported, so
  // the press is visible on the frame it happened rather than only once the
  // document has come back round — the same reason a dragged clip moves in the
  // view before the model has agreed to it.
  if (const auto hit = control_at(event.x, event.y)) {
    TrackSwitches& on = model_.tracks[hit->track].switches;
    switch (hit->control) {
      case TrackControl::Mute: on.mute = !on.mute; break;
      case TrackControl::Solo: on.solo = !on.solo; break;
      case TrackControl::Lock: on.lock = !on.lock; break;
      case TrackControl::Hide: on.hide = !on.hide; break;
    }
    if (on_track_toggle_) on_track_toggle_(*hit);
    return true;
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

  select(hit);
  if (on_select_) on_select_(hit);

  if (hit.has_value()) {
    mode_ = zone_at(event.x, event.y);
    drag_ = hit;
    origin_ = model_.tracks[hit->track].blocks[hit->block];
    press_x_ = event.x;
    moved_ = false;
    if (mode_ == DragMode::Slide) capture_neighbours();
  }
  return true;
}

bool TimelineView::on_mouse_move(const MouseEvent& event) {
  // Capture means these arrive with the pointer far outside the widget, which
  // is where a drag spends most of its time.
  if (mode_ == DragMode::Scrub) {
    scrub_to(event.x);
    return true;
  }
  if (!drag_.has_value()) return false;

  // A press that has not travelled yet is still a click. Without this,
  // selecting a clip nudges it by however much the hand wobbled.
  if (!moved_ && std::abs(event.x - press_x_) < kDragThreshold) return true;
  moved_ = true;

  drag_to(event.x);
  return true;
}

bool TimelineView::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || mode_ == DragMode::None) return false;

  // Reported once, at the end. The model has been updated all along so the
  // drag can be seen; firing on every move would put a hundred entries in the
  // undo stack for one gesture.
  if (moved_ && drag_.has_value() && on_edit_) {
    const std::vector<TimelineBlock>& blocks = model_.tracks[drag_->track].blocks;
    if (drag_->block < blocks.size()) {
      on_edit_(TimelineEdit{
          .block = *drag_,
          .mode = mode_,
          .result = blocks[drag_->block],
          // Frame-snapped, so a slip moves the source by whole frames like
          // every other edit rather than by however many pixels the hand
          // travelled.
          .delta = core::snap_to_frame((event.x - press_x_) / scale_.pixels_per_second,
                                       model_.fps)});
    }
  }

  mode_ = DragMode::None;
  drag_.reset();
  before_.reset();
  after_.reset();
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
  // Unhandled when it could not move, so the wheel bubbles to whatever is
  // outside rather than dying against a timeline already at its end.
  return scale_.start != before;
}

}  // namespace cutline::ui
