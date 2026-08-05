#pragma once

/// Where the panels are: the arrangement a user drags into shape.
///
/// A layout is a tree. Its leaves are tab groups — several panels stacked in
/// one place with one showing — and its branches are splits, a row or column of
/// children with draggable dividers between them. Everything an editor's
/// docking does is a rearrangement of that tree: dropping a panel on another
/// one's edge splits it, dropping it in the middle adds a tab, and dragging it
/// out of the window entirely gives it a tree of its own.
///
/// This is pure data and pure operations on it, deliberately. Docking is the
/// part of an interface most likely to go subtly wrong — a panel that survives
/// being dragged onto itself, a split left holding one child, a fraction that
/// no longer sums to one after three moves — and none of that needs a window to
/// find. Realising a tree as widgets belongs above; nothing here knows what a
/// panel contains or how a tab is drawn.
///
/// The one rule the whole file exists to keep: the tree stays *canonical*.
/// After any operation there are no empty groups, no splits with a single
/// child, and no split nested directly inside a split along the same axis.
/// Without that, repeated docking silently builds a tower of one-child splits
/// that behaves correctly and lays out worse every time.

#include "cutline/ui/layout.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::ui {

/// A panel's identity. Opaque here: what it contains is the application's
/// business, and the layout only ever moves the name around.
using PanelId = std::string;

enum class DockKind {
  /// Panels stacked in one place, one of them showing.
  Tabs,
  /// A row or column of children.
  Split,
};

/// One node of a layout.
///
/// Children are held by value rather than by pointer, so a layout copies,
/// compares and is asserted against like any other struct — which is most of
/// what makes the operations below testable.
struct DockNode {
  DockKind kind = DockKind::Tabs;

  /// `Tabs` only. Order is the order the tabs appear in.
  std::vector<PanelId> panels;
  /// `Tabs` only: which panel is showing. Always in range once normalised.
  std::size_t active = 0;

  /// `Split` only.
  Axis axis = Axis::Horizontal;
  std::vector<DockNode> children;
  /// `Split` only: one per child, summing to one.
  std::vector<double> fractions;

  [[nodiscard]] bool is_split() const noexcept { return kind == DockKind::Split; }
  /// A tab group with nothing in it, or a split with no children.
  [[nodiscard]] bool empty() const noexcept;

  /// The panel showing, or nothing when this is not a tab group or is empty.
  [[nodiscard]] std::optional<PanelId> active_panel() const;

  friend bool operator==(const DockNode&, const DockNode&) = default;

  /// A tab group holding these panels, the first one showing.
  [[nodiscard]] static DockNode tabs(std::vector<PanelId> panels);
  /// A split of these children, evenly divided.
  [[nodiscard]] static DockNode split(Axis axis, std::vector<DockNode> children);
};

/// A tree that has been dragged out into a window of its own.
struct FloatingDock {
  /// Stable for as long as the window exists, and not derived from what is in
  /// it — the panels come and go. What lets the platform layer match a real
  /// window to its entry here after a rearrangement, and what a saved
  /// workspace names its windows by.
  std::string id;

  DockNode root;
  /// Where the window is, in whatever coordinates the platform layer uses.
  /// Nothing here interprets them.
  Rect bounds;

  friend bool operator==(const FloatingDock&, const FloatingDock&) = default;
};

/// The whole arrangement: the main window's tree, and any torn-out ones.
struct DockLayout {
  DockNode root;
  std::vector<FloatingDock> floating;

  /// The panel filling the main window on its own, or empty when the
  /// arrangement is showing as laid out.
  ///
  /// Premiere's `~`. Held *beside* the tree rather than by rewriting it, which
  /// is the whole of why coming back is exact: nothing is moved, so nothing has
  /// to be moved back, and a workspace saved while maximised still describes
  /// the arrangement somebody built.
  PanelId maximised;

  friend bool operator==(const DockLayout&, const DockLayout&) = default;
};

/// Which edge of a target a panel was dropped on.
enum class DockSide {
  Left,
  Right,
  Top,
  Bottom,
  /// The middle: join the target's tab group rather than splitting it.
  Centre,
};

[[nodiscard]] std::string_view to_string(DockSide side) noexcept;

// ------------------------------------------------------------------ reading --

/// Every panel in a tree, in the order they appear.
[[nodiscard]] std::vector<PanelId> panels_in(const DockNode& node);
/// Every panel anywhere in the layout, main window and floating alike.
[[nodiscard]] std::vector<PanelId> panels_in(const DockLayout& layout);

[[nodiscard]] bool contains_panel(const DockNode& node, std::string_view panel);

/// The tab group holding a panel, or null.
[[nodiscard]] const DockNode* group_of(const DockNode& node, std::string_view panel);
[[nodiscard]] DockNode* group_of(DockNode& node, std::string_view panel);

/// Whether a panel is in a torn-out window rather than the main one.
[[nodiscard]] bool is_floating(const DockLayout& layout, std::string_view panel);

// ------------------------------------------------------------ canonical form --

/// Puts a tree back into canonical form: no empty groups, no split holding a
/// single child, no split directly inside a split along the same axis, one
/// fraction per child summing to one, and `active` in range.
///
/// Every operation below ends with this. Calling it twice changes nothing,
/// which is the property worth relying on — it can be applied to a tree of
/// unknown provenance, such as one that has just been read from a file.
void normalise(DockNode& node);
void normalise(DockLayout& layout);

// --------------------------------------------------------------- operations --
//
// Each reports whether it changed anything, so a caller can skip rebuilding the
// interface for a drag that came to nothing.

/// Moves `panel` next to `target`, from wherever it currently is.
///
/// `Centre` adds it to the target's tab group; any other side splits that
/// group in two, with `panel` on the side named. Dropping a panel onto itself,
/// or onto a target that is not there, does nothing.
bool dock_panel(DockLayout& layout, std::string_view panel, std::string_view target,
                DockSide side);

/// Moves `panel` to an outer edge of the main window, spanning the whole of it.
///
/// This is the drop zone around the rim: dropping there means "down the left of
/// everything", not "beside whichever panel happens to be nearest".
bool dock_panel_at_edge(DockLayout& layout, std::string_view panel, DockSide side);

/// A window id no floating window is using. Deterministic, so the same layout
/// always produces the same next id and a test can name it.
[[nodiscard]] std::string fresh_window_id(const DockLayout& layout);

/// Takes `panel` out into a window of its own at `bounds`.
///
/// A panel that is already alone in a floating window is only moved, so that
/// dragging one around does not keep destroying and remaking it.
///
/// The new window is given `id`, or a fresh one when that is empty. Passing it
/// explicitly is for restoring a saved workspace, where the windows already
/// have names.
bool float_panel(DockLayout& layout, std::string_view panel, const Rect& bounds,
                 std::string id = {});

/// Shows `panel` in its tab group. Does nothing if it is already showing.
bool activate_panel(DockLayout& layout, std::string_view panel);

/// Removes `panel` from the layout entirely.
bool close_panel(DockLayout& layout, std::string_view panel);

/// Fills the main window with `panel` alone, or puts the arrangement back when
/// that panel is already the one filling it. Reports whether anything changed.
///
/// A panel in a window of its own cannot be maximised: it already is one, and
/// the main window is not where it lives. Naming one is refused rather than
/// quietly maximising something else.
bool toggle_maximised(DockLayout& layout, std::string_view panel);

/// Puts the arrangement back, whatever was filling the window. What a
/// rearrangement calls, since a panel dragged somewhere while another one was
/// maximised would otherwise be dropped into a layout nobody can see.
bool restore_maximised(DockLayout& layout);

/// Makes a layout show exactly the panels the application has.
///
/// Panels it does not recognise are removed, and panels it has that the layout
/// never heard of are opened in the main window. Reports whether anything
/// changed.
///
/// This is what lets a saved arrangement survive the application changing under
/// it. A layout written by a build with a panel this one has dropped would
/// otherwise leave a tab that cannot be filled, and a panel added since would
/// be unreachable with no menu to open it from.
bool reconcile_panels(DockLayout& layout, std::span<const PanelId> known);

/// Adds `panel` to the layout if it is not already there, joining the tab group
/// holding `beside` or the first group in the main window.
///
/// What reopening a closed panel does, and what happens to a panel the
/// application knows about that a saved layout has never heard of.
bool open_panel(DockLayout& layout, PanelId panel, std::string_view beside = {});

}  // namespace cutline::ui
