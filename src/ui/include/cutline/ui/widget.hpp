#pragma once

/// The widget tree, and the routing of input through it.
///
/// A widget is a rectangle with children, a themed `Part`, and handlers that
/// say whether they dealt with an event. It does not draw itself in any direct
/// sense: it asks the theme for the style of its part and state and hands that
/// to `paint_surface`, so a theme can change what every control looks like
/// without any widget knowing.
///
/// `WidgetHost` owns the bits of interaction that are shared and easy to get
/// wrong — which widget is hovered, which has the keyboard, and which has
/// captured the mouse. Capture is the one worth stating plainly: a press that
/// is handled captures the pointer until release, so dragging a slider or
/// scrubbing the timeline keeps working after the cursor leaves the control.
/// Without it, every drag in the application would break at the edge of the
/// thing being dragged.
///
/// None of this needs a window. Events are values, so routing is driven from
/// tests directly and the platform layer only has to translate `WM_*`.

#include "cutline/ui/cursor.hpp"
#include "cutline/ui/event.hpp"
#include "cutline/ui/layout.hpp"
#include "cutline/ui/painter.hpp"
#include "cutline/ui/theme.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cutline::ui {

class WidgetHost;

class Widget {
 public:
  Widget() = default;
  virtual ~Widget() = default;

  Widget(const Widget&) = delete;
  Widget& operator=(const Widget&) = delete;

  // ------------------------------------------------------------------ tree --

  [[nodiscard]] Widget* parent() const noexcept { return parent_; }
  [[nodiscard]] std::span<const std::unique_ptr<Widget>> children() const noexcept {
    return children_;
  }

  Widget& add(std::unique_ptr<Widget> child);

  /// Builds a child in place and returns it as its own type, so a caller can
  /// keep hold of it without a cast.
  template <typename T, typename... Args>
  T& emplace(Args&&... args) {
    auto owned = std::make_unique<T>(std::forward<Args>(args)...);
    T& ref = *owned;
    add(std::move(owned));
    return ref;
  }

  /// Removes every child. Any of them that were hovered, focused or holding
  /// capture are dropped by the host first, so nothing is left pointing at
  /// freed memory.
  void clear_children();

  /// Detaches one child and hands ownership back. Null if it is not a child.
  ///
  /// Deliberately does *not* tell the host to forget it. The widget is still
  /// alive and is usually on its way somewhere else — a panel being dragged
  /// into another dock group is the case this exists for — and dropping focus
  /// and capture on every rearrangement would end the very gesture that caused
  /// it. Whoever takes a child and then destroys it calls
  /// `WidgetHost::forget` first.
  [[nodiscard]] std::unique_ptr<Widget> take(Widget* child);

  /// A name for tests and for a future inspector. Not shown to anyone.
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  void set_name(std::string name) { name_ = std::move(name); }

  // -------------------------------------------------------------- geometry --

  [[nodiscard]] const Rect& bounds() const noexcept { return bounds_; }

  /// Places this widget and lays its subtree out inside the new bounds.
  void arrange(const Rect& bounds, const LayoutContext& context);

  /// Moves this widget and everything under it, keeping the subtree's internal
  /// geometry intact.
  ///
  /// Only for changes that are genuinely a move: scrolling, where the content
  /// keeps the layout it was given and simply sits somewhere else. Anything
  /// that changes a *size* has to go through layout, because the children need
  /// to be given their share of it again.
  void translate(double dx, double dy) noexcept;

  /// Asks for this tree to be laid out again before it is next painted.
  ///
  /// This is how an input handler changes geometry. It cannot lay out itself:
  /// sizing needs a theme and a way to measure text, and the frame loop is the
  /// only place those exist. Marking and deferring is also what stops a drag
  /// from re-laying out the world on every mouse move it receives.
  void invalidate_layout() noexcept;

  /// Positions the children. The override point for containers, which call
  /// `arrange` on each child; the default leaves them where they are.
  virtual void layout(const LayoutContext& context);

  /// How much room this widget wants along an axis.
  ///
  /// The default takes whatever it is given. A control with a natural size —
  /// a button around its label, a header the height the theme says — overrides
  /// it, and gets the theme's metrics rather than a constant, which is what
  /// lets bevelled chrome be roomier than flat chrome without any widget
  /// knowing which theme is in use.
  [[nodiscard]] virtual LayoutItem sizing(Axis axis, const LayoutContext& context) const;

  // ----------------------------------------------------------------- state --

  [[nodiscard]] bool visible() const noexcept { return visible_; }
  void set_visible(bool visible) noexcept { visible_ = visible; }

  [[nodiscard]] bool enabled() const noexcept { return enabled_; }
  void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

  [[nodiscard]] bool selected() const noexcept { return selected_; }
  void set_selected(bool selected) noexcept { selected_ = selected; }

  /// Whether the keyboard can land here. Panels and labels are not focusable;
  /// buttons and fields are.
  [[nodiscard]] bool focusable() const noexcept { return focusable_; }
  void set_focusable(bool focusable) noexcept { focusable_ = focusable; }

  /// Whether children are clipped to these bounds when painting.
  [[nodiscard]] bool clips_children() const noexcept { return clips_children_; }
  void set_clips_children(bool clips) noexcept { clips_children_ = clips; }

  [[nodiscard]] bool hovered() const noexcept { return hovered_; }
  [[nodiscard]] bool pressed() const noexcept { return pressed_; }
  [[nodiscard]] bool focused() const noexcept { return focused_; }

  /// The state to look the theme up in.
  ///
  /// Ordered most-specific first: disabled beats everything, a pressed control
  /// looks pressed even while the pointer is over it, and a selected clip stays
  /// looking selected when hovered. Overridable for anything that disagrees.
  [[nodiscard]] virtual State state() const noexcept;

  // ------------------------------------------------------------ appearance --

  /// Which themed surface this is. The default draws nothing at all, which is
  /// what a bare container should do.
  [[nodiscard]] virtual Part part() const noexcept { return Part::Panel; }
  [[nodiscard]] virtual bool paints_surface() const noexcept { return false; }

  /// Draws this widget and then its children, clipping if asked to.
  ///
  /// Overriding `paint_content` is usually what is wanted; overriding this
  /// skips the surface and the clipping too.
  virtual void paint(Painter& painter, const Theme& theme) const;

  /// Drawn after the surface and before the children. Labels, icons, the
  /// contents of a timeline track.
  virtual void paint_content(Painter& painter, const Theme& theme) const;

  /// Drawn after the children, and outside the clip they were drawn in.
  /// Scrollbars, focus rings — anything that sits on top of its own contents.
  virtual void paint_overlay(Painter& painter, const Theme& theme) const;

  // ---------------------------------------------------------------- events --

  /// Handlers return whether they dealt with the event. An unhandled event
  /// carries on to the parent, so a click on a label inside a button still
  /// presses the button.
  virtual bool on_mouse_down(const MouseEvent& event);
  virtual bool on_mouse_up(const MouseEvent& event);
  virtual bool on_mouse_move(const MouseEvent& event);
  virtual bool on_wheel(const WheelEvent& event);
  virtual bool on_key_down(const KeyEvent& event);
  virtual bool on_key_up(const KeyEvent& event);
  /// A typed character, already through the keyboard layout and any IME.
  virtual bool on_text(char32_t codepoint);

  /// Whether this widget is being typed into.
  ///
  /// The window asks before spending a key on a shortcut: a digit means "switch
  /// theme" and a space means "play" everywhere *except* inside something that
  /// takes text, where they mean a digit and a space. Asking the widget beats
  /// the window guessing from its type.
  [[nodiscard]] virtual bool wants_text() const noexcept { return false; }

  virtual void on_mouse_enter();
  virtual void on_mouse_leave();
  virtual void on_focus_changed(bool focused);

  /// What the pointer should look like at this point, in this widget's own
  /// coordinates.
  ///
  /// `Arrow` means "nothing to say", not "an arrow": the host asks the deepest
  /// widget first and then its parents, so a widget that does not care lets the
  /// question through rather than blanking out an answer from underneath it.
  [[nodiscard]] virtual Cursor cursor_at(double x, double y) const;

  /// Whether a point counts as inside. The default is the bounding rectangle;
  /// a widget with a non-rectangular shape overrides it.
  [[nodiscard]] virtual bool hit(double x, double y) const;

  /// The deepest visible widget at a point, or null.
  ///
  /// Later children are on top, matching paint order. A point outside a
  /// widget's own bounds never reaches its children, so anything that has to
  /// escape its parent — a menu, a tooltip — belongs at the top of the tree
  /// rather than nested where it appears.
  [[nodiscard]] Widget* at(double x, double y);

  /// Whether `ancestor` is this widget or above it.
  [[nodiscard]] bool descends_from(const Widget* ancestor) const noexcept;

  /// The host this widget's tree is attached to, or null if it is loose.
  /// Found by walking to the root, so it stays correct as the tree is built.
  [[nodiscard]] WidgetHost* host() const noexcept;

 private:
  friend class WidgetHost;

  Widget* parent_ = nullptr;
  /// Set on the root only; everything else finds it by walking up.
  WidgetHost* host_ = nullptr;
  std::vector<std::unique_ptr<Widget>> children_;
  std::string name_;

  Rect bounds_;

  bool visible_ = true;
  bool enabled_ = true;
  bool selected_ = false;
  bool focusable_ = false;
  bool clips_children_ = false;

  // Owned by the host, which is the only thing that can know them.
  bool hovered_ = false;
  bool pressed_ = false;
  bool focused_ = false;
};

/// Routes input into a widget tree and keeps the interaction state that no
/// single widget can own.
class WidgetHost {
 public:
  explicit WidgetHost(std::unique_ptr<Widget> root);

  [[nodiscard]] Widget& root() noexcept { return *root_; }
  [[nodiscard]] const Widget& root() const noexcept { return *root_; }

  /// Resizes the tree. Also refreshes what is hovered, since the widget under
  /// a stationary pointer changes when things move beneath it.
  void resize(const Rect& bounds, const LayoutContext& context);

  [[nodiscard]] const Rect& bounds() const noexcept { return bounds_; }

  /// Whether something asked for layout since it last ran.
  [[nodiscard]] bool needs_layout() const noexcept { return layout_dirty_; }
  void request_layout() noexcept {
    layout_dirty_ = true;
    paint_dirty_ = true;
  }

  /// Whether anything that would change the picture has happened since the last
  /// `clear_paint`.
  ///
  /// Set when the hovered, pressed or focused widget changes, when layout is
  /// asked for, and when any event is handled — an event nobody handled cannot
  /// have changed what is drawn. This is what stops a window repainting itself
  /// because the pointer moved across a panel that does not care, which at a
  /// few milliseconds a frame is the difference between an interface that feels
  /// immediate and one that does not.
  ///
  /// A widget that changes its own appearance without any of that happening has
  /// to say so, through `invalidate_layout` or `request_paint`.
  [[nodiscard]] bool needs_paint() const noexcept { return paint_dirty_; }
  void request_paint() noexcept { paint_dirty_ = true; }
  void clear_paint() noexcept { paint_dirty_ = false; }

  /// Lays the tree out again if anything asked for it, and reports whether it
  /// did. Call this once per frame, before painting — it is the point at which
  /// a theme and a text measurer are in hand.
  bool update_layout(const LayoutContext& context);

  // -------------------------------------------------------------- dispatch --

  /// Each returns whether anything handled the event.
  bool mouse_down(const MouseEvent& event);
  bool mouse_up(const MouseEvent& event);
  bool mouse_move(const MouseEvent& event);
  bool wheel(const WheelEvent& event);
  bool key_down(const KeyEvent& event);
  bool key_up(const KeyEvent& event);
  bool text(char32_t codepoint);

  /// The pointer left the window. Clears hover; capture is deliberately kept,
  /// because a drag that runs off the edge of the window is still a drag.
  void mouse_exit();

  // ----------------------------------------------------------------- state --

  [[nodiscard]] Widget* hovered() const noexcept { return hovered_; }

  /// What the pointer should look like where it currently is.
  ///
  /// The widget holding the capture when there is one, so a cursor does not
  /// change halfway through the drag it belongs to — a trim dragged past the
  /// end of its clip is still a trim.
  [[nodiscard]] Cursor cursor() const;
  [[nodiscard]] Widget* focused() const noexcept { return focused_; }
  [[nodiscard]] Widget* captured() const noexcept { return captured_; }

  /// Moves the keyboard. Null clears it. A widget that is not focusable, not
  /// visible or not enabled is refused.
  bool set_focus(Widget* widget);

  /// Moves to the next focusable widget in tree order, wrapping. This is Tab.
  bool focus_next(bool backwards = false);

  /// Sends every subsequent mouse event to `widget` until released.
  ///
  /// A handled press captures automatically and the release lets go, which is
  /// what makes dragging work past the edge of the control being dragged.
  /// Explicit capture is for the rarer case of a drag that starts somewhere
  /// other than a press.
  void capture(Widget* widget);
  void release_capture();

  /// Drops any reference to `widget` or its descendants. Called for you when a
  /// subtree is cleared; call it directly before removing a widget by hand.
  void forget(Widget* widget);

  // ---------------------------------------------------------------- popups --

  /// Shows `content` above everything else, near `anchor`.
  ///
  /// The layer a menu and a dropdown list need, and it cannot be part of the
  /// tree: a list opening from a control near the bottom of a panel has to
  /// draw over its neighbours and outside its parent's clip, and no amount of
  /// arranging inside the tree gets that — the panel would clip it away.
  ///
  /// One at a time, which is what these are for. Opening a second closes the
  /// first.
  ///
  /// `anchor` is in window coordinates and is what the popup hangs under —
  /// flipped above it when there is no room below, and pushed back inside when
  /// it would run off an edge. A list that opens half outside the window is
  /// worse than one that opens somewhere slightly unexpected.
  void open_popup(std::unique_ptr<Widget> content, const Rect& anchor);

  /// Closes it, if one is open.
  ///
  /// The widget is not destroyed here. What asks for this is very often a
  /// button *inside* the popup, running its own click handler, so freeing it
  /// now would return into freed memory. It goes at the next `update_layout`
  /// — the same deferral the dock rearrangement uses, for the same reason.
  void close_popup() noexcept;

  [[nodiscard]] bool popup_open() const noexcept {
    return popup_ != nullptr && !popup_closing_;
  }
  [[nodiscard]] Widget* popup() const noexcept { return popup_closing_ ? nullptr : popup_.get(); }

  void paint(Painter& painter, const Theme& theme) const;

 private:
  /// Places the popup near its anchor, inside the window.
  void arrange_popup(const LayoutContext& context);

  /// What the pointer is over: the popup when one is open, the tree otherwise.
  /// Null outside an open popup, because nothing under it is reachable.
  [[nodiscard]] Widget* target_at(double x, double y);
  /// Walks up from `target` offering the event to each widget until one takes
  /// it. Stops at a disabled widget, which swallows rather than passing on:
  /// clicking a greyed-out button should do nothing, not hit the panel behind.
  ///
  /// Also marks the picture as needing repainting when something takes it: an
  /// event nobody handled cannot have changed what is drawn, and this is the
  /// one place every kind of event passes through.
  template <typename Fn>
  [[nodiscard]] Widget* bubble(Widget* target, Fn&& deliver);

  void set_hovered(Widget* widget);
  void set_pressed(Widget* widget);
  void collect_focusable(Widget& widget, std::vector<Widget*>& out) const;

  std::unique_ptr<Widget> root_;
  /// Drawn after the root and offered input before it. Null when none is open.
  std::unique_ptr<Widget> popup_;
  Rect popup_anchor_;
  /// Set by `close_popup`, acted on at the next layout. See the note there.
  bool popup_closing_ = false;
  Rect bounds_;
  bool layout_dirty_ = false;
  bool paint_dirty_ = true;
  Widget* hovered_ = nullptr;
  Widget* focused_ = nullptr;
  Widget* captured_ = nullptr;
  /// The widget showing as pressed, so the flag is cleared on the same widget
  /// that set it even if the tree changed underneath.
  Widget* pressed_ = nullptr;

  /// Where the pointer was last seen, so hover can be recomputed when the tree
  /// moves beneath a stationary cursor.
  double mouse_x_ = 0.0;
  double mouse_y_ = 0.0;
  bool has_mouse_ = false;
};

}  // namespace cutline::ui
