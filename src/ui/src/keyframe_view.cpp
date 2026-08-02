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

/// How much one notch of the wheel zooms, and how much of the visible span one
/// notch scrolls by. A fifth of a screen per notch is far enough to get
/// somewhere and short enough to keep your place.
constexpr double kZoomNotch = 1.25;
constexpr double kScrollNotch = 0.2;

/// Time as a ruler label. Minutes appear only once there are any, so a
/// four-second clip is not labelled `0:01`.
[[nodiscard]] std::string label_for(double t, double duration) {
  if (duration < 60.0) return std::format("{:.2g}s", t);
  const auto minutes = static_cast<int>(t / 60.0);
  return std::format("{}:{:04.1f}", minutes, t - minutes * 60.0);
}

}  // namespace

KeyframeView::KeyframeView() {
  set_clips_children(true);
  // Focusable so Delete reaches it. Nothing else here takes the keyboard, and
  // a selection that could only be removed with the mouse would be the one
  // gesture in the application with no key behind it.
  set_focusable(true);
}

void KeyframeView::set_model(Model model) {
  // A different clip is a different axis. Keeping the zoom would show the new
  // one's first two seconds because the old one happened to be looked at there.
  if (std::abs(model.duration - model_.duration) > kShortest) reset_view();
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

/// Asks for a fresh frame without asking for a fresh layout. Zooming moves
/// nothing in the tree — only what this widget draws inside its own bounds.
void KeyframeView::repaint() {
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
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

bool KeyframeView::is_expanded(std::size_t lane) const {
  if (lane >= model_.lanes.size()) return false;
  // A lane with no curve has no graph to open, whatever was remembered about
  // it — an effect parameter that stopped being sampled, for instance.
  if (model_.lanes[lane].curve.empty()) return false;
  return expanded_.contains(model_.lanes[lane].name);
}

void KeyframeView::set_expanded(std::string_view name, bool expanded) {
  if (expanded) {
    expanded_.emplace(name);
  } else {
    if (const auto found = expanded_.find(name); found != expanded_.end()) {
      expanded_.erase(found);
    }
  }
  invalidate_layout();
}

double KeyframeView::lane_height(std::size_t lane) const {
  return row_height_ + (is_expanded(lane) ? kGraphHeight : 0.0);
}

Rect KeyframeView::lane_rect(std::size_t lane) const {
  if (lane >= model_.lanes.size()) return Rect{};
  double top = bounds().y + ruler_height_;
  for (std::size_t above = 0; above < lane; ++above) top += lane_height(above);
  return Rect{bounds().x, top, bounds().width, lane_height(lane)};
}

Rect KeyframeView::track_rect(std::size_t lane) const {
  const Rect row = lane_rect(lane);
  if (row.width <= 0.0) return Rect{};
  const double gutter = std::min(kNameWidth, row.width);
  // One row high whatever the lane's own height: the diamonds stay where they
  // were and the graph appears underneath them.
  return Rect{row.x + gutter, row.y, row.width - gutter, row_height_};
}

Rect KeyframeView::graph_rect(std::size_t lane) const {
  if (!is_expanded(lane)) return Rect{};
  const Rect track = track_rect(lane);
  return Rect{track.x, track.bottom(), track.width, kGraphHeight};
}

Rect KeyframeView::reveal_rect(std::size_t lane) const {
  const Rect row = lane_rect(lane);
  if (row.width <= 0.0 || model_.lanes[lane].curve.empty()) return Rect{};
  return Rect{row.x, row.y, kRevealWidth, row_height_};
}

double KeyframeView::axis_x() const {
  return bounds().x + std::min(kNameWidth, bounds().width) + kEdgeInset;
}

double KeyframeView::axis_width() const {
  return std::max(0.0, bounds().width - std::min(kNameWidth, bounds().width) - kEdgeInset * 2.0);
}

double KeyframeView::view_span() const noexcept {
  return view_span_ > 0.0 ? view_span_ : model_.duration;
}

void KeyframeView::set_view(double start, double span) {
  if (model_.duration < kShortest) {
    view_start_ = 0.0;
    view_span_ = 0.0;
    return;
  }

  view_span_ = std::clamp(span, model_.duration * kMinSpan, model_.duration);
  // Never past either end. There is nothing before a clip starts or after it
  // finishes, and scrolling into it only produces empty space nobody asked for.
  view_start_ = std::clamp(start, 0.0, model_.duration - view_span_);
}

void KeyframeView::reset_view() noexcept {
  view_start_ = 0.0;
  view_span_ = 0.0;
}

void KeyframeView::zoom_about(double x, double factor) {
  if (factor <= 0.0 || model_.duration < kShortest) return;
  const double anchor = time_at(x);
  const double span = view_span() / factor;
  // The anchor keeps its place along the axis, so what is under the pointer
  // stays under it.
  const double fraction =
      axis_width() > 0.0 ? std::clamp((x - axis_x()) / axis_width(), 0.0, 1.0) : 0.0;
  set_view(anchor - fraction * span, span);
}

double KeyframeView::x_of(double t) const {
  const double width = axis_width();
  const double span = view_span();
  if (span < kShortest || width <= 0.0) return axis_x();
  return axis_x() + (t - view_start_) / span * width;
}

double KeyframeView::time_at(double x) const {
  const double width = axis_width();
  const double span = view_span();
  if (span < kShortest || width <= 0.0) return 0.0;
  return std::clamp(view_start_ + (x - axis_x()) / width * span, 0.0, model_.duration);
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
  double top = bounds().y + ruler_height_;
  if (y < top) return model_.lanes.size();
  for (std::size_t lane = 0; lane < model_.lanes.size(); ++lane) {
    top += lane_height(lane);
    if (y < top) return lane;
  }
  return model_.lanes.size();
}

namespace {

/// The value span a lane's graph is drawn against, and the inset inside it.
///
/// Against the curve's own highest and lowest rather than the parameter's
/// range, for the reason `paint_graph` gives. Kept here so the painting and the
/// hit testing cannot drift apart about where a value sits.
constexpr double kGraphInset = 4.0;

}  // namespace

double KeyframeView::graph_y_of(std::size_t lane, double value) const {
  const Rect box = graph_rect(lane);
  if (box.empty() || lane >= model_.lanes.size()) return 0.0;
  const std::vector<double>& curve = model_.lanes[lane].curve;
  if (curve.size() < 2) return box.y + box.height / 2.0;

  const auto [low, high] = std::ranges::minmax(curve);
  const double span = high - low;
  const double fraction = span < kShortest ? 0.5 : (value - low) / span;
  return box.bottom() - kGraphInset - fraction * (box.height - kGraphInset * 2.0);
}

std::optional<double> KeyframeView::graph_value_at(std::size_t lane, double y) const {
  const Rect box = graph_rect(lane);
  if (box.empty() || lane >= model_.lanes.size()) return std::nullopt;
  const std::vector<double>& curve = model_.lanes[lane].curve;
  if (curve.size() < 2) return std::nullopt;

  const auto [low, high] = std::ranges::minmax(curve);
  const double span = high - low;
  // A curve that never moves is drawn down the middle, and there is no scale to
  // invert. Saying so is better than dividing by nothing and reporting a value
  // that would send a handle to infinity.
  if (span < kShortest) return std::nullopt;

  const double usable = box.height - kGraphInset * 2.0;
  if (usable <= 0.0) return std::nullopt;
  return low + (box.bottom() - kGraphInset - y) / usable * span;
}

Rect KeyframeView::handle_rect(std::size_t lane, std::size_t index,
                               KeyframeHandle side) const {
  if (side == KeyframeHandle::None || lane >= model_.lanes.size()) return Rect{};
  if (!is_selected(lane, index)) return Rect{};

  const Rect box = graph_rect(lane);
  if (box.empty()) return Rect{};

  const Lane& row = model_.lanes[lane];
  // Values and handles are optional on a lane. Without them there is nothing to
  // place a handle against, and guessing would put a control somewhere it does
  // not belong.
  if (row.values.size() != row.times.size() || row.handles.size() != row.times.size()) {
    return Rect{};
  }
  if (index >= row.times.size()) return Rect{};

  // The keyframe at the far end of the segment this handle shapes. Without one
  // there is no segment, and a handle for it would shape nothing.
  const bool outgoing = side == KeyframeHandle::Out;
  if (outgoing && index + 1 >= row.times.size()) return Rect{};
  if (!outgoing && index == 0) return Rect{};
  const std::size_t far = outgoing ? index + 1 : index - 1;

  const KeyframeHandles& handles = row.handles[index];
  const double fx = outgoing ? handles.out_x : handles.in_x;
  const double fy = outgoing ? handles.out_y : handles.in_y;

  // Measured from this keyframe toward the far one, which is what makes the two
  // sides symmetric: pulling either outward is a larger number.
  const double t = row.times[index] + (row.times[far] - row.times[index]) * fx;
  const double v = row.values[index] + (row.values[far] - row.values[index]) * fy;

  const double x = x_of(t);
  const double y = graph_y_of(lane, v);
  return Rect{x - kHandleSize, y - kHandleSize, kHandleSize * 2.0, kHandleSize * 2.0};
}

KeyframeHandle KeyframeView::handle_at(double x, double y, std::size_t& lane,
                                       std::size_t& index) const {
  for (std::size_t l = 0; l < model_.lanes.size(); ++l) {
    if (graph_rect(l).empty()) continue;
    for (const KeyframeHit& picked : selection_) {
      if (picked.lane != l) continue;
      for (const KeyframeHandle side : {KeyframeHandle::Out, KeyframeHandle::In}) {
        const Rect box = handle_rect(l, picked.index, side);
        if (box.empty()) continue;
        const double cx = box.x + box.width / 2.0;
        const double cy = box.y + box.height / 2.0;
        if (std::hypot(cx - x, cy - y) <= kHandleReach) {
          lane = l;
          index = picked.index;
          return side;
        }
      }
    }
  }
  return KeyframeHandle::None;
}

KeyframeHit KeyframeView::keyframe_at(double x, double y) const {
  const std::size_t lane = lane_at(y);
  if (lane >= model_.lanes.size()) return {};

  // Nearest rather than first, so overlapping diamonds resolve to the one the
  // pointer is actually closest to instead of whichever came first in the list.
  KeyframeHit best;
  double closest = kGrabReach;
  const Rect track = track_rect(lane);
  const Lane& row = model_.lanes[lane];
  for (std::size_t i = 0; i < row.times.size(); ++i) {
    const double at = x_of(row.times[i]);
    // Scrolled out of the view is out of reach. Without this a keyframe just
    // past the left edge can be grabbed by pressing on the property's name.
    if (at < track.x || at > track.right()) continue;
    const double distance = std::abs(at - x);
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
  //
  // Measured against the theme's metrics rather than against `row_height_`,
  // which is only right after a layout has run — and sizing is what decides
  // whether one does.
  double rows = 0.0;
  for (std::size_t lane = 0; lane < model_.lanes.size(); ++lane) {
    rows += metrics.list_row_height;
    if (!model_.lanes[lane].curve.empty() && expanded_.contains(model_.lanes[lane].name)) {
      rows += kGraphHeight;
    }
  }
  return LayoutItem::fixed(metrics.panel_header_height + rows);
}

void KeyframeView::layout(const LayoutContext& context) {
  const Metrics& metrics = context.metrics();
  row_height_ = metrics.list_row_height;
  ruler_height_ = metrics.panel_header_height;
  font_size_ = metrics.small_font_size;
}

// ---------------------------------------------------------------- painting --

void KeyframeView::paint_graph(Painter& painter, const Theme& theme, std::size_t lane) const {
  const Rect box = graph_rect(lane);
  if (box.empty()) return;

  const std::vector<double>& curve = model_.lanes[lane].curve;
  if (curve.size() < 2 || model_.duration < kShortest) return;

  const SurfaceStyle& panel = theme.style(Part::Panel, State::Normal);
  const Color faint{panel.text.r, panel.text.g, panel.text.b, 0.35f};
  painter.fill(box, 0.0, Fill::solid(Color{panel.text.r, panel.text.g, panel.text.b, 0.04f}));

  // Against the curve's own highest and lowest rather than against the
  // parameter's range: a rotation that moves by two degrees over a clip is a
  // flat line on its own -180..180 range and a visible ramp on this one, and
  // the second is what anybody wants to see. A curve that never moves is drawn
  // down the middle rather than divided by nothing.
  const auto [low, high] = std::ranges::minmax(curve);
  const double span = high - low;
  const auto y_of = [&](double v) {
    const double inset = 4.0;
    const double fraction = span < kShortest ? 0.5 : (v - low) / span;
    return box.bottom() - inset - fraction * (box.height - inset * 2.0);
  };
  const auto t_of = [&](std::size_t i) {
    return static_cast<double>(i) / static_cast<double>(curve.size() - 1) * model_.duration;
  };

  painter.push_clip(box, 0.0);

  // The speed underneath, fainter: the slope of the value curve, which is what
  // makes a hold read as a hold and an ease as a slow-in. Premiere gives it a
  // graph of its own; there is no room for two here, and one is legible.
  std::vector<double> speed(curve.size(), 0.0);
  double fastest = 0.0;
  for (std::size_t i = 1; i < curve.size(); ++i) {
    speed[i] = std::abs(curve[i] - curve[i - 1]);
    fastest = std::max(fastest, speed[i]);
  }
  if (fastest > kShortest) {
    for (std::size_t i = 2; i < curve.size(); ++i) {
      const double y0 = box.bottom() - speed[i - 1] / fastest * (box.height - 8.0) - 4.0;
      const double y1 = box.bottom() - speed[i] / fastest * (box.height - 8.0) - 4.0;
      painter.line(x_of(t_of(i - 1)), y0, x_of(t_of(i)), y1, faint, 1.0);
    }
  }

  for (std::size_t i = 1; i < curve.size(); ++i) {
    painter.line(x_of(t_of(i - 1)), y_of(curve[i - 1]), x_of(t_of(i)), y_of(curve[i]),
                 theme.accent, 1.5);
  }

  // The handles of whichever keyframes are selected, on top of the curve.
  //
  // Only the selection. Every handle of every keyframe at once is a thicket
  // nobody can aim at, and it is the selection that any operation acts on
  // anyway. Premiere shows them the same way.
  const Lane& row = model_.lanes[lane];
  if (row.values.size() == row.times.size() && row.handles.size() == row.times.size()) {
    for (const KeyframeHit& picked : selection_) {
      if (picked.lane != lane || picked.index >= row.times.size()) continue;

      const double anchor_x = x_of(row.times[picked.index]);
      const double anchor_y = y_of(row.values[picked.index]);
      for (const KeyframeHandle side : {KeyframeHandle::Out, KeyframeHandle::In}) {
        const Rect grip = handle_rect(lane, picked.index, side);
        if (grip.empty()) continue;

        // Solid on a segment that is listening to it, hollow on one that is
        // not: a keyframe still set to Linear shows where its handles *would*
        // be, and pulling one is what switches the segment over. A filled
        // control that did nothing would be a lie.
        const bool live = row.handles[picked.index].bezier;
        const double cx = grip.x + grip.width / 2.0;
        const double cy = grip.y + grip.height / 2.0;
        painter.line(anchor_x, anchor_y, cx, cy, live ? theme.accent : faint, 1.0);
        painter.fill(grip, 0.0, Fill::solid(live ? theme.accent : faint));
      }
    }
  }

  painter.pop_clip();
}

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
  // Against the *visible* span rather than the whole clip, so zooming in gives
  // finer marks instead of the same six spread further apart.
  TimeScale scale;
  scale.fit(view_span(), std::max(1.0, axis_width()));
  const Rect strip_track{axis_x() - kEdgeInset, strip.y,
                         axis_width() + kEdgeInset * 2.0, strip.height};
  painter.push_clip(strip_track, 0.0);
  for (const Tick& tick :
       ruler_ticks(scale, view_start_, view_start_ + view_span(), kRulerFps)) {
    const double x = x_of(tick.time);
    const double height = tick.major ? strip.height * 0.5 : strip.height * 0.25;
    painter.line(x, strip.bottom() - height, x, strip.bottom(), ruler_style.text, 1.0);
    if (!tick.major) continue;
    painter.text(text_run(Rect{x + 3.0, strip.y, kNameWidth, strip.height * 0.7},
                          label_for(tick.time, model_.duration), ruler_style, font_size_));
  }
  painter.pop_clip();

  for (std::size_t lane = 0; lane < model_.lanes.size(); ++lane) {
    const Rect row = lane_rect(lane);
    const Rect track = track_rect(lane);

    // Alternating rows, so a diamond can be traced back to its property across
    // a panel's width without counting.
    if (lane % 2 == 1) {
      painter.fill(row, 0.0, Fill::solid(Color{panel.text.r, panel.text.g, panel.text.b, 0.05f}));
    }

    // The chevron, for a lane that has a curve to show. Right for closed and
    // down for open, the same way the inspector's disclosure reads.
    const Rect reveal = reveal_rect(lane);
    if (!reveal.empty()) {
      const double cx = reveal.x + reveal.width * 0.5;
      const double cy = reveal.y + row_height_ * 0.5;
      constexpr double reach = 3.0;
      if (is_expanded(lane)) {
        painter.line(cx - reach, cy - reach * 0.5, cx, cy + reach * 0.5, panel.text, 1.0);
        painter.line(cx, cy + reach * 0.5, cx + reach, cy - reach * 0.5, panel.text, 1.0);
      } else {
        painter.line(cx - reach * 0.5, cy - reach, cx + reach * 0.5, cy, panel.text, 1.0);
        painter.line(cx + reach * 0.5, cy, cx - reach * 0.5, cy + reach, panel.text, 1.0);
      }
    }

    painter.text(text_run(Rect{row.x + kRevealWidth, row.y,
                               kNameWidth - kRevealWidth - kDiamond, row_height_},
                          model_.lanes[lane].name, panel, font_size_));

    paint_graph(painter, theme, lane);

    // The line the keyframes sit on, so a lane with one point still reads as a
    // span of time rather than as a stray mark.
    const double mid = track.y + track.height * 0.5;
    painter.line(track.x, mid, track.right(), mid, Color{panel.text.r, panel.text.g,
                                                         panel.text.b, 0.25},
                 1.0);

    // Zoomed in, most of a lane's keyframes are off the ends of the axis.
    // Clipped rather than skipped, so one that is half in view is drawn half.
    painter.push_clip(track, 0.0);
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
    painter.pop_clip();
  }

  // Last, so it is never hidden under a lane's fill or a diamond. Only when it
  // is in view: a line drawn at the playhead's off-screen x would run down the
  // property names, or outside the widget altogether.
  const double head = x_of(playhead_);
  if (head >= axis_x() - kEdgeInset && head <= axis_x() + axis_width() + kEdgeInset) {
    painter.line(head, strip.y, head, bounds().bottom(), theme.accent, 1.0);
  }

  if (!marquee_.empty()) {
    painter.fill(marquee_, 0.0,
                 Fill::solid(Color{theme.accent.r, theme.accent.g, theme.accent.b, 0.15f}));
    painter.stroke(marquee_, 0.0, theme.accent, 1.0);
  }
}

// ------------------------------------------------------------------ input --

void KeyframeView::select_within(const Rect& band, bool add) {
  selection_ = add ? before_band_ : std::vector<KeyframeHit>{};

  for (std::size_t lane = 0; lane < model_.lanes.size(); ++lane) {
    // By the lane's *row*, not by the diamond's own few pixels. A band swept
    // across a row is meant to take everything on it, and asking anybody to
    // drag through the vertical middle of a diamond would make the gesture
    // useless.
    const Rect row = lane_rect(lane);
    if (row.bottom() <= band.y || row.y >= band.bottom()) continue;

    for (std::size_t i = 0; i < model_.lanes[lane].times.size(); ++i) {
      const double x = x_of(model_.lanes[lane].times[i]);
      if (x < band.x || x > band.right()) continue;
      const KeyframeHit hit{.lane = lane, .index = i, .found = true};
      if (!is_selected(lane, i)) selection_.push_back(hit);
    }
  }
}

bool KeyframeView::on_mouse_down(const MouseEvent& event) {
  const KeyframeHit hit = keyframe_at(event.x, event.y);

  if (event.button == MouseButton::Right) {
    // A right-click on an unselected keyframe takes it first, the way every
    // context menu everywhere does. On one that is already selected it leaves
    // the selection alone, so a menu can act on all of them.
    if (hit.found && !is_selected(hit.lane, hit.index)) {
      select_only(hit);
      if (on_select_) on_select_();
    }
    // Nothing selected means nothing to offer, and an empty menu is worse than
    // none. Unhandled, so it carries on to whatever is behind.
    if (selection_.empty()) return false;
    if (on_context_menu_) on_context_menu_(event.x, event.y);
    return true;
  }

  if (event.button != MouseButton::Left) return false;

  // A handle, before the chevron and before the keyframes. It is drawn on top
  // of the graph and sits inside a lane's row, so anything tested first would
  // take the press out from under it.
  {
    std::size_t lane = 0;
    std::size_t index = 0;
    if (const KeyframeHandle side = handle_at(event.x, event.y, lane, index);
        side != KeyframeHandle::None) {
      handle_side_ = side;
      handle_lane_ = lane;
      handle_index_ = index;
      const KeyframeHandles& handles = model_.lanes[lane].handles[index];
      handle_x_ = side == KeyframeHandle::Out ? handles.out_x : handles.in_x;
      handle_y_ = side == KeyframeHandle::Out ? handles.out_y : handles.in_y;

      dragging_ = {};
      banding_ = false;
      pressed_ = true;
      moved_ = false;
      press_x_ = event.x;
      press_y_ = event.y;
      return true;
    }
  }

  // The chevron, before anything else — it sits in the gutter, where a press
  // would otherwise begin a rubber band that could never select anything.
  if (const std::size_t lane = lane_at(event.y); lane < model_.lanes.size()) {
    const Rect reveal = reveal_rect(lane);
    if (!reveal.empty() && reveal.contains(event.x, event.y)) {
      const bool opening = !is_expanded(lane);
      set_expanded(model_.lanes[lane].name, opening);
      if (on_expand_) on_expand_(model_.lanes[lane].name, opening);
      return true;
    }
  }

  // The ruler scrubs. It is the one part of the view that is about the
  // playhead rather than about keyframes.
  if (event.y < ruler().bottom()) {
    dragging_ = {};
    banding_ = false;
    pressed_ = false;
    if (on_scrub_) on_scrub_(time_at(event.x));
    return true;
  }

  if (hit.found) {
    // Shift adds to the selection; a plain click replaces it — but only when
    // the keyframe pressed is not already in it, so pressing on one of several
    // to drag them all does not throw the rest away first.
    if (event.modifiers.shift) {
      toggle(hit);
    } else if (!is_selected(hit.lane, hit.index)) {
      select_only(hit);
    }
    if (on_select_) on_select_();

    dragging_ = hit;
    banding_ = false;
    pressed_ = true;
    moved_ = false;
    press_x_ = event.x;
    press_y_ = event.y;
    drag_from_ = model_.lanes[hit.lane].times[hit.index];
    return true;
  }

  // Empty lane space begins a rubber band. A press that never moves is a click,
  // and a click on nothing clears the selection — decided at the release, since
  // until then it could still become a sweep.
  dragging_ = {};
  banding_ = true;
  band_adds_ = event.modifiers.shift;
  before_band_ = selection_;
  pressed_ = true;
  moved_ = false;
  press_x_ = event.x;
  press_y_ = event.y;
  marquee_ = Rect{};
  return true;
}

std::pair<double, double> KeyframeView::handle_from(double x, double y) const {
  double fx = handle_x_;
  double fy = handle_y_;
  if (handle_lane_ >= model_.lanes.size()) return {fx, fy};

  const Lane& row = model_.lanes[handle_lane_];
  if (row.values.size() != row.times.size() || handle_index_ >= row.times.size()) {
    return {fx, fy};
  }

  const bool outgoing = handle_side_ == KeyframeHandle::Out;
  if (outgoing && handle_index_ + 1 >= row.times.size()) return {fx, fy};
  if (!outgoing && handle_index_ == 0) return {fx, fy};
  const std::size_t far = outgoing ? handle_index_ + 1 : handle_index_ - 1;

  // Both axes are a fraction of the segment, measured from this keyframe's own
  // end, so the arithmetic is the same in each direction and the sign falls out
  // of which keyframe is further along.
  if (const double span = row.times[far] - row.times[handle_index_]; std::abs(span) > kShortest) {
    fx = (time_at(x) - row.times[handle_index_]) / span;
  }
  if (const std::optional<double> value = graph_value_at(handle_lane_, y); value.has_value()) {
    if (const double rise = row.values[far] - row.values[handle_index_];
        std::abs(rise) > kShortest) {
      fy = (*value - row.values[handle_index_]) / rise;
    }
  }

  // Clamped in time only. A handle dragged back past its own keyframe describes
  // a curve at two values in the same instant; one pulled past the far keyframe
  // in *value* is an overshoot, which is a shape somebody wants.
  return {std::clamp(fx, 0.0, 1.0), fy};
}

bool KeyframeView::on_mouse_move(const MouseEvent& event) {
  if (!pressed_) return false;

  if (!moved_) {
    // Vertically too, for the band: a sweep straight down a column of lanes is
    // a perfectly ordinary gesture and moves the pointer no distance sideways.
    // Vertically too for a handle, which is dragged in both directions — a
    // handle pulled straight up moves no distance sideways at all.
    const bool two_dimensional = banding_ || handle_side_ != KeyframeHandle::None;
    const double travelled = two_dimensional ? std::max(std::abs(event.x - press_x_),
                                                        std::abs(event.y - press_y_))
                                             : std::abs(event.x - press_x_);
    if (travelled < kDragThreshold) return true;
    moved_ = true;
  }

  if (handle_side_ != KeyframeHandle::None) {
    const auto [x, y] = handle_from(event.x, event.y);
    handle_x_ = x;
    handle_y_ = y;
    if (on_handle_) on_handle_(handle_lane_, handle_index_, handle_side_, x, y);
    return true;
  }

  if (banding_) {
    marquee_ = Rect{std::min(press_x_, event.x), std::min(press_y_, event.y),
                    std::abs(event.x - press_x_), std::abs(event.y - press_y_)};
    select_within(marquee_, band_adds_);
    if (on_select_) on_select_();
    return true;
  }

  if (!dragging_.found) return true;
  if (on_move_) on_move_(dragging_.lane, dragging_.index, time_at(event.x));
  return true;
}

bool KeyframeView::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !pressed_) return false;
  pressed_ = false;

  if (handle_side_ != KeyframeHandle::None) {
    const KeyframeHandle side = std::exchange(handle_side_, KeyframeHandle::None);
    if (moved_) {
      moved_ = false;
      if (on_handle_commit_) {
        on_handle_commit_(handle_lane_, handle_index_, side, handle_x_, handle_y_);
      }
    }
    return true;
  }

  if (banding_) {
    banding_ = false;
    marquee_ = Rect{};
    // It never moved, so it was a click on nothing, and that clears.
    if (!moved_ && !event.modifiers.shift && !selection_.empty()) {
      selection_.clear();
      if (on_select_) on_select_();
    }
    moved_ = false;
    return true;
  }

  if (!moved_ || !dragging_.found) return true;

  moved_ = false;
  const double to = time_at(event.x);
  if (on_move_commit_ && to != drag_from_) {
    on_move_commit_(dragging_.lane, dragging_.index, drag_from_, to);
  }
  return true;
}

bool KeyframeView::on_key_down(const KeyEvent& event) {
  if (event.modifiers.control) {
    // Only with something to act on, so Ctrl+C with nothing selected still
    // means whatever it means elsewhere — copying the selected clip's effects,
    // for one.
    if (event.key == Key::C && !selection_.empty()) {
      if (on_copy_) on_copy_();
      return true;
    }
    if (event.key == Key::V && on_paste_) {
      on_paste_();
      return true;
    }
    return false;
  }

  if (event.key != Key::Delete || selection_.empty()) return false;
  if (on_delete_) on_delete_();
  return true;
}

bool KeyframeView::on_wheel(const WheelEvent& event) {
  if (model_.duration < kShortest || event.delta_y == 0.0) return false;

  // Shift scrolls, everything else zooms. The opposite way round from a
  // document, and the right way round here: the axis is the whole point of the
  // view, and there is nothing to scroll to until it has been zoomed into.
  if (event.modifiers.shift) {
    if (view_span_ <= 0.0) return false;  // the whole clip is showing
    set_view(view_start_ + event.delta_y * view_span() * kScrollNotch, view_span());
    repaint();
    return true;
  }

  zoom_about(event.x, event.delta_y < 0.0 ? kZoomNotch : 1.0 / kZoomNotch);
  repaint();
  return true;
}

}  // namespace cutline::ui
