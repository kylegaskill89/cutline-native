#pragma once

/// The controls the editor is actually built from.
///
/// Each one is small on purpose. A widget owns three things — the size it wants
/// (`sizing`), where it puts its children (`layout`), and what it adds on top
/// of its themed surface (`paint_content`) — and nothing else. Everything about
/// how it *looks* comes from the theme, and everything about how input reaches
/// it comes from `WidgetHost`.
///
/// The consequence worth stating: none of these mention a colour, a corner
/// radius, or a fixed number of pixels of padding. A control that hard-coded
/// any of those would be the one thing a theme could not change, and there is
/// no way to find those except by reading every widget.

#include "cutline/ui/layout.hpp"
#include "cutline/ui/widget.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cutline::ui {

/// Empty, and takes whatever room is going.
///
/// How a toolbar pushes the rest of its contents to the far end without anyone
/// computing a gap.
class Spacer final : public Widget {};

/// A run of text.
class Label : public Widget {
 public:
  explicit Label(std::string text = {}, Part part = Part::Panel);

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  void set_text(std::string text) { text_ = std::move(text); }

  void set_align(TextAlign align) noexcept { align_ = align; }
  void set_bold(bool bold) noexcept { bold_ = bold; }
  /// Uses the theme's smaller size. For secondary text: timecode, hints.
  void set_small(bool small) noexcept { small_ = small; }

  [[nodiscard]] Part part() const noexcept override { return part_; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  [[nodiscard]] double font_size(const Metrics& metrics) const noexcept;

  std::string text_;
  Part part_;
  TextAlign align_ = TextAlign::Left;
  bool bold_ = false;
  bool small_ = false;
};

/// A pressable control.
///
/// Fires when the button comes back up over itself, not when it goes down —
/// pressing and then sliding off is how a click is cancelled, and it only works
/// because the press captured the pointer and so the release arrives here
/// wherever it happens.
class Button : public Widget {
 public:
  explicit Button(std::string text = {}, std::function<void()> on_click = {});

  [[nodiscard]] const std::string& text() const noexcept { return text_; }
  void set_text(std::string text) { text_ = std::move(text); }
  void set_on_click(std::function<void()> on_click) { on_click_ = std::move(on_click); }

  /// `Part::ToolButton` for the icon-sized ones in a toolbar, which the theme
  /// styles differently and which lay out square.
  void set_part(Part part) noexcept { part_ = part; }

  [[nodiscard]] Part part() const noexcept override { return part_; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  void fire();

  std::string text_;
  std::function<void()> on_click_;
  Part part_ = Part::Button;
};

/// A row or column of children.
///
/// Spacing is unset by default and comes from the theme, which is the point:
/// the same toolbar is tighter under a flat theme than under one whose buttons
/// have bevels to sit around. Padding is nothing by default, because a box is a
/// grouping and nesting several would otherwise accumulate margins nobody asked
/// for — `Panel` is the one that surrounds its contents.
class Box : public Widget {
 public:
  explicit Box(Axis axis = Axis::Horizontal);

  [[nodiscard]] Axis axis() const noexcept { return axis_; }
  void set_axis(Axis axis) noexcept { axis_ = axis; }

  /// Overrides the theme's spacing. Reset by passing nothing.
  void set_spacing(std::optional<double> spacing) noexcept { spacing_ = spacing; }
  void set_padding(std::optional<Edges> padding) noexcept { padding_ = padding; }
  void set_main(Align align) noexcept { main_ = align; }
  void set_cross(Align align) noexcept { cross_ = align; }

  /// Whether the box asks for room across its own axis.
  ///
  /// Off by default, and the default is the one that matters: a toolbar in a
  /// column should be as tall as its controls and no taller. Left to inherit
  /// from its children it would not be, because a spacer is flexible in every
  /// direction and would make the whole toolbar swallow the window.
  void set_fills_cross(bool fills) noexcept { fills_cross_ = fills; }

  void layout(const LayoutContext& context) override;
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;

 protected:
  /// Lays the children out inside an explicit rectangle rather than the whole
  /// of `bounds`. What a panel uses for the space left under its header.
  void layout_into(const Rect& area, const LayoutContext& context);

  [[nodiscard]] double spacing_for(const LayoutContext& context) const noexcept;
  [[nodiscard]] Edges padding_for(const LayoutContext& context) const noexcept;

 private:
  Axis axis_;
  std::optional<double> spacing_;
  std::optional<Edges> padding_;
  Align main_ = Align::Start;
  Align cross_ = Align::Stretch;
  bool fills_cross_ = false;
};

/// A row that can be grabbed and right-clicked, with anything inside it.
///
/// A `Box` that adds the two gestures a *list entry* needs and nothing else: a
/// drag, for reordering, and a right-click, for a menu about the row. Its
/// children keep their own behaviour — a checkbox inside one still toggles —
/// because a press only reaches here when nothing inside wanted it.
///
/// It exists because an effect card's header is several controls that together
/// mean "this effect", and there was no way to say anything about that row as a
/// whole. Premiere reorders its effect stack by dragging exactly this.
class GrabRow : public Box {
 public:
  /// How far a press must travel before it is a drag rather than a click.
  static constexpr double kDragThreshold = 4.0;

  explicit GrabRow(Axis axis = Axis::Horizontal);

  /// Dragged to a point in the window, on every move once it has started.
  void set_on_drag(std::function<void(double x, double y)> on_drag) {
    on_drag_ = std::move(on_drag);
  }
  /// Released, after a drag that actually moved.
  void set_on_drop(std::function<void(double x, double y)> on_drop) {
    on_drop_ = std::move(on_drop);
  }
  /// Right-clicked, at the point it happened.
  void set_on_context_menu(std::function<void(double x, double y)> on_menu) {
    on_context_menu_ = std::move(on_menu);
  }

  [[nodiscard]] bool dragging() const noexcept { return dragging_; }

  /// Draws the insertion line when the row is `selected`.
  ///
  /// A line across the top rather than a highlight over the whole row: the
  /// question a reorder asks is *where does it go*, and a lit row answers
  /// "onto this one", which is not a place.
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

 private:
  bool pressed_ = false;
  bool dragging_ = false;
  double press_x_ = 0.0;
  double press_y_ = 0.0;

  std::function<void(double, double)> on_drag_;
  std::function<void(double, double)> on_drop_;
  std::function<void(double, double)> on_context_menu_;
};


/// The window's own caption, drawn rather than left to the system.
///
/// Windows' title bar is the one part of a window an application does not own,
/// and it cannot be themed. An editor whose themes are meant to change chrome
/// has to draw its own, or the top thirty pixels stay grey while everything
/// beneath them turns into Luna.
///
/// The children are the caption buttons and sit at the trailing edge; the
/// title is drawn at the leading edge. The platform layer asks the widget tree
/// which parts of this are draggable, so the buttons stay clickable while the
/// rest moves the window.
class TitleBar : public Box {
 public:
  explicit TitleBar(std::string title = {});

  [[nodiscard]] const std::string& title() const noexcept { return title_; }
  void set_title(std::string title) { title_ = std::move(title); }

  /// Whether to draw the application's mark before the title.
  ///
  /// Off by default, because a torn-out panel's caption is a *window's* title
  /// bar too and stamping the product mark on every one of them turns branding
  /// into wallpaper. The main window sets it; nothing else does.
  void set_mark(bool mark) noexcept { mark_ = mark; }
  [[nodiscard]] bool mark() const noexcept { return mark_; }

  [[nodiscard]] Part part() const noexcept override { return Part::TitleBar; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  std::string title_;
  bool mark_ = false;
};

/// One of the three buttons at the end of a title bar.
///
/// The glyphs are drawn from primitives rather than set in a font. A caption
/// font is not something that can be relied on to exist, and a close button
/// that renders as a missing-glyph box is worse than one drawn by hand.
class CaptionButton : public Button {
 public:
  enum class Kind { Minimise, Maximise, Restore, Close };

  explicit CaptionButton(Kind kind, std::function<void()> on_click = {});

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  void set_kind(Kind kind) noexcept { kind_ = kind; }

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  Kind kind_;
};

/// Panes with draggable dividers between them: the docked layout itself.
///
/// Each child is a pane, in order. Sizes are fractions, so resizing the window
/// keeps the proportions the user dragged to, and the dividers take their width
/// and their look from the theme like everything else.
class Splitter : public Widget {
 public:
  explicit Splitter(Axis axis = Axis::Horizontal);

  [[nodiscard]] Axis axis() const noexcept { return axis_; }

  /// Proportions of the panes, normalised. If the count does not match the
  /// visible children by the time it is laid out, an even split is used
  /// instead — a mismatch means the caller is out of step with the tree, and
  /// guessing which pane the spare fraction belonged to would be worse.
  void set_fractions(std::vector<double> fractions);
  [[nodiscard]] std::span<const double> fractions() const noexcept;

  /// Which divider is being dragged, or `SplitLayout::kNoDivider`.
  [[nodiscard]] std::size_t dragging() const noexcept { return dragging_; }

  /// Called once, when a divider is let go.
  ///
  /// Not on every move: the panes already follow the pointer as it goes, and
  /// whoever wants to keep the new proportions only needs to hear about them
  /// at the end. Without this a splitter inside a rebuildable tree loses its
  /// size the next time the tree is rebuilt.
  void set_on_resize(std::function<void()> on_resize) { on_resize_ = std::move(on_resize); }

  void layout(const LayoutContext& context) override;
  void paint_overlay(Painter& painter, const Theme& theme) const override;

  /// A resize cursor over a divider, along the axis it actually moves in. The
  /// dividers already lit up under the pointer; what they did not say is which
  /// way they go.
  [[nodiscard]] Cursor cursor_at(double x, double y) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

 private:
  [[nodiscard]] std::vector<Widget*> panes() const;

  Axis axis_;
  SplitLayout split_;
  std::size_t dragging_ = SplitLayout::kNoDivider;
  /// Which divider the pointer is over, for the theme's hover state. Only
  /// meaningful because dividers are painted by the splitter rather than being
  /// widgets of their own.
  std::size_t hovered_divider_ = SplitLayout::kNoDivider;

  std::function<void()> on_resize_;
};

/// A window onto content taller or wider than itself.
///
/// The content is an ordinary widget laid out at its natural size and then
/// moved: scrolling is `translate` and nothing more, so it costs no measuring
/// and no second pass through layout.
class ScrollView : public Widget {
 public:
  explicit ScrollView(Axis axis = Axis::Vertical);

  /// Replaces whatever was being scrolled.
  Widget& set_content(std::unique_ptr<Widget> content);
  [[nodiscard]] Widget* content() const noexcept;

  [[nodiscard]] const Viewport& viewport() const noexcept { return view_; }
  /// Scrolls and moves the content to match. Clamped.
  void scroll_to(double offset);
  void scroll_by(double delta);

  /// The scrollbar track and its thumb. Empty when there is nothing to scroll.
  [[nodiscard]] Rect track() const;
  [[nodiscard]] Rect thumb() const;

  void layout(const LayoutContext& context) override;
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_overlay(Painter& painter, const Theme& theme) const override;

  bool on_wheel(const WheelEvent& event) override;
  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

 private:
  Axis axis_;
  Viewport view_;
  /// Taken from the theme during layout, because input arrives without one.
  double bar_width_ = 12.0;
  double step_ = 40.0;
  bool has_bar_ = false;

  /// Where in the thumb it was grabbed, so it does not jump under the cursor.
  double grab_ = 0.0;
  bool dragging_ = false;
};

/// A docked region: a themed surface, an optional header with a title, and a
/// column of contents inside the theme's padding.
class Panel : public Box {
 public:
  explicit Panel(std::string title = {});

  [[nodiscard]] const std::string& title() const noexcept { return title_; }
  void set_title(std::string title) { title_ = std::move(title); }

  /// Where the header is, once laid out. Empty when there is no title.
  [[nodiscard]] const Rect& header() const noexcept { return header_; }

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  void layout(const LayoutContext& context) override;
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  std::string title_;
  Rect header_;
};

}  // namespace cutline::ui
