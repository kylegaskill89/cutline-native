#include "cutline/ui/widget.hpp"

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

void Widget::paint(Painter& painter, const Theme& theme) const {
  if (!visible_ || bounds_.empty()) return;

  const SurfaceStyle& style = theme.style(part(), state());
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
  bounds_ = bounds;
  root_->arrange(bounds_, context);
  layout_dirty_ = false;
  // The pointer has not moved but what is under it may have. Without this, a
  // panel that opens under a resting cursor never lights up until it is
  // nudged.
  if (has_mouse_ && captured_ == nullptr) set_hovered(root_->at(mouse_x_, mouse_y_));
}

bool WidgetHost::update_layout(const LayoutContext& context) {
  if (!layout_dirty_) return false;
  resize(bounds_, context);
  return true;
}

template <typename Fn>
Widget* WidgetHost::bubble(Widget* target, Fn&& deliver) const {
  for (Widget* node = target; node != nullptr; node = node->parent_) {
    // A disabled widget swallows rather than passing on: clicking a greyed-out
    // button should do nothing at all, not fall through to the panel behind it.
    if (!node->enabled_ || !node->visible_) return nullptr;
    if (deliver(*node)) return node;
  }
  return nullptr;
}

bool WidgetHost::mouse_down(const MouseEvent& event) {
  mouse_x_ = event.x;
  mouse_y_ = event.y;
  has_mouse_ = true;

  Widget* target = captured_ != nullptr ? captured_ : root_->at(event.x, event.y);
  if (target == nullptr) return false;

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
  for (Widget* node = handler; node != nullptr; node = node->parent_) {
    if (node->focusable_ && interactive(node)) {
      set_focus(node);
      break;
    }
  }
  return true;
}

bool WidgetHost::mouse_up(const MouseEvent& event) {
  mouse_x_ = event.x;
  mouse_y_ = event.y;
  has_mouse_ = true;

  Widget* target = captured_ != nullptr ? captured_ : root_->at(event.x, event.y);
  Widget* handler =
      target == nullptr
          ? nullptr
          : bubble(target, [&](Widget& widget) { return widget.on_mouse_up(event); });

  if (event.button == MouseButton::Left) {
    set_pressed(nullptr);
    release_capture();
    // Hover was frozen for the duration of the drag, so it is very likely
    // stale by the time the button comes back up.
    set_hovered(root_->at(event.x, event.y));
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

  Widget* target = root_->at(event.x, event.y);
  set_hovered(target);
  if (target == nullptr) return false;
  return bubble(target, [&](Widget& widget) { return widget.on_mouse_move(event); }) != nullptr;
}

bool WidgetHost::wheel(const WheelEvent& event) {
  Widget* target = captured_ != nullptr ? captured_ : root_->at(event.x, event.y);
  if (target == nullptr) return false;
  // Bubbles, so scrolling over a clip scrolls the timeline that holds it
  // rather than doing nothing.
  return bubble(target, [&](Widget& widget) { return widget.on_wheel(event); }) != nullptr;
}

bool WidgetHost::key_down(const KeyEvent& event) {
  Widget* target = focused_ != nullptr ? focused_ : root_.get();
  return bubble(target, [&](Widget& widget) { return widget.on_key_down(event); }) != nullptr;
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
  if (pressed_ != nullptr) pressed_->pressed_ = false;
  pressed_ = widget;
  if (pressed_ != nullptr) pressed_->pressed_ = true;
}

bool WidgetHost::set_focus(Widget* widget) {
  if (widget != nullptr && (!widget->focusable_ || !interactive(widget))) return false;
  if (focused_ == widget) return true;

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
  collect_focusable(*root_, order);
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
  return set_focus(order[next]);
}

void WidgetHost::capture(Widget* widget) { captured_ = widget; }

void WidgetHost::release_capture() { captured_ = nullptr; }

void WidgetHost::forget(Widget* widget) {
  if (widget == nullptr) return;
  if (hovered_ != nullptr && hovered_->descends_from(widget)) set_hovered(nullptr);
  if (pressed_ != nullptr && pressed_->descends_from(widget)) set_pressed(nullptr);
  if (focused_ != nullptr && focused_->descends_from(widget)) set_focus(nullptr);
  if (captured_ != nullptr && captured_->descends_from(widget)) release_capture();
}

void WidgetHost::paint(Painter& painter, const Theme& theme) const {
  root_->paint(painter, theme);
}

}  // namespace cutline::ui
