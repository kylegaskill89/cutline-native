#include "cutline/ui/widget.hpp"

#include "cutline/ui/controls.hpp"
#include "cutline/ui/dialog.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace cutline::ui {
namespace {

/// Names for the keys whose value is their own character, built once so the
/// printable half of `Key` needs no table written out by hand.
[[nodiscard]] const std::array<std::string, 128>& ascii_names() {
  static const std::array<std::string, 128> names = [] {
    std::array<std::string, 128> table;
    for (std::size_t i = 0; i < table.size(); ++i) {
      table[i] = std::string(1, static_cast<char>(i));
    }
    return table;
  }();
  return names;
}

/// Whether `widget` is `ancestor` or sits somewhere inside it.
[[nodiscard]] bool within(const Widget* widget, const Widget* ancestor) noexcept {
  for (const Widget* node = widget; node != nullptr; node = node->parent()) {
    if (node == ancestor) return true;
  }
  return false;
}

/// Whether a widget can be interacted with at all, which means it and every
/// one of its ancestors is visible and enabled. A control inside a hidden panel
/// is not reachable however it is flagged itself.
[[nodiscard]] bool interactive(const Widget* widget) noexcept {
  for (const Widget* node = widget; node != nullptr; node = node->parent()) {
    if (!node->visible() || !node->enabled()) return false;
  }
  return widget != nullptr;
}

}  // namespace

std::string_view to_string(Key key) noexcept {
  const auto value = static_cast<std::uint16_t>(key);
  if (key == Key::None) return "None";
  if (value < 128) return ascii_names()[value];

  switch (key) {
    case Key::Escape: return "Escape";
    case Key::Tab: return "Tab";
    case Key::Enter: return "Enter";
    case Key::Backspace: return "Backspace";
    case Key::Delete: return "Delete";
    case Key::Insert: return "Insert";
    case Key::Left: return "Left";
    case Key::Right: return "Right";
    case Key::Up: return "Up";
    case Key::Down: return "Down";
    case Key::Home: return "Home";
    case Key::End: return "End";
    case Key::PageUp: return "PageUp";
    case Key::PageDown: return "PageDown";
    case Key::F1: return "F1";
    case Key::F2: return "F2";
    case Key::F3: return "F3";
    case Key::F4: return "F4";
    case Key::F5: return "F5";
    case Key::F6: return "F6";
    case Key::F7: return "F7";
    case Key::F8: return "F8";
    case Key::F9: return "F9";
    case Key::F10: return "F10";
    case Key::F11: return "F11";
    case Key::F12: return "F12";
    default: break;
  }
  return "Unknown";
}

// ------------------------------------------------------------------ widget --

Widget& Widget::add(std::unique_ptr<Widget> child) {
  child->parent_ = this;
  children_.push_back(std::move(child));
  return *children_.back();
}

void Widget::clear_children() {
  // The host may be pointing at one of these as hovered, focused or holding
  // capture. Dropping those first is the difference between a rebuilt panel
  // and a dangling pointer that survives until the next mouse move.
  if (WidgetHost* owner = host(); owner != nullptr) {
    for (const std::unique_ptr<Widget>& child : children_) owner->forget(child.get());
  }
  children_.clear();
}

std::unique_ptr<Widget> Widget::take(Widget* child) {
  const auto found = std::ranges::find(children_, child, &std::unique_ptr<Widget>::get);
  if (found == children_.end()) return nullptr;

  std::unique_ptr<Widget> taken = std::move(*found);
  children_.erase(found);
  // Cleared so that `host()` walks up from wherever it is added next, rather
  // than reporting the host it used to belong to while it is loose.
  taken->parent_ = nullptr;
  return taken;
}

WidgetHost* Widget::host() const noexcept {
  const Widget* node = this;
  while (node->parent_ != nullptr) node = node->parent_;
  return node->host_;
}

void Widget::arrange(const Rect& bounds, const LayoutContext& context) {
  bounds_ = bounds;
  layout(context);
}

void Widget::translate(double dx, double dy) noexcept {
  bounds_.x += dx;
  bounds_.y += dy;
  for (const std::unique_ptr<Widget>& child : children_) child->translate(dx, dy);
}

void Widget::invalidate_layout() noexcept {
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_layout();
}

void Widget::layout(const LayoutContext&) {}

LayoutItem Widget::sizing(Axis, const LayoutContext&) const { return LayoutItem::flexible(); }

State Widget::state() const noexcept {
  if (!enabled_) return State::Disabled;
  if (pressed_) return State::Pressed;
  if (selected_) return State::Selected;
  if (hovered_) return State::Hover;
  if (focused_) return State::Focused;
  return State::Normal;
}

SurfaceStyle Widget::surface_style(const Theme& theme) const {
  const State now = state();
  const double in = theme.metrics.hover_in;
  const double out = theme.metrics.hover_out;
  const bool fades = in > 0.0 || out > 0.0;
  // Only between rest, hover and pressed: those are what the pointer moves a
  // control through. Selected, focused and disabled are decisions, and show at
  // once.
  const bool moving = now == State::Normal || now == State::Hover || now == State::Pressed;
  // Through `host()`, which walks up to the root: only the root holds it.
  WidgetHost* owner = fades && moving ? host() : nullptr;
  if (owner == nullptr) return theme.style(part(), now);

  Animator& clock = owner->animator();
  const float hover = clock.toward(this, channel::kHover, hovered_ || pressed_, in, out);
  // A press lands at once -- a button that is slow to look pressed feels
  // broken -- and lets go as a hover does.
  const float press = clock.toward(this, channel::kPress, pressed_, 0.0, out);
  SurfaceStyle style = mix(theme.style(part(), State::Normal), theme.style(part(), State::Hover), hover);
  if (press > 0.0f) style = mix(style, theme.style(part(), State::Pressed), press);
  return style;
}

void Widget::paint(Painter& painter, const Theme& theme) const {
  if (!visible_ || bounds_.empty()) return;

  const SurfaceStyle style = surface_style(theme);
  if (paints_surface()) paint_surface(painter, bounds_, style);
  paint_content(painter, theme);

  if (!children_.empty()) {
    if (clips_children_) painter.push_clip(bounds_, style.corner_radius);
    // In order, so a later child draws over an earlier one — the same order
    // `at` searches in reverse, which is what keeps hit testing agreeing with
    // what is actually on top.
    for (const std::unique_ptr<Widget>& child : children_) child->paint(painter, theme);
    if (clips_children_) painter.pop_clip();
  }

  // Outside the clip on purpose: a scrollbar belongs to the view, not to the
  // content it is scrolling, and would be cut off along with it.
  paint_overlay(painter, theme);
}

void Widget::paint_content(Painter&, const Theme&) const {}

void Widget::paint_overlay(Painter&, const Theme&) const {}

bool Widget::on_mouse_down(const MouseEvent&) { return false; }
bool Widget::on_mouse_up(const MouseEvent&) { return false; }
bool Widget::on_mouse_move(const MouseEvent&) { return false; }
bool Widget::on_wheel(const WheelEvent&) { return false; }
bool Widget::on_key_down(const KeyEvent&) { return false; }
bool Widget::on_key_up(const KeyEvent&) { return false; }
bool Widget::on_text(char32_t) { return false; }

void Widget::on_mouse_enter() {}
void Widget::on_mouse_leave() {}
void Widget::on_focus_changed(bool) {}

bool Widget::hit(double x, double y) const { return bounds_.contains(x, y); }

Widget* Widget::at(double x, double y) {
  if (!visible_ || !hit(x, y)) return nullptr;
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    if (Widget* found = (*it)->at(x, y); found != nullptr) return found;
  }
  return this;
}

bool Widget::descends_from(const Widget* ancestor) const noexcept {
  for (const Widget* node = this; node != nullptr; node = node->parent_) {
    if (node == ancestor) return true;
  }
  return false;
}

// -------------------------------------------------------------------- host --

WidgetHost::WidgetHost(std::unique_ptr<Widget> root) : root_(std::move(root)) {
  if (root_ == nullptr) root_ = std::make_unique<Widget>();
  root_->host_ = this;
}

void WidgetHost::resize(const Rect& bounds, const LayoutContext& context) {
  context.text.set_typeface(context.theme.metrics.font_family);
  bounds_ = bounds;
  root_->arrange(bounds_, context);
  if (popup_ != nullptr && !popup_closing_) arrange_popup(context);
  layout_dirty_ = false;
  // The pointer has not moved but what is under it may have. Without this, a
  // panel that opens under a resting cursor never lights up until it is
  // nudged.
  if (has_mouse_ && captured_ == nullptr) set_hovered(target_at(mouse_x_, mouse_y_));
}

void WidgetHost::arrange_popup(const LayoutContext& context) {
  // Its own preferred size, not the anchor's: a list is as wide as its longest
  // entry, which is usually wider than the control it came from.
  const double width = std::max(popup_->sizing(Axis::Horizontal, context).basis,
                                popup_anchor_.width);
  const double height = popup_->sizing(Axis::Vertical, context).basis;

  // Under the anchor by preference, above it when there is no room — the same
  // rule every menu on the platform follows, and the reason a dropdown near
  // the bottom of a window opens upwards instead of off the edge.
  double y = popup_anchor_.bottom();
  if (y + height > bounds_.bottom() && popup_anchor_.y - height >= bounds_.y) {
    y = popup_anchor_.y - height;
  }

  double x = popup_anchor_.x;
  if (x + width > bounds_.right()) x = bounds_.right() - width;

  // Clamped last, so a popup taller or wider than the window still starts
  // inside it rather than being pushed out by the corrections above.
  x = std::max(bounds_.x, x);
  y = std::max(bounds_.y, y);

  popup_->arrange(Rect{x, y, width, height}, context);
}

void WidgetHost::tick_me(Widget* widget) {
  if (widget == nullptr || std::ranges::contains(ticking_, widget)) return;
  ticking_.push_back(widget);
  paint_dirty_ = true;
}

bool WidgetHost::update_layout(const LayoutContext& context) {
  context.text.set_typeface(context.theme.metrics.font_family);
  // Moved on first, so what is laid out and drawn this frame is where each
  // has got to. Taken from the list before any runs: one that has not
  // arrived asks again, for the frame after.
  if (!ticking_.empty()) {
    std::vector<Widget*> due;
    due.swap(ticking_);
    for (Widget* widget : due) widget->tick();
    paint_dirty_ = true;
  }
  // Before the early return: a popup closes whether or not anything asked for
  // layout, and this is the one point in the frame where destroying it cannot
  // land inside the click handler that asked for it.
  if (popup_closing_) {
    if (popup_ != nullptr) {
      forget(popup_.get());
      popup_.reset();
    }
    popup_closing_ = false;
    // Back where it was, so a menu used in passing does not leave the
    // timeline without its keys.
    if (focused_ == nullptr && focus_before_popup_ != nullptr && return_focus_) {
      set_focus(focus_before_popup_);
    }
    focus_before_popup_ = nullptr;
    return_focus_ = true;
    paint_dirty_ = true;
    // What the pointer is over has changed, and the pointer has not moved.
    if (has_mouse_ && captured_ == nullptr) set_hovered(root_->at(mouse_x_, mouse_y_));
  }

  if (!layout_dirty_) return false;
  resize(bounds_, context);

  // A popup just opened takes the keyboard: a dialog starts with its first
  // control in hand, so it can be typed into straight away and Tab goes round
  // it rather than behind it, and a menu answers the arrows and Enter.
  if (popup_fresh_ && popup_ != nullptr && !popup_closing_) {
    popup_fresh_ = false;
    std::vector<Widget*> order;
    collect_focusable(*popup_, order);
    if (!order.empty()) {
      // Not when one popup replaced another: where it was before is still
      // where it was before the first.
      if (focused_ != nullptr && !focused_->descends_from(popup_.get())) {
        focus_before_popup_ = focused_;
      }
      set_focus(order.front());
    }
  }
  return true;
}

void WidgetHost::open_popup(std::unique_ptr<Widget> content, const Rect& anchor) {
  if (content == nullptr) return;

  // The one being replaced goes now rather than at the next frame: it is not
  // the thing running, because whatever asked for this belongs to the tree or
  // to a popup that is on its way out either way.
  if (popup_ != nullptr) forget(popup_.get());

  popup_ = std::move(content);
  popup_->host_ = this;
  popup_anchor_ = anchor;
  popup_closing_ = false;
  popup_fresh_ = true;
  ++popups_opened_;
  // Placed at the next layout, where a theme and a text measurer are in hand.
  // Nothing that opens a popup — a click handler — has either.
  request_layout();
}

void WidgetHost::close_popup(bool return_focus) noexcept {
  if (popup_ == nullptr) return;
  popup_closing_ = true;
  return_focus_ = return_focus;
  paint_dirty_ = true;
}

Cursor Widget::cursor_at(double /*x*/, double /*y*/) const { return Cursor::Arrow; }

Cursor WidgetHost::cursor() const {
  if (!has_mouse_) return Cursor::Arrow;

  // Upwards until something answers. A widget with nothing to say returns
  // `Arrow`, which is how a label lying across a timeline lets the timeline's
  // own answer through instead of covering it with the default.
  Widget* target = captured_ != nullptr ? captured_ : hovered_;
  for (Widget* widget = target; widget != nullptr; widget = widget->parent()) {
    if (const Cursor found = widget->cursor_at(mouse_x_, mouse_y_); found != Cursor::Arrow) {
      return found;
    }
  }
  return Cursor::Arrow;
}

std::string WidgetHost::tooltip_at_pointer() const {
  if (!has_mouse_ || captured_ != nullptr) return {};
  for (Widget* widget = hovered_; widget != nullptr; widget = widget->parent()) {
    if (std::string found = widget->tooltip_at(mouse_x_, mouse_y_); !found.empty()) {
      return found;
    }
  }
  return {};
}

void WidgetHost::show_tooltip(std::string text) {
  if (text == tooltip_shown_) return;
  tooltip_shown_ = std::move(text);
  tooltip_x_ = mouse_x_;
  tooltip_y_ = mouse_y_;
  request_paint();
}

void WidgetHost::hide_tooltip() {
  if (tooltip_shown_.empty()) return;
  tooltip_shown_.clear();
  request_paint();
}

Widget* WidgetHost::target_at(double x, double y) {
  if (!popup_open()) return root_->at(x, y);
  // Inside the popup, or nothing. What is underneath is unreachable while one
  // is open, which is the whole meaning of a modal layer: a click that lands
  // outside dismisses it and goes no further.
  return popup_->at(x, y);
}

template <typename Fn>
Widget* WidgetHost::bubble(Widget* target, Fn&& deliver) {
  for (Widget* node = target; node != nullptr; node = node->parent_) {
    // A disabled widget swallows rather than passing on: clicking a greyed-out
    // button should do nothing at all, not fall through to the panel behind it.
    if (!node->enabled_ || !node->visible_) return nullptr;
    if (deliver(*node)) {
      // Handled, so assume it changed something. The converse is the one that
      // pays: a mouse move nobody wanted leaves the picture exactly as it was,
      // and repainting for it is most of a window's idle cost.
      paint_dirty_ = true;
      return node;
    }
  }
  return nullptr;
}

bool WidgetHost::mouse_down(const MouseEvent& event) {
  mouse_x_ = event.x;
  mouse_y_ = event.y;
  has_mouse_ = true;
  if (keyboard_cues_) {
    keyboard_cues_ = false;
    paint_dirty_ = true;
  }

  // A press outside an open popup dismisses it and stops there. Letting it
  // through as well would mean closing a menu also pressed whatever happened
  // to be behind it, which is never what was meant.
  if (popup_open() && captured_ == nullptr && popup_->at(event.x, event.y) == nullptr) {
    close_popup();
    return true;
  }

  Widget* target = captured_ != nullptr ? captured_ : target_at(event.x, event.y);
  if (target == nullptr) return false;

  // Typing is a mode, and a press outside the thing being typed into is how
  // anyone leaves it. Decided on where the press *landed* rather than on what
  // handled it: a label handles nothing, and clicking one still has to end an
  // edit — nothing writes a field's value down until the keyboard goes.
  if (focused_ != nullptr && focused_->wants_text() && !within(target, focused_)) {
    set_focus(nullptr);
  }

  // Noted before the handler runs, so the focus-follows-press below can tell
  // "nobody said" from "the handler said".
  Widget* const focus_before = focused_;

  Widget* handler = bubble(target, [&](Widget& widget) { return widget.on_mouse_down(event); });
  if (handler == nullptr) return false;

  if (event.button == MouseButton::Left && captured_ == nullptr) {
    // A handled press takes the pointer until release. Every drag in the
    // application depends on this: without it a slider stops tracking the
    // moment the cursor leaves the thumb.
    capture(handler);
    set_pressed(handler);
  }

  // Focus follows a press only onto something that can take it. Clearing it
  // otherwise would mean clicking a toolbar button leaves the keyboard
  // nowhere, and the transport shortcuts stop working until you click back.
  //
  // And not at all when the handler moved it itself. A press that opens a
  // popup with a field in it and focuses that field has said where the keyboard
  // should go; walking up from the widget that was pressed would take it
  // straight back, which is how renaming a track from a double-click on its
  // header came out as a field nobody could type into until they clicked it.
  if (focused_ == focus_before) {
    for (Widget* node = handler; node != nullptr; node = node->parent_) {
      if (node->focusable_ && interactive(node)) {
        set_focus(node);
        break;
      }
    }
  }
  return true;
}

bool WidgetHost::mouse_up(const MouseEvent& event) {
  mouse_x_ = event.x;
  mouse_y_ = event.y;
  has_mouse_ = true;

  Widget* target = captured_ != nullptr ? captured_ : target_at(event.x, event.y);
  Widget* handler =
      target == nullptr
          ? nullptr
          : bubble(target, [&](Widget& widget) { return widget.on_mouse_up(event); });

  if (event.button == MouseButton::Left) {
    set_pressed(nullptr);
    release_capture();
    // Hover was frozen for the duration of the drag, so it is very likely
    // stale by the time the button comes back up.
    set_hovered(target_at(event.x, event.y));
  }
  return handler != nullptr;
}

bool WidgetHost::mouse_move(const MouseEvent& event) {
  mouse_x_ = event.x;
  mouse_y_ = event.y;
  has_mouse_ = true;

  if (captured_ != nullptr) {
    // Hover deliberately stays where it is: a slider being dragged should not
    // hand its highlight to whatever the cursor happens to pass over.
    return bubble(captured_, [&](Widget& widget) { return widget.on_mouse_move(event); }) !=
           nullptr;
  }

  Widget* target = target_at(event.x, event.y);
  set_hovered(target);
  if (target == nullptr) return false;
  return bubble(target, [&](Widget& widget) { return widget.on_mouse_move(event); }) != nullptr;
}

bool WidgetHost::wheel(const WheelEvent& event) {
  Widget* target = captured_ != nullptr ? captured_ : target_at(event.x, event.y);
  if (target == nullptr) return false;
  // Bubbles, so scrolling over a clip scrolls the timeline that holds it
  // rather than doing nothing.
  return bubble(target, [&](Widget& widget) { return widget.on_wheel(event); }) != nullptr;
}

bool WidgetHost::key_down(const KeyEvent& event) {
  // Escape closes a popup before anything else sees it. A menu that will not
  // go away without a click is a trap, and this is the key everyone reaches
  // for — which also means nothing behind it should act on the same press.
  if (popup_open() && event.key == Key::Escape) {
    // A dialog is told, so it can do whatever cancelling means for it.
    if (auto* dialog = dynamic_cast<Dialog*>(popup_.get()); dialog != nullptr) {
      dialog->cancel();
    } else {
      close_popup();
    }
    return true;
  }

  Widget* target = focused_ != nullptr ? focused_ : root_.get();
  const bool taken =
      bubble(target, [&](Widget& widget) { return widget.on_key_down(event); }) != nullptr;

  // Enter accepts a dialog, unless the control with focus took the key for
  // itself -- a focused Cancel is pressed, not overruled. A text field is the
  // exception: it takes Enter to finish its edit, and the answer it just
  // finished is exactly what accepting is about.
  if (event.key == Key::Enter) {
    // The popup's dialog when one is open; otherwise the one this window is,
    // if it is one -- Settings and Export are dialogs in windows of their own.
    Dialog* dialog = popup_open() ? dynamic_cast<Dialog*>(popup_.get()) : find_dialog(*root_);
    if (dialog != nullptr) {
      const bool field = dynamic_cast<TextField*>(focused_) != nullptr;
      if (!taken || field) {
        dialog->accept();
        return true;
      }
    }
  }
  return taken;
}

bool WidgetHost::key_up(const KeyEvent& event) {
  Widget* target = focused_ != nullptr ? focused_ : root_.get();
  return bubble(target, [&](Widget& widget) { return widget.on_key_up(event); }) != nullptr;
}

bool WidgetHost::text(char32_t codepoint) {
  Widget* target = focused_ != nullptr ? focused_ : root_.get();
  return bubble(target, [&](Widget& widget) { return widget.on_text(codepoint); }) != nullptr;
}

void WidgetHost::mouse_exit() {
  has_mouse_ = false;
  // Capture survives on purpose. Dragging a clip out past the edge of the
  // window and back is one gesture, and dropping it halfway would be worse
  // than useless.
  if (captured_ == nullptr) set_hovered(nullptr);
}

void WidgetHost::set_hovered(Widget* widget) {
  if (hovered_ == widget) return;
  paint_dirty_ = true;
  Widget* previous = hovered_;
  hovered_ = widget;
  // Flags updated before the callbacks, so a handler asking what is hovered
  // gets the answer that is about to be painted rather than the old one.
  if (previous != nullptr) {
    previous->hovered_ = false;
    previous->on_mouse_leave();
  }
  if (hovered_ != nullptr) {
    hovered_->hovered_ = true;
    hovered_->on_mouse_enter();
  }
}

void WidgetHost::set_pressed(Widget* widget) {
  if (pressed_ == widget) return;
  paint_dirty_ = true;
  if (pressed_ != nullptr) pressed_->pressed_ = false;
  pressed_ = widget;
  if (pressed_ != nullptr) pressed_->pressed_ = true;
}

bool WidgetHost::set_focus(Widget* widget) {
  if (widget != nullptr && (!widget->focusable_ || !interactive(widget))) return false;
  if (focused_ == widget) return true;

  paint_dirty_ = true;
  Widget* previous = focused_;
  focused_ = widget;
  if (previous != nullptr) {
    previous->focused_ = false;
    previous->on_focus_changed(false);
  }
  if (focused_ != nullptr) {
    focused_->focused_ = true;
    focused_->on_focus_changed(true);
  }
  return true;
}

void WidgetHost::collect_focusable(Widget& widget, std::vector<Widget*>& out) const {
  // A hidden or disabled widget takes its whole subtree out of the running,
  // so Tab cannot land inside a collapsed panel.
  if (!widget.visible_ || !widget.enabled_) return;
  if (widget.focusable_) out.push_back(&widget);
  for (const std::unique_ptr<Widget>& child : widget.children_) collect_focusable(*child, out);
}

bool WidgetHost::focus_next(bool backwards) {
  std::vector<Widget*> order;
  // Within an open popup, so Tab goes round a dialog rather than behind it.
  collect_focusable(popup_open() ? *popup_ : *root_, order);
  if (order.empty()) {
    set_focus(nullptr);
    return false;
  }

  const auto found = std::find(order.begin(), order.end(), focused_);
  std::size_t next = 0;
  if (found == order.end()) {
    next = backwards ? order.size() - 1 : 0;
  } else {
    const auto current = static_cast<std::size_t>(found - order.begin());
    next = backwards ? (current + order.size() - 1) % order.size() : (current + 1) % order.size();
  }
  keyboard_cues_ = true;
  paint_dirty_ = true;
  return set_focus(order[next]);
}

void WidgetHost::capture(Widget* widget) { captured_ = widget; }

void WidgetHost::release_capture() { captured_ = nullptr; }

void WidgetHost::forget(Widget* widget) {
  if (widget == nullptr) return;
  if (hovered_ != nullptr && hovered_->descends_from(widget)) set_hovered(nullptr);
  if (pressed_ != nullptr && pressed_->descends_from(widget)) set_pressed(nullptr);
  if (focused_ != nullptr && focused_->descends_from(widget)) set_focus(nullptr);
  if (focus_before_popup_ != nullptr && focus_before_popup_->descends_from(widget)) {
    focus_before_popup_ = nullptr;
  }
  std::erase_if(ticking_, [widget](const Widget* due) { return due->descends_from(widget); });
  if (captured_ != nullptr && captured_->descends_from(widget)) release_capture();
}

void WidgetHost::paint(Painter& painter, const Theme& theme) const {
  painter.set_typeface(theme.metrics.font_family);
  animator_.begin_frame();
  root_->paint(painter, theme);
  paint_focus_cue(painter, theme, *root_);
  // After everything, and outside every clip the tree set up. That is the
  // entire reason this is not a widget in the tree.
  if (popup_ != nullptr && !popup_closing_) {
    // Arriving rather than appearing, at the theme's pace: XP and Vista both
    // faded their menus in.
    const float arrived =
        animator_.progress(this, channel::kArrive, popups_opened_, theme.metrics.popup_fade);
    const bool fading = arrived < 1.0F;
    if (fading) painter.push_opacity(arrived);
    popup_->paint(painter, theme);
    paint_focus_cue(painter, theme, *popup_);
    if (fading) painter.pop_opacity();
  }
  paint_tooltip(painter, theme);
}

void WidgetHost::paint_focus_cue(Painter& painter, const Theme& theme,
                                 const Widget& under) const {
  if (!keyboard_cues_ || focused_ == nullptr || !focused_->descends_from(&under)) return;
  if (!focused_->visible() || focused_->bounds().empty()) return;
  const bool dotted = theme.metrics.focus_cue == FocusCue::Dotted;
  if (dotted && focused_->shows_caret()) return;

  // Inside whatever clips the control, so a field scrolled half out of its
  // list does not have its cue drawn over the panel below it.
  Rect visible = bounds_;
  for (const Widget* up = focused_->parent(); up != nullptr; up = up->parent()) {
    if (up->clips_children()) visible = visible.intersect(up->bounds());
  }
  if (visible.intersect(focused_->bounds()).empty()) return;
  painter.push_clip(visible, 0.0);

  const Rect& at = focused_->bounds();
  if (dotted) {
    // Every other device pixel, which is what made the period's cue read as
    // dotted rather than as a grey line at any scale.
    const double pixel = 1.0 / std::max(painter.device_scale(), 1.0);
    const double inset = std::min(3.0, std::min(at.width, at.height) / 4.0);
    const Rect box = at.inset(inset);
    const Color ink = focused_->surface_style(theme).text;
    const Fill dot = Fill::solid(ink);
    for (double x = box.x; x < box.right(); x += 2.0 * pixel) {
      painter.fill(Rect{x, box.y, pixel, pixel}, 0.0, dot);
      painter.fill(Rect{x, box.bottom() - pixel, pixel, pixel}, 0.0, dot);
    }
    for (double y = box.y; y < box.bottom(); y += 2.0 * pixel) {
      painter.fill(Rect{box.x, y, pixel, pixel}, 0.0, dot);
      painter.fill(Rect{box.right() - pixel, y, pixel, pixel}, 0.0, dot);
    }
  } else {
    // Just outside, so it frames the control rather than drawing over its own
    // border, and rounded to match.
    constexpr double kWidth = 2.0;
    const double radius = focused_->surface_style(theme).corner_radius;
    painter.stroke(at.inset(-kWidth), radius + kWidth, theme.accent, kWidth);
  }
  painter.pop_clip();
}

void WidgetHost::paint_tooltip(Painter& painter, const Theme& theme) const {
  if (tooltip_shown_.empty()) return;

  const Metrics& metrics = theme.metrics;
  const SurfaceStyle& style = theme.style(Part::Tooltip, State::Normal);
  const double text_width = painter.measure(tooltip_shown_, metrics.small_font_size, false);
  const double width = text_width + 2.0 * metrics.padding_x;
  const double height = metrics.small_font_size * metrics.line_height + metrics.padding_y;

  // Below and to the right, where the hand is not. Pushed back inside the
  // window when there is no room there, because a tooltip half off the edge is
  // worse than one on the other side of the pointer.
  constexpr double kGap = 14.0;
  double x = tooltip_x_ + kGap;
  double y = tooltip_y_ + kGap;
  if (x + width > bounds_.right()) x = std::max(bounds_.x, tooltip_x_ - width - 4.0);
  if (y + height > bounds_.bottom()) y = std::max(bounds_.y, tooltip_y_ - height - 4.0);

  const Rect box{x, y, width, height};
  paint_surface(painter, box, style);
  painter.text(text_run(box, tooltip_shown_, style, metrics.small_font_size, TextAlign::Center,
                        false));
}

}  // namespace cutline::ui
