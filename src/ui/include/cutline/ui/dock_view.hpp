#pragma once

/// A dock layout, realised as widgets.
///
/// `dock.hpp` says where the panels are; this puts them there. A `DockNode`
/// tree becomes splitters and tab groups, and dragging a tab reports back what
/// the drop meant so the caller can change the tree and hand it back.
///
/// The rearrangement is never done here. A widget that edited the layout it was
/// drawing would be the one place where what is on screen and what the layout
/// says could disagree, and the disagreement would only show on the next drag.
/// So the view reports and the caller decides — the same shape as the timeline
/// reporting an edit rather than performing one.
///
/// Panel contents are kept alive across every rearrangement. A browser that
/// lost its scroll position and its selection each time a neighbouring panel
/// was dragged would be unusable, and rebuilding content is also the one thing
/// here that could cost real work.

#include "cutline/ui/dock.hpp"
#include "cutline/ui/layout.hpp"
#include "cutline/ui/widget.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cutline::ui {

/// The row of tabs along the top of a dock group.
class TabStrip : public Widget {
 public:
  struct Tab {
    PanelId id;
    std::string title;
    bool closable = true;

    friend bool operator==(const Tab&, const Tab&) = default;
  };

  TabStrip();

  [[nodiscard]] const std::vector<Tab>& tabs() const noexcept { return tabs_; }
  [[nodiscard]] std::size_t active() const noexcept { return active_; }
  void set_tabs(std::vector<Tab> tabs, std::size_t active);

  void set_on_activate(std::function<void(std::size_t)> on_activate) {
    on_activate_ = std::move(on_activate);
  }
  void set_on_close(std::function<void(std::size_t)> on_close) { on_close_ = std::move(on_close); }

  /// Tabs are as wide as their titles, up to a share of the strip. Past that
  /// they are all squeezed equally — the alternative is the last tab of a full
  /// strip being unreachable, which is how a panel gets lost.
  [[nodiscard]] Rect tab_rect(std::size_t index) const;
  /// The little cross at the trailing end of a tab. Empty when it has none, or
  /// when the tab is too narrow to hold one.
  [[nodiscard]] Rect close_rect(std::size_t index) const;
  [[nodiscard]] std::optional<std::size_t> tab_at(double x, double y) const;

  [[nodiscard]] Part part() const noexcept override { return Part::TabBar; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  void layout(const LayoutContext& context) override;
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  /// Shows the tab that was pressed, closes one whose cross was pressed, and
  /// then *declines* a press on a tab so the gesture belongs to the `DockView`
  /// above.
  ///
  /// Declining is the whole point. A press that captures the pointer here would
  /// be lost the moment the layout rebuilt — which showing the pressed tab does
  /// immediately — because the rebuild destroys this strip and the host drops
  /// the capture with it. Dragging a tab that was not already showing then did
  /// nothing at all, which is most tabs in a group of several.
  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  void on_mouse_leave() override;

 private:
  std::vector<Tab> tabs_;
  std::size_t active_ = 0;
  /// Measured during layout, because input arrives with no way to measure text.
  std::vector<double> widths_;
  Metrics metrics_;

  std::size_t hovered_tab_ = 0;
  bool has_hovered_tab_ = false;

  std::function<void(std::size_t)> on_activate_;
  std::function<void(std::size_t)> on_close_;
};

/// One tab group: the strip, and whichever panel is showing under it.
class DockGroup : public Widget {
 public:
  DockGroup();

  [[nodiscard]] const std::vector<PanelId>& panels() const noexcept { return panels_; }
  [[nodiscard]] TabStrip& strip() noexcept { return *strip_; }
  [[nodiscard]] const TabStrip& strip() const noexcept { return *strip_; }

  /// The panel showing, or empty when the group has none.
  [[nodiscard]] PanelId active_panel() const;
  /// Everything below the tabs: where a panel's content goes, and the area a
  /// drop is measured against.
  [[nodiscard]] Rect content_area() const;

  void layout(const LayoutContext& context) override;

 private:
  friend class DockView;

  std::vector<PanelId> panels_;
  TabStrip* strip_ = nullptr;
  /// The content widget, owned as a child. Null when the group is empty.
  Widget* content_ = nullptr;
  /// Which panel the content belongs to. Held rather than derived from the
  /// active tab, so a rebuild puts it back under the right name even if the
  /// active tab has moved on since.
  PanelId content_id_;
};

/// Where a drop would put a panel.
struct DropTarget {
  /// The panel whose group is being dropped onto. Empty when `at_edge`.
  PanelId onto;
  DockSide side = DockSide::Centre;
  /// Dropped on the rim of the whole view rather than onto any one group.
  bool at_edge = false;

  friend bool operator==(const DropTarget&, const DropTarget&) = default;
};

/// A whole dock tree, drawn and dragged.
class DockView : public Widget {
 public:
  /// Makes the content for a panel. Called once per panel, ever: the result is
  /// kept and moved around as the layout changes.
  using ContentFactory = std::function<std::unique_ptr<Widget>(const PanelId&)>;
  /// What a panel is called on its tab.
  using TitleLookup = std::function<std::string(const PanelId&)>;

  DockView();

  void set_content_factory(ContentFactory factory) { factory_ = std::move(factory); }
  void set_titles(TitleLookup titles) { titles_ = std::move(titles); }

  [[nodiscard]] const DockNode& node() const noexcept { return node_; }

  /// Rebuilds the widget tree for a layout, keeping every panel's content
  /// alive across the change.
  void set_node(DockNode node);

  /// The groups in the built tree, in the order they were built. For hit
  /// testing a drop, and for asserting what came out.
  [[nodiscard]] std::vector<DockGroup*> groups() const;
  [[nodiscard]] DockGroup* group_showing(std::string_view panel) const;

  /// The fractions of the built splitters, so a divider the user dragged can
  /// be read back into the layout rather than being lost on the next rebuild.
  void read_fractions_into(DockNode& node) const;

  // ------------------------------------------------------------------ drops --

  /// What a drop at this point would mean, or nothing when it would mean
  /// leaving the view entirely — which is how a panel is torn out.
  [[nodiscard]] std::optional<DropTarget> drop_target(double x, double y) const;

  /// The panel being dragged and where the pointer is, so the drop zone can be
  /// drawn. Set by the view itself while a tab is dragged; exposed so a drag
  /// arriving from another window can be shown the same way.
  ///
  /// This is the *highlight*, not the gesture — see `carrying`. A window told
  /// there is nothing to show clears its zone and nothing else, which is what
  /// lets one drag be drawn in whichever window the pointer is over without any
  /// of them forgetting whose drag it is.
  void set_drag(std::optional<PanelId> panel, double x, double y);
  [[nodiscard]] const std::optional<PanelId>& dragging() const noexcept { return drag_; }

  /// The panel this view is dragging, if the gesture started here.
  ///
  /// Separate from `dragging` because they answer different questions and the
  /// answers differ exactly when it matters: a tab dragged clean out of every
  /// window has no drop zone to draw anywhere, and if that cleared the gesture
  /// too the release would do nothing — which is why a panel could not be torn
  /// out into a window of its own.
  [[nodiscard]] const std::optional<PanelId>& carrying() const noexcept { return carrying_; }

  /// Called when a tab is released over the view. The caller changes the
  /// layout and hands the new tree back through `set_node`.
  void set_on_dock(std::function<void(PanelId, DropTarget)> on_dock) {
    on_dock_ = std::move(on_dock);
  }
  /// Called when a tab is released outside the view: it wants to be a window.
  void set_on_tear_out(std::function<void(PanelId, double, double)> on_tear_out) {
    on_tear_out_ = std::move(on_tear_out);
  }
  /// Called on every move of a tab drag that started in this view.
  ///
  /// What lets a drag be shown in a *different* window from the one it began
  /// in: this view cannot know what else is on screen, so it reports and the
  /// application forwards.
  void set_on_drag(std::function<void(const PanelId&, double, double)> on_drag) {
    on_drag_ = std::move(on_drag);
  }

  void set_on_close(std::function<void(PanelId)> on_close) { on_close_ = std::move(on_close); }
  void set_on_activate(std::function<void(PanelId)> on_activate) {
    on_activate_ = std::move(on_activate);
  }
  /// Called when a divider is let go, with the fractions read back out of the
  /// built splitters. Without it a resize is undone by the next rearrangement.
  void set_on_resize(std::function<void()> on_resize) { on_resize_ = std::move(on_resize); }

  void layout(const LayoutContext& context) override;
  void paint_overlay(Painter& painter, const Theme& theme) const override;

  /// Takes the press on a tab, and the pointer with it.
  ///
  /// Here rather than in the strip that was pressed because this outlives the
  /// rebuild that showing a tab causes, and a drop rebuilds the tree as well.
  /// The strip decides *which* tab; the gesture belongs here.
  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

 private:
  /// Builds the widgets for one node, taking content out of `spare_`.
  [[nodiscard]] std::unique_ptr<Widget> build(const DockNode& node);
  /// Pulls every panel's content back out of the tree so a rebuild can put it
  /// back rather than making it again.
  void reclaim(Widget& widget);
  void wire(DockGroup& group);

  [[nodiscard]] std::string title_of(const PanelId& panel) const;
  /// The panel whose tab is under a point, or nothing.
  [[nodiscard]] std::optional<PanelId> tab_at(double x, double y) const;

 public:
  /// The panel under a point, counting the whole group rather than its tab —
  /// which is what "the panel the pointer is over" means, and what maximising
  /// with `~` acts on. The panel *showing* in that group, since the others are
  /// behind it.
  [[nodiscard]] std::optional<PanelId> panel_at(double x, double y) const;

 private:
  /// The rectangle a drop would fill, for the highlight.
  [[nodiscard]] Rect drop_rect(const DropTarget& target) const;

  DockNode node_;
  ContentFactory factory_;
  TitleLookup titles_;

  /// The groups in the built tree, in build order. Collected as they are made
  /// rather than searched for afterwards, which keeps hit testing a drop from
  /// walking the whole tree on every mouse move of a drag.
  std::vector<DockGroup*> groups_;

  /// Panel content that is not in the tree: closed tabs, and everything during
  /// a rebuild. Owns what it holds.
  std::map<PanelId, std::unique_ptr<Widget>, std::less<>> spare_;

  /// What a drop would be shown as, which may be somebody else's drag.
  std::optional<PanelId> drag_;
  double drag_x_ = 0.0;
  double drag_y_ = 0.0;

  /// The press, before it has travelled far enough to be a drag, and the drag
  /// once it has. Both survive a rebuild of the tree because this widget does.
  std::optional<PanelId> pressed_;
  double press_x_ = 0.0;
  double press_y_ = 0.0;
  std::optional<PanelId> carrying_;

  Metrics metrics_;

  std::function<void(const PanelId&, double, double)> on_drag_;
  std::function<void(PanelId, DropTarget)> on_dock_;
  std::function<void(PanelId, double, double)> on_tear_out_;
  std::function<void(PanelId)> on_close_;
  std::function<void(PanelId)> on_activate_;
  std::function<void()> on_resize_;
};

/// How far into a group's edge counts as a split rather than a tab, as a
/// fraction of the group.
///
/// A fifth leaves the middle three fifths each way — well over a third of the
/// group — meaning "join this one". Joining is the common intent and the thing
/// people aim at, and a scheme where every drop near the centre is a coin toss
/// between four different splits makes docking feel unpredictable.
inline constexpr double kDockEdgeFraction = 0.2;

/// How far into the rim of the whole view counts as docking against the window
/// itself, in pixels.
inline constexpr double kDockRimWidth = 24.0;

/// How far a press on a tab has to move before it is a drag.
inline constexpr double kTabDragThreshold = 4.0;

}  // namespace cutline::ui
