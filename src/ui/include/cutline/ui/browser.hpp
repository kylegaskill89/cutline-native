#pragma once

/// The project browser: the pool of media a sequence is built from.
///
/// Like the timeline, it draws plain data rather than reading a project. The
/// widget can be laid out, hit tested and painted from a vector of structs with
/// no project, no media and no decoder anywhere in sight; turning a pool into
/// that data belongs to the editor, above both.
///
/// Rows are drawn rather than being widgets of their own. A real edit's pool
/// runs to hundreds of entries, and a widget each would cost a layout pass over
/// all of them to show the twenty that are on screen — the same reason the
/// timeline draws its clips instead of building them.

#include "cutline/ui/layout.hpp"
#include "cutline/ui/widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cutline::ui {

/// What a pool entry is, which decides its badge and how it sorts.
enum class MediaKind {
  Video,
  Audio,
  Image,
  Title,
  Color,
  Adjustment,
};

[[nodiscard]] std::string_view to_string(MediaKind kind) noexcept;

/// The letters on an entry's badge. Two at most, because it is drawn in a
/// square the height of a row.
[[nodiscard]] std::string_view badge_text(MediaKind kind) noexcept;

/// One entry in the pool, as far as drawing is concerned.
struct MediaItem {
  /// Opaque to the browser, which never looks inside it. The editor puts a
  /// media id here, which is how a double-click finds its way back to the
  /// project without the widget knowing a project exists.
  std::string id;
  std::string name;
  /// The right-hand column: duration, size, frame rate — whatever is worth
  /// knowing about this entry at a glance.
  std::string detail;
  MediaKind kind = MediaKind::Video;

  /// Source length in seconds, which `detail` usually spells out as timecode.
  /// Carried as a number as well so the list can be ordered by it without
  /// parsing back what was written for a human.
  double duration = 0.0;

  /// The file is gone from disk. The entry stays in the pool regardless,
  /// because removing it would take every clip that used it out of the
  /// sequence — which is exactly what a user reconnecting a moved drive does
  /// not want.
  bool offline = false;

  /// How many clips on the timeline use it. Worth showing precisely when it is
  /// zero: unused media is otherwise impossible to find in a large pool.
  int uses = 0;

  /// How far this row is indented, with the top level at zero.
  ///
  /// A number rather than a tree, for the same reason the rows are drawn rather
  /// than built: the browser is handed a list already flattened in the order it
  /// should appear, and nesting is arithmetic in `row_rect` instead of a
  /// structure to walk on every paint.
  int depth = 0;

  /// This row is a bin rather than a piece of media.
  ///
  /// Bins are rows in the same list because that is what they look like on
  /// screen and what the arrow keys should walk through. Whoever builds the
  /// list decides what a bin's `id` means; the browser only ever hands it back.
  bool is_bin = false;

  /// Whether a bin is showing what is inside it. Meaningless on anything else.
  bool expanded = false;

  friend bool operator==(const MediaItem&, const MediaItem&) = default;
};

/// A scrolling list of the media in a project.
class MediaBrowser : public Widget {
 public:
  MediaBrowser();

  [[nodiscard]] const std::vector<MediaItem>& items() const noexcept { return items_; }

  /// Replaces the list. Selection is kept by id rather than by index, so
  /// rebuilding after a sort or a filter does not silently select whatever
  /// happens to have moved into the old row.
  void set_items(std::vector<MediaItem> items);

  // ------------------------------------------------------------- selection --

  [[nodiscard]] std::optional<std::size_t> selection() const noexcept { return selection_; }
  [[nodiscard]] const MediaItem* selected() const noexcept;

  /// Selects a row and scrolls it into view. Out of range clears the
  /// selection. Does not call `on_select` — that is for input, so setting the
  /// selection from a callback cannot loop.
  void select(std::optional<std::size_t> index);
  /// Selects by entry id. Reports whether one matched.
  bool select_id(std::string_view id);

  void set_on_select(std::function<void(std::optional<std::size_t>)> on_select) {
    on_select_ = std::move(on_select);
  }
  /// A double-click or Enter: put this entry in the sequence.
  void set_on_activate(std::function<void(std::size_t)> on_activate) {
    on_activate_ = std::move(on_activate);
  }
  /// A row dragged out of the list and released, at a point in the same
  /// coordinates as widget bounds — so whoever wired it up can ask the timeline
  /// what time that was.
  ///
  /// The browser cannot do the drop itself: where a clip lands is a question
  /// about a sequence, and this layer does not know sequences exist.
  void set_on_drop(std::function<void(std::size_t, double, double)> on_drop) {
    on_drop_ = std::move(on_drop);
  }

  /// A bin's chevron clicked, or a bin activated: show or hide what is in it.
  ///
  /// Which of those it currently is stays here rather than in the widget,
  /// because what is inside a bin is a question about the project — the browser
  /// is handed a flattened list and cannot know what a collapsed bin is hiding.
  void set_on_toggle(std::function<void(std::size_t)> on_toggle) {
    on_toggle_ = std::move(on_toggle);
  }

  /// A row dragged onto a bin row and released there: file it in that bin.
  ///
  /// Reported instead of `on_drop`, never as well as it. A release inside the
  /// browser is a filing and a release outside is a placement, and a gesture
  /// that did both would put a clip on the timeline every time somebody tidied
  /// the pool.
  void set_on_file(std::function<void(std::size_t, std::size_t)> on_file) {
    on_file_ = std::move(on_file);
  }

  /// The bin row a drag is currently over, or nothing. What the highlight is
  /// drawn from, and the same answer the release acts on — so what is shown and
  /// what happens cannot disagree.
  [[nodiscard]] std::optional<std::size_t> file_target() const noexcept;

  /// A right-click, at a point in the same coordinates as widget bounds.
  ///
  /// The row under it is selected first, exactly as the timeline does: a menu
  /// that acted on something other than what was clicked would be a trap.
  void set_on_context_menu(std::function<void(double, double)> on_context_menu) {
    on_context_menu_ = std::move(on_context_menu);
  }

  /// The row being dragged out, once the pointer has moved far enough for the
  /// gesture to be a drag rather than a click.
  [[nodiscard]] std::optional<std::size_t> dragging() const noexcept { return drag_; }
  /// Where the drag has got to. Only meaningful while `dragging`.
  [[nodiscard]] double drag_x() const noexcept { return drag_x_; }
  [[nodiscard]] double drag_y() const noexcept { return drag_y_; }

  // -------------------------------------------------------------- geometry --
  //
  // Exposed for the same reason the timeline's is: every one of these is
  // something to assert rather than measure off a screenshot.

  [[nodiscard]] double row_height() const noexcept;
  /// Everything except the scrollbar's gutter.
  [[nodiscard]] Rect list_area() const;
  /// A row's rectangle, or empty when it is scrolled out of sight.
  [[nodiscard]] Rect row_rect(std::size_t index) const;
  /// The square badge at the leading edge of a row, past its indent. Empty for
  /// a bin, whose chevron stands in that place instead.
  [[nodiscard]] Rect badge_rect(std::size_t index) const;
  /// The chevron's square on a bin row, and empty on anything else.
  [[nodiscard]] Rect twist_rect(std::size_t index) const;
  [[nodiscard]] std::optional<std::size_t> row_at(double x, double y) const;

  /// Empty when everything fits.
  [[nodiscard]] Rect scrollbar() const;
  [[nodiscard]] Rect scroll_thumb() const;

  [[nodiscard]] const Viewport& vertical() const noexcept { return vertical_; }
  void scroll_to(double offset);
  void scroll_by(double delta);

  // ------------------------------------------------------------- behaviour --

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;
  bool on_wheel(const WheelEvent& event) override;
  bool on_key_down(const KeyEvent& event) override;

 private:
  void refresh_bounds();
  /// Moves the selection by `delta` rows, clamped, and reveals it.
  bool move_selection(int delta);

  std::vector<MediaItem> items_;
  Viewport vertical_;
  std::optional<std::size_t> selection_;

  /// The row a press landed on, before it is known whether this is a click or
  /// a drag. Held separately from `drag_` so a click never reports a drop.
  std::optional<std::size_t> pressed_row_;
  std::optional<std::size_t> drag_;
  double press_x_ = 0.0;
  double press_y_ = 0.0;
  double drag_x_ = 0.0;
  double drag_y_ = 0.0;

  /// Where inside the scrollbar thumb it was grabbed.
  double grab_ = 0.0;
  bool scrolling_ = false;

  /// Taken from the theme during layout, because input arrives without one.
  Metrics metrics_;

  std::function<void(std::optional<std::size_t>)> on_select_;
  std::function<void(std::size_t)> on_activate_;
  std::function<void(std::size_t, double, double)> on_drop_;
  std::function<void(std::size_t)> on_toggle_;
  std::function<void(std::size_t, std::size_t)> on_file_;
  std::function<void(double, double)> on_context_menu_;
};

/// How far one level of bin indents a row.
inline constexpr double kBrowserIndent = 14.0;
/// The chevron's square on a bin row.
inline constexpr double kBrowserTwist = 14.0;

/// How far a press has to move before it counts as dragging an entry out.
inline constexpr double kBrowserDragThreshold = 4.0;

}  // namespace cutline::ui
