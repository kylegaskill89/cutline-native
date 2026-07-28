#include "cutline/ui/widgets.hpp"

#include <algorithm>
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
  // panel wider than it has room to be.
  const double width = context.text.measure(text_, size, bold_);
  return LayoutItem{.basis = width, .grow = 0.0, .shrink = 1.0, .min = 0.0, .max = kUnbounded};
}

void Label::paint_content(Painter& painter, const Theme& theme) const {
  if (text_.empty()) return;
  const SurfaceStyle& style = theme.style(part_, state());
  painter.text(text_run(bounds(), text_, style, font_size(theme.metrics), align_, bold_));
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
