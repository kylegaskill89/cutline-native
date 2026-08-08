#include "cutline/ui/controls.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <numbers>
#include <utility>
#include <system_error>

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

// ---------------------------------------------------------- numeric field --

NumericField::NumericField(ValueRange range, double value) : range_(range) {
  set_focusable(true);
  value_ = range_.quantise(value);

  // Built now and hidden, rather than made when an edit starts. What ends an
  // edit is usually the field's own key handler, and destroying it there would
  // return into freed memory.
  field_ = &emplace<TextField>();
  field_->set_visible(false);
  field_->set_on_commit([this](const std::string& text) {
    if (const std::optional<double> parsed = parse(text)) {
      gesture_start_ = value_;
      commit(*parsed);
      finish();
    }
  });
  field_->set_on_finish([this] { end_edit(); });
}

void NumericField::set_value(double value) { value_ = range_.quantise(value); }

void NumericField::set_range(const ValueRange& range) {
  range_ = range;
  value_ = range_.quantise(value_);
  invalidate_layout();
}

void NumericField::set_decimals(int decimals) noexcept {
  decimals_ = std::clamp(decimals, 0, 6);
  invalidate_layout();
}

void NumericField::set_suffix(std::string suffix) {
  suffix_ = std::move(suffix);
  invalidate_layout();
}

void NumericField::set_scrub_step(double step) noexcept {
  scrub_step_ = std::max(0.0, step);
}

double NumericField::scrub_step() const noexcept {
  if (scrub_step_ > 0.0) return scrub_step_;
  const double span = std::abs(range_.maximum - range_.minimum);
  if (span < kDegenerate) return 0.0;
  return span / kScrubTravel;
}

std::string NumericField::number_text() const {
  return std::format("{:.{}f}", value_, decimals_);
}

std::string NumericField::display_text() const {
  // Unspaced, the way a unit is written: 50% and 90°, not 50 % and 90 °.
  return number_text() + suffix_;
}

std::optional<double> NumericField::parse(std::string_view text) const {
  const auto space = [](char c) { return c == ' ' || c == '\t'; };
  while (!text.empty() && space(text.front())) text.remove_prefix(1);
  while (!text.empty() && space(text.back())) text.remove_suffix(1);
  if (text.empty()) return std::nullopt;

  double parsed = 0.0;
  const char* const begin = text.data();
  const char* const end = begin + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{}) return std::nullopt;

  // Whatever follows the number has to be the unit, and nothing else. "50%"
  // and "50 %" are the same value; "50 pixels or so" is a typing mistake, and
  // taking 50 from it would be guessing.
  std::string_view rest(result.ptr, static_cast<std::size_t>(end - result.ptr));
  while (!rest.empty() && space(rest.front())) rest.remove_prefix(1);
  if (!rest.empty() && rest != suffix_) return std::nullopt;

  return parsed;
}

void NumericField::commit(double value) {
  const double next = range_.quantise(value);
  if (next == value_) return;
  value_ = next;
  invalidate_layout();
  if (on_change_) on_change_(value_);
}

void NumericField::finish() {
  if (value_ == gesture_start_) return;
  if (on_commit_) on_commit_(value_);
}

bool NumericField::editing() const noexcept {
  return field_ != nullptr && field_->visible();
}

void NumericField::begin_edit() {
  if (field_ == nullptr || editing()) return;
  // The number alone: the suffix is decoration, and having to type round it
  // would make the fast path slower than the slow one.
  field_->set_text(number_text());
  field_->set_visible(true);
  field_->select_all();
  if (WidgetHost* const owner = host(); owner != nullptr) owner->set_focus(field_);
  invalidate_layout();
}

void NumericField::end_edit() {
  if (!editing()) return;
  // Hidden first, so the focus move below cannot come back round through the
  // field's own focus-lost handler and start this again.
  field_->set_visible(false);
  if (WidgetHost* const owner = host(); owner != nullptr && owner->focused() == field_) {
    owner->set_focus(this);
  }
  invalidate_layout();
}

LayoutItem NumericField::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) return LayoutItem::fixed(metrics.control_height);

  // Sized for the widest number the range can produce rather than for the one
  // showing, so a row does not shuffle sideways as its value is scrubbed past
  // 9.9 and back.
  const auto width_of = [&](double value) {
    return context.text.measure(std::format("{:.{}f}", value, decimals_) + suffix_,
                                metrics.font_size, false);
  };
  // `padding_y`, not `padding_x`. A number is not a button: the wider padding
  // is there so a label has room to breathe inside a bevel, and spending it
  // twice on every field is what pushed a parameter row over its own width —
  // the label and the number ended up drawn on top of each other, which neither
  // `--check`'s empty-widget test nor its clipping test can see.
  const double widest = std::max(width_of(range_.minimum), width_of(range_.maximum));

  // It will give up room before the label beside it does, down to a floor.
  //
  // A parameter row with two numbers on it, a reset and the three keyframe
  // controls does not fit a narrow panel, and something has to give. The name
  // is what must not: a number with its tail cut off can be read by widening
  // the panel, whereas a row with no name on it is a mystery — which is what
  // was on screen, an animated Position showing two numbers and nothing saying
  // what they were.
  const double floor = width_of(0.0) + metrics.padding_y * 2.0;
  return LayoutItem{.basis = widest + metrics.padding_y * 2.0,
                    .grow = 0.0,
                    .shrink = 1.0,
                    .min = std::min(floor, widest + metrics.padding_y * 2.0),
                    .max = kUnbounded};
}

void NumericField::layout(const LayoutContext& context) {
  font_size_ = context.metrics().font_size;
  padding_ = context.metrics().padding_y;
  // The field sits over the number exactly, so committing does not make the
  // row appear to move.
  if (field_ != nullptr) field_->arrange(bounds(), context);
}

void NumericField::paint_content(Painter& painter, const Theme& theme) const {
  if (editing()) return;  // the field is drawing itself over this

  const SurfaceStyle& style = theme.style(part(), state());
  const Rect area = bounds();
  const Rect where{area.x + padding_, area.y, area.width - padding_ * 2.0, area.height};

  // Premiere's blue. The accent rather than the label colour, because that
  // colour is the whole affordance: a number drawn like a label says nothing
  // can be done to it, and there is no other hint that this one can be dragged.
  TextRun run = text_run(where, display_text(), style, font_size_, TextAlign::Left);
  if (enabled()) run.color = theme.accent;

  // Clipped when it has been squeezed, for the same reason a label is: without
  // this a short number simply draws over whatever is beside it, which reads as
  // two controls overlapping rather than as one that ran out of room.
  const bool fits = painter.measure(display_text(), font_size_, false) <= where.width + 0.5;
  if (!fits) painter.push_clip(area, 0.0);
  painter.text(run);
  if (!fits) painter.pop_clip();

  // Underlined under the pointer, which is the second half of the same
  // affordance and the part that says *this* number rather than the row.
  if (hovered() && enabled() && !scrubbing_) {
    const double width = painter.measure(display_text(), font_size_, false);
    const double y = std::round(area.y + (area.height + font_size_) / 2.0) + 1.0;
    painter.line(where.x, y, where.x + width, y, run.color, 1.0);
  }
}

bool NumericField::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  gesture_start_ = value_;

  if (event.click_count >= 2 && default_.has_value()) {
    commit(*default_);
    finish();
    return true;
  }

  // Nothing happens yet. Whether this is a scrub or a click is decided by
  // whether the pointer moves, and deciding it now would mean every click on
  // the number also nudged it.
  pressed_here_ = true;
  scrubbing_ = false;
  press_x_ = event.x;
  scrub_value_ = value_;
  return true;
}

bool NumericField::on_mouse_move(const MouseEvent& event) {
  if (!pressed_here_) return false;

  const double moved = event.x - press_x_;
  if (!scrubbing_) {
    if (std::abs(moved) < kScrubThreshold) return true;
    scrubbing_ = true;
  }

  double rate = scrub_step();
  if (event.modifiers.shift) rate *= kCoarseScrub;
  if (event.modifiers.control) rate *= kFineScrub;

  // Against the press, not against the last move. Accumulating deltas drifts,
  // and a drag that returns to where it started has to return to the value it
  // started at.
  scrub_value_ = range_.clamp(gesture_start_ + moved * rate);
  commit(scrub_value_);
  return true;
}

bool NumericField::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !pressed_here_) return false;
  pressed_here_ = false;

  if (scrubbing_) {
    scrubbing_ = false;
    finish();
    return true;
  }

  // It never moved, so it was a click, and a click on a number means type one.
  begin_edit();
  return true;
}

bool NumericField::on_key_down(const KeyEvent& event) {
  if (event.modifiers.alt) return false;
  if (event.modifiers.control) {
    if (event.key != Key::A) return false;
    begin_edit();
    return true;
  }

  const double amount = range_.nudge() * (event.modifiers.shift ? kCoarseNudge : 1.0);
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
    case Key::Enter:
      begin_edit();
      return true;
    default:
      return false;
  }

  finish();
  return true;
}

// ---------------------------------------------------------------- checkbox --

// ---------------------------------------------------------------- faders --

namespace {
/// The levels a fader prints beside its throw, loudest first.
///
/// Premiere's own set, which is not evenly spaced on purpose: the marks crowd
/// where the ear does. A decibel either side of unity is a change anybody can
/// hear and wants to place exactly; twelve decibels down is a region rather
/// than a number.
constexpr std::array<double, 9> kFaderMarks{6.0, 3.0, 0.0, -3.0, -6.0, -9.0, -15.0, -24.0, -48.0};
}  // namespace

Fader::Fader(ValueRange range, double value) : range_(range) {
  set_focusable(true);
  set_value(value);
}

std::span<const double> Fader::scale_marks() noexcept { return kFaderMarks; }

void Fader::set_value(double value) { value_ = range_.clamp(value); }

void Fader::set_range(const ValueRange& range) {
  range_ = range;
  value_ = range_.clamp(value_);
}

void Fader::layout(const LayoutContext& context) {
  const Metrics& metrics = context.metrics();
  thumb_height_ = metrics.control_height * 0.55;
  throw_width_ = metrics.control_height * 0.7;
  font_size_ = std::max(8.0, metrics.font_size - 3.0);
  // Wide enough for the widest mark it will print. "-48" is three characters
  // and a minus, and measuring it is cheaper than being wrong about the font.
  scale_width_ = shows_scale_ ? context.text.measure("-48", font_size_, false) + metrics.spacing
                              : 0.0;
}

LayoutItem Fader::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) {
    // A fader wants length. Four control heights is the least that is still a
    // throw rather than a switch, and it takes more gladly.
    return LayoutItem::flexible(1.0, metrics.control_height * 4.0);
  }
  const double thrown = metrics.control_height * 0.7;
  if (!shows_scale_) return LayoutItem::fixed(thrown);
  return LayoutItem::fixed(thrown + context.text.measure("-48", font_size_, false) +
                           metrics.spacing);
}

Rect Fader::groove() const {
  const Rect area = bounds();
  // The scale sits to the left, as Premiere prints it, so the throw is against
  // the meter that will be drawn beside it.
  const double x = area.x + std::min(scale_width_, area.width);
  return Rect{x, area.y, std::max(0.0, std::min(throw_width_, area.right() - x)), area.height};
}

Rect Fader::thumb() const {
  const Rect track = groove();
  const double height = std::min(thumb_height_, track.height);
  const double travel = std::max(0.0, track.height - height);
  // Upside down, because loud is up: a fader at its maximum sits at the top.
  return Rect{track.x, track.y + (1.0 - fraction()) * travel, track.width, height};
}

double Fader::y_of(double db) const {
  const Rect track = groove();
  const double height = std::min(thumb_height_, track.height);
  const double travel = std::max(0.0, track.height - height);
  // Measured at the thumb's centre, so a mark lines up with the middle of the
  // cap rather than its top edge.
  return track.y + height / 2.0 + (1.0 - range_.to_fraction(db)) * travel;
}

double Fader::value_at(double y) const {
  const Rect track = groove();
  const double height = std::min(thumb_height_, track.height);
  const double travel = track.height - height;
  if (travel <= 0.0) return range_.maximum;
  return range_.from_fraction(1.0 - (y - track.y - height / 2.0) / travel);
}

void Fader::move_to(double value) {
  const double next = range_.clamp(value);
  if (next == value_) return;
  value_ = next;
  if (on_change_) on_change_(value_);
}

void Fader::finish() {
  if (value_ == gesture_start_) return;
  if (on_commit_) on_commit_(value_);
}

void Fader::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(Part::Slider, state());
  const Rect track = groove();
  paint_surface(painter, track, style);

  const SurfaceStyle& cap = theme.style(Part::SliderThumb, state());
  if (shows_scale_ && scale_width_ > 0.0) {
    const Rect area = bounds();
    for (const double mark : kFaderMarks) {
      if (mark > range_.maximum || mark < range_.minimum) continue;
      const double y = y_of(mark);
      // A tick into the throw as well as the number, so the eye can find the
      // level without reading. Unity gets a full-width one — it is the mark
      // anybody is actually looking for.
      const double reach = mark == 0.0 ? track.width : track.width * 0.4;
      painter.line(track.x, y, track.x + reach, y, cap.text, 1.0);

      const Rect text{area.x, y - font_size_, std::max(0.0, scale_width_ - 2.0),
                      font_size_ * 2.0};
      painter.text(text_run(text, std::format("{:g}", mark), style, font_size_,
                            TextAlign::Right, false));
    }
  }

  paint_surface(painter, thumb(), cap);
}

bool Fader::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  gesture_start_ = value_;

  if (event.click_count == 2 && default_.has_value()) {
    move_to(*default_);
    finish();
    return true;
  }

  dragging_ = true;
  move_to(value_at(event.y));
  return true;
}

bool Fader::on_mouse_move(const MouseEvent& event) {
  if (!dragging_) return false;
  move_to(value_at(event.y));
  return true;
}

bool Fader::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || !dragging_) return false;
  dragging_ = false;
  finish();
  return true;
}

bool Fader::on_key_down(const KeyEvent& event) {
  if (!event.modifiers.none()) return false;

  // A decibel a press, and six with a page. Both are levels somebody means
  // rather than fractions of a travel whose length depends on the layout.
  double step = 0.0;
  switch (event.key) {
    case Key::Up: step = 1.0; break;
    case Key::Down: step = -1.0; break;
    case Key::PageUp: step = 6.0; break;
    case Key::PageDown: step = -6.0; break;
    case Key::Home: step = range_.maximum - value_; break;
    case Key::End: step = range_.minimum - value_; break;
    default: return false;
  }

  gesture_start_ = value_;
  move_to(value_ + step);
  finish();
  return true;
}

// ----------------------------------------------------------- radio groups --

RadioGroup::RadioGroup(std::vector<std::string> options, std::size_t selected)
    : options_(std::move(options)) {
  set_focusable(true);
  select(selected);
}

void RadioGroup::set_options(std::vector<std::string> options, std::size_t selected) {
  options_ = std::move(options);
  // Reset rather than kept: the old index named a row in the old list, which is
  // to say the wrong one.
  selected_ = 0;
  select(selected);
}

void RadioGroup::select(std::size_t index) {
  if (index >= options_.size()) return;
  selected_ = index;
}

void RadioGroup::choose(std::size_t index) {
  if (index >= options_.size() || index == selected_) return;
  selected_ = index;
  if (on_change_) on_change_(selected_);
}

void RadioGroup::layout(const LayoutContext& context) {
  const Metrics& metrics = context.metrics();
  row_height_ = metrics.control_height;
  dot_size_ = metrics.control_height * 0.6;
  gap_ = metrics.spacing;
  font_size_ = metrics.font_size;
}

LayoutItem RadioGroup::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) {
    return LayoutItem::fixed(metrics.control_height * static_cast<double>(options_.size()));
  }

  // As wide as the longest row needs. A group narrower than its longest label
  // would cut the answer somebody is being asked to choose between.
  const double circle = metrics.control_height * 0.6;
  double widest = 0.0;
  for (const std::string& option : options_) {
    widest = std::max(widest, context.text.measure(option, metrics.font_size, false));
  }
  if (options_.empty()) return LayoutItem::fixed(0.0);
  return LayoutItem::fixed(circle + metrics.spacing + widest);
}

Rect RadioGroup::row_rect(std::size_t index) const {
  if (index >= options_.size()) return {};
  const Rect area = bounds();
  return Rect{area.x, area.y + static_cast<double>(index) * row_height_, area.width,
              row_height_};
}

Rect RadioGroup::dot(std::size_t index) const {
  const Rect row = row_rect(index);
  if (row.width <= 0.0 && row.height <= 0.0) return {};
  const double size = std::min(dot_size_, std::min(row.width, row.height));
  return Rect{row.x, row.y + (row.height - size) / 2.0, size, size};
}

std::size_t RadioGroup::row_at(double y) const {
  const Rect area = bounds();
  if (row_height_ <= 0.0 || y < area.y) return options_.size();
  const auto index = static_cast<std::size_t>((y - area.y) / row_height_);
  return index < options_.size() ? index : options_.size();
}

void RadioGroup::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(Part::Input, state());
  const SurfaceStyle& taken = theme.style(Part::Input, State::Selected);

  for (std::size_t i = 0; i < options_.size(); ++i) {
    const Rect circle = dot(i);
    // A circle is a rectangle whose corners are as round as they go. There is
    // no ellipse primitive and this needs none: half the width is exactly a
    // circle at every size, and it goes through the same fill and stroke every
    // other themed surface uses, so a radio looks like it belongs in all four.
    const double radius = circle.width / 2.0;
    painter.fill(circle, radius, style.fill);
    if (style.border_width > 0.0) {
      painter.stroke(circle, radius, style.border, style.border_width);
    }

    if (i == selected_) {
      // The mark is a smaller filled circle inside the first, which is what a
      // radio button has looked like since before any of this.
      const double inset_by = circle.width * 0.28;
      const Rect mark{circle.x + inset_by, circle.y + inset_by, circle.width - inset_by * 2.0,
                      circle.height - inset_by * 2.0};
      painter.fill(mark, mark.width / 2.0, Fill::solid(taken.text));
    }

    const Rect row = row_rect(i);
    const Rect text{circle.right() + gap_, row.y,
                    std::max(0.0, row.right() - circle.right() - gap_), row.height};
    painter.text(text_run(text, options_[i], style, font_size_, TextAlign::Left, false));
  }
}

bool RadioGroup::on_mouse_down(const MouseEvent& event) {
  // Taken so the press captures; the choice lands on release, so sliding off a
  // row cancels it exactly as it does on a button or a checkbox.
  return event.button == MouseButton::Left;
}

bool RadioGroup::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  if (hit(event.x, event.y)) choose(row_at(event.y));
  return true;
}

bool RadioGroup::on_key_down(const KeyEvent& event) {
  if (!event.modifiers.none() || options_.empty()) return false;

  // The arrows move the *selection*, not a highlight — which is what a radio
  // group does everywhere on this platform, and the reason the whole group is
  // one tab stop rather than a row of them.
  switch (event.key) {
    case Key::Up:
    case Key::Left:
      if (selected_ > 0) choose(selected_ - 1);
      return true;
    case Key::Down:
    case Key::Right:
      choose(selected_ + 1);
      return true;
    case Key::Home:
      choose(0);
      return true;
    case Key::End:
      choose(options_.size() - 1);
      return true;
    default:
      return false;
  }
}

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
  // The ticks belonged to the old items. Kept, they would tick rows by
  // position — which is to say, the wrong ones.
  checked_.clear();
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
    const double needed = rows + 2.0 * padding_;
    // A category column takes the height it is given; a menu is as tall as what
    // is in it. Never *shorter* than its rows either way, so filling can only
    // add room and cannot hide the last entry.
    return fills_height_ ? LayoutItem::flexible(1.0, needed) : LayoutItem::fixed(needed);
  }

  double widest = 0.0;
  for (const std::string& item : items_) {
    widest = std::max(widest, context.text.measure(item, metrics.font_size, false));
  }
  // The tick gutter is width the labels no longer have, so it is asked for
  // here as well as drawn — otherwise ticking a menu clips its longest label.
  const double gutter = checked_.empty() ? 0.0 : metrics.list_row_height * 0.6;
  // Room either side, and never so narrow that a one-word menu is a sliver.
  return LayoutItem::fixed(std::max(widest + gutter + 4.0 * metrics.padding_x, 96.0));
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
    double indent = padding_ + 6.0;

    if (!checked_.empty()) {
      const double gutter = row.height * 0.6;
      if (i < checked_.size() && checked_[i]) {
        // The same two lines the checkbox draws, and for the same reason: no
        // font can be relied on to have a tick in it.
        const double size = gutter * 0.5;
        const double left = row.x + indent + (gutter - size) * 0.5;
        const double right = left + size;
        const double top = row.y + (row.height - size) * 0.5;
        const double bottom = top + size;
        const double elbow_x = left + size * 0.36;

        painter.line(left, top + size * 0.5, elbow_x, bottom, style.text, 2.0);
        painter.line(elbow_x, bottom, right, top, style.text, 2.0);
      }
      indent += gutter;
    }

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

Cursor TextField::cursor_at(double /*x*/, double /*y*/) const { return Cursor::Text; }

LayoutItem TextField::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Horizontal) {
    if (columns_ > 0) {
      // Measured from a digit rather than from the text in it, so the field
      // does not change width as it is typed into — and "0" is the widest
      // character a timecode can hold.
      const double glyph = context.text.measure("0", metrics.font_size, false);
      return LayoutItem::fixed(glyph * columns_ + 2.0 * metrics.padding_x);
    }
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
  if (on_finish_) on_finish_();
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
        if (on_finish_) on_finish_();
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
      // Ending the edit even when nothing changed. A field that swallowed
      // Escape and stayed open would be the one thing on screen with no way
      // out of it.
      if (on_finish_) on_finish_();
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

LayoutItem IconButton::sizing(Axis axis, const LayoutContext& context) const {
  if (!narrow_ || axis == Axis::Vertical) return Button::sizing(axis, context);
  // Three quarters. Enough to still be an easy target, little enough that three
  // of them together cost about what two square ones would — and never narrower
  // than the mark it has to hold, which grew with the theme's font.
  const Metrics& metrics = context.metrics();
  const double mark = metrics.font_size * 0.92 + 4.0;
  return LayoutItem::fixed(std::round(std::max(metrics.control_height * 0.75, mark)));
}

void draw_icon(Painter& painter, IconButton::Icon icon, const Rect& area, const Color& color,
               double reach, double width, bool on) {
  using Icon = IconButton::Icon;
  const SurfaceStyle style{.text = color};
  const double cx = area.x + area.width * 0.5;
  const double cy = area.y + area.height * 0.5;

  switch (icon) {
    case Icon::ArrowUp:
      painter.line(cx - reach, cy + reach * 0.5, cx, cy - reach * 0.5, style.text, width);
      painter.line(cx, cy - reach * 0.5, cx + reach, cy + reach * 0.5, style.text, width);
      break;
    case Icon::ArrowDown:
      painter.line(cx - reach, cy - reach * 0.5, cx, cy + reach * 0.5, style.text, width);
      painter.line(cx, cy + reach * 0.5, cx + reach, cy - reach * 0.5, style.text, width);
      break;
    case Icon::ArrowLeft:
      painter.line(cx + reach * 0.5, cy - reach, cx - reach * 0.5, cy, style.text, width);
      painter.line(cx - reach * 0.5, cy, cx + reach * 0.5, cy + reach, style.text, width);
      break;
    case Icon::ArrowRight:
      painter.line(cx - reach * 0.5, cy - reach, cx + reach * 0.5, cy, style.text, width);
      painter.line(cx + reach * 0.5, cy, cx - reach * 0.5, cy + reach, style.text, width);
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

    case Icon::Disclosure: {
      // The state is the direction, the way the tool buttons' state is which
      // one is current: right for closed, down for open. Two thirds the reach
      // of the arrows, so a row of effect controls does not read as three
      // arrows meaning three different things.
      const double small = reach * 0.66;
      if (on) {
        painter.line(cx - small, cy - small * 0.5, cx, cy + small * 0.5, style.text, width);
        painter.line(cx, cy + small * 0.5, cx + small, cy - small * 0.5, style.text, width);
      } else {
        painter.line(cx - small * 0.5, cy - small, cx + small * 0.5, cy, style.text, width);
        painter.line(cx + small * 0.5, cy, cx - small * 0.5, cy + small, style.text, width);
      }
      break;
    }

    case Icon::Reset: {
      // A ring with a gap at the top right, and a head on the end nearest it.
      // Straight segments rather than an arc: the painter has no curve, and at
      // eight pixels across a hexagon reads as a circle anyway.
      constexpr int kSides = 8;
      // Three quarters of the way round, leaving the gap an arrow can sit in.
      constexpr int kDrawn = 6;
      const auto point = [cx, cy, reach](int step) {
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(step) /
                             static_cast<double>(kSides);
        return std::pair{cx + reach * std::cos(angle), cy + reach * std::sin(angle)};
      };
      for (int i = 0; i < kDrawn; ++i) {
        const auto [x0, y0] = point(i);
        const auto [x1, y1] = point(i + 1);
        painter.line(x0, y0, x1, y1, style.text, width);
      }
      // The head, at the open end, pointing the way round the ring goes.
      const auto [hx, hy] = point(0);
      painter.line(hx, hy, hx - reach * 0.5, hy - reach * 0.4, style.text, width);
      painter.line(hx, hy, hx - reach * 0.1, hy + reach * 0.6, style.text, width);
      break;
    }

    case Icon::Pointer: {
      // A cursor: tip at the top, two flanks, and a tail.
      const double tip_x = cx - reach * 0.5;
      const double tip_y = cy - reach;
      painter.line(tip_x, tip_y, cx + reach * 0.55, cy + reach * 0.25, style.text, width);
      painter.line(cx + reach * 0.55, cy + reach * 0.25, cx - reach * 0.05, cy + reach * 0.4,
                   style.text, width);
      painter.line(cx - reach * 0.05, cy + reach * 0.4, tip_x, tip_y, style.text, width);
      painter.line(cx + reach * 0.2, cy + reach * 0.35, cx + reach * 0.5, cy + reach,
                   style.text, width);
      break;
    }

    case Icon::Razor: {
      // One clip becoming two. What the cut leaves behind reads at eight pixels
      // where a blade does not: a blade that size is a grey smudge.
      const double w = reach * 0.75;
      const double h = reach * 1.6;
      painter.stroke(Rect{cx - reach, cy - h * 0.5, w, h}, 1.0, style.text, width);
      painter.stroke(Rect{cx + reach - w, cy - h * 0.5, w, h}, 1.0, style.text, width);
      break;
    }

    case Icon::RateStretch:
      // A fixed span with speed inside it: two end bars and a pair of chevrons,
      // which is a stretch that is going somewhere rather than a trim.
      painter.line(cx - reach, cy - reach * 0.8, cx - reach, cy + reach * 0.8, style.text,
                   width);
      painter.line(cx + reach, cy - reach * 0.8, cx + reach, cy + reach * 0.8, style.text,
                   width);
      for (const double at : {-reach * 0.6, 0.0}) {
        painter.line(cx + at, cy - reach * 0.5, cx + at + reach * 0.5, cy, style.text, width);
        painter.line(cx + at + reach * 0.5, cy, cx + at, cy + reach * 0.5, style.text, width);
      }
      break;

    case Icon::Slip:
      // A bare double-headed arrow: the source moving, with nothing around it
      // to say the clip does too. Two earlier attempts put it inside a box to
      // mean "the frame stays put", and at eight pixels a box with an arrowhead
      // in it is a blob. The plainest mark that reads is the right one, and this
      // one cannot be confused with the two that have end bars.
      painter.line(cx - reach, cy, cx + reach, cy, style.text, width);
      painter.line(cx - reach, cy, cx - reach * 0.4, cy - reach * 0.6, style.text, width);
      painter.line(cx - reach, cy, cx - reach * 0.4, cy + reach * 0.6, style.text, width);
      painter.line(cx + reach, cy, cx + reach * 0.4, cy - reach * 0.6, style.text, width);
      painter.line(cx + reach, cy, cx + reach * 0.4, cy + reach * 0.6, style.text, width);
      break;

    case Icon::Slide:
      // A block moving between two that stay where they are.
      painter.line(cx - reach, cy - reach * 0.8, cx - reach, cy + reach * 0.8, style.text,
                   width);
      painter.line(cx + reach, cy - reach * 0.8, cx + reach, cy + reach * 0.8, style.text,
                   width);
      painter.fill(Rect{cx - reach * 0.45, cy - reach * 0.6, reach * 0.9, reach * 1.2}, 1.0,
                   Fill::solid(style.text));
      break;

    case Icon::Ripple:
      // One edge, and what is behind it going the same way. The single bar is
      // what separates this from the roll beside it, which has two.
      painter.line(cx - reach * 0.4, cy - reach * 0.8, cx - reach * 0.4, cy + reach * 0.8,
                   style.text, width);
      painter.line(cx - reach * 0.1, cy, cx + reach, cy, style.text, width);
      painter.line(cx + reach, cy, cx + reach * 0.45, cy - reach * 0.5, style.text, width);
      painter.line(cx + reach, cy, cx + reach * 0.45, cy + reach * 0.5, style.text, width);
      break;

    case Icon::Roll:
      // The join itself, with the arrows on it going both ways: what moves is
      // the line between two clips, and neither end of the pair does.
      painter.line(cx - reach, cy - reach * 0.8, cx - reach, cy + reach * 0.8, style.text,
                   width);
      painter.line(cx + reach, cy - reach * 0.8, cx + reach, cy + reach * 0.8, style.text,
                   width);
      painter.line(cx, cy - reach * 0.8, cx, cy + reach * 0.8, style.text, width);
      painter.line(cx - reach * 0.55, cy, cx + reach * 0.55, cy, style.text, width);
      painter.line(cx - reach * 0.55, cy, cx - reach * 0.2, cy - reach * 0.35, style.text,
                   width);
      painter.line(cx - reach * 0.55, cy, cx - reach * 0.2, cy + reach * 0.35, style.text,
                   width);
      painter.line(cx + reach * 0.55, cy, cx + reach * 0.2, cy - reach * 0.35, style.text,
                   width);
      painter.line(cx + reach * 0.55, cy, cx + reach * 0.2, cy + reach * 0.35, style.text,
                   width);
      break;
  }

}

void IconButton::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(part(), state());

  // Scaled with the *text* rather than with the button, because these sit in a
  // row with it and want to match its weight rather than the theme's padding —
  // but scaled, where this was a flat four pixels whatever the theme said.
  //
  // Eight pixels across was too small to read. Half of these marks have
  // internal structure — a razor is two blades and a gap, a slide is a block
  // between two bars — and at eight pixels with a one-and-a-half pixel stroke
  // there is no room for the structure to survive. Twelve is still smaller than
  // the capital letters beside it and is legible at a glance, which is the
  // whole job.
  const double reach = std::max(4.0, theme.metrics.font_size * 0.46);
  const double width = std::max(1.5, theme.metrics.font_size * 0.135);
  draw_icon(painter, icon_, bounds(), style.text, reach, width, selected());

  // Whether the toggle is on is drawn into the mark as well as left to the
  // surface beneath it. The themes do define a selected state now, but a lit
  // background is a weaker signal than a filled centre at this size, and a
  // stopwatch that looks the same running as stopped is worse than none.
  if (selected() && (icon_ == Icon::Stopwatch || icon_ == Icon::Diamond)) {
    const Rect area = bounds();
    const double cx = area.x + area.width * 0.5;
    const double cy = area.y + area.height * 0.5;
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
