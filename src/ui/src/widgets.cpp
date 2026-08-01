#include "cutline/ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cutline::ui {

// ------------------------------------------------------------------- label --

Label::Label(std::string text, Part part) : text_(std::move(text)), part_(part) {}

double Label::font_size(const Metrics& metrics) const noexcept {
  return small_ ? metrics.small_font_size : metrics.font_size;
}

LayoutItem Label::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  const double size = font_size(metrics);

  if (axis == Axis::Vertical) return LayoutItem::fixed(size * metrics.line_height);

  // Text does not grow, but it will shrink and be clipped rather than force a
  // panel wider than it has room to be. `paint_content` is what makes the
  // second half of that true — without the clip a squeezed label simply draws
  // over whatever is beside it, which is how a parameter row came to read
  // "Opacity100.0%".
  const double width = context.text.measure(text_, size, bold_);
  return LayoutItem{.basis = width, .grow = 0.0, .shrink = 1.0, .min = 0.0, .max = kUnbounded};
}

void Label::paint_content(Painter& painter, const Theme& theme) const {
  if (text_.empty()) return;
  const SurfaceStyle& style = theme.style(part_, state());
  const double size = font_size(theme.metrics);

  // Clipped only when it has to be. A clip costs a save and restore in the
  // backend, and nearly every label in the application has the room it asked
  // for — but the one that does not must be cut off at its own edge rather than
  // drawn across whatever is beside it.
  const bool fits = painter.measure(text_, size, bold_) <= bounds().width + 0.5;
  if (!fits) painter.push_clip(bounds(), 0.0);
  painter.text(text_run(bounds(), text_, style, size, align_, bold_));
  if (!fits) painter.pop_clip();
}

// ------------------------------------------------------------------ button --

Button::Button(std::string text, std::function<void()> on_click)
    : text_(std::move(text)), on_click_(std::move(on_click)) {
  set_focusable(true);
}

LayoutItem Button::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) return LayoutItem::fixed(metrics.control_height);

  // An icon-sized button is square, whatever the theme's control height is.
  if (text_.empty()) return LayoutItem::fixed(metrics.control_height);

  const double label = context.text.measure(text_, metrics.font_size, false);
  const double width = label + 2.0 * metrics.padding_x;
  // Never narrower than it is tall, so a one-character button is not a sliver.
  return LayoutItem::fixed(std::max(width, metrics.control_height));
}

void Button::paint_content(Painter& painter, const Theme& theme) const {
  if (text_.empty()) return;
  const SurfaceStyle& style = theme.style(part_, state());
  painter.text(
      text_run(bounds(), text_, style, theme.metrics.font_size, TextAlign::Center, false));
}

bool Button::on_mouse_down(const MouseEvent& event) {
  // Taken so the host captures the pointer; the click itself is decided on the
  // way back up.
  return event.button == MouseButton::Left;
}

bool Button::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  // Capture means this arrives wherever the release happened, so pressing and
  // sliding off cancels the click, which is what a button is expected to do.
  if (hit(event.x, event.y)) fire();
  return true;
}

bool Button::on_key_down(const KeyEvent& event) {
  if (event.key != Key::Space && event.key != Key::Enter) return false;
  if (!event.modifiers.none()) return false;
  fire();
  return true;
}

void Button::fire() {
  if (on_click_) on_click_();
}

// --------------------------------------------------------------------- box --

Box::Box(Axis axis) : axis_(axis) {}

double Box::spacing_for(const LayoutContext& context) const noexcept {
  return spacing_.value_or(context.metrics().spacing);
}

Edges Box::padding_for(const LayoutContext&) const noexcept {
  // Nothing by default: a plain box is a grouping, and nesting a few of them
  // would otherwise accumulate padding nobody asked for. `Panel` is the one
  // that surrounds its contents, and takes the amount from the theme.
  return padding_.value_or(Edges{});
}

void Box::layout(const LayoutContext& context) { layout_into(bounds(), context); }

void Box::layout_into(const Rect& area, const LayoutContext& context) {
  std::vector<BoxChild> items;
  std::vector<Widget*> targets;
  items.reserve(children().size());
  targets.reserve(children().size());

  for (const std::unique_ptr<Widget>& child : children()) {
    // A hidden child takes no room at all rather than leaving a gap where it
    // would have been.
    if (!child->visible()) continue;
    items.push_back(BoxChild{.main = child->sizing(axis_, context),
                             .cross_size = child->sizing(cross_axis(axis_), context).basis});
    targets.push_back(child.get());
  }

  const std::vector<Rect> laid =
      layout_box(area,
                 BoxLayout{.axis = axis_,
                           .spacing = spacing_for(context),
                           .padding = padding_for(context),
                           .cross = cross_,
                           .main = main_},
                 items);

  for (std::size_t i = 0; i < targets.size(); ++i) targets[i]->arrange(laid[i], context);
}

LayoutItem Box::sizing(Axis axis, const LayoutContext& context) const {
  const Edges pad = padding_for(context);
  const double gap = spacing_for(context);
  const bool along = axis == axis_;

  double basis = 0.0;
  double minimum = 0.0;
  double grow = 0.0;
  std::size_t counted = 0;

  for (const std::unique_ptr<Widget>& child : children()) {
    if (!child->visible()) continue;
    const LayoutItem item = child->sizing(axis, context);
    if (along) {
      basis += item.basis;
      minimum += item.min;
      grow += item.grow;
    } else {
      // Across the axis the children sit side by side, so the box needs to be
      // as big as the largest rather than as big as all of them. Flexibility
      // is not inherited here: a spacer is flexible in every direction, and a
      // toolbar that took that on would grow to swallow the window.
      basis = std::max(basis, item.basis);
      minimum = std::max(minimum, item.min);
    }
    ++counted;
  }

  if (along && counted > 1) {
    const double gaps = gap * static_cast<double>(counted - 1);
    basis += gaps;
    minimum += gaps;
  }

  const double surround = axis == Axis::Horizontal ? pad.horizontal() : pad.vertical();
  return LayoutItem{.basis = basis + surround,
                    .grow = along ? grow : (fills_cross_ ? 1.0 : 0.0),
                    .shrink = 1.0,
                    .min = minimum + surround,
                    .max = kUnbounded};
}

// ----------------------------------------------------------------- grab row --

GrabRow::GrabRow(Axis axis) : Box(axis) {}

void GrabRow::paint_content(Painter& painter, const Theme& theme) const {
  if (!selected()) return;
  painter.line(bounds().x, bounds().y, bounds().right(), bounds().y, theme.accent, 2.0);
}

bool GrabRow::on_mouse_down(const MouseEvent& event) {
  // Only presses nothing inside wanted. A press on a checkbox in this row is
  // the checkbox's; reaching here at all means it bubbled past everything.
  if (event.button == MouseButton::Right) {
    if (!on_context_menu_) return false;
    on_context_menu_(event.x, event.y);
    return true;
  }
  if (event.button != MouseButton::Left) return false;

  pressed_ = true;
  dragging_ = false;
  press_x_ = event.x;
  press_y_ = event.y;
  // Taken so the host captures the pointer, which is what lets a drag carry on
  // past this row's own edges — and a reorder always leaves them.
  return true;
}

bool GrabRow::on_mouse_move(const MouseEvent& event) {
  if (!pressed_) return false;

  if (!dragging_) {
    // Vertically as well: a stack is a column, and a reorder moves the pointer
    // hardly any distance sideways.
    if (std::max(std::abs(event.x - press_x_), std::abs(event.y - press_y_)) < kDragThreshold) {
      return true;
    }
    dragging_ = true;
  }

  if (on_drag_) on_drag_(event.x, event.y);
  return true;
}

bool GrabRow::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !pressed_) return false;
  const bool moved = dragging_;
  pressed_ = false;
  dragging_ = false;
  // A press and release in place is a click on the row, which means nothing
  // here — the controls inside it are what a click is for.
  if (moved && on_drop_) on_drop_(event.x, event.y);
  return true;
}

// --------------------------------------------------------------- title bar --

TitleBar::TitleBar(std::string title) : Box(Axis::Horizontal), title_(std::move(title)) {
  // The caption buttons sit at the trailing edge; the title is drawn at the
  // leading one, so nothing has to be laid out around it.
  set_main(Align::End);
  set_spacing(0.0);
}

LayoutItem TitleBar::sizing(Axis axis, const LayoutContext& context) const {
  if (axis == Axis::Vertical) return LayoutItem::fixed(context.metrics().title_bar_height);
  return LayoutItem::flexible();
}

void TitleBar::paint_content(Painter& painter, const Theme& theme) const {
  if (title_.empty()) return;

  const SurfaceStyle& style = theme.style(Part::TitleBar, state());
  const Rect area = inset(bounds(), Edges::symmetric(theme.metrics.padding_x, 0.0));
  painter.text(text_run(area, title_, style, theme.metrics.font_size, TextAlign::Left, true));
}

// ---------------------------------------------------------- caption button --

CaptionButton::CaptionButton(Kind kind, std::function<void()> on_click)
    : Button({}, std::move(on_click)), kind_(kind) {
  set_part(Part::ToolButton);
  // Not in the tab order. Closing a window by accident because the focus
  // happened to be resting on the close button would be unforgivable.
  set_focusable(false);
}

LayoutItem CaptionButton::sizing(Axis axis, const LayoutContext& context) const {
  const double height = context.metrics().title_bar_height;
  if (axis == Axis::Vertical) return LayoutItem::fixed(height);
  // Wider than tall, the way every caption button on the platform is.
  return LayoutItem::fixed(std::round(height * 1.6));
}

void CaptionButton::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(part(), state());
  const Color& ink = style.text;

  // A square in the middle, the same size whichever glyph is drawn in it, so
  // the three buttons look like a set rather than three separate decisions.
  constexpr double kGlyph = 10.0;
  const double cx = bounds().x + bounds().width / 2.0;
  const double cy = bounds().y + bounds().height / 2.0;
  const double half = kGlyph / 2.0;
  const double weight = 1.0;

  switch (kind_) {
    case Kind::Minimise:
      painter.line(cx - half, cy, cx + half, cy, ink, weight);
      break;

    case Kind::Maximise:
      painter.stroke(Rect{cx - half, cy - half, kGlyph, kGlyph}, 0.0, ink, weight);
      break;

    case Kind::Restore: {
      // Two overlapping squares, the front one offset down and left.
      constexpr double kOffset = 2.5;
      const double small = kGlyph - kOffset;
      painter.stroke(Rect{cx - half + kOffset, cy - half, small, small}, 0.0, ink, weight);
      painter.stroke(Rect{cx - half, cy - half + kOffset, small, small}, 0.0, ink, weight);
      break;
    }

    case Kind::Close:
      painter.line(cx - half, cy - half, cx + half, cy + half, ink, weight);
      painter.line(cx + half, cy - half, cx - half, cy + half, ink, weight);
      break;
  }
}

// ---------------------------------------------------------------- splitter --

Splitter::Splitter(Axis axis) : axis_(axis), split_(axis, {}) {}

void Splitter::set_fractions(std::vector<double> fractions) {
  if (fractions.empty()) return;
  // The divider width and minimum are the theme's, and are filled in at the
  // next layout; only the proportions are being set here.
  split_ = SplitLayout(axis_, std::move(fractions));
}

std::span<const double> Splitter::fractions() const noexcept { return split_.fractions(); }

std::vector<Widget*> Splitter::panes() const {
  std::vector<Widget*> out;
  for (const std::unique_ptr<Widget>& child : children()) {
    if (child->visible()) out.push_back(child.get());
  }
  return out;
}

void Splitter::layout(const LayoutContext& context) {
  const std::vector<Widget*> visible = panes();
  if (visible.empty()) return;

  // Rebuilt each time so the divider width and minimum follow the theme, but
  // carrying the fractions over: whatever the user has dragged to survives a
  // resize, and survives changing theme.
  std::vector<double> carried(split_.fractions().begin(), split_.fractions().end());
  if (carried.size() != visible.size()) carried.assign(visible.size(), 1.0);

  const Metrics& metrics = context.metrics();
  split_ = SplitLayout(axis_, std::move(carried), metrics.splitter_width, metrics.min_pane);

  const std::vector<Rect> laid = split_.panes(bounds());
  for (std::size_t i = 0; i < visible.size(); ++i) visible[i]->arrange(laid[i], context);
}

void Splitter::paint_overlay(Painter& painter, const Theme& theme) const {
  // Over the panes rather than between them: a divider with a shadow or a
  // bevel needs to sit on top of what it separates, not in a gap left for it.
  for (std::size_t i = 0; i < split_.divider_count(); ++i) {
    const State state = i == dragging_    ? State::Pressed
                        : i == hovered_divider_ ? State::Hover
                                                : State::Normal;
    paint_surface(painter, split_.divider(bounds(), i), theme.style(Part::Splitter, state));
  }
}

bool Splitter::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  const std::size_t found = split_.divider_at(bounds(), event.x, event.y);
  if (found == SplitLayout::kNoDivider) return false;

  // Taking it means the host captures, which is what keeps the drag alive once
  // the pointer runs ahead of the divider it is pulling.
  dragging_ = found;
  return true;
}

bool Splitter::on_mouse_move(const MouseEvent& event) {
  if (dragging_ == SplitLayout::kNoDivider) {
    hovered_divider_ = split_.divider_at(bounds(), event.x, event.y);
    return false;
  }

  const double position = axis_ == Axis::Horizontal ? event.x : event.y;
  if (!split_.drag(bounds(), dragging_, position)) return true;

  // A drag changes how big the panes are, not just where they start, so their
  // contents have to be given their share of the new size. That needs a theme
  // and a text measurer, which no input handler has — hence marking it and
  // letting the frame loop do it once, rather than per mouse move.
  invalidate_layout();
  return true;
}

bool Splitter::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  if (dragging_ == SplitLayout::kNoDivider) return false;
  dragging_ = SplitLayout::kNoDivider;
  hovered_divider_ = split_.divider_at(bounds(), event.x, event.y);
  // Once, at the end. Whoever is keeping these proportions somewhere outside
  // the widget only needs to hear about them when the gesture is over.
  if (on_resize_) on_resize_();
  return true;
}

// -------------------------------------------------------------- scroll view --

ScrollView::ScrollView(Axis axis) : axis_(axis) {
  // The content is meant to run past the edge; that is the entire point.
  set_clips_children(true);
}

Widget& ScrollView::set_content(std::unique_ptr<Widget> content) {
  clear_children();
  view_.offset = 0.0;
  return add(std::move(content));
}

Widget* ScrollView::content() const noexcept {
  return children().empty() ? nullptr : children().front().get();
}

void ScrollView::layout(const LayoutContext& context) {
  Widget* inner = content();
  if (inner == nullptr) {
    view_ = Viewport{};
    has_bar_ = false;
    return;
  }

  const Metrics& metrics = context.metrics();
  bar_width_ = metrics.scrollbar_width;
  // Three lines a notch, which is what every other application does.
  step_ = metrics.font_size * metrics.line_height * 3.0;

  const bool vertical = axis_ == Axis::Vertical;
  const double visible = vertical ? bounds().height : bounds().width;
  const double wanted = inner->sizing(axis_, context).basis;

  view_.visible = visible;
  view_.content = std::max(wanted, visible);
  view_.clamp();

  // The gutter is taken from the other axis, so reserving it cannot change how
  // long the content is and one pass is enough.
  has_bar_ = view_.scrollable();
  const double gutter = has_bar_ ? bar_width_ : 0.0;

  const Rect area = vertical
                        ? Rect{bounds().x, bounds().y, std::max(0.0, bounds().width - gutter),
                               bounds().height}
                        : Rect{bounds().x, bounds().y, bounds().width,
                               std::max(0.0, bounds().height - gutter)};

  inner->arrange(vertical ? Rect{area.x, area.y - view_.offset, area.width, view_.content}
                          : Rect{area.x - view_.offset, area.y, view_.content, area.height},
                 context);
}

LayoutItem ScrollView::sizing(Axis axis, const LayoutContext& context) const {
  // Along the scrolling axis it will take whatever it is given, because
  // reporting the content's full length would defeat the point of scrolling.
  if (axis == axis_) return LayoutItem::flexible();
  const Widget* inner = content();
  if (inner == nullptr) return LayoutItem::flexible();
  return inner->sizing(axis, context);
}

void ScrollView::scroll_to(double offset) {
  const double before = view_.offset;
  view_.scroll_to(offset);
  const double moved = before - view_.offset;
  if (moved == 0.0) return;
  if (Widget* inner = content(); inner != nullptr) {
    axis_ == Axis::Vertical ? inner->translate(0.0, moved) : inner->translate(moved, 0.0);
  }
}

void ScrollView::scroll_by(double delta) { scroll_to(view_.offset + delta); }

Rect ScrollView::track() const {
  if (!has_bar_) return {};
  return axis_ == Axis::Vertical
             ? Rect{bounds().right() - bar_width_, bounds().y, bar_width_, bounds().height}
             : Rect{bounds().x, bounds().bottom() - bar_width_, bounds().width, bar_width_};
}

Rect ScrollView::thumb() const {
  const Rect bar = track();
  if (bar.empty()) return {};

  const bool vertical = axis_ == Axis::Vertical;
  const double length = vertical ? bar.height : bar.width;
  const double size = view_.thumb_size(length);
  const double at = view_.thumb_offset(length);

  return vertical ? Rect{bar.x, bar.y + at, bar.width, size}
                  : Rect{bar.x + at, bar.y, size, bar.height};
}

void ScrollView::paint_overlay(Painter& painter, const Theme& theme) const {
  const Rect bar = track();
  if (bar.empty()) return;

  paint_surface(painter, bar, theme.style(Part::Scrollbar, State::Normal));
  paint_surface(painter, thumb(),
                theme.style(Part::ScrollThumb, dragging_ ? State::Pressed : State::Normal));
}

bool ScrollView::on_wheel(const WheelEvent& event) {
  if (!view_.scrollable()) return false;
  const double delta = axis_ == Axis::Vertical ? event.delta_y : event.delta_x;
  if (delta == 0.0) return false;

  const double before = view_.offset;
  scroll_by(delta * step_);
  // Unhandled at the ends, so the wheel keeps bubbling to whatever is outside
  // rather than dying against a view that cannot move any further.
  return view_.offset != before;
}

bool ScrollView::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  const Rect grip = thumb();
  if (!grip.empty() && grip.contains(event.x, event.y)) {
    // Remembering where inside the thumb it was grabbed is what stops it
    // snapping its top to the cursor on the first pixel of movement.
    grab_ = (axis_ == Axis::Vertical ? event.y - grip.y : event.x - grip.x);
    dragging_ = true;
    return true;
  }

  const Rect bar = track();
  if (bar.empty() || !bar.contains(event.x, event.y)) return false;
  // Clicking the empty track pages towards the click.
  const bool after = axis_ == Axis::Vertical ? event.y > grip.y : event.x > grip.x;
  scroll_by(after ? view_.visible : -view_.visible);
  return true;
}

bool ScrollView::on_mouse_move(const MouseEvent& event) {
  if (!dragging_) return false;

  const Rect bar = track();
  const bool vertical = axis_ == Axis::Vertical;
  const double length = vertical ? bar.height : bar.width;
  const double at = (vertical ? event.y - bar.y : event.x - bar.x) - grab_;

  const double before = view_.offset;
  view_.drag_thumb(length, at);
  const double moved = before - view_.offset;
  if (moved != 0.0) {
    if (Widget* inner = content(); inner != nullptr) {
      vertical ? inner->translate(0.0, moved) : inner->translate(moved, 0.0);
    }
  }
  return true;
}

bool ScrollView::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !dragging_) return false;
  dragging_ = false;
  return true;
}

// ------------------------------------------------------------------- panel --

Panel::Panel(std::string title) : Box(Axis::Vertical), title_(std::move(title)) {
  // Contents that overflow are cut off at the panel edge rather than drawn
  // over the neighbouring panel.
  set_clips_children(true);
}

void Panel::layout(const LayoutContext& context) {
  const Metrics& metrics = context.metrics();
  Rect body = bounds();

  if (title_.empty()) {
    header_ = Rect{};
  } else {
    const double height = std::min(metrics.panel_header_height, body.height);
    header_ = Rect{body.x, body.y, body.width, height};
    body = Rect{body.x, body.y + height, body.width, body.height - height};
  }

  layout_into(inset(body, panel_padding(metrics)), context);
}

LayoutItem Panel::sizing(Axis axis, const LayoutContext& context) const {
  LayoutItem item = Box::sizing(axis, context);
  const Metrics& metrics = context.metrics();

  const double surround = axis == Axis::Horizontal ? panel_padding(metrics).horizontal()
                                                   : panel_padding(metrics).vertical();
  item.basis += surround;
  item.min += surround;

  if (axis == Axis::Vertical && !title_.empty()) {
    item.basis += metrics.panel_header_height;
    item.min += metrics.panel_header_height;
  }
  return item;
}

void Panel::paint_content(Painter& painter, const Theme& theme) const {
  if (header_.empty()) return;

  const SurfaceStyle& style = theme.style(Part::PanelHeader, State::Normal);
  paint_surface(painter, header_, style);

  const Rect text_area = inset(header_, Edges::symmetric(theme.metrics.padding_x, 0.0));
  painter.text(
      text_run(text_area, title_, style, theme.metrics.font_size, TextAlign::Left, true));
}

}  // namespace cutline::ui
