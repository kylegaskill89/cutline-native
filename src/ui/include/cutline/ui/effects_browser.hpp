#pragma once

/// The effects library: a searchable tree of everything that can be applied.
///
/// Premiere has a *panel* for this, and it is the right shape. A menu was the
/// right answer while there were eleven effects and no panel machinery; it stops
/// being the right answer at about twenty, and it was already the reason the
/// video and audio stacks needed separate buttons opening separate menus.
///
/// Like the browser and the timeline, this draws its rows rather than building a
/// widget for each. A catalogue with several dozen entries in a dozen folders is
/// a layout pass over all of them to show the fifteen on screen.
///
/// It knows nothing about effects. A row has an opaque id, a name and a folder,
/// and what applying one means belongs above — which is what lets the same
/// widget hold video effects, audio effects and transitions without learning the
/// difference between them.

#include "cutline/ui/layout.hpp"
#include "cutline/ui/widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::ui {

/// One thing that can be applied.
struct EffectEntry {
  /// Opaque to the browser, which never looks inside it. The editor puts an
  /// effect type here, which is how a double-click finds its way back to the
  /// catalogue without the widget knowing a catalogue exists.
  std::string id;
  std::string name;
  /// Which folder it sits under, as a path: `Video Effects/Colour`. Folders are
  /// made from these rather than declared, so adding an effect in a new
  /// category needs nothing here — and nesting one needs nothing either, since
  /// a deeper path is just a longer string.
  ///
  /// A leading or trailing separator, or an empty step, is skipped rather than
  /// producing a folder with no name.
  std::string folder;

  friend bool operator==(const EffectEntry&, const EffectEntry&) = default;
};

class EffectsBrowser : public Widget {
 public:
  /// The separator between the steps of a folder path.
  static constexpr char kFolderSeparator = '/';

  /// A row on screen: either a folder's heading or one entry under it.
  struct Row {
    /// Empty for a folder row, which is named by `folder`.
    std::string id;
    /// The last step of the path for a folder, the entry's own name otherwise.
    std::string name;
    /// The whole path, which is what `is_open` is keyed by — two folders called
    /// Colour under different parents are two folders.
    std::string folder;
    bool is_folder = false;
    /// How deep in the tree, from zero. What the row is indented by, and the
    /// only thing about nesting the painting has to know.
    std::size_t depth = 0;

    friend bool operator==(const Row&, const Row&) = default;
  };

  EffectsBrowser();

  void set_items(std::vector<EffectEntry> items);
  [[nodiscard]] const std::vector<EffectEntry>& items() const noexcept { return items_; }

  /// Narrows the tree to what matches, as you type.
  ///
  /// Matched case-insensitively against a row's name, and a folder whose name
  /// matches keeps all of its contents — searching for "blur" should find the
  /// Blur folder as readily as an effect with Blur in its name.
  ///
  /// While a filter is set every folder is drawn open, whatever was collapsed
  /// before: a search that hid its own results behind a closed folder would be
  /// a search that appeared to find nothing.
  void set_filter(std::string filter);
  [[nodiscard]] const std::string& filter() const noexcept { return filter_; }

  /// The rows as they stand, folders and entries interleaved.
  [[nodiscard]] const std::vector<Row>& rows() const noexcept { return rows_; }

  [[nodiscard]] bool is_open(std::string_view folder) const;
  void set_open(std::string_view folder, bool open);

  // ------------------------------------------------------------- selection --

  [[nodiscard]] const std::string& selected() const noexcept { return selected_; }
  /// By id rather than by row, so rebuilding after a search does not silently
  /// select whatever has moved into the old row.
  void select(std::string id);

  /// A double-click, or Enter. What applying means belongs above this.
  void set_on_choose(std::function<void(const std::string& id)> on_choose) {
    on_choose_ = std::move(on_choose);
  }
  void set_on_select(std::function<void(const std::string& id)> on_select) {
    on_select_ = std::move(on_select);
  }

  // ------------------------------------------------------------- dragging --

  /// How far a press must travel before it becomes a drag rather than a click.
  static constexpr double kDragThreshold = 4.0;

  /// An entry is being dragged, and the pointer is at this point in the
  /// window. Called on every move, so whoever is listening can say whether a
  /// drop there would land on anything.
  ///
  /// The browser cannot answer that itself and should not try. The pointer is
  /// over a *different panel* — the timeline, most likely — and the only thing
  /// that knows about both is the composition root above them. A handled press
  /// captures the pointer, so these keep arriving however far outside the
  /// browser the cursor goes; that capture is the whole reason a drag can cross
  /// a panel boundary at all.
  void set_on_drag(std::function<void(const std::string& id, double x, double y)> on_drag) {
    on_drag_ = std::move(on_drag);
  }
  /// The button came up somewhere, ending a drag that had actually moved.
  void set_on_drop(std::function<void(const std::string& id, double x, double y)> on_drop) {
    on_drop_ = std::move(on_drop);
  }

  /// The entry being dragged, or empty. What the timeline's highlight and the
  /// cursor are drawn from.
  [[nodiscard]] const std::string& dragging() const noexcept { return dragging_; }

  // -------------------------------------------------------------- geometry --

  [[nodiscard]] double row_height() const noexcept { return row_height_; }
  /// Where a row is, in window coordinates. Empty when it is not one, or when
  /// it is scrolled out of sight.
  [[nodiscard]] Rect row_rect(std::size_t index) const;
  /// The row at a point, or past the end when there is none.
  [[nodiscard]] std::size_t row_at(double y) const;

  /// How far down the list has been scrolled, in pixels.
  [[nodiscard]] double scroll() const noexcept { return scroll_; }
  void set_scroll(double offset);
  /// The tallest the content is, which is what scrolling is clamped against.
  [[nodiscard]] double content_height() const noexcept;

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
  /// Rebuilds `rows_` from the items, the filter and what is open.
  void rebuild();
  [[nodiscard]] bool matches(const EffectEntry& entry) const;

  std::vector<EffectEntry> items_;
  std::vector<Row> rows_;
  std::string filter_;
  std::string selected_;

  /// Folders that have been *opened*, rather than the ones that are shut.
  ///
  /// Everything starts collapsed. The whole catalogue laid out at once is forty
  /// names in a narrow column, which is a list rather than a library — the
  /// folders are what make it navigable, and they only do that job while they
  /// are shut. Remembering the exception this way round also means a category
  /// added later arrives collapsed like the rest rather than spilling open.
  std::set<std::string, std::less<>> open_;

  /// The entry a press landed on, and whether it has travelled far enough to
  /// count as a drag. Held separately from the selection: a press picks
  /// immediately, and only movement turns it into something being carried.
  std::string pressed_;
  std::string dragging_;
  double press_x_ = 0.0;
  double press_y_ = 0.0;

  double scroll_ = 0.0;
  double row_height_ = 22.0;
  double font_size_ = 13.0;

  std::function<void(const std::string&)> on_choose_;
  std::function<void(const std::string&)> on_select_;
  std::function<void(const std::string&, double, double)> on_drag_;
  std::function<void(const std::string&, double, double)> on_drop_;
};

}  // namespace cutline::ui
