#include "cutline/ui/browser.hpp"

#include "cutline/ui/painter.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cutline::ui {
namespace {

/// The badge is a square inset from the row, and everything after it starts
/// past it. Kept in one place so the rectangle a row is hit tested against and
/// the one it is drawn in cannot drift apart.
[[nodiscard]] double badge_inset(const Metrics& metrics) noexcept {
  return std::min(metrics.padding_y, 4.0);
}

}  // namespace

std::string_view to_string(MediaKind kind) noexcept {
  switch (kind) {
    case MediaKind::Video: return "video";
    case MediaKind::Audio: return "audio";
    case MediaKind::Image: return "image";
    case MediaKind::Title: return "title";
    case MediaKind::Color: return "color";
    case MediaKind::Adjustment: return "adjustment";
  }
  return "video";
}

std::string_view badge_text(MediaKind kind) noexcept {
  switch (kind) {
    case MediaKind::Video: return "V";
    case MediaKind::Audio: return "A";
    case MediaKind::Image: return "IM";
    case MediaKind::Title: return "T";
    case MediaKind::Color: return "C";
    case MediaKind::Adjustment: return "AD";
  }
  return "V";
}

// ----------------------------------------------------------------- browser --

MediaBrowser::MediaBrowser() {
  // Rows run past the bottom edge; that is what the scrolling is for.
  set_clips_children(true);
  // Arrow keys move the selection, so the keyboard has to be able to land here.
  set_focusable(true);
}

void MediaBrowser::set_items(std::vector<MediaItem> items) {
  // Kept by id rather than by index. After a sort or a filter the row that was
  // selected has usually moved, and holding the index would quietly select
  // whatever slid into its place.
  std::string kept;
  if (selection_.has_value() && *selection_ < items_.size()) kept = items_[*selection_].id;

  items_ = std::move(items);
  selection_.reset();
  if (!kept.empty()) {
    const auto found = std::ranges::find(items_, kept, &MediaItem::id);
    if (found != items_.end()) {
      selection_ = static_cast<std::size_t>(found - items_.begin());
    }
  }

  // A press that was in flight refers to a row that may no longer exist.
  pressed_row_.reset();
  drag_.reset();
  refresh_bounds();
}

const MediaItem* MediaBrowser::selected() const noexcept {
  if (!selection_.has_value() || *selection_ >= items_.size()) return nullptr;
  return &items_[*selection_];
}

void MediaBrowser::select(std::optional<std::size_t> index) {
  if (index.has_value() && *index >= items_.size()) index.reset();
  selection_ = index;
  if (!index.has_value()) return;

  const double top = static_cast<double>(*index) * row_height();
  vertical_.reveal(top, top + row_height());
}

bool MediaBrowser::select_id(std::string_view id) {
  const auto found = std::ranges::find(items_, id, &MediaItem::id);
  if (found == items_.end()) return false;
  select(static_cast<std::size_t>(found - items_.begin()));
  return true;
}

// -------------------------------------------------------------- geometry --

double MediaBrowser::row_height() const noexcept { return std::max(1.0, metrics_.list_row_height); }

Rect MediaBrowser::list_area() const {
  Rect area = bounds();
  if (vertical_.scrollable()) area.width = std::max(0.0, area.width - metrics_.scrollbar_width);
  return area;
}

Rect MediaBrowser::row_rect(std::size_t index) const {
  if (index >= items_.size()) return {};

  const Rect area = list_area();
  const double height = row_height();
  const double top = area.y + static_cast<double>(index) * height - vertical_.offset;

  // Rows outside the window report nothing, so painting and hit testing can
  // both simply skip an empty rectangle.
  if (top + height <= area.y || top >= area.bottom()) return {};
  return Rect{area.x, top, area.width, height};
}

Rect MediaBrowser::badge_rect(std::size_t index) const {
  const Rect row = row_rect(index);
  if (row.empty()) return {};

  const double inset = badge_inset(metrics_);
  const double size = std::max(0.0, row.height - 2.0 * inset);
  return Rect{row.x + inset, row.y + inset, size, size};
}

std::optional<std::size_t> MediaBrowser::row_at(double x, double y) const {
  const Rect area = list_area();
  if (!area.contains(x, y)) return std::nullopt;

  const double at = (y - area.y + vertical_.offset) / row_height();
  if (at < 0.0) return std::nullopt;

  const auto index = static_cast<std::size_t>(at);
  if (index >= items_.size()) return std::nullopt;
  return index;
}

Rect MediaBrowser::scrollbar() const {
  if (!vertical_.scrollable()) return {};
  return Rect{bounds().right() - metrics_.scrollbar_width, bounds().y, metrics_.scrollbar_width,
              bounds().height};
}

Rect MediaBrowser::scroll_thumb() const {
  const Rect bar = scrollbar();
  if (bar.empty()) return {};
  return Rect{bar.x, bar.y + vertical_.thumb_offset(bar.height), bar.width,
              vertical_.thumb_size(bar.height)};
}

void MediaBrowser::scroll_to(double offset) { vertical_.scroll_to(offset); }
void MediaBrowser::scroll_by(double delta) { vertical_.scroll_by(delta); }

void MediaBrowser::refresh_bounds() {
  vertical_.visible = bounds().height;
  vertical_.content = static_cast<double>(items_.size()) * row_height();
  vertical_.clamp();
}

// ---------------------------------------------------------------- layout --

void MediaBrowser::layout(const LayoutContext& context) {
  metrics_ = context.metrics();
  refresh_bounds();
}

// ----------------------------------------------------------------- paint --

void MediaBrowser::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = list_area();
  if (area.empty()) return;

  const SurfaceStyle& panel = theme.style(Part::Panel, State::Normal);
  const double font = theme.metrics.font_size;
  const double small = theme.metrics.small_font_size;
  const double pad = theme.metrics.padding_x;

  painter.push_clip(area, 0.0);

  for (std::size_t i = 0; i < items_.size(); ++i) {
    const Rect row = row_rect(i);
    if (row.empty()) continue;

    const MediaItem& item = items_[i];
    const bool chosen = selection_.has_value() && *selection_ == i;

    // A selected row is a themed surface rather than a colour wash, so a theme
    // with bevelled selection gets one.
    if (chosen) paint_surface(painter, row, theme.style(Part::Clip, State::Selected));

    // The badge borrows the tool button's surface: it is the small square
    // themed thing every theme already describes.
    const Rect badge = badge_rect(i);
    if (!badge.empty()) {
      const SurfaceStyle& badge_style =
          theme.style(Part::ToolButton, item.offline ? State::Disabled : State::Normal);
      paint_surface(painter, badge, badge_style);
      painter.text(text_run(badge, std::string(badge_text(item.kind)), badge_style, small,
                            TextAlign::Center, true));
    }

    const SurfaceStyle& text_style =
        item.offline ? theme.style(Part::Panel, State::Disabled)
                     : theme.style(Part::Clip, chosen ? State::Selected : State::Normal);

    // The detail column is measured rather than guessed at, so a long name is
    // cut off before it runs into the duration rather than over it.
    const std::string detail = item.offline ? "offline" : item.detail;
    const double detail_width = detail.empty() ? 0.0 : painter.measure(detail, small, false) + pad;

    const double name_x = badge.empty() ? row.x + pad : badge.right() + pad;
    const Rect name_area{name_x, row.y, std::max(0.0, row.right() - name_x - detail_width),
                         row.height};
    if (!name_area.empty()) {
      painter.text(text_run(name_area, item.name, text_style, font, TextAlign::Left, chosen));
    }

    if (!detail.empty()) {
      const Rect detail_area{row.right() - detail_width, row.y, detail_width - pad * 0.5,
                             row.height};
      painter.text(text_run(detail_area, detail, theme.style(Part::Panel, State::Disabled), small,
                            TextAlign::Right));
    }
  }

  painter.pop_clip();

  const Rect bar = scrollbar();
  if (!bar.empty()) {
    paint_surface(painter, bar, theme.style(Part::Scrollbar, State::Normal));
    paint_surface(painter, scroll_thumb(),
                  theme.style(Part::ScrollThumb, scrolling_ ? State::Pressed : State::Normal));
  }

  // The empty pool says so. A blank panel is indistinguishable from one that
  // failed to draw.
  if (items_.empty()) {
    const Rect message = area.inset(theme.metrics.panel_padding);
    painter.text(text_run(message, "No media. Ctrl+I imports a file.", panel, small,
                          TextAlign::Center));
  }
}

// ----------------------------------------------------------------- input --

bool MediaBrowser::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  const Rect thumb = scroll_thumb();
  if (!thumb.empty() && thumb.contains(event.x, event.y)) {
    grab_ = event.y - thumb.y;
    scrolling_ = true;
    return true;
  }
  if (const Rect bar = scrollbar(); !bar.empty() && bar.contains(event.x, event.y)) {
    vertical_.scroll_by(event.y > thumb.y ? vertical_.visible : -vertical_.visible);
    return true;
  }

  const auto row = row_at(event.x, event.y);
  if (!row.has_value()) {
    // Clicking past the last row clears the selection, the same as clicking
    // empty space anywhere else.
    if (selection_.has_value()) {
      selection_.reset();
      if (on_select_) on_select_(std::nullopt);
    }
    return true;
  }

  select(row);
  if (on_select_) on_select_(selection_);

  if (event.click_count >= 2) {
    // A double-click is not the start of a drag: the press that opened it has
    // already selected the row, and letting it also arm a drag would make an
    // impatient double-click drop a clip somewhere.
    pressed_row_.reset();
    if (on_activate_) on_activate_(*row);
    return true;
  }

  pressed_row_ = row;
  press_x_ = event.x;
  press_y_ = event.y;
  return true;
}

bool MediaBrowser::on_mouse_move(const MouseEvent& event) {
  if (scrolling_) {
    const Rect bar = scrollbar();
    vertical_.drag_thumb(bar.height, event.y - bar.y - grab_);
    return true;
  }

  if (!pressed_row_.has_value()) return false;

  if (!drag_.has_value()) {
    const double moved = std::hypot(event.x - press_x_, event.y - press_y_);
    if (moved < kBrowserDragThreshold) return true;
    drag_ = pressed_row_;
  }

  drag_x_ = event.x;
  drag_y_ = event.y;
  return true;
}

bool MediaBrowser::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  if (scrolling_) {
    scrolling_ = false;
    return true;
  }

  const std::optional<std::size_t> dropped = drag_;
  pressed_row_.reset();
  drag_.reset();

  if (!dropped.has_value()) return false;
  // Reported wherever it was released, including outside the browser — the
  // press captured the pointer, so a drop on the timeline still arrives here.
  if (on_drop_) on_drop_(*dropped, event.x, event.y);
  return true;
}

bool MediaBrowser::on_wheel(const WheelEvent& event) {
  if (!vertical_.scrollable() || event.delta_y == 0.0) return false;

  const double before = vertical_.offset;
  vertical_.scroll_by(event.delta_y * row_height() * 3.0);
  // Unhandled at the ends, so the wheel carries on to whatever is outside
  // rather than dying against a list that cannot move any further.
  return vertical_.offset != before;
}

bool MediaBrowser::move_selection(int delta) {
  if (items_.empty()) return false;

  const auto last = static_cast<int>(items_.size()) - 1;
  const int from = selection_.has_value() ? static_cast<int>(*selection_) : (delta > 0 ? -1 : last + 1);
  const int to = std::clamp(from + delta, 0, last);
  if (selection_.has_value() && to == from) return false;

  select(static_cast<std::size_t>(to));
  if (on_select_) on_select_(selection_);
  return true;
}

bool MediaBrowser::on_key_down(const KeyEvent& event) {
  if (!event.modifiers.none()) return false;

  switch (event.key) {
    case Key::Up: return move_selection(-1);
    case Key::Down: return move_selection(1);
    case Key::Home: return move_selection(-static_cast<int>(items_.size()));
    case Key::End: return move_selection(static_cast<int>(items_.size()));
    case Key::PageUp:
    case Key::PageDown: {
      const int page = std::max(1, static_cast<int>(vertical_.visible / row_height()) - 1);
      return move_selection(event.key == Key::PageUp ? -page : page);
    }
    case Key::Enter:
      if (!selection_.has_value()) return false;
      if (on_activate_) on_activate_(*selection_);
      return true;
    default: return false;
  }
}

}  // namespace cutline::ui
