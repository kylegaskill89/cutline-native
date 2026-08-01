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
  /// Which folder it sits under. Folders are made from these rather than
  /// declared, so adding an effect in a new category needs nothing here.
  std::string folder;

  friend bool operator==(const EffectEntry&, const EffectEntry&) = default;
};

class EffectsBrowser : public Widget {
 public:
  /// A row on screen: either a folder's heading or one entry under it.
  struct Row {
    /// Empty for a folder row, which is named by `folder`.
    std::string id;
    std::string name;
    std::string folder;
    bool is_folder = false;

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

  /// Folders that are *closed*, rather than the ones that are open.
  ///
  /// A catalogue is worth more open than shut — the whole point of a library is
  /// seeing what is in it — and remembering the exception means a folder added
  /// later arrives open rather than hidden.
  std::set<std::string, std::less<>> closed_;

  double scroll_ = 0.0;
  double row_height_ = 22.0;
  double font_size_ = 13.0;

  std::function<void(const std::string&)> on_choose_;
  std::function<void(const std::string&)> on_select_;
};

}  // namespace cutline::ui
