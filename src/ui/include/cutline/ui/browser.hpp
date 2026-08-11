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
#include "cutline/ui/timeline.hpp"
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
  /// A sequence, which is a source like any other once it is in the pool.
  Sequence,
};

/// How many kinds there are, for anything that keeps one entry per kind. A
/// count rather than a sentinel enumerator: a `Count` in the enumeration is a
/// value every switch has to remember not to handle.
inline constexpr std::size_t kMediaKindCount = 7;
static_assert(static_cast<std::size_t>(MediaKind::Sequence) + 1 == kMediaKindCount,
              "a kind was added without the count following it");

[[nodiscard]] std::string_view to_string(MediaKind kind) noexcept;

/// The letters on an entry's badge. Two at most, because it is drawn in a
/// square the height of a row.
[[nodiscard]] std::string_view badge_text(MediaKind kind) noexcept;

/// How the pool is laid out.
enum class BrowserView {
  /// One row an entry, with columns. What a large pool is worked in.
  List,
  /// A grid of pictures. What a pool is *looked* at in — the point of it is
  /// recognising a shot rather than reading its name.
  Icons,
};

/// A column of the list, and the heading that sorts by it.
///
/// Every one of these is drawn from what a `MediaItem` already carries, so
/// adding a column costs a case in two switches rather than another field on
/// every row. What the *duration* column shows is the row's `detail`, because
/// turning seconds into timecode needs the sequence's frame rate and this layer
/// has never known one.
enum class MediaColumn {
  Name,
  Duration,
  Uses,
  Kind,
};

[[nodiscard]] std::string_view column_heading(MediaColumn column) noexcept;

/// The columns a pool shows, in order, when nothing else is asked for.
///
/// Kind is last because the badge already says it — so it is the first to go
/// when the panel is too narrow to hold them all, and losing it costs nothing.
[[nodiscard]] std::vector<MediaColumn> default_columns();

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

  /// Frames from across the source, for the icon view to draw one of.
  ///
  /// Shared rather than copied: these are megabytes, the same cache the
  /// timeline draws its filmstrips from owns them, and a pool of forty entries
  /// rebuilding after every gesture cannot afford to copy any of it. Null is
  /// ordinary — it means the worker has not got there yet, and the tile draws a
  /// placeholder until it does.
  std::shared_ptr<const Filmstrip> filmstrip;

  /// The colour this row is labelled with, as "#rrggbb", or empty for none.
  ///
  /// Drawn as a stripe down the leading edge rather than as a wash over the
  /// row: a tinted row fights the selection for the same pixels, and a label is
  /// meant to be found at a glance down the list rather than read one entry at
  /// a time. The timeline draws clip labels the same way.
  std::string label_color;

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

  // ------------------------------------------------------------------ view --

  void set_view(BrowserView view);
  [[nodiscard]] BrowserView view() const noexcept { return view_; }

  /// Which entry the pointer is over, and how far across it — from 0 at the
  /// left edge to 1 at the right.
  ///
  /// The second is what makes an icon scrub: the tile shows the frame that far
  /// through the source, so running the pointer across a picture plays it. Only
  /// meaningful in the icon view, where a whole tile is worth scrubbing across;
  /// a row twenty pixels tall is not.
  [[nodiscard]] std::optional<std::size_t> hovered_item() const noexcept { return hover_; }
  [[nodiscard]] double hovered_fraction() const noexcept { return hover_fraction_; }

  /// How many tiles fit across. One in the list view, where a row is a row.
  [[nodiscard]] int tiles_across() const noexcept;

  // --------------------------------------------------------------- columns --

  /// Replaces the columns. An empty list means no header and no columns, which
  /// is what a panel too narrow for even a name should fall back to.
  void set_columns(std::vector<MediaColumn> columns);
  [[nodiscard]] const std::vector<MediaColumn>& columns() const noexcept { return columns_; }

  /// The columns that actually fit, in order. Never empty while `columns` is
  /// not: the name is always drawn, however narrow the panel gets, because a
  /// list of durations with no names is not a list of anything.
  [[nodiscard]] std::vector<MediaColumn> visible_columns() const;

  /// Which column the list is ordered by, for the arrow on its heading. The
  /// browser does not sort — that is a question about media, and answering it
  /// here would mean knowing what a clip is.
  void set_sorted_by(std::optional<MediaColumn> column, bool descending);

  /// A heading clicked. Sorting by the same one again is the caller's cue to
  /// reverse it, which is what every list of files anywhere does.
  void set_on_sort(std::function<void(MediaColumn)> on_sort) { on_sort_ = std::move(on_sort); }

  /// The header strip, or empty when there are no columns.
  [[nodiscard]] Rect header_rect() const;
  /// A column's rectangle inside a row, or empty when it does not fit.
  [[nodiscard]] Rect column_rect(std::size_t index, MediaColumn column) const;
  /// The same, in the header.
  [[nodiscard]] Rect heading_rect(MediaColumn column) const;

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

  void on_mouse_leave() override;

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

  std::vector<MediaColumn> columns_ = default_columns();
  std::optional<MediaColumn> sorted_by_;
  bool sorted_descending_ = false;
  std::function<void(MediaColumn)> on_sort_;

  /// Where each visible column starts and how wide it is, worked out once per
  /// layout. Held rather than recomputed per row: the arithmetic is the same for
  /// every row, and doing it in the paint loop is where the header and the rows
  /// would eventually stop lining up.
  struct Span {
    MediaColumn column = MediaColumn::Name;
    double x = 0.0;
    double width = 0.0;
  };
  std::vector<Span> spans_;
  void refresh_columns();
  void paint_tiles(Painter& painter, const Theme& theme) const;

  BrowserView view_ = BrowserView::List;
  std::optional<std::size_t> hover_;
  double hover_fraction_ = 0.0;
};

/// How wide a tile is in the icon view, and how tall its picture is.
///
/// Wide enough that a face is recognisable, which is the entire purpose — a
/// thumbnail too small to tell two takes apart is a slower list. Three fit
/// across the project panel at its usual width.
inline constexpr double kBrowserTileWidth = 108.0;
inline constexpr double kBrowserTilePicture = 61.0;  ///< 16:9 of the width

/// How wide each fixed column is. The name takes whatever is left.
inline constexpr double kBrowserDurationColumn = 84.0;
inline constexpr double kBrowserUsesColumn = 40.0;
inline constexpr double kBrowserKindColumn = 64.0;
/// The narrowest the name may be squeezed to before a column is dropped.
inline constexpr double kBrowserNameFloor = 90.0;

/// How far one level of bin indents a row.
inline constexpr double kBrowserIndent = 14.0;
/// The chevron's square on a bin row.
inline constexpr double kBrowserTwist = 14.0;
/// How wide the stripe down a labelled row is. Thin: it is a mark saying which
/// shot this is, not a second border.
inline constexpr double kBrowserLabelStripe = 3.0;

/// How far a press has to move before it counts as dragging an entry out.
inline constexpr double kBrowserDragThreshold = 4.0;

}  // namespace cutline::ui
