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

std::string_view column_heading(MediaColumn column) noexcept {
  switch (column) {
    case MediaColumn::Name: return "Name";
    case MediaColumn::Duration: return "Duration";
    case MediaColumn::Uses: return "Used";
    case MediaColumn::Kind: return "Kind";
  }
  return "Name";
}

std::vector<MediaColumn> default_columns() {
  return {MediaColumn::Name, MediaColumn::Duration, MediaColumn::Uses, MediaColumn::Kind};
}

namespace {

/// How wide a column is, or zero for the name — which takes what is left.
[[nodiscard]] double fixed_width(MediaColumn column) noexcept {
  switch (column) {
    case MediaColumn::Name: return 0.0;
    case MediaColumn::Duration: return kBrowserDurationColumn;
    case MediaColumn::Uses: return kBrowserUsesColumn;
    case MediaColumn::Kind: return kBrowserKindColumn;
  }
  return 0.0;
}

/// What a row shows in a column. Everything comes from what the row already
/// carries, which is why a column costs no extra field.
[[nodiscard]] std::string cell_text(const MediaItem& item, MediaColumn column) {
  switch (column) {
    case MediaColumn::Name: return item.name;
    case MediaColumn::Duration:
      // A file that will not open says so where its length would be. That is
      // the column somebody scanning for what is broken is already looking at.
      if (item.offline) return "offline";
      return item.is_bin ? std::string{} : item.detail;
    case MediaColumn::Uses:
      // Bins have no clips, and a folder claiming "0" reads as a fault.
      return item.is_bin ? std::string{} : std::to_string(item.uses);
    case MediaColumn::Kind:
      // A bin's own count instead, which is the only thing it has to say.
      return item.is_bin ? item.detail : std::string(to_string(item.kind));
  }
  return {};
}

[[nodiscard]] TextAlign cell_align(MediaColumn column) noexcept {
  // Numbers to the right, words to the left, which is how every list of files
  // is read.
  return column == MediaColumn::Duration || column == MediaColumn::Uses ? TextAlign::Right
                                                                        : TextAlign::Left;
}

}  // namespace

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

double MediaBrowser::row_height() const noexcept {
  // A tile is its picture plus one line for the name beneath it, so an icon
  // grid is the same arithmetic as a list with taller rows — which is what lets
  // scrolling, hit testing and selection stay one implementation.
  if (view_ == BrowserView::Icons) {
    return kBrowserTilePicture + std::max(1.0, metrics_.list_row_height);
  }
  return std::max(1.0, metrics_.list_row_height);
}

void MediaBrowser::set_columns(std::vector<MediaColumn> columns) {
  columns_ = std::move(columns);
  refresh_columns();
  refresh_bounds();
}

void MediaBrowser::set_sorted_by(std::optional<MediaColumn> column, bool descending) {
  sorted_by_ = column;
  sorted_descending_ = descending;
}

void MediaBrowser::refresh_columns() {
  spans_.clear();
  if (columns_.empty()) return;

  Rect area = bounds();
  if (vertical_.scrollable()) area.width = std::max(0.0, area.width - metrics_.scrollbar_width);
  if (area.width <= 0.0) return;

  // Trailing columns are dropped until the name has room to be read. Dropped
  // from the end rather than proportionally squeezed, because a column narrow
  // enough to cut its own heading in half says less than no column at all —
  // and the order they are given in is what decides which one goes first.
  std::vector<MediaColumn> keeping = columns_;
  const auto fixed_total = [](const std::vector<MediaColumn>& list) {
    double total = 0.0;
    for (const MediaColumn column : list) total += fixed_width(column);
    return total;
  };
  const bool has_name = std::ranges::find(keeping, MediaColumn::Name) != keeping.end();
  const double floor = has_name ? kBrowserNameFloor : 0.0;
  while (keeping.size() > 1 && area.width - fixed_total(keeping) < floor) {
    keeping.pop_back();
  }

  double x = area.x;
  const double flexible = std::max(0.0, area.width - fixed_total(keeping));
  for (const MediaColumn column : keeping) {
    const double width = column == MediaColumn::Name ? flexible : fixed_width(column);
    spans_.push_back(Span{.column = column, .x = x, .width = width});
    x += width;
  }
}

std::vector<MediaColumn> MediaBrowser::visible_columns() const {
  std::vector<MediaColumn> out;
  out.reserve(spans_.size());
  for (const Span& span : spans_) out.push_back(span.column);
  return out;
}

Rect MediaBrowser::header_rect() const {
  // No headings over a grid: there are no columns to head, and a row of words
  // saying "Name" above a wall of pictures heads nothing.
  if (view_ == BrowserView::Icons || spans_.empty()) return {};
  return Rect{bounds().x, bounds().y, bounds().width, row_height()};
}

Rect MediaBrowser::heading_rect(MediaColumn column) const {
  const Rect header = header_rect();
  if (header.empty()) return {};
  const auto found = std::ranges::find(spans_, column, &Span::column);
  if (found == spans_.end()) return {};
  return Rect{found->x, header.y, found->width, header.height};
}

Rect MediaBrowser::column_rect(std::size_t index, MediaColumn column) const {
  const Rect row = row_rect(index);
  if (row.empty()) return {};
  const auto found = std::ranges::find(spans_, column, &Span::column);
  if (found == spans_.end()) return {};
  return Rect{found->x, row.y, found->width, row.height};
}

Rect MediaBrowser::list_area() const {
  Rect area = bounds();
  // Below the header, which is not part of the list: a row scrolled up under it
  // would otherwise draw over the headings.
  const double header = header_rect().height;
  area.y += header;
  area.height = std::max(0.0, area.height - header);
  if (vertical_.scrollable()) area.width = std::max(0.0, area.width - metrics_.scrollbar_width);
  return area;
}

int MediaBrowser::tiles_across() const noexcept {
  if (view_ != BrowserView::Icons) return 1;
  Rect area = bounds();
  if (vertical_.scrollable()) area.width = std::max(0.0, area.width - metrics_.scrollbar_width);
  // At least one, however narrow: a tile clipped at the edge is worse to look
  // at than one, but no tiles at all is a panel that draws nothing.
  return std::max(1, static_cast<int>(area.width / kBrowserTileWidth));
}

void MediaBrowser::set_view(BrowserView view) {
  if (view_ == view) return;
  view_ = view;
  hover_.reset();
  // Both, and in this order: the columns depend on whether there is a header,
  // and in the icon view there is not one.
  refresh_bounds();
  refresh_columns();
  refresh_bounds();
}

Rect MediaBrowser::row_rect(std::size_t index) const {
  if (index >= items_.size()) return {};

  const Rect area = list_area();
  const double height = row_height();

  if (view_ == BrowserView::Icons) {
    const int across = tiles_across();
    const double width = area.width / across;
    const auto row = static_cast<double>(index / static_cast<std::size_t>(across));
    const auto column = static_cast<double>(index % static_cast<std::size_t>(across));
    const double top = area.y + row * height - vertical_.offset;
    if (top + height <= area.y || top >= area.bottom()) return {};
    return Rect{area.x + column * width, top, width, height};
  }

  const double top = area.y + static_cast<double>(index) * height - vertical_.offset;

  // Rows outside the window report nothing, so painting and hit testing can
  // both simply skip an empty rectangle.
  if (top + height <= area.y || top >= area.bottom()) return {};
  return Rect{area.x, top, area.width, height};
}

Rect MediaBrowser::badge_rect(std::size_t index) const {
  const Rect row = row_rect(index);
  if (row.empty()) return {};
  // A bin has a chevron where a badge would go. Both would leave the name
  // starting further right on a folder than on the media inside it, which reads
  // as the tree being indented the wrong way round.
  if (items_[index].is_bin) return {};

  const double inset = badge_inset(metrics_);
  const double size = std::max(0.0, row.height - 2.0 * inset);
  const double step = static_cast<double>(items_[index].depth) * kBrowserIndent;
  return Rect{row.x + inset + step, row.y + inset, size, size};
}

Rect MediaBrowser::twist_rect(std::size_t index) const {
  const Rect row = row_rect(index);
  if (row.empty() || !items_[index].is_bin) return {};

  const double inset = badge_inset(metrics_);
  const double step = static_cast<double>(items_[index].depth) * kBrowserIndent;
  return Rect{row.x + inset + step, row.y, kBrowserTwist, row.height};
}

std::optional<std::size_t> MediaBrowser::file_target() const noexcept {
  if (!drag_.has_value()) return std::nullopt;

  const auto over = row_at(drag_x_, drag_y_);
  if (!over.has_value() || *over == *drag_) return std::nullopt;
  // Only onto a bin. Dropping media onto other media means nothing here — there
  // is no order to insert into, since the pool's order is the order it was
  // imported in.
  if (!items_[*over].is_bin) return std::nullopt;

  // A bin cannot go inside itself, and the browser can tell without knowing the
  // tree: what is inside a row is the unbroken run of rows after it with a
  // greater depth. Refused here as well as in the model so the highlight never
  // offers something that would then quietly not happen.
  if (items_[*drag_].is_bin && *over > *drag_) {
    for (std::size_t i = *drag_ + 1; i < items_.size(); ++i) {
      if (items_[i].depth <= items_[*drag_].depth) break;
      if (i == *over) return std::nullopt;
    }
  }
  return over;
}

std::optional<std::size_t> MediaBrowser::row_at(double x, double y) const {
  const Rect area = list_area();
  if (!area.contains(x, y)) return std::nullopt;

  const double at = (y - area.y + vertical_.offset) / row_height();
  if (at < 0.0) return std::nullopt;

  std::size_t index = static_cast<std::size_t>(at);
  if (view_ == BrowserView::Icons) {
    const int across = tiles_across();
    const auto column = static_cast<std::size_t>((x - area.x) / (area.width / across));
    // Past the last tile of a part-filled final row is empty space, not the
    // first tile of the row after it.
    if (column >= static_cast<std::size_t>(across)) return std::nullopt;
    index = index * static_cast<std::size_t>(across) + column;
  }
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
  vertical_.visible = std::max(0.0, bounds().height - header_rect().height);
  const auto across = static_cast<std::size_t>(std::max(1, tiles_across()));
  const std::size_t rows = (items_.size() + across - 1) / across;
  vertical_.content = static_cast<double>(rows) * row_height();
  vertical_.clamp();
}

// ---------------------------------------------------------------- layout --

void MediaBrowser::layout(const LayoutContext& context) {
  metrics_ = context.metrics();
  // Twice, and in this order. The columns need to know whether a scrollbar is
  // taking a gutter, and whether one is depends on how much room the header
  // leaves — which the columns decide. One pass settles it either way; the
  // second only matters when adding the scrollbar changed which columns fit.
  refresh_bounds();
  refresh_columns();
  refresh_bounds();
  refresh_columns();
}

// ----------------------------------------------------------------- paint --

void MediaBrowser::paint_tiles(Painter& painter, const Theme& theme) const {
  const Rect area = list_area();
  const SurfaceStyle& panel = theme.style(Part::Panel, State::Normal);
  const double small = theme.metrics.small_font_size;

  painter.push_clip(area, 0.0);

  for (std::size_t i = 0; i < items_.size(); ++i) {
    const Rect tile = row_rect(i);
    if (tile.empty()) continue;

    const MediaItem& item = items_[i];
    const bool chosen = selection_.has_value() && *selection_ == i;
    if (chosen) paint_surface(painter, tile, theme.style(Part::Clip, State::Selected));

    const Rect picture{tile.x + 3.0, tile.y + 3.0, std::max(0.0, tile.width - 6.0),
                       std::max(0.0, kBrowserTilePicture - 6.0)};

    // A well behind the picture, so a tile whose frames have not arrived is
    // still a tile rather than a hole, and a picture narrower than its slot is
    // letterboxed rather than floating.
    paint_surface(painter, picture, theme.style(Part::ToolButton, State::Normal));

    const FilmFrame* frame = nullptr;
    if (item.filmstrip != nullptr && !item.filmstrip->empty()) {
      // Where the pointer is across the tile, in source seconds. This is the
      // scrub: running the pointer along a picture walks the source, which is
      // the whole reason to look at a pool as pictures.
      const bool scrubbing = hover_.has_value() && *hover_ == i;
      const double at = scrubbing ? hover_fraction_ * item.duration : item.duration * 0.5;
      frame = item.filmstrip->nearest(at);
    }
    if (frame != nullptr && !frame->rgba.empty()) {
      painter.push_clip(picture, 0.0);
      // Fitted rather than stretched: a 4:3 source squeezed into a 16:9 tile
      // looks like a fault in the decoder rather than like a choice.
      const double scale = std::min(picture.width / std::max(1, frame->width),
                                    picture.height / std::max(1, frame->height));
      const double w = frame->width * scale;
      const double h = frame->height * scale;
      painter.image(Rect{picture.x + (picture.width - w) * 0.5,
                         picture.y + (picture.height - h) * 0.5, w, h},
                    ImageView{.pixels = frame->rgba.data(),
                              .width = frame->width,
                              .height = frame->height});
      painter.pop_clip();
    } else {
      // The badge, in the middle, where the picture would be. A bin says what
      // it is the same way, since a folder has no frames to show.
      const SurfaceStyle& quiet = theme.style(Part::Panel, State::Disabled);
      painter.text(text_run(picture, item.is_bin ? "BIN" : std::string(badge_text(item.kind)),
                            quiet, small, TextAlign::Center, true));
    }

    if (!item.label_color.empty()) {
      painter.fill(Rect{picture.x, picture.y, picture.width, kBrowserLabelStripe}, 0.0,
                   Fill::solid(parse_color(item.label_color, panel.text)));
    }

    const SurfaceStyle& text_style =
        item.offline ? theme.style(Part::Panel, State::Disabled)
                     : theme.style(Part::Clip, chosen ? State::Selected : State::Normal);
    const Rect caption{tile.x + 2.0, picture.bottom(), std::max(0.0, tile.width - 4.0),
                       std::max(0.0, tile.bottom() - picture.bottom())};
    if (!caption.empty()) {
      // Clipped for the same reason the list's cells are: a name is drawn from
      // a rectangle rather than bounded by one, and a tile is narrow.
      painter.push_clip(caption, 0.0);
      // Centred while it fits and left-aligned once it does not. Clipping a
      // centred name cuts both ends, and the end it must not cut is the front:
      // a camera file is told from its neighbours by how it starts, and
      // "07-23-2026 10PM-59-" says nothing that "Replay 07-23-2026" does not
      // say better. Found by driving, with exactly that file.
      const bool fits = painter.measure(item.name, small, false) <= caption.width;
      painter.text(text_run(caption, item.name, text_style, small,
                            fits ? TextAlign::Center : TextAlign::Left,
                            chosen || item.is_bin));
      painter.pop_clip();
    }

    if (const auto target = file_target(); target.has_value() && *target == i) {
      painter.stroke(tile, 0.0, theme.accent, 2.0);
    }
  }

  painter.pop_clip();

  const Rect bar = scrollbar();
  if (!bar.empty()) {
    paint_surface(painter, bar, theme.style(Part::Scrollbar, State::Normal));
    paint_surface(painter, scroll_thumb(),
                  theme.style(Part::ScrollThumb, scrolling_ ? State::Pressed : State::Normal));
  }

  if (items_.empty()) {
    const Rect message = area.inset(theme.metrics.panel_padding);
    painter.text(text_run(message, "No media. Ctrl+I imports a file.", panel, small,
                          TextAlign::Center));
  }
}

void MediaBrowser::paint_content(Painter& painter, const Theme& theme) const {
  const Rect area = list_area();
  if (area.empty()) return;

  if (view_ == BrowserView::Icons) {
    paint_tiles(painter, theme);
    return;
  }

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

    // A stripe down the leading edge, before anything else is drawn over it. A
    // wash over the whole row would fight the selection for the same pixels,
    // and a label is meant to be found at a glance down a list rather than read
    // one entry at a time.
    if (!item.label_color.empty()) {
      painter.fill(Rect{row.x, row.y + 1.0, kBrowserLabelStripe,
                        std::max(0.0, row.height - 2.0)},
                   0.0, Fill::solid(parse_color(item.label_color, panel.text)));
    }

    // The bin a drag would file into, drawn the same way a row being hovered
    // for a drop is drawn everywhere else here.
    if (const auto target = file_target(); target.has_value() && *target == i) {
      painter.stroke(row, 0.0, theme.accent, 2.0);
    }

    const Rect twist = twist_rect(i);
    if (!twist.empty()) {
      // The same chevron as the effects browser's folders, for the same reason:
      // a tree drawn two different ways in two panels reads as two different
      // kinds of thing.
      const double cx = twist.x + twist.width * 0.5;
      const double cy = twist.y + twist.height * 0.5;
      constexpr double reach = 3.0;
      const Color& ink = theme.style(Part::Panel, chosen ? State::Selected : State::Normal).text;
      if (item.expanded) {
        painter.line(cx - reach, cy - reach * 0.5, cx, cy + reach * 0.5, ink, 1.0);
        painter.line(cx, cy + reach * 0.5, cx + reach, cy - reach * 0.5, ink, 1.0);
      } else {
        painter.line(cx - reach * 0.5, cy - reach, cx + reach * 0.5, cy, ink, 1.0);
        painter.line(cx + reach * 0.5, cy, cx - reach * 0.5, cy + reach, ink, 1.0);
      }
    }

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
    const SurfaceStyle& quiet = theme.style(Part::Panel, State::Disabled);

    for (const Span& span : spans_) {
      const std::string text = cell_text(item, span.column);
      if (text.empty()) continue;

      // The name starts past whatever is at the leading edge of its row — the
      // chevron on a bin, the badge on media, the indent on both. Every other
      // column starts where its heading does, which is what makes a column of
      // durations line up whatever depth the rows are at.
      double x = span.x;
      double width = span.width;
      if (span.column == MediaColumn::Name) {
        const double from = !twist.empty()  ? twist.right() + pad * 0.5
                            : badge.empty() ? span.x + pad
                                            : badge.right() + pad;
        width = std::max(0.0, span.x + span.width - from - pad * 0.5);
        x = from;
      } else {
        // Inset, so a right-aligned number does not touch the column after it.
        width = std::max(0.0, width - pad * 0.5);
      }
      if (width <= 0.0) continue;

      const bool is_name = span.column == MediaColumn::Name;
      // Clipped to its own cell. Text is drawn from a rectangle but not bounded
      // by one, so a name longer than its column ran straight over the duration
      // beside it — found by driving, with a camera file whose name is half a
      // sentence. Every cell rather than only the name: a right-aligned number
      // in a column too narrow would spill the other way.
      painter.push_clip(Rect{x, row.y, width, row.height}, 0.0);
      // Bins in bold, so a folder reads as a heading rather than as another
      // entry that happens to have no duration.
      painter.text(text_run(Rect{x, row.y, width, row.height}, text,
                            is_name ? text_style : quiet, is_name ? font : small,
                            cell_align(span.column), is_name && (chosen || item.is_bin)));
      painter.pop_clip();
    }
  }

  painter.pop_clip();

  const Rect bar = scrollbar();
  if (!bar.empty()) {
    paint_surface(painter, bar, theme.style(Part::Scrollbar, State::Normal));
    paint_surface(painter, scroll_thumb(),
                  theme.style(Part::ScrollThumb, scrolling_ ? State::Pressed : State::Normal));
  }

  // The header last, over the rows rather than under them: `list_area` already
  // keeps them apart, and drawing it after means a row scrolled to the very top
  // can never leave a pixel above the headings.
  if (const Rect header = header_rect(); !header.empty()) {
    paint_surface(painter, header, theme.style(Part::ToolButton, State::Normal));
    const SurfaceStyle& heading_style = theme.style(Part::Panel, State::Normal);

    for (const Span& span : spans_) {
      const bool sorting = sorted_by_.has_value() && *sorted_by_ == span.column;
      // The arrow takes room from the heading rather than overlapping it, so a
      // narrow column shortens its word instead of drawing over the mark.
      const double arrow = sorting ? small : 0.0;
      const bool right = cell_align(span.column) == TextAlign::Right;
      const double text_x = span.x + (right ? pad * 0.5 : pad * 0.5 + arrow);
      const double text_width = std::max(0.0, span.width - pad - arrow);
      if (text_width > 0.0) {
        const Rect where{text_x, header.y, text_width, header.height};
        painter.push_clip(where, 0.0);
        painter.text(text_run(where, std::string(column_heading(span.column)), heading_style,
                              small, cell_align(span.column), sorting));
        painter.pop_clip();
      }

      if (sorting) {
        // The same two strokes as the bin chevron, pointed up or down. A word
        // saying "ascending" would not fit in forty pixels and a mark does not
        // need to.
        const double cx = right ? span.x + pad * 0.4 : span.x + pad * 0.5 + arrow * 0.5;
        const double cy = header.y + header.height * 0.5;
        constexpr double reach = 3.0;
        const double tip = sorted_descending_ ? reach * 0.5 : -reach * 0.5;
        painter.line(cx - reach, cy - tip, cx, cy + tip, heading_style.text, 1.0);
        painter.line(cx, cy + tip, cx + reach, cy - tip, heading_style.text, 1.0);
      }
    }
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
  // Before anything else, and for either button: a press on the headings is
  // never a press on a row, and treating it as one would select whatever was
  // scrolled underneath.
  if (const Rect header = header_rect();
      !header.empty() && header.contains(event.x, event.y)) {
    if (event.button != MouseButton::Left) return true;
    for (const Span& span : spans_) {
      if (event.x < span.x || event.x >= span.x + span.width) continue;
      if (on_sort_) on_sort_(span.column);
      return true;
    }
    return true;
  }

  if (event.button == MouseButton::Right) {
    // The row is selected first, the same as on the timeline: a menu that acted
    // on something other than what was clicked would be a trap. Empty space
    // clears the selection for exactly that reason — found by driving, where a
    // right-click below the last row offered Remove and would have removed
    // something the pointer was nowhere near.
    const auto row = row_at(event.x, event.y);
    if (row != selection_) {
      select(row);
      if (on_select_) on_select_(selection_);
    }
    if (on_context_menu_) on_context_menu_(event.x, event.y);
    return true;
  }

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

  // The chevron before the selection, so opening a bin does not also make it
  // the selected thing — which would move whatever a menu item acts on out from
  // under somebody merely looking inside a folder.
  if (const Rect twist = twist_rect(*row); !twist.empty() && twist.contains(event.x, event.y)) {
    if (on_toggle_) on_toggle_(*row);
    return true;
  }

  select(row);
  if (on_select_) on_select_(selection_);

  if (event.click_count >= 2 && items_[*row].is_bin) {
    // A bin has nothing to put in a sequence, so the gesture that places media
    // opens it instead. Premiere does the same.
    pressed_row_.reset();
    if (on_toggle_) on_toggle_(*row);
    return true;
  }

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

  // Where the pointer is, whether or not a button is down. This is what an icon
  // scrubs from, and it is tracked in both views because the cost is one hit
  // test and the alternative is two code paths that drift.
  if (const auto over = row_at(event.x, event.y); over != hover_ || over.has_value()) {
    const std::optional<std::size_t> was = hover_;
    const double fraction_was = hover_fraction_;
    hover_ = over;
    hover_fraction_ = 0.0;
    if (over.has_value()) {
      const Rect tile = row_rect(*over);
      if (tile.width > 0.0) {
        hover_fraction_ = std::clamp((event.x - tile.x) / tile.width, 0.0, 1.0);
      }
    }
    // Repainted only when it would look different. A pointer crossing a list
    // moves every few milliseconds, and asking for a paint on each one would
    // make hovering the pool the most expensive thing on screen.
    const bool moved_frame = view_ == BrowserView::Icons &&
                             std::abs(hover_fraction_ - fraction_was) > 0.01;
    if ((was != hover_ || moved_frame) && host() != nullptr) host()->request_paint();
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
  // Asked before the drag is forgotten, since that is what it is computed from.
  const std::optional<std::size_t> onto = file_target();
  pressed_row_.reset();
  drag_.reset();

  if (!dropped.has_value()) return false;

  // Filing or placing, never both. A release over a bin is somebody tidying the
  // pool; a release anywhere else is somebody putting a clip down.
  if (onto.has_value()) {
    if (on_file_) on_file_(*dropped, *onto);
    return true;
  }
  // Reported wherever it was released, including outside the browser — the
  // press captured the pointer, so a drop on the timeline still arrives here.
  if (on_drop_) on_drop_(*dropped, event.x, event.y);
  return true;
}

void MediaBrowser::on_mouse_leave() {
  // Or the last tile the pointer crossed keeps showing whichever frame it was
  // left on, which reads as a picture that changed on its own.
  if (!hover_.has_value()) return;
  hover_.reset();
  hover_fraction_ = 0.0;
  if (host() != nullptr) host()->request_paint();
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
      // The same split as the double-click it stands in for: a bin opens, media
      // is placed.
      if (items_[*selection_].is_bin) {
        if (on_toggle_) on_toggle_(*selection_);
        return true;
      }
      if (on_activate_) on_activate_(*selection_);
      return true;
    default: return false;
  }
}

}  // namespace cutline::ui
