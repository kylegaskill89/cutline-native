#include "cutline/ui/effects_browser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace cutline::ui {
namespace {

/// How far an entry is indented under its folder.
constexpr double kIndent = 14.0;
/// The chevron's square at the left of a folder row.
constexpr double kChevron = 14.0;
/// Notches of the wheel are worth this many rows.
constexpr double kWheelRows = 3.0;

[[nodiscard]] std::string lowered(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

[[nodiscard]] bool contains_fold(std::string_view haystack, std::string_view needle) {
  return lowered(haystack).find(lowered(needle)) != std::string::npos;
}

}  // namespace

EffectsBrowser::EffectsBrowser() {
  set_clips_children(true);
  set_focusable(true);
}

void EffectsBrowser::set_items(std::vector<EffectEntry> items) {
  items_ = std::move(items);
  rebuild();
}

void EffectsBrowser::set_folders(std::vector<std::string> folders) {
  folders_ = std::move(folders);
  rebuild();
}

void EffectsBrowser::set_drop_folder(std::string folder) {
  if (folder == drop_folder_) return;
  drop_folder_ = std::move(folder);
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

void EffectsBrowser::set_filter(std::string filter) {
  if (filter == filter_) return;
  filter_ = std::move(filter);
  // Back to the top. A search whose results begin below the scroll position
  // looks like a search that found nothing.
  scroll_ = 0.0;
  rebuild();
}

bool EffectsBrowser::is_open(std::string_view folder) const {
  // Everything is open while a search is running, whatever was collapsed
  // before: a search that hid its own results would appear to find nothing.
  if (!filter_.empty()) return true;
  return open_.contains(folder);
}

void EffectsBrowser::set_open(std::string_view folder, bool open) {
  if (open) {
    open_.emplace(folder);
  } else {
    if (const auto found = open_.find(folder); found != open_.end()) open_.erase(found);
  }
  rebuild();
}

bool EffectsBrowser::matches(const EffectEntry& entry) const {
  if (filter_.empty()) return true;
  // A folder whose name matches keeps its contents. Searching for "blur"
  // should find the Blur folder as readily as an effect with Blur in its name.
  return contains_fold(entry.name, filter_) || contains_fold(entry.folder, filter_);
}

namespace {

/// A folder path split into its steps, with empty ones dropped.
[[nodiscard]] std::vector<std::string_view> steps_of(std::string_view path) {
  std::vector<std::string_view> steps;
  std::size_t at = 0;
  while (at <= path.size()) {
    const std::size_t next = path.find(EffectsBrowser::kFolderSeparator, at);
    const std::string_view step =
        path.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
    if (!step.empty()) steps.push_back(step);
    if (next == std::string_view::npos) break;
    at = next + 1;
  }
  return steps;
}

/// The path up to and including `depth` steps.
[[nodiscard]] std::string prefix_of(const std::vector<std::string_view>& steps,
                                    std::size_t depth) {
  std::string out;
  for (std::size_t i = 0; i <= depth && i < steps.size(); ++i) {
    if (!out.empty()) out += EffectsBrowser::kFolderSeparator;
    out += steps[i];
  }
  return out;
}

}  // namespace

void EffectsBrowser::rebuild() {
  rows_.clear();

  // In the order the catalogue gave them, so a folder's position is the
  // registry's decision rather than the alphabet's. Paths rather than names, so
  // two folders called Colour under different parents stay two folders.
  // Declared folders first, so the ones somebody made by hand sit above the
  // catalogue rather than wherever their contents happen to fall in it.
  std::vector<std::string> paths;
  for (const std::string& folder : folders_) {
    if (!folder.empty() && std::ranges::find(paths, folder) == paths.end()) {
      paths.push_back(folder);
    }
  }
  for (const EffectEntry& entry : items_) {
    if (std::ranges::find(paths, entry.folder) == paths.end()) paths.push_back(entry.folder);
  }

  // Which headings have been written already, so a parent shared by three
  // children is drawn once. Walked in catalogue order, so the first entry that
  // needs a folder is what decides where it sits.
  std::vector<std::string> written;

  for (const std::string& path : paths) {
    std::vector<const EffectEntry*> shown;
    for (const EffectEntry& entry : items_) {
      if (entry.folder == path && matches(entry)) shown.push_back(&entry);
    }
    // A folder with nothing in it after filtering is not drawn at all. A column
    // of empty headings is the least useful possible answer to a search.
    //
    // A folder that was *declared* is the exception, because it was made rather
    // than implied and an invisible one looks like a failure. Only while nothing
    // is being searched for, though, or by matching the search itself: an empty
    // folder cannot be a search result.
    if (shown.empty()) {
      const bool declared = std::ranges::find(folders_, path) != folders_.end();
      if (!declared || !(filter_.empty() || contains_fold(path, filter_))) continue;
    }

    const std::vector<std::string_view> steps = steps_of(path);

    // Every step of the path, so a nested folder brings its parents with it.
    // Closed at any level hides everything below, which is what a tree means.
    bool hidden = false;
    for (std::size_t depth = 0; depth < steps.size(); ++depth) {
      const std::string prefix = prefix_of(steps, depth);
      if (std::ranges::find(written, prefix) == written.end()) {
        if (!hidden) {
          rows_.push_back(Row{.name = std::string(steps[depth]),
                              .folder = prefix,
                              .is_folder = true,
                              .depth = depth});
        }
        written.push_back(prefix);
      }
      if (!is_open(prefix)) hidden = true;
    }
    if (hidden) continue;

    for (const EffectEntry* entry : shown) {
      rows_.push_back(Row{.id = entry->id,
                          .name = entry->name,
                          .folder = path,
                          .depth = steps.size()});
    }
  }

  set_scroll(scroll_);
  invalidate_layout();
}

void EffectsBrowser::select(std::string id, std::string folder) {
  selected_ = std::move(id);
  selected_folder_ = std::move(folder);
}

// --------------------------------------------------------------- geometry --

double EffectsBrowser::content_height() const noexcept {
  return static_cast<double>(rows_.size()) * row_height_;
}

void EffectsBrowser::set_scroll(double offset) {
  const double most = std::max(0.0, content_height() - bounds().height);
  scroll_ = std::clamp(offset, 0.0, most);
}

Rect EffectsBrowser::row_rect(std::size_t index) const {
  if (index >= rows_.size()) return Rect{};
  return Rect{bounds().x, bounds().y + static_cast<double>(index) * row_height_ - scroll_,
              bounds().width, row_height_};
}

std::size_t EffectsBrowser::row_at(double y) const {
  if (row_height_ <= 0.0) return rows_.size();
  const double offset = y - bounds().y + scroll_;
  if (offset < 0.0) return rows_.size();
  const auto index = static_cast<std::size_t>(offset / row_height_);
  return index < rows_.size() ? index : rows_.size();
}

std::string EffectsBrowser::folder_at(double y) const {
  const std::size_t index = row_at(y);
  if (index >= rows_.size()) return {};
  // An entry's folder, not nothing: dropping onto the things already gathered in
  // a folder should land in it, rather than only the inch of heading above them.
  return rows_[index].folder;
}

void EffectsBrowser::layout(const LayoutContext& context) {
  row_height_ = context.metrics().list_row_height;
  font_size_ = context.metrics().font_size;
  // The rows may not have moved, but the room for them has, and a list scrolled
  // to the bottom of a taller panel would otherwise stay scrolled past its end.
  set_scroll(scroll_);
}

// ---------------------------------------------------------------- painting --

void EffectsBrowser::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& panel = theme.style(Part::Panel, State::Normal);

  if (rows_.empty()) {
    const Rect where = bounds().inset(6.0);
    painter.text(text_run(where, filter_.empty() ? "No effects" : "Nothing matches " + filter_,
                          panel, font_size_));
    return;
  }

  for (std::size_t i = 0; i < rows_.size(); ++i) {
    const Rect row = row_rect(i);
    // Only what is on screen. A catalogue of several dozen in a panel showing
    // fifteen is most of the work thrown away.
    if (row.bottom() < bounds().y || row.y > bounds().bottom()) continue;

    const Row& entry = rows_[i];
    if (entry.is_folder) {
      // Indented by its depth like an entry is. Without this a folder inside a
      // folder starts where its parent does, and the tree reads as a flat list
      // of headings — which is exactly what folders are there to avoid.
      const double step = static_cast<double>(entry.depth) * kIndent;
      const double cx = row.x + step + kChevron * 0.5 + 4.0;
      const double cy = row.y + row.height * 0.5;
      constexpr double reach = 3.0;
      if (is_open(entry.folder)) {
        painter.line(cx - reach, cy - reach * 0.5, cx, cy + reach * 0.5, panel.text, 1.0);
        painter.line(cx, cy + reach * 0.5, cx + reach, cy - reach * 0.5, panel.text, 1.0);
      } else {
        painter.line(cx - reach * 0.5, cy - reach, cx + reach * 0.5, cy, panel.text, 1.0);
        painter.line(cx + reach * 0.5, cy, cx - reach * 0.5, cy + reach, panel.text, 1.0);
      }
      painter.text(text_run(Rect{row.x + step + kChevron + 6.0, row.y,
                                 row.width - step - kChevron - 6.0, row.height},
                            entry.name, panel, font_size_, TextAlign::Left, true));
      continue;
    }

    const bool picked = !entry.id.empty() && entry.id == selected_ &&
                        (selected_folder_.empty() || entry.folder == selected_folder_);
    if (picked) {
      painter.fill(row, 0.0, Fill::solid(theme.accent));
    }
    const SurfaceStyle& style =
        picked ? theme.style(Part::MenuItem, State::Selected) : panel;
    const double inset = kChevron + kIndent + static_cast<double>(entry.depth) * kIndent;
    painter.text(text_run(Rect{row.x + inset, row.y, row.width - inset - 4.0, row.height},
                          entry.name, style, font_size_));
  }

  paint_drop_folder(painter, theme);
}

void EffectsBrowser::paint_drop_folder(Painter& painter, const Theme& theme) const {
  if (drop_folder_.empty()) return;

  // Round the heading *and* what is already in it, because that is the shape of
  // what the drop means: into this folder, alongside these. An outline round the
  // heading alone reads as landing on the heading.
  bool any = false;
  double top = 0.0;
  double bottom = 0.0;
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].folder != drop_folder_) continue;
    const Rect row = row_rect(i);
    top = any ? std::min(top, row.y) : row.y;
    bottom = any ? std::max(bottom, row.bottom()) : row.bottom();
    any = true;
  }
  if (!any) return;

  painter.stroke(Rect{bounds().x, top, bounds().width, bottom - top}.inset(1.0), 3.0,
                 theme.accent, 1.5);
}

// ------------------------------------------------------------------ input --

bool EffectsBrowser::on_mouse_down(const MouseEvent& event) {
  if (event.button == MouseButton::Right) {
    // Selects what is under it first, so a menu built from the selection is
    // built from what was clicked — and a right-click on a folder heading leaves
    // the selection alone rather than clearing it, because the menu for a folder
    // is about the folder.
    const std::size_t index = row_at(event.y);
    if (index < rows_.size() && !rows_[index].is_folder) {
      select(rows_[index].id, rows_[index].folder);
      if (on_select_) on_select_(selected_);
    }
    if (on_context_menu_) on_context_menu_(event.x, event.y);
    return true;
  }

  if (event.button != MouseButton::Left) return false;

  const std::size_t index = row_at(event.y);
  if (index >= rows_.size()) return false;

  const Row& row = rows_[index];
  if (row.is_folder) {
    pressed_.clear();
    dragging_.clear();
    // Anywhere on the heading, not only on the chevron. A folder's name is a
    // much larger target than a six-pixel mark, and both mean the same thing.
    set_open(row.folder, !is_open(row.folder));
    return true;
  }

  select(row.id, row.folder);
  if (on_select_) on_select_(selected_);

  // Noted, not started. Whether this press is a click or a drag is decided by
  // whether the pointer moves, and deciding it now would mean every click on an
  // effect also picked it up.
  pressed_ = row.id;
  dragging_.clear();
  press_x_ = event.x;
  press_y_ = event.y;

  // Premiere applies on a double-click. A single one only picks, so the
  // catalogue can be read without changing anything.
  if (event.click_count >= 2 && on_choose_) on_choose_(row.id);
  return true;
}

bool EffectsBrowser::on_mouse_move(const MouseEvent& event) {
  if (pressed_.empty()) return false;

  if (dragging_.empty()) {
    // Both axes: dragging an effect down onto a track below moves the pointer
    // hardly any distance sideways.
    const double travelled =
        std::max(std::abs(event.x - press_x_), std::abs(event.y - press_y_));
    if (travelled < kDragThreshold) return true;
    dragging_ = pressed_;
  }

  if (on_drag_) on_drag_(dragging_, event.x, event.y);
  return true;
}

bool EffectsBrowser::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || pressed_.empty()) return false;

  const std::string carried = std::move(dragging_);
  dragging_.clear();
  pressed_.clear();

  // Only a drag that actually moved is a drop. A press and release in place is
  // a click, and it has already done what a click does.
  if (!carried.empty() && on_drop_) on_drop_(carried, event.x, event.y);
  return true;
}

bool EffectsBrowser::on_wheel(const WheelEvent& event) {
  if (event.delta_y == 0.0) return false;
  const double before = scroll_;
  set_scroll(scroll_ + event.delta_y * row_height_ * kWheelRows);
  // Unhandled when there was nowhere to go, so a list that cannot scroll does
  // not swallow the wheel from whatever is behind it.
  if (scroll_ == before) return false;
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
  return true;
}

bool EffectsBrowser::on_key_down(const KeyEvent& event) {
  if (rows_.empty()) return false;

  const auto current = std::ranges::find(rows_, selected_, &Row::id);
  const auto position = current == rows_.end()
                            ? rows_.size()
                            : static_cast<std::size_t>(current - rows_.begin());

  const auto pick = [this](std::size_t index) {
    if (index >= rows_.size() || rows_[index].is_folder) return false;
    select(rows_[index].id, rows_[index].folder);
    if (on_select_) on_select_(selected_);
    // Kept in view, or the keyboard walks off the bottom of the panel.
    const Rect row = row_rect(index);
    if (row.y < bounds().y) set_scroll(scroll_ - (bounds().y - row.y));
    if (row.bottom() > bounds().bottom()) set_scroll(scroll_ + (row.bottom() - bounds().bottom()));
    return true;
  };

  switch (event.key) {
    case Key::Down:
      // Past the folder headings, which are not selectable.
      for (std::size_t i = position + 1; i < rows_.size(); ++i) {
        if (pick(i)) return true;
      }
      return true;
    case Key::Up:
      for (std::size_t i = position; i-- > 0;) {
        if (pick(i)) return true;
      }
      return true;
    case Key::Enter:
      if (!selected_.empty() && on_choose_) on_choose_(selected_);
      return true;
    default:
      return false;
  }
}

}  // namespace cutline::ui
