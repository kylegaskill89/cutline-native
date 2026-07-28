#pragma once

/// Where things go.
///
/// Pure geometry: rectangles in, rectangles out. No widgets, no painting, no
/// state. That is deliberate — layout is the part of a UI most likely to be
/// subtly wrong (an off-by-one in a divider, a scrollbar thumb that cannot
/// reach the end) and the part least pleasant to debug by looking at a window.
/// Kept as functions over plain data, every one of those questions is an
/// assertion instead.
///
/// The pieces are the ones an editor actually needs:
///
///   - `distribute`, which shares an axis between children that want fixed,
///     flexible, or bounded amounts of it. Toolbars and panel contents.
///   - `SplitLayout`, the draggable dividers between docked regions.
///   - `Viewport`, scrolling and zooming a region larger than its window —
///     the timeline, and anything with a scrollbar.
///
/// Sizes are in layout pixels, y down, matching `Rect`.

#include "cutline/ui/painter.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace cutline::ui {

/// What a widget needs in order to say how big it wants to be.
///
/// The theme, because spacing is part of a look rather than a constant; and a
/// way to measure text, because a button's width is its label's width and only
/// the backend that rasterises the font knows that.
struct LayoutContext {
  const Theme& theme;
  const TextMeasurer& text;

  [[nodiscard]] const Metrics& metrics() const noexcept { return theme.metrics; }
};

/// Space inside the edges of a rectangle.
struct Edges {
  double left = 0.0;
  double top = 0.0;
  double right = 0.0;
  double bottom = 0.0;

  [[nodiscard]] static Edges all(double amount) noexcept;
  [[nodiscard]] static Edges symmetric(double x, double y) noexcept;

  [[nodiscard]] double horizontal() const noexcept { return left + right; }
  [[nodiscard]] double vertical() const noexcept { return top + bottom; }

  friend bool operator==(const Edges&, const Edges&) = default;
};

/// `bounds` shrunk by `edges`, never past zero size.
[[nodiscard]] Rect inset(const Rect& bounds, const Edges& edges) noexcept;

/// The padding a theme asks for around a panel's contents, and around the
/// contents of a control. Metrics live on the theme because bevelled chrome
/// needs more room than flat chrome; these just spell out which number goes
/// where so widgets do not each decide for themselves.
[[nodiscard]] Edges panel_padding(const Metrics& metrics) noexcept;
[[nodiscard]] Edges control_padding(const Metrics& metrics) noexcept;

enum class Axis { Horizontal, Vertical };

[[nodiscard]] constexpr Axis cross_axis(Axis axis) noexcept {
  return axis == Axis::Horizontal ? Axis::Vertical : Axis::Horizontal;
}

inline constexpr double kUnbounded = std::numeric_limits<double>::infinity();

/// One child's demand for space along an axis.
///
/// `basis` is what it would like. `grow` is its share of anything left over,
/// `shrink` its share of any shortfall; zero for either means it does not
/// participate in that direction. `min` and `max` are hard.
struct LayoutItem {
  double basis = 0.0;
  double grow = 0.0;
  double shrink = 1.0;
  double min = 0.0;
  double max = kUnbounded;

  /// A child of exactly this size, which neither grows nor shrinks.
  [[nodiscard]] static LayoutItem fixed(double size) noexcept;
  /// A child that takes a share of the space, with an optional floor.
  [[nodiscard]] static LayoutItem flexible(double grow = 1.0, double min = 0.0) noexcept;

  friend bool operator==(const LayoutItem&, const LayoutItem&) = default;
};

/// Sizes for each item along an axis `available` long, with `spacing` between
/// adjacent items.
///
/// Surplus is shared in proportion to `grow`, shortfall in proportion to
/// `shrink * basis` — weighting by basis so a wide panel gives up more than a
/// narrow one rather than both losing the same absolute amount. Items that hit
/// a bound are frozen and the remainder is shared out again, so one child
/// clamping does not silently eat the space it could not use.
///
/// Sizes are never negative, but they can overflow `available`: when everything
/// has hit its minimum there is nothing left to give, and reporting the honest
/// overflow lets the caller clip or scroll rather than layout quietly lying.
[[nodiscard]] std::vector<double> distribute(std::span<const LayoutItem> items, double available,
                                             double spacing = 0.0);

/// Placement across the axis a box runs along.
enum class Align {
  Start,
  Center,
  End,
  /// Fill the cross axis. On the main axis this behaves as `Start`, since
  /// filling there is what `grow` is for.
  Stretch,
};

struct BoxLayout {
  Axis axis = Axis::Horizontal;
  double spacing = 0.0;
  Edges padding;
  /// Across the axis: how a child shorter than the box sits in it.
  Align cross = Align::Stretch;
  /// Along the axis: where the children sit when they do not fill it.
  Align main = Align::Start;
};

struct BoxChild {
  LayoutItem main;
  /// Size across the axis. Ignored when `BoxLayout::cross` is `Stretch`.
  double cross_size = 0.0;
};

/// Lays children out in a row or column inside `bounds`.
///
/// Returns one rectangle per child, in the same order. Children are placed even
/// when they overflow — an overflowing box is a scrolling box, not an error.
[[nodiscard]] std::vector<Rect> layout_box(const Rect& bounds, const BoxLayout& box,
                                           std::span<const BoxChild> children);

/// A row or column of panes with draggable dividers between them.
///
/// Sizes are held as fractions rather than pixels so that resizing the window
/// keeps the proportions the user chose, which is what every editor does and
/// what storing pixels would get wrong the first time someone maximises.
class SplitLayout {
 public:
  /// `fractions` need not sum to one; they are normalised. Fewer than two panes
  /// is allowed and simply has no dividers.
  SplitLayout(Axis axis, std::vector<double> fractions, double divider_size = 6.0,
              double min_pane = 40.0);

  [[nodiscard]] Axis axis() const noexcept { return axis_; }
  [[nodiscard]] std::size_t pane_count() const noexcept { return fractions_.size(); }
  [[nodiscard]] std::size_t divider_count() const noexcept;
  [[nodiscard]] std::span<const double> fractions() const noexcept { return fractions_; }

  [[nodiscard]] std::vector<Rect> panes(const Rect& bounds) const;

  /// The draggable strip between pane `index` and `index + 1`. Empty for an
  /// index with no divider.
  [[nodiscard]] Rect divider(const Rect& bounds, std::size_t index) const;

  /// The divider under a point, if any. Widened by `grab` on each side, because
  /// a six pixel target is not one a mouse can reliably hit.
  [[nodiscard]] std::size_t divider_at(const Rect& bounds, double x, double y,
                                       double grab = 3.0) const;
  static constexpr std::size_t kNoDivider = static_cast<std::size_t>(-1);

  /// Drags divider `index` so its centre sits at `position` along the axis, in
  /// the same coordinates as `bounds`.
  ///
  /// Only the two panes either side move; the rest keep their sizes. That is
  /// what a divider feels like it does, and letting the change ripple would
  /// make dragging one edge quietly reshuffle the far side of the window.
  /// Clamped so neither neighbour goes below the minimum. Returns whether
  /// anything actually moved.
  bool drag(const Rect& bounds, std::size_t index, double position);

 private:
  [[nodiscard]] double content_size(const Rect& bounds) const noexcept;

  Axis axis_;
  std::vector<double> fractions_;
  double divider_size_;
  double min_pane_;
};

/// A window onto content larger than itself, along one axis.
///
/// Everything here is one number derived from three, but they are the numbers
/// that a scrollbar gets wrong: thumb travel is the track minus the thumb, not
/// the track, so a naive mapping can never quite reach the end.
struct Viewport {
  /// Total size of the content.
  double content = 0.0;
  /// Size of the window onto it.
  double visible = 0.0;
  /// How far the window has been scrolled, from 0 to `max_offset`.
  double offset = 0.0;

  [[nodiscard]] double max_offset() const noexcept;
  [[nodiscard]] bool scrollable() const noexcept { return max_offset() > 0.0; }

  /// Pulls `offset` back into range. Called by everything here, and worth
  /// calling directly after changing `content` or `visible`.
  void clamp() noexcept;
  void scroll_by(double delta) noexcept;
  void scroll_to(double position) noexcept;

  /// Scrolls the least amount that brings `[begin, end)` into view, with
  /// `margin` of context either side where there is room.
  void reveal(double begin, double end, double margin = 0.0) noexcept;

  /// Length of a scrollbar thumb on a track of `track` pixels, floored at
  /// `minimum` so it stays grabbable on very long content.
  [[nodiscard]] double thumb_size(double track, double minimum = 20.0) const noexcept;
  /// Where that thumb starts.
  [[nodiscard]] double thumb_offset(double track, double minimum = 20.0) const noexcept;
  /// Scrolls so the thumb starts at `position` on the track.
  void drag_thumb(double track, double position, double minimum = 20.0) noexcept;
};

/// Adjusts a scroll offset so the content under `anchor` stays put while the
/// scale changes.
///
/// `anchor` is a position within the visible window, not within the content —
/// the pixel under the mouse. This is the whole difference between a timeline
/// that zooms where you are pointing and one that zooms to the left edge and
/// makes you scroll back every time.
[[nodiscard]] double zoom_about(double offset, double anchor, double old_scale,
                                double new_scale) noexcept;

}  // namespace cutline::ui
