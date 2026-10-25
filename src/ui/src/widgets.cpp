#include "cutline/ui/widgets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>
#include <vector>

namespace cutline::ui {

// ------------------------------------------------------------------- label --

Label::Label(std::string text, Part part) : text_(std::move(text)), part_(part) {}

LayoutItem Spacer::sizing(Axis axis, const LayoutContext& context) const {
  // Fixed only along the axis it was given, so a sized gap in a column does not
  // also pin the column's width to the same number.
  if (along_.has_value() && *along_ == axis) return LayoutItem::fixed(size_);
  return Widget::sizing(axis, context);
}

double Label::font_size(const Metrics& metrics) const noexcept {
  if (heading_) return metrics.heading_font_size;
  return small_ ? metrics.small_font_size : metrics.font_size;
}

double Label::lead(const Metrics& metrics) const noexcept {
  // Only down a column, and not for the heading that opens one: space above
  // the first thing in a box is the box's padding, which it already has.
  const auto* column = dynamic_cast<const Box*>(parent());
  if (!heading_ || column == nullptr || column->axis() != Axis::Vertical) return 0.0;
  for (const std::unique_ptr<Widget>& sibling : column->children()) {
    if (!sibling->visible()) continue;
    if (sibling.get() == this) return 0.0;
    break;
  }
  return std::max(0.0, metrics.group_spacing - metrics.spacing);
}

LayoutItem Label::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  const double size = font_size(metrics);

  if (axis == Axis::Vertical) return LayoutItem::fixed(lead(metrics) + size * metrics.line_height);

  // Text does not grow, but it will shrink and be clipped rather than force a
  // panel wider than it has room to be. `paint_content` is what makes the
  // second half of that true — without the clip a squeezed label simply draws
  // over whatever is beside it, which is how a parameter row came to read
  // "Opacity100.0%".
  const double width = std::max(context.text.measure(text_, size, bold_ || heading_), min_width_);
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
  Rect area = bounds();
  const double above = lead(theme.metrics);
  area.y += above;
  area.height -= above;
  const bool fits = painter.measure(text_, size, bold_ || heading_) <= area.width + 0.5;
  if (!fits) painter.push_clip(area, 0.0);
  painter.text(text_run(area, text_, style, size, align_, bold_ || heading_));
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
  return LayoutItem::fixed(std::max({width, metrics.control_height, min_width_}));
}

State Button::state() const noexcept {
  const State own = Widget::state();
  return default_ && own == State::Normal ? State::Focused : own;
}

SurfaceStyle Button::surface_style(const Theme& theme) const {
  SurfaceStyle style = Widget::surface_style(theme);
  const double period = theme.metrics.default_pulse;
  WidgetHost* owner = host();
  if (!default_ || state() != State::Focused || period <= 0.0 || owner == nullptr) return style;
  // A sine, brightest a fifth of the way round, as measured from Vista's.
  const float phase = owner->animator().cycle(this, channel::kPulse, period);
  const auto glow = static_cast<float>(
      0.5 + 0.5 * std::cos(2.0 * std::numbers::pi * (static_cast<double>(phase) - 0.2)));
  return mix(style, theme.style(Part::DefaultGlow), glow);
}

void Button::paint_content(Painter& painter, const Theme& theme) const {
  if (text_.empty()) return;
  const SurfaceStyle style = surface_style(theme);
  const double size = theme.metrics.font_size;
  painter.text(text_run(bounds(), text_, style, size, TextAlign::Center, false));

  // Under the first letter, where the eye looks for it, and only while the
  // keyboard is being used -- the way Windows has hidden these since 2000.
  if (mnemonic_ && host() != nullptr && host()->keyboard_cues()) {
    const double whole = painter.measure(text_, size, false);
    const double first = painter.measure(text_.substr(0, 1), size, false);
    const double left = bounds().x + (bounds().width - whole) / 2.0;
    const double under = bounds().y + bounds().height / 2.0 + size * 0.5;
    painter.line(left, under, left + first, under, style.text, 1.0);
  }
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
  if (spacing_) return *spacing_;
  return space(context.metrics(), spacing_step_.value_or(Space::Normal));
}

Edges Box::padding_for(const LayoutContext& context) const noexcept {
  if (padding_step_) return Edges::all(space(context.metrics(), *padding_step_));
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
  const SurfaceStyle& style = theme.style(Part::TitleBar, state());

  // Rounded at the top and square where it meets the window below, as every
  // caption on the platform is: drawn taller by its radius and cut back to its
  // bounds, with the gradient's stops drawn in so each colour stays on its row.
  if (const double radius = style.corner_radius; radius > 0.0) {
    SurfaceStyle taller = style;
    const auto scale = static_cast<float>(bounds().height / (bounds().height + radius));
    for (GradientStop& stop : taller.fill.stops) stop.at *= scale;
    painter.push_clip(bounds(), 0.0);
    paint_surface(painter, Rect{bounds().x, bounds().y, bounds().width, bounds().height + radius},
                  taller);
    painter.pop_clip();
  } else {
    paint_surface(painter, bounds(), style);
  }

  if (title_.empty()) return;
  Rect area = inset(bounds(), Edges::symmetric(theme.metrics.padding_x, 0.0));

  if (mark_) {
    // The application's mark, drawn rather than loaded. An icon file would have
    // to be decoded, scaled and themed, and this is three rectangles — so it is
    // sharp at every size and every scaling factor for free, and it borrows the
    // theme's accent instead of carrying a blue that four themes would fight.
    const double size = std::min(area.height * 0.5, theme.metrics.font_size * 1.15);
    const double x = area.x;
    const double y = area.y + (area.height - size) * 0.5;
    const double row = size * 0.4;
    const double gap = size * 0.2;
    const double cut = x + size * 0.46;
    const double hair = std::max(1.0, size * 0.1);

    // The far side of each cut is the same colour worn thinner, which is what
    // makes two pieces of one clip read as two pieces rather than as two clips.
    const auto thinner = [](Color color, float amount) {
      color.a *= amount;
      return color;
    };
    // One hue, not two. The full icon distinguishes picture from sound by
    // colour, and at thirteen pixels nobody reads that — what they read is
    // whether it is one object, and a second hue against four themes' four
    // accents is three chances out of four to clash for nothing.
    const Color sound = thinner(theme.accent, 0.62f);

    painter.fill(Rect{x, y, cut - hair - x, row}, hair, Fill::solid(theme.accent));
    painter.fill(Rect{cut + hair, y, x + size - cut - hair, row}, hair,
                 Fill::solid(thinner(theme.accent, 0.55f)));
    painter.fill(Rect{x, y + row + gap, cut - hair - x, row}, hair, Fill::solid(sound));
    painter.fill(Rect{cut + hair, y + row + gap, x + size - cut - hair, row}, hair,
                 Fill::solid(thinner(sound, 0.55f)));
    painter.fill(Rect{cut - hair * 0.5, y - gap * 0.5, hair, size + gap * 2.0}, 0.0,
                 Fill::solid(style.text));

    const double taken = size + theme.metrics.spacing;
    area = Rect{area.x + taken, area.y, std::max(0.0, area.width - taken), area.height};
  }

  const Metrics& metrics = theme.metrics;
  TextRun run = text_run(area, title_, style,
                         metrics.caption_font_size > 0.0 ? metrics.caption_font_size
                                                         : metrics.font_size,
                         TextAlign::Left, true);
  run.family = metrics.caption_font_family;
  painter.text(run);
}

// ---------------------------------------------------------- caption button --

CaptionButton::CaptionButton(Kind kind, std::function<void()> on_click)
    : Button({}, std::move(on_click)), kind_(kind) {
  set_part(kind == Kind::Close ? Part::CaptionClose : Part::Caption);
  // Not in the tab order. Closing a window by accident because the focus
  // happened to be resting on the close button would be unforgivable.
  set_focusable(false);
}

LayoutItem CaptionButton::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  const double height = metrics.title_bar_height;
  if (axis == Axis::Vertical) return LayoutItem::fixed(height);
  // A face of the theme's size with a gap after it; the bar's height is still
  // the button's, so the pointer finds it anywhere above or below the face.
  if (metrics.caption_width > 0.0) {
    const double width = kind_ == Kind::Close && metrics.caption_close_width > 0.0
                             ? metrics.caption_close_width
                             : metrics.caption_width;
    return LayoutItem::fixed(width + metrics.caption_gap);
  }
  // Wider than tall, the way every caption button on the platform is.
  return LayoutItem::fixed(std::round(height * 1.6));
}

Rect CaptionButton::face(const Theme& theme) const {
  const Metrics& metrics = theme.metrics;
  if (metrics.caption_width <= 0.0) return bounds();
  const double width = std::max(0.0, bounds().width - metrics.caption_gap);
  const double height = std::min(bounds().height, metrics.caption_height);
  const double y = metrics.caption_hangs ? bounds().y : bounds().y + (bounds().height - height) / 2.0;
  return Rect{bounds().x, y, width, height};
}

bool CaptionButton::paints_surface() const noexcept { return false; }

namespace {

/// XP's glyphs, on the eleven-pixel grid they sit on five pixels into a
/// twenty-one pixel button. `a` is a pixel half covered, where a stroke of
/// the cross ends.
constexpr std::array<std::string_view, 11> kLunaMinimise{{
    "...........", "...........", "...........", "...........", "...........", "...........",
    "...........", "...........", "#######....", "#######....", "#######....",
}};
constexpr std::array<std::string_view, 11> kLunaMaximise{{
    "###########", "###########", "###########", "#.........#", "#.........#", "#.........#",
    "#.........#", "#.........#", "#.........#", "#.........#", "###########",
}};
constexpr std::array<std::string_view, 11> kLunaRestore{{
    "...########", "...########", "...#......#", "########..#", "########..#", "#......#..#",
    "#......####", "#......#...", "#......#...", "#......#...", "########...",
}};
constexpr std::array<std::string_view, 11> kLunaClose{{
    "a#.......#a", "###.....###", ".###...###.", "..###.###..", "...#####...", "....###....",
    "...#####...", "..###.###..", ".###...###.", "###.....###", "a#.......#a",
}};

/// Vista's, measured from its caption buttons: `#` the white body, `o` its
/// dark outline and `h` the outline half covered. Restore was not among the
/// captures, and is the maximise box twice over in the same hand.
constexpr std::array<std::string_view, 9> kVistaMinimise{{
    "...........", "...........", "...........", "...........", ".ooooooooo.",
    "o#########o", "o#########o", "o#########o", ".ooooooooo.",
}};
constexpr std::array<std::string_view, 9> kVistaMaximise{{
    ".ooooooooo.", "o#########o", "o#########o", "o##ooooo##o", "o##o...o##o",
    "o##ooooo##o", "o#########o", "o#########o", ".ooooooooo.",
}};
constexpr std::array<std::string_view, 9> kVistaRestore{{
    "...oooooooo", "...o######o", "ooooooo###o", "o#####o###o", "o#ooo#o###o",
    "o#o.o#ooooo", "o#ooo#o....", "o#####o....", "ooooooo....",
}};
constexpr std::array<std::string_view, 9> kVistaClose{{
    "hoooh.hoooh", "o###oho###o", "ho###o###oh", ".ho#####oh.", "..ho###oh..",
    ".ho#####oh.", "ho###o###oh", "o###oho###o", "hoooh.hoooh",
}};

}  // namespace

void CaptionButton::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle style = surface_style(theme);
  const Rect square = face(theme);
  paint_surface(painter, square, style);
  const Color& ink = style.text;

  if (theme.metrics.caption_glyphs == CaptionGlyphs::Vista) {
    const std::array<std::string_view, 9>* glyph = &kVistaClose;
    switch (kind_) {
      case Kind::Minimise: glyph = &kVistaMinimise; break;
      case Kind::Maximise: glyph = &kVistaMaximise; break;
      case Kind::Restore: glyph = &kVistaRestore; break;
      case Kind::Close: break;
    }
    const Color outline = style.text_shadow ? style.text_shadow->color : style.border;
    Color half = outline;
    half.a *= 0.5f;
    const double left = std::round(square.x + (square.width - 11.0) / 2.0);
    const double top = std::round(square.y + (square.height - 9.0) / 2.0);
    for (std::size_t row = 0; row < glyph->size(); ++row) {
      const std::string_view line = (*glyph)[row];
      for (std::size_t col = 0; col < line.size(); ++col) {
        Color colour;
        switch (line[col]) {
          case '#': colour = ink; break;
          case 'o': colour = outline; break;
          case 'h': colour = half; break;
          default: continue;
        }
        painter.fill(Rect{left + static_cast<double>(col), top + static_cast<double>(row), 1.0, 1.0},
                     0.0, Fill::solid(colour));
      }
    }
    return;
  }

  if (theme.metrics.caption_glyphs == CaptionGlyphs::Luna) {
    const std::array<std::string_view, 11>* glyph = &kLunaClose;
    switch (kind_) {
      case Kind::Minimise: glyph = &kLunaMinimise; break;
      case Kind::Maximise: glyph = &kLunaMaximise; break;
      case Kind::Restore: glyph = &kLunaRestore; break;
      case Kind::Close: break;
    }
    const double unit = square.width / 21.0;
    Color half = ink;
    half.a *= 0.5f;
    for (std::size_t row = 0; row < glyph->size(); ++row) {
      const std::string_view line = (*glyph)[row];
      for (std::size_t col = 0; col < line.size(); ++col) {
        if (line[col] == '.') continue;
        painter.fill(Rect{square.x + (5.0 + static_cast<double>(col)) * unit,
                          square.y + (5.0 + static_cast<double>(row)) * unit, unit, unit},
                     0.0, Fill::solid(line[col] == 'a' ? half : ink));
      }
    }
    return;
  }

  // A square in the middle, the same size whichever glyph is drawn in it, so
  // the three buttons look like a set rather than three separate decisions.
  constexpr double kGlyph = 10.0;
  const double cx = square.x + square.width / 2.0;
  const double cy = square.y + square.height / 2.0;
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

Cursor Splitter::cursor_at(double x, double y) const {
  // While one is being dragged it keeps its cursor wherever the pointer has
  // got to, for the same reason a trim does: the gesture is what is being
  // described, not the pixel.
  const bool over = dragging_ != SplitLayout::kNoDivider ||
                    split_.divider_at(bounds(), x, y) != SplitLayout::kNoDivider;
  if (!over) return Cursor::Arrow;
  // A horizontal splitter stacks its panes side by side, so its dividers move
  // left and right.
  return axis_ == Axis::Horizontal ? Cursor::ResizeWE : Cursor::ResizeNS;
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
  motion_ = metrics.motion;

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

  // Across it, the content's own size *plus the gutter the bar will take out of
  // it*. Asking for the content's width alone and then giving the content that
  // width minus a scrollbar leaves it short by exactly the bar — which is a
  // squeeze in everything it holds, and it was one: the settings pages were
  // ten points narrow in every theme until this was written down.
  //
  // Reserved whether or not a bar is showing. Whether one is depends on how
  // much room this gets along the *other* axis, which is not known while its
  // size is being asked for, and a view that changed width when its content
  // grew a row would shuffle everything beside it.
  const LayoutItem wanted = inner->sizing(axis, context);
  const double gutter = context.metrics().scrollbar_width;
  return LayoutItem{.basis = wanted.basis + gutter,
                    .grow = wanted.grow,
                    .shrink = wanted.shrink,
                    .min = wanted.min + gutter,
                    .max = wanted.max};
}

void ScrollView::scroll_to(double offset) {
  // Anything that sends the view somewhere outright -- the thumb, a jump to
  // a row -- overrules a wheel scroll still on its way.
  glide_.reset();
  place(offset);
}

void ScrollView::place(double offset) {
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

  paint_scroll_track(painter, bar, theme);
  paint_scroll_thumb(painter, thumb(), theme, dragging_ ? State::Pressed : State::Normal);
}

bool ScrollView::on_wheel(const WheelEvent& event) {
  if (!view_.scrollable()) return false;
  const double delta = axis_ == Axis::Vertical ? event.delta_y : event.delta_x;
  if (delta == 0.0) return false;

  // From where a scroll already on its way is going, so notches add up.
  const double from = glide_.has_value() ? glide_->to : view_.offset;
  Viewport aimed = view_;
  aimed.scroll_to(from + delta * step_);
  // Unhandled at the ends, so the wheel keeps bubbling to whatever is outside
  // rather than dying against a view that cannot move any further.
  if (aimed.offset == from) return false;

  if (motion_ <= 0.0 || host() == nullptr) {
    place(aimed.offset);
    return true;
  }
  glide_ = Glide{.from = view_.offset, .to = aimed.offset, .change = ++glides_};
  host()->tick_me(this);
  return true;
}

void ScrollView::tick() {
  if (!glide_.has_value()) return;
  WidgetHost* owner = host();
  // A wheel scroll is a shorter move than a zoom and takes a shorter time:
  // half the theme's, which is about what a notch takes on the platform.
  const float t = owner == nullptr ? 1.0F
                                   : owner->animator().progress(this, channel::kScroll, glide_->change,
                                                                motion_ * 0.5);
  place(std::lerp(glide_->from, glide_->to, static_cast<double>(t)));
  if (t >= 1.0F) {
    glide_.reset();
    return;
  }
  owner->tick_me(this);
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
