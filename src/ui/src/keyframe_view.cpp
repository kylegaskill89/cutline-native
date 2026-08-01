#include "cutline/ui/keyframe_view.hpp"

#include "cutline/ui/timescale.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace cutline::ui {
namespace {

/// A duration shorter than this has no useful axis, and mapping time across it
/// would be a division by nearly zero.
constexpr double kShortest = 1e-6;

/// The clip's whole length is fitted across the track, so this is only used to
/// choose the ruler's marks. Frames are not known here — the view is about a
/// clip's own seconds — so a ruler in whole seconds is the honest answer.
constexpr double kRulerFps = 1.0;

/// Time as a ruler label. Minutes appear only once there are any, so a
/// four-second clip is not labelled `0:01`.
[[nodiscard]] std::string label_for(double t, double duration) {
  if (duration < 60.0) return std::format("{:.2g}s", t);
  const auto minutes = static_cast<int>(t / 60.0);
  return std::format("{}:{:04.1f}", minutes, t - minutes * 60.0);
}

}  // namespace

KeyframeView::KeyframeView() { set_clips_children(true); }

void KeyframeView::set_model(Model model) {
  model_ = std::move(model);
  // Indices into the old model mean nothing in the new one, and a selection
  // kept across a rebuild is how a right-click ends up acting on a keyframe
  // that is no longer there.
  selection_.clear();
  dragging_ = {};
  pressed_ = false;
  invalidate_layout();
}

void KeyframeView::nudge(std::size_t lane, std::size_t index, double t) {
  if (lane >= model_.lanes.size()) return;
  if (index >= model_.lanes[lane].times.size()) return;
  model_.lanes[lane].times[index] = std::clamp(t, 0.0, model_.duration);
}

void KeyframeView::set_playhead(double t) noexcept {
  playhead_ = std::max(0.0, t);
}

void KeyframeView::set_selection(std::vector<KeyframeHit> selection) {
  selection_ = std::move(selection);
}

bool KeyframeView::is_selected(std::size_t lane, std::size_t index) const noexcept {
  return std::ranges::any_of(selection_, [lane, index](const KeyframeHit& hit) {
    return hit.found && hit.lane == lane && hit.index == index;
  });
}

void KeyframeView::select_only(const KeyframeHit& hit) {
  selection_.clear();
  if (hit.found) selection_.push_back(hit);
}

void KeyframeView::toggle(const KeyframeHit& hit) {
  if (!hit.found) return;
  const auto found = std::ranges::find(selection_, hit);
  if (found == selection_.end()) {
    selection_.push_back(hit);
  } else {
    selection_.erase(found);
  }
}

// --------------------------------------------------------------- geometry --

Rect KeyframeView::ruler() const {
  return Rect{bounds().x, bounds().y, bounds().width, ruler_height_};
}

Rect KeyframeView::lane_rect(std::size_t lane) const {
  if (lane >= model_.lanes.size()) return Rect{};
  return Rect{bounds().x, bounds().y + ruler_height_ + static_cast<double>(lane) * row_height_,
              bounds().width, row_height_};
}

Rect KeyframeView::track_rect(std::size_t lane) const {
  const Rect row = lane_rect(lane);
  if (row.width <= 0.0) return Rect{};
  const double gutter = std::min(kNameWidth, row.width);
  return Rect{row.x + gutter, row.y, row.width - gutter, row.height};
}

double KeyframeView::axis_x() const {
  return bounds().x + std::min(kNameWidth, bounds().width) + kEdgeInset;
}

double KeyframeView::axis_width() const {
  return std::max(0.0, bounds().width - std::min(kNameWidth, bounds().width) - kEdgeInset * 2.0);
}

double KeyframeView::x_of(double t) const {
  const double width = axis_width();
  if (model_.duration < kShortest || width <= 0.0) return axis_x();
  return axis_x() + std::clamp(t / model_.duration, 0.0, 1.0) * width;
}

double KeyframeView::time_at(double x) const {
  const double width = axis_width();
  if (model_.duration < kShortest || width <= 0.0) return 0.0;
  return std::clamp((x - axis_x()) / width, 0.0, 1.0) * model_.duration;
}

Rect KeyframeView::keyframe_rect(std::size_t lane, std::size_t index) const {
  if (lane >= model_.lanes.size()) return Rect{};
  const Lane& row = model_.lanes[lane];
  if (index >= row.times.size()) return Rect{};

  const Rect track = track_rect(lane);
  const double cx = x_of(row.times[index]);
  const double cy = track.y + track.height * 0.5;
  return Rect{cx - kDiamond, cy - kDiamond, kDiamond * 2.0, kDiamond * 2.0};
}

std::size_t KeyframeView::lane_at(double y) const {
  const double top = bounds().y + ruler_height_;
  if (y < top || row_height_ <= 0.0) return model_.lanes.size();
  const auto lane = static_cast<std::size_t>((y - top) / row_height_);
  return lane < model_.lanes.size() ? lane : model_.lanes.size();
}

KeyframeHit KeyframeView::keyframe_at(double x, double y) const {
  const std::size_t lane = lane_at(y);
  if (lane >= model_.lanes.size()) return {};

  // Nearest rather than first, so overlapping diamonds resolve to the one the
  // pointer is actually closest to instead of whichever came first in the list.
  KeyframeHit best;
  double closest = kGrabReach;
  const Lane& row = model_.lanes[lane];
  for (std::size_t i = 0; i < row.times.size(); ++i) {
    const double distance = std::abs(x_of(row.times[i]) - x);
    if (distance <= closest) {
      closest = distance;
      best = KeyframeHit{.lane = lane, .index = i, .found = true};
    }
  }
  return best;
}

// ----------------------------------------------------------------- layout --

LayoutItem KeyframeView::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Horizontal) return LayoutItem::flexible(1.0, kNameWidth * 2.0);

  // Exactly its rows. A view that asked for more would draw empty lanes, and
  // one that asked for less would hide the last property animated.
  const double rows = static_cast<double>(model_.lanes.size()) * metrics.list_row_height;
  return LayoutItem::fixed(metrics.panel_header_height + rows);
}

void KeyframeView::layout(const LayoutContext& context) {
  const Metrics& metrics = context.metrics();
  row_height_ = metrics.list_row_height;
  ruler_height_ = metrics.panel_header_height;
  font_size_ = metrics.small_font_size;
}

// ---------------------------------------------------------------- painting --

void KeyframeView::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& panel = theme.style(Part::Panel, State::Normal);
  const SurfaceStyle& ruler_style = theme.style(Part::Ruler, State::Normal);
  const Rect strip = ruler();

  paint_surface(painter, strip, ruler_style);

  if (model_.lanes.empty()) {
    painter.text(text_run(Rect{bounds().x + kDiamond, bounds().y, bounds().width, bounds().height},
                          "Nothing is animated", panel, font_size_));
    return;
  }

  // The marks, across the track area only — the gutter is names, and a tick
  // drawn through them reads as a strikethrough.
  TimeScale scale;
  scale.fit(model_.duration, std::max(1.0, axis_width()));
  for (const Tick& tick : ruler_ticks(scale, 0.0, model_.duration, kRulerFps)) {
    const double x = x_of(tick.time);
    const double height = tick.major ? strip.height * 0.5 : strip.height * 0.25;
    painter.line(x, strip.bottom() - height, x, strip.bottom(), ruler_style.text, 1.0);
    if (!tick.major) continue;
    painter.text(text_run(Rect{x + 3.0, strip.y, kNameWidth, strip.height * 0.7},
                          label_for(tick.time, model_.duration), ruler_style, font_size_));
  }

  for (std::size_t lane = 0; lane < model_.lanes.size(); ++lane) {
    const Rect row = lane_rect(lane);
    const Rect track = track_rect(lane);

    // Alternating rows, so a diamond can be traced back to its property across
    // a panel's width without counting.
    if (lane % 2 == 1) {
      painter.fill(row, 0.0, Fill::solid(Color{panel.text.r, panel.text.g, panel.text.b, 0.05}));
    }

    painter.text(text_run(Rect{row.x + kDiamond, row.y, kNameWidth - kDiamond * 2.0, row.height},
                          model_.lanes[lane].name, panel, font_size_));

    // The line the keyframes sit on, so a lane with one point still reads as a
    // span of time rather than as a stray mark.
    const double mid = track.y + track.height * 0.5;
    painter.line(track.x, mid, track.right(), mid, Color{panel.text.r, panel.text.g,
                                                         panel.text.b, 0.25},
                 1.0);

    for (std::size_t i = 0; i < model_.lanes[lane].times.size(); ++i) {
      const Rect mark = keyframe_rect(lane, i);
      const double cx = mark.x + mark.width * 0.5;
      const double cy = mark.y + mark.height * 0.5;
      // Selected diamonds take the accent, which is what every other selection
      // in the application is drawn in.
      const Color colour = is_selected(lane, i) ? theme.accent : panel.text;
      painter.line(cx, cy - kDiamond, cx + kDiamond, cy, colour, 1.5);
      painter.line(cx + kDiamond, cy, cx, cy + kDiamond, colour, 1.5);
      painter.line(cx, cy + kDiamond, cx - kDiamond, cy, colour, 1.5);
      painter.line(cx - kDiamond, cy, cx, cy - kDiamond, colour, 1.5);
    }
  }

  // Last, so it is never hidden under a lane's fill or a diamond.
  const double head = x_of(playhead_);
  painter.line(head, strip.y, head, bounds().bottom(), theme.accent, 1.0);
}

// ------------------------------------------------------------------ input --

bool KeyframeView::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  const KeyframeHit hit = keyframe_at(event.x, event.y);
  if (hit.found) {
    // Shift adds to the selection; a plain click replaces it — but only when
    // the keyframe pressed is not already in it, so dragging several at once
    // stays possible once that lands.
    if (event.modifiers.shift) {
      toggle(hit);
    } else if (!is_selected(hit.lane, hit.index)) {
      select_only(hit);
    }
    if (on_select_) on_select_();

    dragging_ = hit;
    pressed_ = true;
    moved_ = false;
    press_x_ = event.x;
    drag_from_ = model_.lanes[hit.lane].times[hit.index];
    return true;
  }

  // Empty space clears the selection and scrubs, which is what the ruler and
  // the gaps between keyframes are for.
  if (!event.modifiers.shift && !selection_.empty()) {
    selection_.clear();
    if (on_select_) on_select_();
  }
  dragging_ = {};
  pressed_ = false;
  if (on_scrub_) on_scrub_(time_at(event.x));
  return true;
}

bool KeyframeView::on_mouse_move(const MouseEvent& event) {
  if (!pressed_ || !dragging_.found) return false;

  if (!moved_) {
    if (std::abs(event.x - press_x_) < kDragThreshold) return true;
    moved_ = true;
  }

  if (on_move_) on_move_(dragging_.lane, dragging_.index, time_at(event.x));
  return true;
}

bool KeyframeView::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !pressed_) return false;
  pressed_ = false;
  if (!moved_ || !dragging_.found) return true;

  moved_ = false;
  const double to = time_at(event.x);
  if (on_move_commit_ && to != drag_from_) {
    on_move_commit_(dragging_.lane, dragging_.index, drag_from_, to);
  }
  return true;
}

}  // namespace cutline::ui
