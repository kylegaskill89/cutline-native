#include "cutline/ui/dock.hpp"

#include <algorithm>
#include <utility>

namespace cutline::ui {
namespace {

/// Whether a panel is anywhere in the whole layout.
[[nodiscard]] bool anywhere(const DockLayout& layout, std::string_view panel) {
  if (contains_panel(layout.root, panel)) return true;
  for (const FloatingDock& window : layout.floating) {
    if (contains_panel(window.root, panel)) return true;
  }
  return false;
}

/// The tree a panel lives in — the main one or a floating one — or null.
[[nodiscard]] DockNode* tree_holding(DockLayout& layout, std::string_view panel) {
  if (contains_panel(layout.root, panel)) return &layout.root;
  for (FloatingDock& window : layout.floating) {
    if (contains_panel(window.root, panel)) return &window.root;
  }
  return nullptr;
}

/// The leftmost tab group in a tree. Where a panel goes when nothing said
/// where it should go.
[[nodiscard]] DockNode* first_group(DockNode& node) {
  if (!node.is_split()) return &node;
  for (DockNode& child : node.children) {
    if (DockNode* found = first_group(child); found != nullptr) return found;
  }
  return nullptr;
}

/// Takes a panel out of whichever tab group has it. Leaves the tree in
/// whatever shape that left behind; the caller normalises.
bool detach(DockNode& node, std::string_view panel) {
  if (!node.is_split()) {
    const auto found = std::ranges::find(node.panels, panel);
    if (found == node.panels.end()) return false;

    const auto index = static_cast<std::size_t>(found - node.panels.begin());
    node.panels.erase(found);
    // The tab that was showing has to keep showing, which means following it
    // along when something before it was removed.
    if (node.active > index) --node.active;
    if (!node.panels.empty() && node.active >= node.panels.size()) {
      node.active = node.panels.size() - 1;
    }
    return true;
  }
  for (DockNode& child : node.children) {
    if (detach(child, panel)) return true;
  }
  return false;
}

bool detach(DockLayout& layout, std::string_view panel) {
  if (detach(layout.root, panel)) return true;
  for (FloatingDock& window : layout.floating) {
    if (detach(window.root, panel)) return true;
  }
  return false;
}

/// One fraction per child, summing to one.
void fit_fractions(DockNode& node) {
  const std::size_t count = node.children.size();
  if (count == 0) {
    node.fractions.clear();
    return;
  }

  // A count that does not match means the caller is out of step with the tree,
  // and guessing which child the spare fraction belonged to would be worse
  // than dividing the space evenly.
  if (node.fractions.size() != count) {
    node.fractions.assign(count, 1.0 / static_cast<double>(count));
    return;
  }

  double total = 0.0;
  for (double& share : node.fractions) {
    share = std::max(0.0, share);
    total += share;
  }
  if (total <= 0.0) {
    node.fractions.assign(count, 1.0 / static_cast<double>(count));
    return;
  }
  for (double& share : node.fractions) share /= total;
}

/// Two panes in the order a side asks for.
[[nodiscard]] DockNode ordered_split(Axis axis, DockNode leading, DockNode trailing) {
  std::vector<DockNode> children;
  children.push_back(std::move(leading));
  children.push_back(std::move(trailing));
  return DockNode::split(axis, std::move(children));
}

/// The axis a side splits along, and whether the new pane goes first.
[[nodiscard]] Axis axis_of(DockSide side) noexcept {
  return (side == DockSide::Left || side == DockSide::Right) ? Axis::Horizontal : Axis::Vertical;
}

[[nodiscard]] bool leads(DockSide side) noexcept {
  return side == DockSide::Left || side == DockSide::Top;
}

}  // namespace

std::string_view to_string(DockSide side) noexcept {
  switch (side) {
    case DockSide::Left: return "left";
    case DockSide::Right: return "right";
    case DockSide::Top: return "top";
    case DockSide::Bottom: return "bottom";
    case DockSide::Centre: return "centre";
  }
  return "centre";
}

// -------------------------------------------------------------------- node --

bool DockNode::empty() const noexcept {
  return is_split() ? children.empty() : panels.empty();
}

std::optional<PanelId> DockNode::active_panel() const {
  if (is_split() || panels.empty() || active >= panels.size()) return std::nullopt;
  return panels[active];
}

DockNode DockNode::tabs(std::vector<PanelId> panels) {
  DockNode node;
  node.kind = DockKind::Tabs;
  node.panels = std::move(panels);
  return node;
}

DockNode DockNode::split(Axis axis, std::vector<DockNode> children) {
  DockNode node;
  node.kind = DockKind::Split;
  node.axis = axis;
  node.children = std::move(children);
  fit_fractions(node);
  return node;
}

// ----------------------------------------------------------------- reading --

std::vector<PanelId> panels_in(const DockNode& node) {
  std::vector<PanelId> out;
  const auto walk = [&out](this const auto& self, const DockNode& current) -> void {
    if (!current.is_split()) {
      out.insert(out.end(), current.panels.begin(), current.panels.end());
      return;
    }
    for (const DockNode& child : current.children) self(child);
  };
  walk(node);
  return out;
}

std::vector<PanelId> panels_in(const DockLayout& layout) {
  std::vector<PanelId> out = panels_in(layout.root);
  for (const FloatingDock& window : layout.floating) {
    const std::vector<PanelId> theirs = panels_in(window.root);
    out.insert(out.end(), theirs.begin(), theirs.end());
  }
  return out;
}

bool contains_panel(const DockNode& node, std::string_view panel) {
  return group_of(node, panel) != nullptr;
}

const DockNode* group_of(const DockNode& node, std::string_view panel) {
  if (!node.is_split()) {
    return std::ranges::find(node.panels, panel) == node.panels.end() ? nullptr : &node;
  }
  for (const DockNode& child : node.children) {
    if (const DockNode* found = group_of(child, panel); found != nullptr) return found;
  }
  return nullptr;
}

DockNode* group_of(DockNode& node, std::string_view panel) {
  // The const version does the looking; this is the only place the cast is
  // made, rather than at every call site that needs to change something.
  return const_cast<DockNode*>(group_of(std::as_const(node), panel));
}

bool is_floating(const DockLayout& layout, std::string_view panel) {
  for (const FloatingDock& window : layout.floating) {
    if (contains_panel(window.root, panel)) return true;
  }
  return false;
}

// --------------------------------------------------------- canonical form --

void normalise(DockNode& node) {
  if (!node.is_split()) {
    node.active = node.panels.empty() ? 0 : std::min(node.active, node.panels.size() - 1);
    return;
  }

  // Depth first, so a child that collapses into something simpler is already
  // simple by the time this level looks at it.
  for (DockNode& child : node.children) normalise(child);
  fit_fractions(node);

  std::vector<DockNode> kept;
  std::vector<double> shares;
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    DockNode& child = node.children[i];
    const double share = node.fractions[i];
    if (child.empty()) continue;

    // A row inside a row is the same row. Left nested, three drags produce a
    // tree several levels deep that lays out identically and drags worse: its
    // dividers no longer line up with the ones beside them.
    if (child.is_split() && child.axis == node.axis) {
      for (std::size_t j = 0; j < child.children.size(); ++j) {
        kept.push_back(std::move(child.children[j]));
        shares.push_back(share * child.fractions[j]);
      }
      continue;
    }
    kept.push_back(std::move(child));
    shares.push_back(share);
  }

  node.children = std::move(kept);
  node.fractions = std::move(shares);

  if (node.children.empty()) {
    node = DockNode{};
    return;
  }
  if (node.children.size() == 1) {
    // A split with one pane is not a split. Left in place it draws a divider
    // with nothing on one side of it.
    DockNode only = std::move(node.children.front());
    node = std::move(only);
    return;
  }
  fit_fractions(node);
}

void normalise(DockLayout& layout) {
  normalise(layout.root);
  for (FloatingDock& window : layout.floating) normalise(window.root);
  // A window whose last panel went somewhere else is not a window any more.
  std::erase_if(layout.floating,
                [](const FloatingDock& window) { return window.root.empty(); });
}

// -------------------------------------------------------------- operations --

bool dock_panel(DockLayout& layout, std::string_view panel, std::string_view target,
                DockSide side) {
  if (panel.empty() || target.empty() || panel == target) return false;
  if (!anywhere(layout, panel) || !anywhere(layout, target)) return false;

  // Dropping a panel in the middle of the group it is already in means "show
  // this one", not "take it out and put it back at the end" — which would
  // reorder the tabs behind the user's back.
  if (side == DockSide::Centre) {
    const DockNode* tree = tree_holding(layout, target);
    if (tree != nullptr) {
      const DockNode* group = group_of(*tree, target);
      if (group != nullptr && std::ranges::find(group->panels, panel) != group->panels.end()) {
        return activate_panel(layout, panel);
      }
    }
  }

  const DockLayout before = layout;

  detach(layout, panel);
  // Before finding the target again: taking the panel out may have emptied a
  // group and collapsed a split, which moves every node around it.
  normalise(layout);

  DockNode* tree = tree_holding(layout, target);
  if (tree == nullptr) {
    layout = before;
    return false;
  }
  DockNode* group = group_of(*tree, target);
  if (group == nullptr) {
    layout = before;
    return false;
  }

  if (side == DockSide::Centre) {
    group->panels.emplace_back(panel);
    group->active = group->panels.size() - 1;
    normalise(layout);
    return layout != before;
  }

  DockNode existing = std::move(*group);
  DockNode incoming = DockNode::tabs({PanelId(panel)});

  *group = leads(side) ? ordered_split(axis_of(side), std::move(incoming), std::move(existing))
                       : ordered_split(axis_of(side), std::move(existing), std::move(incoming));

  normalise(layout);
  return layout != before;
}

bool dock_panel_at_edge(DockLayout& layout, std::string_view panel, DockSide side) {
  // The rim has no middle. A drop there is always a split of the whole window.
  if (side == DockSide::Centre) return false;
  if (panel.empty() || !anywhere(layout, panel)) return false;

  const DockLayout before = layout;

  detach(layout, panel);
  normalise(layout);

  DockNode existing = std::move(layout.root);
  DockNode incoming = DockNode::tabs({PanelId(panel)});

  layout.root = leads(side)
                    ? ordered_split(axis_of(side), std::move(incoming), std::move(existing))
                    : ordered_split(axis_of(side), std::move(existing), std::move(incoming));

  // If the root was already a row and this is a row, the two flatten into one
  // here — which is what puts the panel down the whole side rather than beside
  // one pane of it.
  normalise(layout);
  return layout != before;
}

std::string fresh_window_id(const DockLayout& layout) {
  // Counted up from one rather than from a running total, so the id a layout
  // produces depends only on the layout — closing a window and opening another
  // reuses the name instead of drifting upwards forever.
  for (int i = 1;; ++i) {
    std::string candidate = "w" + std::to_string(i);
    if (std::ranges::find(layout.floating, candidate, &FloatingDock::id) ==
        layout.floating.end()) {
      return candidate;
    }
  }
}

bool float_panel(DockLayout& layout, std::string_view panel, const Rect& bounds, std::string id) {
  if (panel.empty() || !anywhere(layout, panel)) return false;

  // Already out on its own: moved rather than remade, so that dragging a
  // floating window about does not destroy and rebuild the thing being dragged.
  for (FloatingDock& window : layout.floating) {
    if (window.root.is_split() || window.root.panels.size() != 1) continue;
    if (window.root.panels.front() != panel) continue;
    if (window.bounds == bounds) return false;
    window.bounds = bounds;
    return true;
  }

  const DockLayout before = layout;

  detach(layout, panel);
  normalise(layout);
  // Named after the detach, so a window that emptied and was dropped has
  // already released whatever it was called.
  if (id.empty()) id = fresh_window_id(layout);
  layout.floating.push_back(FloatingDock{
      .id = std::move(id), .root = DockNode::tabs({PanelId(panel)}), .bounds = bounds});

  return layout != before;
}

bool activate_panel(DockLayout& layout, std::string_view panel) {
  DockNode* tree = tree_holding(layout, panel);
  if (tree == nullptr) return false;

  DockNode* group = group_of(*tree, panel);
  if (group == nullptr) return false;

  const auto found = std::ranges::find(group->panels, panel);
  const auto index = static_cast<std::size_t>(found - group->panels.begin());
  if (group->active == index) return false;

  group->active = index;
  return true;
}

bool close_panel(DockLayout& layout, std::string_view panel) {
  if (!detach(layout, panel)) return false;
  normalise(layout);
  return true;
}

bool reconcile_panels(DockLayout& layout, std::span<const PanelId> known) {
  const DockLayout before = layout;

  // Unknown ones first, so that opening the missing ones does not put them
  // beside a panel that is about to be taken away.
  for (const PanelId& panel : panels_in(layout)) {
    if (std::ranges::find(known, panel) == known.end()) close_panel(layout, panel);
  }
  for (const PanelId& panel : known) {
    if (!anywhere(layout, panel)) open_panel(layout, panel);
  }

  normalise(layout);
  return layout != before;
}

bool open_panel(DockLayout& layout, PanelId panel, std::string_view beside) {
  if (panel.empty() || anywhere(layout, panel)) return false;

  DockNode* group = nullptr;
  if (!beside.empty()) {
    if (DockNode* tree = tree_holding(layout, beside); tree != nullptr) {
      group = group_of(*tree, beside);
    }
  }
  // Falling back to the main window rather than to whatever floating window was
  // last made: a panel that reappears in a torn-out window the user has since
  // moved off screen is a panel they cannot find.
  if (group == nullptr) group = first_group(layout.root);
  if (group == nullptr) return false;

  group->panels.push_back(std::move(panel));
  group->active = group->panels.size() - 1;
  normalise(layout);
  return true;
}

}  // namespace cutline::ui
