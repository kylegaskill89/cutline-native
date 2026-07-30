#include "cutline/ui/controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace cutline::ui {
namespace {

/// A range narrower than this is treated as having no extent at all, which
/// keeps every fraction from becoming a division by nearly zero.
constexpr double kDegenerate = 1e-12;

/// Coarse nudge, for arrow keys with shift held.
constexpr double kCoarseNudge = 10.0;

}  // namespace

// -------------------------------------------------------------- the range --

double ValueRange::clamp(double value) const noexcept {
  return std::clamp(value, std::min(minimum, maximum), std::max(minimum, maximum));
}

double ValueRange::quantise(double value) const noexcept {
  if (step <= 0.0) return clamp(value);
  // Counted from the minimum, not from zero. A control from 5 to 100 in tens
  // must offer 5, 15, 25 — quantising from zero would offer 10, 20, 30 and
  // never let it reach either end.
  const double steps = std::round((value - minimum) / step);
  return clamp(minimum + steps * step);
}

double ValueRange::to_fraction(double value) const noexcept {
  const double span = maximum - minimum;
  if (std::abs(span) < kDegenerate) return 0.0;
  return std::clamp((clamp(value) - minimum) / span, 0.0, 1.0);
}

double ValueRange::from_fraction(double fraction) const noexcept {
  return quantise(minimum + std::clamp(fraction, 0.0, 1.0) * (maximum - minimum));
}

double ValueRange::nudge() const noexcept {
  if (step > 0.0) return step;
  const double span = std::abs(maximum - minimum);
  return span < kDegenerate ? 0.0 : span / 100.0;
}

// ------------------------------------------------------------------ slider --

Slider::Slider(ValueRange range, double value) : range_(range) {
  set_focusable(true);
  value_ = range_.quantise(value);
}

void Slider::set_value(double value) { value_ = range_.quantise(value); }

void Slider::set_range(const ValueRange& range) {
  range_ = range;
  value_ = range_.quantise(value_);
}

void Slider::commit(double value) {
  const double next = range_.quantise(value);
  if (next == value_) return;
  value_ = next;
  if (on_change_) on_change_(value_);
}

LayoutItem Slider::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) return LayoutItem::fixed(metrics.control_height);
  // Wide enough that the thumb has somewhere to travel, and happy to be wider.
  return LayoutItem::flexible(1.0, metrics.control_height * 3.0);
}

void Slider::layout(const LayoutContext& context) {
  // From the theme rather than a constant: a bevelled thumb needs more room
  // than a flat one, and no control should be deciding that for itself.
  thumb_size_ = context.metrics().control_height * 0.6;
}

Rect Slider::groove() const {
  const double height = std::max(4.0, bounds().height * 0.25);
  return Rect{bounds().x, bounds().y + (bounds().height - height) / 2.0, bounds().width,
              height};
}

Rect Slider::thumb() const {
  const Rect track = groove();
  const double size = std::min(thumb_size_, bounds().width);
  // The thumb travels the groove minus its own width. Mapping against the full
  // width instead is the same bug as a scrollbar that cannot reach the end.
  const double travel = std::max(0.0, track.width - size);
  return Rect{track.x + fraction() * travel, bounds().y, size, bounds().height};
}

double Slider::value_at(double x) const {
  const Rect track = groove();
  const double size = std::min(thumb_size_, bounds().width);
  const double travel = track.width - size;
  if (travel <= 0.0) return range_.minimum;
  // Measured from the thumb's centre, so the value under the pointer is the
  // one the thumb ends up showing.
  return range_.from_fraction((x - track.x - size / 2.0) / travel);
}

void Slider::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& groove_style = theme.style(Part::Slider, state());
  const Rect track = groove();
  paint_surface(painter, track, groove_style);

  // The filled part, so the value is readable without finding the thumb.
  const Rect knob = thumb();
  const double filled = knob.x + knob.width / 2.0 - track.x;
  if (filled > 0.0) {
    const SurfaceStyle& thumb_style = theme.style(Part::SliderThumb, state());
    painter.fill(Rect{track.x, track.y, filled, track.height}, groove_style.corner_radius,
                 thumb_style.fill);
  }

  paint_surface(painter, knob, theme.style(Part::SliderThumb, state()));
}

void Slider::finish() {
  if (value_ == gesture_start_) return;
  if (on_commit_) on_commit_(value_);
}

bool Slider::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  gesture_start_ = value_;

  // A double-click returns to the parameter's default. Getting back to it is
  // otherwise a matter of dragging very carefully.
  if (event.click_count >= 2 && default_.has_value()) {
    commit(*default_);
    finish();
    return true;
  }

  // Clicking anywhere on the groove jumps there rather than paging. For a
  // value this is what is wanted: the pointer is already where the answer is.
  dragging_ = true;
  commit(value_at(event.x));
  return true;
}

bool Slider::on_mouse_move(const MouseEvent& event) {
  if (!dragging_) return false;
  commit(value_at(event.x));
  return true;
}

bool Slider::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !dragging_) return false;
  dragging_ = false;
  finish();
  return true;
}

bool Slider::on_key_down(const KeyEvent& event) {
  if (event.modifiers.control || event.modifiers.alt) return false;
  const double amount = range_.nudge() * (event.modifiers.shift ? kCoarseNudge : 1.0);

  // Each press is a gesture of its own, so holding an arrow key records one
  // entry per step rather than one for the whole hold. That is what makes
  // undoing a nudge undo exactly that nudge.
  gesture_start_ = value_;

  switch (event.key) {
    case Key::Left:
    case Key::Down:
      commit(value_ - amount);
      break;
    case Key::Right:
    case Key::Up:
      commit(value_ + amount);
      break;
    case Key::Home:
      commit(range_.minimum);
      break;
    case Key::End:
      commit(range_.maximum);
      break;
    default:
      return false;
  }

  finish();
  return true;
}

// ---------------------------------------------------------------- checkbox --

Checkbox::Checkbox(std::string label, bool checked)
    : label_(std::move(label)), checked_(checked) {
  set_focusable(true);
}

void Checkbox::layout(const LayoutContext& context) {
  const Metrics& metrics = context.metrics();
  box_size_ = metrics.control_height * 0.6;
  gap_ = metrics.spacing;
  font_size_ = metrics.font_size;
}

Rect Checkbox::box() const {
  const double size = std::min(box_size_, std::min(bounds().width, bounds().height));
  return Rect{bounds().x, bounds().y + (bounds().height - size) / 2.0, size, size};
}

LayoutItem Checkbox::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) return LayoutItem::fixed(metrics.control_height);

  const double box = metrics.control_height * 0.6;
  if (label_.empty()) return LayoutItem::fixed(box);
  return LayoutItem::fixed(box + metrics.spacing +
                           context.text.measure(label_, metrics.font_size, false));
}

void Checkbox::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(Part::Input, state());
  const Rect square = box();
  paint_surface(painter, square, style);

  if (checked_) {
    // Drawn from lines rather than a glyph, for the same reason the caption
    // buttons are: there is no font that can be relied on to have a tick.
    const double inset_by = square.width * 0.25;
    const double left = square.x + inset_by;
    const double right = square.right() - inset_by;
    const double top = square.y + inset_by;
    const double bottom = square.bottom() - inset_by;
    const double elbow_x = left + (right - left) * 0.36;

    painter.line(left, top + (bottom - top) * 0.5, elbow_x, bottom, style.text, 2.0);
    painter.line(elbow_x, bottom, right, top, style.text, 2.0);
  }

  if (label_.empty()) return;
  const Rect text{square.right() + gap_, bounds().y,
                  std::max(0.0, bounds().right() - square.right() - gap_), bounds().height};
  painter.text(text_run(text, label_, style, font_size_, TextAlign::Left, false));
}

void Checkbox::toggle() {
  checked_ = !checked_;
  if (on_change_) on_change_(checked_);
}

bool Checkbox::on_mouse_down(const MouseEvent& event) {
  // Taken so the press captures; the toggle happens on release, so sliding off
  // cancels it the way it does on a button.
  return event.button == MouseButton::Left;
}

bool Checkbox::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  if (hit(event.x, event.y)) toggle();
  return true;
}

bool Checkbox::on_key_down(const KeyEvent& event) {
  if (event.key != Key::Space || !event.modifiers.none()) return false;
  toggle();
  return true;
}

// ------------------------------------------------------------- menu lists --

namespace {
/// Past the end of any list, which is how "nothing" is spelled here. A
/// sentinel rather than an optional because it is compared against sizes far
/// more often than it is tested for emptiness.
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
}  // namespace

MenuList::MenuList(std::vector<std::string> items) : items_(std::move(items)) {}

void MenuList::set_items(std::vector<std::string> items) {
  items_ = std::move(items);
  highlighted_ = kNone;
}

Rect MenuList::row_rect(std::size_t index) const {
  if (index >= items_.size()) return {};
  const Rect area = bounds();
  const double y = area.y + padding_ + static_cast<double>(index) * row_height_;
  // Clipped to the list, so a row past the bottom is empty rather than a
  // rectangle drawn outside it.
  if (y >= area.bottom()) return {};
  return Rect{area.x, y, area.width, std::min(row_height_, area.bottom() - y)};
}

std::size_t MenuList::row_at(double y) const {
  const Rect area = bounds();
  if (items_.empty() || row_height_ <= 0.0) return kNone;
  if (y < area.y + padding_ || y >= area.bottom() - padding_) return kNone;

  const auto index = static_cast<std::size_t>((y - area.y - padding_) / row_height_);
  return index < items_.size() ? index : kNone;
}

LayoutItem MenuList::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) {
    const double rows = static_cast<double>(items_.size()) * metrics.list_row_height;
    return LayoutItem::fixed(rows + 2.0 * padding_);
  }

  double widest = 0.0;
  for (const std::string& item : items_) {
    widest = std::max(widest, context.text.measure(item, metrics.font_size, false));
  }
  // Room either side, and never so narrow that a one-word menu is a sliver.
  return LayoutItem::fixed(std::max(widest + 4.0 * metrics.padding_x, 96.0));
}

void MenuList::layout(const LayoutContext& context) {
  row_height_ = context.metrics().list_row_height;
  font_size_ = context.metrics().font_size;
}

void MenuList::paint_content(Painter& painter, const Theme& theme) const {
  for (std::size_t i = 0; i < items_.size(); ++i) {
    const Rect row = row_rect(i);
    if (row.empty()) continue;

    // Hover wins over the current value: the row about to be taken is the one
    // worth pointing at, and the one already chosen is where the pointer came
    // from often enough that showing both reads as a muddle.
    State row_state = State::Normal;
    if (i == highlighted_) {
      row_state = State::Hover;
    } else if (i == current_) {
      row_state = State::Selected;
    }

    const SurfaceStyle& style = theme.style(Part::MenuItem, row_state);
    if (row_state != State::Normal) paint_surface(painter, row, style);
    // Indented horizontally only: insetting the height too would shrink the
    // text box and push short labels off their own row.
    const double indent = padding_ + 6.0;
    const Rect text{row.x + indent, row.y, std::max(0.0, row.width - 2.0 * indent), row.height};
    painter.text(text_run(text, items_[i], style, font_size_, TextAlign::Left, false));
  }
}

void MenuList::choose(std::size_t index) {
  if (index >= items_.size()) return;
  current_ = index;
  // Copied before it is called: what it does very often closes this list, and
  // touching a member afterwards would be touching a widget on its way out.
  const auto callback = on_choose_;
  if (callback) callback(index);
}

bool MenuList::on_mouse_move(const MouseEvent& event) {
  highlighted_ = row_at(event.y);
  return true;
}

bool MenuList::on_mouse_down(const MouseEvent& event) {
  // Taken so the release lands here too, and so the host does not mistake a
  // press inside the list for one outside it.
  return event.button == MouseButton::Left;
}

bool MenuList::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  choose(row_at(event.y));
  return true;
}

bool MenuList::on_key_down(const KeyEvent& event) {
  if (items_.empty()) return false;

  const std::size_t last = items_.size() - 1;
  switch (event.key) {
    case Key::Down:
      highlighted_ = highlighted_ == kNone || highlighted_ >= last ? 0 : highlighted_ + 1;
      return true;
    case Key::Up:
      highlighted_ = highlighted_ == kNone || highlighted_ == 0 ? last : highlighted_ - 1;
      return true;
    case Key::Home:
      highlighted_ = 0;
      return true;
    case Key::End:
      highlighted_ = last;
      return true;
    case Key::Enter:
      choose(highlighted_);
      return true;
    default:
      return false;
  }
}

// --------------------------------------------------------------- dropdown --

Dropdown::Dropdown(std::vector<std::string> options, std::size_t selected)
    : options_(std::move(options)), selected_(selected) {
  set_focusable(true);
}

Dropdown::~Dropdown() {
  // The open list holds a callback capturing this. Closing is deferred, but a
  // closing popup takes no input and is not painted, so the callback cannot
  // run after this returns.
  if (open_ && host() != nullptr) host()->close_popup();
}

void Dropdown::set_options(std::vector<std::string> options) {
  options_ = std::move(options);
  if (selected_ >= options_.size()) selected_ = 0;
}

void Dropdown::set_selected(std::size_t index) noexcept {
  if (index < options_.size()) selected_ = index;
}

const std::string& Dropdown::value() const noexcept {
  static const std::string empty;
  return selected_ < options_.size() ? options_[selected_] : empty;
}

Rect Dropdown::arrow() const {
  const Rect area = bounds();
  const double width = std::min(arrow_width_, area.width);
  return Rect{area.right() - width, area.y, width, area.height};
}

void Dropdown::open() {
  WidgetHost* owner = host();
  if (owner == nullptr || options_.empty()) return;

  auto list = std::make_unique<MenuList>(options_);
  list->set_current(selected_);
  list->set_on_choose([this](std::size_t index) {
    selected_ = index;
    open_ = false;
    if (host() != nullptr) host()->close_popup();
    if (on_change_) on_change_(index);
  });

  open_ = true;
  owner->open_popup(std::move(list), bounds());
}

LayoutItem Dropdown::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) return LayoutItem::fixed(metrics.control_height);

  // Wide enough for the longest option, not merely the current one: a dropdown
  // that resizes as its value changes drags the whole row about with it.
  double widest = 0.0;
  for (const std::string& option : options_) {
    widest = std::max(widest, context.text.measure(option, metrics.font_size, false));
  }
  return LayoutItem::fixed(widest + 2.0 * metrics.padding_x + arrow_width_);
}

void Dropdown::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(part(), state());
  const Rect area = bounds();
  const Rect mark = arrow();

  const Rect text{area.x + 8.0, area.y, std::max(0.0, mark.x - area.x - 8.0), area.height};
  painter.text(text_run(text, value(), style, 13.0, TextAlign::Left, false));

  // Two lines rather than a glyph, for the same reason the tick is: no font
  // can be relied on to have one.
  const double centre_x = mark.x + mark.width * 0.5;
  const double centre_y = mark.y + mark.height * 0.5;
  constexpr double reach = 3.5;
  painter.line(centre_x - reach, centre_y - reach * 0.5, centre_x, centre_y + reach * 0.5,
               style.text, 1.5);
  painter.line(centre_x, centre_y + reach * 0.5, centre_x + reach, centre_y - reach * 0.5,
               style.text, 1.5);
}

bool Dropdown::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  open();
  return true;
}

bool Dropdown::on_key_down(const KeyEvent& event) {
  if (!event.modifiers.none()) return false;

  switch (event.key) {
    case Key::Enter:
    case Key::Space:
      open();
      return true;
    // Stepping without opening: how a keyboard user changes one of these
    // quickly, and what every native dropdown does.
    case Key::Down:
      if (selected_ + 1 < options_.size()) {
        selected_ += 1;
        if (on_change_) on_change_(selected_);
      }
      return true;
    case Key::Up:
      if (selected_ > 0) {
        selected_ -= 1;
        if (on_change_) on_change_(selected_);
      }
      return true;
    default:
      return false;
  }
}

// -------------------------------------------------------------- text field --

namespace {

/// The same colour, thinner. What a selection wash and a dimmed placeholder are
/// both made of.
[[nodiscard]] Color thinned(const Color& color, float amount) noexcept {
  return Color{color.r, color.g, color.b, color.a * amount};
}

/// Whether a byte is a UTF-8 continuation. Every caret movement lands where one
/// is not, so a multi-byte character is never cut in half.
[[nodiscard]] bool is_continuation(unsigned char byte) noexcept {
  return (byte & 0xC0) == 0x80;
}

/// One code point, encoded. Anything outside Unicode becomes the replacement
/// character rather than a broken sequence.
[[nodiscard]] std::string encode_utf8(char32_t codepoint) {
  auto value = static_cast<std::uint32_t>(codepoint);
  if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) value = 0xFFFD;

  std::string out;
  if (value < 0x80) {
    out.push_back(static_cast<char>(value));
  } else if (value < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (value >> 6)));
    out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
  } else if (value < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (value >> 12)));
    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (value >> 18)));
    out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
  }
  return out;
}

}  // namespace

TextField::TextField(std::string text) : text_(std::move(text)) {
  set_focusable(true);
  caret_ = text_.size();
  anchor_ = caret_;
}

void TextField::set_text(std::string text) {
  text_ = std::move(text);
  caret_ = std::min(caret_, text_.size());
  while (caret_ > 0 && caret_ < text_.size() &&
         is_continuation(static_cast<unsigned char>(text_[caret_]))) {
    --caret_;
  }
  anchor_ = caret_;
  invalidate_layout();
}

void TextField::set_multiline(bool multiline) noexcept {
  multiline_ = multiline;
  invalidate_layout();
}

void TextField::set_caret(std::size_t index) noexcept {
  caret_ = std::min(index, text_.size());
  while (caret_ > 0 && caret_ < text_.size() &&
         is_continuation(static_cast<unsigned char>(text_[caret_]))) {
    --caret_;
  }
  anchor_ = caret_;
  invalidate_layout();
}

void TextField::select_all() noexcept {
  anchor_ = 0;
  caret_ = text_.size();
  invalidate_layout();
}

std::size_t TextField::next_boundary(std::size_t index) const noexcept {
  if (index >= text_.size()) return text_.size();
  ++index;
  while (index < text_.size() && is_continuation(static_cast<unsigned char>(text_[index]))) {
    ++index;
  }
  return index;
}

std::size_t TextField::previous_boundary(std::size_t index) const noexcept {
  if (index == 0) return 0;
  --index;
  while (index > 0 && is_continuation(static_cast<unsigned char>(text_[index]))) --index;
  return index;
}

std::size_t TextField::line_of(std::size_t index) const noexcept {
  for (std::size_t i = 0; i < lines_.size(); ++i) {
    // `end` is before the newline, so an index sitting on the break belongs to
    // the line it ends.
    if (index <= lines_[i].end) return i;
  }
  return lines_.empty() ? 0 : lines_.size() - 1;
}

LayoutItem TextField::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Horizontal) {
    // Flexible: a field takes the width it is given. Its content decides
    // nothing about how wide it should be, which is what keeps a row of them
    // aligned.
    return LayoutItem::flexible(1.0, metrics.control_height * 3.0);
  }

  if (!multiline_) return LayoutItem::fixed(metrics.control_height);

  const auto rows = static_cast<double>(std::max<std::size_t>(
      static_cast<std::size_t>(min_lines_), std::max<std::size_t>(lines_.size(), 1)));
  return LayoutItem::fixed(rows * metrics.font_size * 1.4 + metrics.padding_y * 2.0);
}

void TextField::layout(const LayoutContext& context) {
  const Metrics& metrics = context.metrics();
  font_size_ = metrics.font_size;
  line_height_ = metrics.font_size * 1.4;
  padding_ = metrics.padding_y;

  lines_.clear();
  std::size_t begin = 0;
  while (true) {
    const std::size_t newline = multiline_ ? text_.find('\n', begin) : std::string::npos;
    const std::size_t end = newline == std::string::npos ? text_.size() : newline;

    Line line;
    line.begin = begin;
    line.end = end;
    // One offset per boundary, including both ends, so a caret anywhere in the
    // line has an x to be drawn at.
    line.offsets.push_back(0.0);
    for (std::size_t at = begin; at < end;) {
      at = next_boundary(at);
      line.offsets.push_back(context.text.measure(
          std::string_view(text_).substr(begin, std::min(at, end) - begin), font_size_, false));
    }
    lines_.push_back(std::move(line));

    if (newline == std::string::npos) break;
    begin = newline + 1;
  }
}

std::size_t TextField::index_at(double x, double y) const {
  if (lines_.empty()) return 0;

  const Rect area = bounds();
  const auto row = static_cast<std::size_t>(
      std::clamp((y - area.y - padding_) / std::max(1.0, line_height_), 0.0,
                 static_cast<double>(lines_.size() - 1)));
  const Line& line = lines_[row];

  const double local = x - (area.x + padding_);
  // The boundary nearest the pointer, not the one before it: clicking in the
  // right half of a character puts the caret after it, which is what every
  // other field does.
  std::size_t best = 0;
  double closest = std::numeric_limits<double>::max();
  for (std::size_t i = 0; i < line.offsets.size(); ++i) {
    const double distance = std::abs(line.offsets[i] - local);
    if (distance < closest) {
      closest = distance;
      best = i;
    }
  }

  std::size_t index = line.begin;
  for (std::size_t step = 0; step < best && index < line.end; ++step) {
    index = next_boundary(index);
  }
  return index;
}

Rect TextField::caret_rect(std::size_t index) const {
  const Rect area = bounds();
  if (lines_.empty()) {
    return Rect{area.x + padding_, area.y + padding_, 1.0, line_height_};
  }

  const std::size_t row = line_of(index);
  const Line& line = lines_[row];

  std::size_t steps = 0;
  for (std::size_t at = line.begin; at < index && at < line.end; at = next_boundary(at)) ++steps;
  const double offset = steps < line.offsets.size() ? line.offsets[steps] : line.offsets.back();

  return Rect{area.x + padding_ + offset, area.y + padding_ + static_cast<double>(row) * line_height_,
              1.0, line_height_};
}

void TextField::replace_selection(std::string_view with) {
  const std::size_t from = selection_begin();
  const std::size_t to = selection_end();
  text_.replace(from, to - from, with);
  caret_ = from + with.size();
  anchor_ = caret_;
  changed();
}

void TextField::erase_before_caret() {
  if (has_selection()) {
    replace_selection({});
    return;
  }
  if (caret_ == 0) return;
  const std::size_t from = previous_boundary(caret_);
  text_.erase(from, caret_ - from);
  caret_ = from;
  anchor_ = caret_;
  changed();
}

void TextField::erase_after_caret() {
  if (has_selection()) {
    replace_selection({});
    return;
  }
  if (caret_ >= text_.size()) return;
  const std::size_t to = next_boundary(caret_);
  text_.erase(caret_, to - caret_);
  changed();
}

void TextField::move_caret(std::size_t to, bool extend) noexcept {
  caret_ = std::min(to, text_.size());
  if (!extend) anchor_ = caret_;
  // Nothing has moved, but the caret has to be drawn where it now is.
  invalidate_layout();
}

void TextField::changed() {
  // The offset table is stale the moment the text is, and both painting and
  // hit-testing read it.
  invalidate_layout();
  if (on_change_) on_change_(text_);
}

void TextField::commit() {
  if (text_ == committed_) return;
  committed_ = text_;
  if (on_commit_) on_commit_(text_);
}

void TextField::on_focus_changed(bool focused) {
  if (focused) {
    committed_ = text_;
    return;
  }
  // Leaving the field is a commit: it is the moment a value has settled, and
  // writing on every keystroke would fill an undo history with single letters.
  dragging_ = false;
  commit();
}

bool TextField::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  if (event.click_count >= 2) {
    select_all();
    dragging_ = false;
    return true;
  }

  set_caret(index_at(event.x, event.y));
  dragging_ = true;
  return true;
}

bool TextField::on_mouse_move(const MouseEvent& event) {
  if (!dragging_) return false;
  move_caret(index_at(event.x, event.y), true);
  return true;
}

bool TextField::on_mouse_up(const MouseEvent& event) {
  if (!dragging_) return false;
  move_caret(index_at(event.x, event.y), true);
  dragging_ = false;
  return true;
}

bool TextField::on_key_down(const KeyEvent& event) {
  const bool extend = event.modifiers.shift;

  if (event.modifiers.control) {
    if (event.key == Key::A) {
      select_all();
      return true;
    }
    return false;
  }

  switch (event.key) {
    case Key::Left:
      move_caret(previous_boundary(has_selection() && !extend ? selection_begin() : caret_),
                 extend);
      return true;
    case Key::Right:
      move_caret(next_boundary(has_selection() && !extend ? selection_end() - 1 : caret_), extend);
      return true;

    case Key::Home:
      move_caret(lines_.empty() ? 0 : lines_[line_of(caret_)].begin, extend);
      return true;
    case Key::End:
      move_caret(lines_.empty() ? text_.size() : lines_[line_of(caret_)].end, extend);
      return true;

    case Key::Up:
    case Key::Down: {
      if (!multiline_ || lines_.size() < 2) return false;
      const std::size_t row = line_of(caret_);
      if (event.key == Key::Up && row == 0) return true;
      if (event.key == Key::Down && row + 1 >= lines_.size()) return true;

      // The column is kept in characters rather than in pixels, which is what
      // makes moving down a line and back up again land where it started.
      std::size_t column = 0;
      for (std::size_t at = lines_[row].begin; at < caret_; at = next_boundary(at)) ++column;

      const Line& target = lines_[event.key == Key::Up ? row - 1 : row + 1];
      std::size_t index = target.begin;
      for (std::size_t step = 0; step < column && index < target.end; ++step) {
        index = next_boundary(index);
      }
      move_caret(index, extend);
      return true;
    }

    case Key::Backspace:
      erase_before_caret();
      return true;
    case Key::Delete:
      erase_after_caret();
      return true;

    case Key::Enter:
      if (multiline_) {
        replace_selection("\n");
      } else {
        commit();
      }
      return true;

    case Key::Escape:
      // Back to what it was when the keyboard arrived, which is the only
      // undo a field can offer on its own.
      if (text_ != committed_) {
        text_ = committed_;
        caret_ = std::min(caret_, text_.size());
        anchor_ = caret_;
        invalidate_layout();
        if (on_change_) on_change_(text_);
      }
      return true;

    default:
      return false;
  }
}

bool TextField::on_text(char32_t codepoint) {
  // Control characters are keys, not text. A newline arrives as Enter, which
  // decides for itself whether this field takes one.
  if (codepoint < 0x20 || codepoint == 0x7F) return false;
  replace_selection(encode_utf8(codepoint));
  return true;
}

void TextField::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(part(), state());
  const Rect area = bounds();

  if (text_.empty() && !placeholder_.empty()) {
    const Rect where{area.x + padding_, area.y, area.width - padding_ * 2.0, area.height};
    painter.text(text_run(where, placeholder_, style, font_size_, TextAlign::Left, false));
  }

  // The selection first, so the text sits on top of it.
  if (has_selection() && focused()) {
    const std::size_t from = selection_begin();
    const std::size_t to = selection_end();
    for (std::size_t row = 0; row < lines_.size(); ++row) {
      const Line& line = lines_[row];
      if (to < line.begin || from > line.end) continue;

      const Rect start = caret_rect(std::max(from, line.begin));
      const Rect stop = caret_rect(std::min(to, line.end));
      const double width = std::max(1.0, stop.x - start.x);
      painter.fill(Rect{start.x, start.y, width, line_height_}, 0.0,
                   Fill::solid(thinned(theme.accent, 0.45f)));
    }
  }

  for (std::size_t row = 0; row < lines_.size(); ++row) {
    const Line& line = lines_[row];
    if (line.end <= line.begin) continue;

    const Rect where{area.x + padding_, area.y + padding_ + static_cast<double>(row) * line_height_,
                     std::max(0.0, area.width - padding_ * 2.0), line_height_};
    painter.text(text_run(where, text_.substr(line.begin, line.end - line.begin), style,
                          font_size_, TextAlign::Left, false));
  }

  if (focused()) {
    const Rect caret = caret_rect(caret_);
    painter.fill(Rect{caret.x, caret.y, 1.0, caret.height}, 0.0, Fill::solid(style.text));
  }
}

// ------------------------------------------------------------ icon button --

IconButton::IconButton(Icon icon, std::function<void()> on_click)
    : Button({}, std::move(on_click)), icon_(icon) {
  set_part(Part::ToolButton);
}

void IconButton::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(part(), state());
  const Rect area = bounds();
  const double cx = area.x + area.width * 0.5;
  const double cy = area.y + area.height * 0.5;

  // Fixed rather than scaled with the button: these sit in a row with text and
  // want to match its weight, not the theme's padding.
  constexpr double reach = 4.0;
  constexpr double width = 1.5;

  switch (icon_) {
    case Icon::ArrowUp:
      painter.line(cx - reach, cy + reach * 0.5, cx, cy - reach * 0.5, style.text, width);
      painter.line(cx, cy - reach * 0.5, cx + reach, cy + reach * 0.5, style.text, width);
      break;
    case Icon::ArrowDown:
      painter.line(cx - reach, cy - reach * 0.5, cx, cy + reach * 0.5, style.text, width);
      painter.line(cx, cy + reach * 0.5, cx + reach, cy - reach * 0.5, style.text, width);
      break;
    case Icon::Cross:
      painter.line(cx - reach, cy - reach, cx + reach, cy + reach, style.text, width);
      painter.line(cx - reach, cy + reach, cx + reach, cy - reach, style.text, width);
      break;
    case Icon::Plus:
      painter.line(cx - reach, cy, cx + reach, cy, style.text, width);
      painter.line(cx, cy - reach, cx, cy + reach, style.text, width);
      break;

    case Icon::Stopwatch: {
      // A circle is a rectangle with its corners rounded all the way, which
      // saves the painter an ellipse primitive it has no other use for.
      const Rect face{cx - reach, cy - reach, reach * 2.0, reach * 2.0};
      painter.stroke(face, reach, style.text, width);
      // One hand, pointing up and to the right, so a clock face reads as one
      // at this size rather than as a ring.
      painter.line(cx, cy, cx + reach * 0.6, cy - reach * 0.6, style.text, width);
      break;
    }

    case Icon::Diamond:
      painter.line(cx, cy - reach, cx + reach, cy, style.text, width);
      painter.line(cx + reach, cy, cx, cy + reach, style.text, width);
      painter.line(cx, cy + reach, cx - reach, cy, style.text, width);
      painter.line(cx - reach, cy, cx, cy - reach, style.text, width);
      break;
  }

  // Whether the toggle is on is drawn into the mark rather than left to the
  // surface underneath. No theme defines a selected state for a tool button, so
  // `set_selected` alone would light nothing — and a stopwatch that looks the
  // same running as stopped is worse than no stopwatch.
  if (selected() && (icon_ == Icon::Stopwatch || icon_ == Icon::Diamond)) {
    const double dot = reach * 0.5;
    painter.fill(Rect{cx - dot, cy - dot, dot * 2.0, dot * 2.0}, dot, Fill::solid(style.text));
  }
}

// ----------------------------------------------------------- progress bar --

ProgressBar::ProgressBar(double fraction) { set_fraction(fraction); }

void ProgressBar::set_fraction(double fraction) noexcept {
  fraction_ = std::clamp(fraction, 0.0, 1.0);
}

Rect ProgressBar::filled() const {
  const Rect area = bounds();
  return Rect{area.x, area.y, area.width * fraction_, area.height};
}

LayoutItem ProgressBar::sizing(Axis axis, const LayoutContext& context) const {
  if (axis == Axis::Vertical) return LayoutItem::fixed(context.metrics().control_height);
  return LayoutItem::flexible(1.0, 80.0);
}

void ProgressBar::paint_content(Painter& painter, const Theme& theme) const {
  const Rect done = filled();
  if (!done.empty()) {
    // The thumb's colour rather than the groove's: this is the part that has
    // happened, and every theme has already decided what the active part of a
    // control looks like.
    paint_surface(painter, done, theme.style(Part::SliderThumb, State::Normal));
  }

  if (text_.empty()) return;
  painter.text(text_run(bounds(), text_, theme.style(Part::Input, State::Normal), 12.0,
                        TextAlign::Center, false));
}

}  // namespace cutline::ui
