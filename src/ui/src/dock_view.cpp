#include "cutline/ui/dock_view.hpp"

#include "cutline/ui/painter.hpp"
#include "cutline/ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cutline::ui {
namespace {

/// Which edge of a rectangle a point is nearest, or the middle.
///
/// The middle is deliberately the largest target. Joining a group as a tab is
/// the common intent, and a scheme where every drop near the centre is a
/// coin toss between four splits makes docking feel unpredictable.
[[nodiscard]] DockSide side_within(const Rect& area, double x, double y) noexcept {
  if (area.empty()) return DockSide::Centre;

  const double fx = (x - area.x) / area.width;
  const double fy = (y - area.y) / area.height;

  const double left = fx;
  const double right = 1.0 - fx;
  const double top = fy;
  const double bottom = 1.0 - fy;

  const double nearest = std::min({left, right, top, bottom});
  if (nearest > kDockEdgeFraction) return DockSide::Centre;

  // Ties go clockwise from the left, which only matters exactly on a corner.
  if (nearest == left) return DockSide::Left;
  if (nearest == top) return DockSide::Top;
  if (nearest == right) return DockSide::Right;
  return DockSide::Bottom;
}

/// The part of `area` a drop on `side` would take.
[[nodiscard]] Rect half_towards(const Rect& area, DockSide side, double share) {
  switch (side) {
    case DockSide::Left: return Rect{area.x, area.y, area.width * share, area.height};
    case DockSide::Right:
      return Rect{area.right() - area.width * share, area.y, area.width * share, area.height};
    case DockSide::Top: return Rect{area.x, area.y, area.width, area.height * share};
    case DockSide::Bottom:
      return Rect{area.x, area.bottom() - area.height * share, area.width, area.height * share};
    case DockSide::Centre: break;
  }
  return area;
}

}  // namespace

// --------------------------------------------------------------- tab strip --

TabStrip::TabStrip() { set_clips_children(true); }

void TabStrip::set_tabs(std::vector<Tab> tabs, std::size_t active) {
  tabs_ = std::move(tabs);
  active_ = tabs_.empty() ? 0 : std::min(active, tabs_.size() - 1);
  widths_.assign(tabs_.size(), 0.0);
  has_hovered_tab_ = false;
  invalidate_layout();
}

void TabStrip::layout(const LayoutContext& context) {
  metrics_ = context.metrics();
  widths_.assign(tabs_.size(), 0.0);
  if (tabs_.empty()) return;

  const double closer = metrics_.font_size + metrics_.padding_x * 0.5;

  double total = 0.0;
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    const double text = context.text.measure(tabs_[i].title, metrics_.font_size, false);
    widths_[i] = text + metrics_.padding_x * 2.0 + (tabs_[i].closable ? closer : 0.0);
    total += widths_[i];
  }

  // Squeezed equally rather than proportionally when they will not fit. A
  // proportional squeeze leaves a long title readable and a short one a sliver
  // — and it is the sliver that then cannot be clicked.
  if (total > bounds().width && bounds().width > 0.0) {
    const double each = bounds().width / static_cast<double>(tabs_.size());
    std::ranges::fill(widths_, each);
  }
}

LayoutItem TabStrip::sizing(Axis axis, const LayoutContext& context) const {
  if (axis == Axis::Horizontal) return LayoutItem::flexible();
  return LayoutItem::fixed(context.metrics().panel_header_height);
}

Rect TabStrip::tab_rect(std::size_t index) const {
  if (index >= widths_.size()) return {};

  double x = bounds().x;
  for (std::size_t i = 0; i < index; ++i) x += widths_[i];
  if (x >= bounds().right()) return {};

  return Rect{x, bounds().y, std::min(widths_[index], bounds().right() - x), bounds().height};
}

Rect TabStrip::close_rect(std::size_t index) const {
  if (index >= tabs_.size() || !tabs_[index].closable) return {};

  const Rect tab = tab_rect(index);
  if (tab.empty()) return {};

  const double size = std::max(0.0, tab.height - metrics_.padding_y * 2.0);
  // A cross that has eaten the whole tab is worse than no cross: the title is
  // gone and every click closes something.
  if (size <= 0.0 || tab.width < size * 3.0) return {};

  return Rect{tab.right() - size - metrics_.padding_y, tab.y + metrics_.padding_y, size, size};
}

std::optional<std::size_t> TabStrip::tab_at(double x, double y) const {
  if (!bounds().contains(x, y)) return std::nullopt;
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    if (tab_rect(i).contains(x, y)) return i;
  }
  return std::nullopt;
}

void TabStrip::paint_content(Painter& painter, const Theme& theme) const {
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    const Rect tab = tab_rect(i);
    if (tab.empty()) continue;

    State state = State::Normal;
    if (i == active_) {
      state = State::Selected;
    } else if (has_hovered_tab_ && i == hovered_tab_) {
      state = State::Hover;
    }

    const SurfaceStyle& style = theme.style(Part::Tab, state);
    paint_surface(painter, tab, style);

    const Rect closer = close_rect(i);
    const Rect text = Rect{tab.x + theme.metrics.padding_x, tab.y,
                           std::max(0.0, (closer.empty() ? tab.right() : closer.x) - tab.x -
                                             theme.metrics.padding_x),
                           tab.height};
    if (!text.empty()) {
      painter.text(text_run(text, tabs_[i].title, style, theme.metrics.font_size,
                            TextAlign::Left, i == active_));
    }

    if (!closer.empty()) {
      // Drawn from lines rather than set in a font, for the same reason the
      // caption's buttons are: a missing glyph box is worse than a hand-drawn
      // cross.
      const Rect mark = closer.inset(closer.width * 0.3);
      painter.line(mark.x, mark.y, mark.right(), mark.bottom(), style.text, 1.0);
      painter.line(mark.right(), mark.y, mark.x, mark.bottom(), style.text, 1.0);
    }
  }
}

bool TabStrip::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  const auto index = tab_at(event.x, event.y);
  if (!index.has_value()) return false;

  if (const Rect closer = close_rect(*index);
      !closer.empty() && closer.contains(event.x, event.y)) {
    if (on_close_) on_close_(*index);
    return true;
  }

  if (*index != active_ && on_activate_) {
    on_activate_(*index);
    // Asked for explicitly. Showing a different tab is somebody else's rebuild,
    // and this press is declined below — so nothing else here marks the window
    // as needing drawing, and the tab that was pressed would not appear to have
    // been pressed until something unrelated caused a frame.
    if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
  }

  // Declined, so the press carries on up to the `DockView`, which takes the
  // pointer and owns the rest of the gesture. Handling it here would capture on
  // a widget that showing the tab is about to destroy, and the drag would end
  // before it began.
  return false;
}

bool TabStrip::on_mouse_move(const MouseEvent& event) {
  const auto over = tab_at(event.x, event.y);
  has_hovered_tab_ = over.has_value();
  if (over.has_value()) hovered_tab_ = *over;
  return false;
}

void TabStrip::on_mouse_leave() { has_hovered_tab_ = false; }

// -------------------------------------------------------------------- group --

DockGroup::DockGroup() { strip_ = &emplace<TabStrip>(); }

PanelId DockGroup::active_panel() const {
  const std::size_t index = strip_->active();
  return index < panels_.size() ? panels_[index] : PanelId{};
}

Rect DockGroup::content_area() const {
  Rect area = bounds();
  const double taken = std::min(strip_->bounds().height, area.height);
  area.y += taken;
  area.height -= taken;
  return area;
}

void DockGroup::layout(const LayoutContext& context) {
  const double strip_height =
      std::min(strip_->sizing(Axis::Vertical, context).basis, bounds().height);
  strip_->arrange(Rect{bounds().x, bounds().y, bounds().width, strip_height}, context);

  if (content_ != nullptr) content_->arrange(content_area(), context);
}

// --------------------------------------------------------------------- view --

DockView::DockView() { set_clips_children(true); }

std::string DockView::title_of(const PanelId& panel) const {
  if (!titles_) return panel;
  std::string title = titles_(panel);
  return title.empty() ? panel : title;
}

void DockView::reclaim(Widget& widget) {
  if (auto* group = dynamic_cast<DockGroup*>(&widget); group != nullptr) {
    if (group->content_ != nullptr) {
      // Taken rather than destroyed: a panel keeps its scroll position, its
      // selection and anything else it was holding across a rearrangement.
      if (std::unique_ptr<Widget> content = group->take(group->content_); content != nullptr) {
        spare_[group->content_id_] = std::move(content);
      }
      group->content_ = nullptr;
    }
    return;
  }
  for (const std::unique_ptr<Widget>& child : widget.children()) reclaim(*child);
}

void DockView::set_node(DockNode node) {
  for (const std::unique_ptr<Widget>& child : children()) reclaim(*child);

  groups_.clear();
  clear_children();

  node_ = std::move(node);
  normalise(node_);

  if (!node_.empty()) add(build(node_));
  invalidate_layout();
}

std::unique_ptr<Widget> DockView::build(const DockNode& node) {
  if (node.is_split()) {
    auto pane = std::make_unique<Splitter>(node.axis);
    pane->set_fractions(node.fractions);
    pane->set_on_resize([this] {
      if (on_resize_) on_resize_();
    });
    for (const DockNode& child : node.children) pane->add(build(child));
    return pane;
  }

  auto group = std::make_unique<DockGroup>();
  group->panels_ = node.panels;

  std::vector<TabStrip::Tab> tabs;
  tabs.reserve(node.panels.size());
  for (const PanelId& panel : node.panels) {
    tabs.push_back(TabStrip::Tab{.id = panel, .title = title_of(panel)});
  }
  group->strip_->set_tabs(std::move(tabs), node.active);

  if (const std::optional<PanelId> showing = node.active_panel(); showing.has_value()) {
    std::unique_ptr<Widget> content;
    if (const auto found = spare_.find(*showing); found != spare_.end()) {
      content = std::move(found->second);
      spare_.erase(found);
    } else if (factory_) {
      content = factory_(*showing);
    }
    if (content != nullptr) {
      group->content_id_ = *showing;
      group->content_ = &group->add(std::move(content));
    }
  }

  wire(*group);
  groups_.push_back(group.get());
  return group;
}

void DockView::wire(DockGroup& group) {
  DockGroup* which = &group;

  which->strip_->set_on_activate([this, which](std::size_t index) {
    if (index < which->panels_.size() && on_activate_) on_activate_(which->panels_[index]);
  });
  which->strip_->set_on_close([this, which](std::size_t index) {
    if (index < which->panels_.size() && on_close_) on_close_(which->panels_[index]);
  });
}

std::vector<DockGroup*> DockView::groups() const { return groups_; }

DockGroup* DockView::group_showing(std::string_view panel) const {
  for (DockGroup* group : groups_) {
    if (std::ranges::find(group->panels_, panel) != group->panels_.end()) return group;
  }
  return nullptr;
}

void DockView::read_fractions_into(DockNode& node) const {
  const auto walk = [](this const auto& self, const Widget* widget, DockNode& target) -> void {
    if (widget == nullptr || !target.is_split()) return;

    const auto* pane = dynamic_cast<const Splitter*>(widget);
    if (pane == nullptr) return;

    const std::span<const double> shares = pane->fractions();
    if (shares.size() == target.children.size()) {
      target.fractions.assign(shares.begin(), shares.end());
    }
    const std::span<const std::unique_ptr<Widget>> panes = pane->children();
    for (std::size_t i = 0; i < target.children.size() && i < panes.size(); ++i) {
      self(panes[i].get(), target.children[i]);
    }
  };
  walk(children().empty() ? nullptr : children().front().get(), node);
}

void DockView::layout(const LayoutContext& context) {
  metrics_ = context.metrics();
  if (!children().empty()) children().front()->arrange(bounds(), context);
}

// --------------------------------------------------------------------- drops --

std::optional<DropTarget> DockView::drop_target(double x, double y) const {
  if (!bounds().contains(x, y)) return std::nullopt;

  // The rim: a drop here means down the whole side of the window, not beside
  // whichever pane happens to be nearest the edge. Capped against the view's
  // own size so a narrow one is not entirely rim.
  const double rim = std::min(kDockRimWidth, std::min(bounds().width, bounds().height) * 0.2);
  if (x - bounds().x < rim) return DropTarget{.side = DockSide::Left, .at_edge = true};
  if (bounds().right() - x < rim) return DropTarget{.side = DockSide::Right, .at_edge = true};
  if (bounds().bottom() - y < rim) return DropTarget{.side = DockSide::Bottom, .at_edge = true};

  // Tabs before the *top* rim only. A strip means "join these", and the topmost
  // group's tabs run the whole length of that rim — letting it win there would
  // make the top panel the one group in the window nothing could be tabbed
  // into. The other three rims overlap a strip for an inch at most, so they
  // win: aiming at the very edge of the window means the edge, and losing that
  // to whichever strip happened to be at that height is why dragging a panel to
  // the side sometimes appeared to do nothing at all.
  for (const DockGroup* group : groups_) {
    if (!group->strip().bounds().contains(x, y)) continue;
    if (const PanelId onto = group->active_panel(); !onto.empty()) {
      return DropTarget{.onto = onto, .side = DockSide::Centre};
    }
  }

  if (y - bounds().y < rim) return DropTarget{.side = DockSide::Top, .at_edge = true};

  for (const DockGroup* group : groups_) {
    if (!group->bounds().contains(x, y)) continue;

    const PanelId onto = group->active_panel();
    if (onto.empty()) continue;
    return DropTarget{.onto = onto, .side = side_within(group->content_area(), x, y)};
  }
  return std::nullopt;
}

void DockView::set_drag(std::optional<PanelId> panel, double x, double y) {
  drag_ = std::move(panel);
  drag_x_ = x;
  drag_y_ = y;
  // The zone is drawn in the overlay, so moving it is a repaint and nothing
  // else. Asked for here rather than left to the caller, since a view being
  // told about a drag in another window has nothing else to prompt it.
  if (WidgetHost* owner = host(); owner != nullptr) owner->request_paint();
}

std::optional<PanelId> DockView::tab_at(double x, double y) const {
  for (const DockGroup* group : groups_) {
    const std::optional<std::size_t> index = group->strip().tab_at(x, y);
    if (!index.has_value() || *index >= group->panels_.size()) continue;
    // Not the cross: that closes the tab, and the strip has already done it.
    if (const Rect closer = group->strip().close_rect(*index);
        !closer.empty() && closer.contains(x, y)) {
      return std::nullopt;
    }
    return group->panels_[*index];
  }
  return std::nullopt;
}

std::optional<PanelId> DockView::panel_at(double x, double y) const {
  // The whole group, tab strip and body alike, and the panel *showing* in it —
  // which is what "the panel under the pointer" means when panels are stacked.
  // The innermost match wins, though groups do not nest today: taking the first
  // hit would be a rule that quietly stops being right if they ever do.
  std::optional<PanelId> found;
  for (const DockGroup* group : groups_) {
    if (!group->bounds().contains(x, y)) continue;
    if (const PanelId showing = group->active_panel(); !showing.empty()) found = showing;
  }
  return found;
}

bool DockView::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  std::optional<PanelId> panel = tab_at(event.x, event.y);
  if (!panel.has_value()) return false;

  // Noted, not started: whether this is a click or a drag is decided by whether
  // the pointer moves. Handling it is what takes the pointer, and taking the
  // pointer *here* is the point — the strip it came from may not survive the
  // next frame.
  pressed_ = std::move(panel);
  press_x_ = event.x;
  press_y_ = event.y;
  carrying_.reset();
  return true;
}

bool DockView::on_mouse_move(const MouseEvent& event) {
  if (!pressed_.has_value()) return false;

  if (!carrying_.has_value()) {
    if (std::hypot(event.x - press_x_, event.y - press_y_) < kTabDragThreshold) return true;
    carrying_ = pressed_;
  }

  set_drag(carrying_, event.x, event.y);
  // Reported outwards as well, so the drag can be shown in another window.
  // Fired from here rather than from inside `set_drag`, so that a view being
  // *told* about someone else's drag does not report it straight back.
  if (on_drag_) on_drag_(*carrying_, event.x, event.y);
  return true;
}

Rect DockView::drop_rect(const DropTarget& target) const {
  if (target.at_edge) return half_towards(bounds(), target.side, 1.0 / 3.0);

  const DockGroup* group = group_showing(target.onto);
  if (group == nullptr) return {};
  if (target.side == DockSide::Centre) return group->bounds();
  return half_towards(group->content_area(), target.side, 0.5);
}

void DockView::paint_overlay(Painter& painter, const Theme& theme) const {
  if (!drag_.has_value()) return;

  const std::optional<DropTarget> target = drop_target(drag_x_, drag_y_);
  if (!target.has_value()) return;

  const Rect where = drop_rect(*target);
  if (where.empty()) return;

  // The accent, washed out. Anything opaque would hide the panel being aimed
  // at, which is the one thing the user is looking at while aiming.
  Color wash = theme.accent;
  wash.a *= 0.28f;
  painter.fill(where, theme.style(Part::Panel).corner_radius, Fill::solid(wash));
  painter.stroke(where, theme.style(Part::Panel).corner_radius, theme.accent, 2.0);
}

bool DockView::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  const std::optional<PanelId> carried = std::move(carrying_);
  pressed_.reset();
  carrying_.reset();
  set_drag(std::nullopt, 0.0, 0.0);

  // A press that never travelled is a click, and showing the tab is all a click
  // does — the strip did that when it was pressed.
  if (!carried.has_value()) return false;

  const PanelId panel = *carried;

  // Resolved here rather than in the strip that started it, because acting on
  // this destroys that strip. See `TabStrip::on_mouse_up`.
  const std::optional<DropTarget> target = drop_target(event.x, event.y);
  if (!target.has_value()) {
    if (on_tear_out_) on_tear_out_(panel, event.x, event.y);
    return true;
  }
  if (on_dock_) on_dock_(panel, *target);
  return true;
}

}  // namespace cutline::ui
