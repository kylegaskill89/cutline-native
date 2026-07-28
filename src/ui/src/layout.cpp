#include "cutline/ui/layout.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace cutline::ui {
namespace {

/// Below this, two sizes are the same size. Layout arithmetic accumulates
/// error across passes, and a loop that stops only on exact equality would
/// keep redistributing quantities too small to see.
constexpr double kEpsilon = 1e-9;

[[nodiscard]] double clamp_to(const LayoutItem& item, double value) noexcept {
  // Minimum last, so a nonsensical min > max still yields the minimum rather
  // than something between them.
  return std::max(item.min, std::min(item.max, std::max(0.0, value)));
}

[[nodiscard]] double axis_size(const Rect& rect, Axis axis) noexcept {
  return axis == Axis::Horizontal ? rect.width : rect.height;
}

/// Where a child of `size` sits in `extent` under an alignment.
[[nodiscard]] double align_offset(Align align, double extent, double size) noexcept {
  switch (align) {
    case Align::Center:
      return (extent - size) / 2.0;
    case Align::End:
      return extent - size;
    case Align::Start:
    case Align::Stretch:
      break;
  }
  return 0.0;
}

}  // namespace

// ------------------------------------------------------------------- edges --

Edges Edges::all(double amount) noexcept { return {amount, amount, amount, amount}; }

Edges Edges::symmetric(double x, double y) noexcept { return {x, y, x, y}; }

Rect inset(const Rect& bounds, const Edges& edges) noexcept {
  return Rect{bounds.x + edges.left, bounds.y + edges.top,
              std::max(0.0, bounds.width - edges.horizontal()),
              std::max(0.0, bounds.height - edges.vertical())};
}

Edges panel_padding(const Metrics& metrics) noexcept {
  return Edges::all(metrics.panel_padding);
}

Edges control_padding(const Metrics& metrics) noexcept {
  return Edges::symmetric(metrics.padding_x, metrics.padding_y);
}

// -------------------------------------------------------------- distribute --

LayoutItem LayoutItem::fixed(double size) noexcept {
  return LayoutItem{.basis = size, .grow = 0.0, .shrink = 0.0, .min = size, .max = size};
}

LayoutItem LayoutItem::flexible(double grow, double min) noexcept {
  return LayoutItem{.basis = min, .grow = grow, .shrink = 1.0, .min = min, .max = kUnbounded};
}

std::vector<double> distribute(std::span<const LayoutItem> items, double available,
                               double spacing) {
  const std::size_t count = items.size();
  std::vector<double> sizes(count, 0.0);
  if (count == 0) return sizes;

  for (std::size_t i = 0; i < count; ++i) sizes[i] = clamp_to(items[i], items[i].basis);

  // Space the children can share, once the gaps between them are paid for.
  const double target = available - spacing * static_cast<double>(count - 1);

  std::vector<bool> frozen(count, false);

  // Each pass either settles or freezes at least one child at a bound, so this
  // cannot run longer than there are children.
  for (std::size_t pass = 0; pass <= count; ++pass) {
    const double used = std::accumulate(sizes.begin(), sizes.end(), 0.0);
    const double slack = target - used;
    if (std::abs(slack) < kEpsilon) break;

    // Weight by grow when there is surplus; by shrink scaled with current size
    // when there is a shortfall, so a wide child gives up more than a narrow
    // one instead of both losing the same absolute amount.
    const bool growing = slack > 0.0;
    double total_weight = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
      if (frozen[i]) continue;
      total_weight += growing ? items[i].grow : items[i].shrink * sizes[i];
    }
    if (total_weight <= kEpsilon) break;

    bool froze_any = false;
    for (std::size_t i = 0; i < count; ++i) {
      if (frozen[i]) continue;
      const double weight = growing ? items[i].grow : items[i].shrink * sizes[i];
      if (weight <= 0.0) continue;

      const double wanted = sizes[i] + slack * (weight / total_weight);
      const double allowed = clamp_to(items[i], wanted);
      if (std::abs(allowed - wanted) > kEpsilon) {
        // It hit a bound. Pin it there and let the next pass share out the
        // space it could not use, rather than losing that space silently.
        frozen[i] = true;
        froze_any = true;
      }
      sizes[i] = allowed;
    }
    if (!froze_any) break;
  }

  return sizes;
}

// ----------------------------------------------------------------- box flow --

std::vector<Rect> layout_box(const Rect& bounds, const BoxLayout& box,
                             std::span<const BoxChild> children) {
  std::vector<Rect> out;
  if (children.empty()) return out;

  const Rect inner = inset(bounds, box.padding);
  const double along = axis_size(inner, box.axis);
  const double across = axis_size(inner, cross_axis(box.axis));

  std::vector<LayoutItem> items;
  items.reserve(children.size());
  for (const BoxChild& child : children) items.push_back(child.main);

  const std::vector<double> sizes = distribute(items, along, box.spacing);

  const double used = std::accumulate(sizes.begin(), sizes.end(), 0.0) +
                      box.spacing * static_cast<double>(sizes.size() - 1);
  // Only shift when there is room to shift into; overflowing content starts at
  // the beginning whatever the alignment says, so the first child stays
  // reachable rather than being pushed off the near edge.
  const double lead = used < along ? align_offset(box.main, along, used) : 0.0;

  const bool horizontal = box.axis == Axis::Horizontal;
  double cursor = (horizontal ? inner.x : inner.y) + lead;
  const double cross_origin = horizontal ? inner.y : inner.x;

  out.reserve(children.size());
  for (std::size_t i = 0; i < children.size(); ++i) {
    const double cross_size =
        box.cross == Align::Stretch ? across : std::min(children[i].cross_size, across);
    const double cross_pos = cross_origin + align_offset(box.cross, across, cross_size);

    out.push_back(horizontal ? Rect{cursor, cross_pos, sizes[i], cross_size}
                             : Rect{cross_pos, cursor, cross_size, sizes[i]});
    cursor += sizes[i] + box.spacing;
  }
  return out;
}

// --------------------------------------------------------------- splitters --

SplitLayout::SplitLayout(Axis axis, std::vector<double> fractions, double divider_size,
                         double min_pane)
    : axis_(axis),
      fractions_(std::move(fractions)),
      divider_size_(std::max(0.0, divider_size)),
      min_pane_(std::max(0.0, min_pane)) {
  double total = 0.0;
  for (double f : fractions_) total += std::max(0.0, f);

  if (fractions_.empty()) return;
  if (total <= kEpsilon) {
    // Nothing usable was asked for, so share the space equally rather than
    // collapsing every pane to nothing.
    const double equal = 1.0 / static_cast<double>(fractions_.size());
    std::fill(fractions_.begin(), fractions_.end(), equal);
    return;
  }
  for (double& f : fractions_) f = std::max(0.0, f) / total;
}

std::size_t SplitLayout::divider_count() const noexcept {
  return fractions_.size() < 2 ? 0 : fractions_.size() - 1;
}

double SplitLayout::content_size(const Rect& bounds) const noexcept {
  return std::max(0.0, axis_size(bounds, axis_) -
                           divider_size_ * static_cast<double>(divider_count()));
}

std::vector<Rect> SplitLayout::panes(const Rect& bounds) const {
  std::vector<Rect> out;
  out.reserve(fractions_.size());
  if (fractions_.empty()) return out;

  const bool horizontal = axis_ == Axis::Horizontal;
  const double content = content_size(bounds);
  const double origin = horizontal ? bounds.x : bounds.y;

  double cursor = origin;
  double consumed = 0.0;
  for (std::size_t i = 0; i < fractions_.size(); ++i) {
    // Walk the cumulative fraction rather than summing individual sizes, and
    // give the last pane whatever is left. Otherwise rounding leaves a sliver
    // of unpainted background down one edge.
    const bool last = i + 1 == fractions_.size();
    const double next = last ? content : std::min(content, consumed + fractions_[i] * content);
    const double size = std::max(0.0, next - consumed);
    consumed = next;

    out.push_back(horizontal ? Rect{cursor, bounds.y, size, bounds.height}
                             : Rect{bounds.x, cursor, bounds.width, size});
    cursor += size + divider_size_;
  }
  return out;
}

Rect SplitLayout::divider(const Rect& bounds, std::size_t index) const {
  if (index >= divider_count()) return {};
  const std::vector<Rect> laid = panes(bounds);
  const Rect& before = laid[index];
  return axis_ == Axis::Horizontal
             ? Rect{before.right(), bounds.y, divider_size_, bounds.height}
             : Rect{bounds.x, before.bottom(), bounds.width, divider_size_};
}

std::size_t SplitLayout::divider_at(const Rect& bounds, double x, double y, double grab) const {
  const double pad = std::max(0.0, grab);
  for (std::size_t i = 0; i < divider_count(); ++i) {
    const Rect strip = divider(bounds, i);
    // Widened along the axis only: a six pixel target is not one a mouse hits
    // reliably, but growing it across the axis would steal clicks from the
    // panes either side.
    const Rect target = axis_ == Axis::Horizontal
                            ? Rect{strip.x - pad, strip.y, strip.width + 2.0 * pad, strip.height}
                            : Rect{strip.x, strip.y - pad, strip.width, strip.height + 2.0 * pad};
    if (target.contains(x, y)) return i;
  }
  return kNoDivider;
}

bool SplitLayout::drag(const Rect& bounds, std::size_t index, double position) {
  if (index >= divider_count()) return false;
  const double content = content_size(bounds);
  if (content <= kEpsilon) return false;

  const std::vector<Rect> laid = panes(bounds);
  const Rect& first = laid[index];
  const Rect& second = laid[index + 1];

  const bool horizontal = axis_ == Axis::Horizontal;
  const double start = horizontal ? first.x : first.y;
  const double pair = axis_size(first, axis_) + divider_size_ + axis_size(second, axis_);

  // Both neighbours have to keep their minimum, and if the pair cannot even
  // hold two of them there is no drag that helps.
  const double room = pair - divider_size_;
  if (room < 2.0 * min_pane_) return false;

  const double wanted = position - divider_size_ / 2.0 - start;
  const double first_size = std::clamp(wanted, min_pane_, room - min_pane_);
  const double second_size = room - first_size;

  const double next_first = first_size / content;
  const double next_second = second_size / content;
  if (std::abs(next_first - fractions_[index]) < kEpsilon &&
      std::abs(next_second - fractions_[index + 1]) < kEpsilon) {
    return false;
  }

  // Only the pair changes, so the fractions still sum to what they did and the
  // far side of the window stays exactly where it was.
  fractions_[index] = next_first;
  fractions_[index + 1] = next_second;
  return true;
}

// ---------------------------------------------------------------- viewport --

double Viewport::max_offset() const noexcept { return std::max(0.0, content - visible); }

void Viewport::clamp() noexcept { offset = std::clamp(offset, 0.0, max_offset()); }

void Viewport::scroll_by(double delta) noexcept {
  offset += delta;
  clamp();
}

void Viewport::scroll_to(double position) noexcept {
  offset = position;
  clamp();
}

void Viewport::reveal(double begin, double end, double margin) noexcept {
  if (end + margin > offset + visible) offset = end + margin - visible;
  // Checked second so that a range taller than the window shows its start
  // rather than its end, which is what reading order expects.
  if (begin - margin < offset) offset = begin - margin;
  clamp();
}

double Viewport::thumb_size(double track, double minimum) const noexcept {
  if (track <= 0.0) return 0.0;
  if (content <= 0.0 || content <= visible) return track;
  const double proportional = track * (visible / content);
  return std::min(track, std::max(minimum, proportional));
}

double Viewport::thumb_offset(double track, double minimum) const noexcept {
  const double travel = track - thumb_size(track, minimum);
  const double range = max_offset();
  if (travel <= 0.0 || range <= 0.0) return 0.0;
  return travel * std::clamp(offset / range, 0.0, 1.0);
}

void Viewport::drag_thumb(double track, double position, double minimum) noexcept {
  // The thumb travels the track minus its own length. Mapping against the full
  // track instead is the classic bug where the last part of the content can
  // never quite be reached.
  const double travel = track - thumb_size(track, minimum);
  if (travel <= 0.0) {
    offset = 0.0;
    return;
  }
  offset = max_offset() * std::clamp(position / travel, 0.0, 1.0);
}

double zoom_about(double offset, double anchor, double old_scale, double new_scale) noexcept {
  if (old_scale <= 0.0 || new_scale <= 0.0) return offset;
  // Content position under the anchor, held fixed across the change of scale.
  const double at = (offset + anchor) / old_scale;
  return std::max(0.0, at * new_scale - anchor);
}

}  // namespace cutline::ui
