/// The dock and the workspaces: dragging panels between windows and groups,
/// tearing them out and bringing them home, each panel's menu, which panels
/// are open, and the arrangements they are kept in.

#include "app.hpp"

namespace cutline::tool {

/// Where a drop at a point on the screen would land: which window, and where in
/// its dock.
///
/// Floating windows are asked before the main one, because they are above it.
/// Nothing means the point is over no dock at all — the desktop, another
/// application, or a window's own caption — which is what makes a new window.
[[nodiscard]] std::optional<std::pair<Shell*, cutline::ui::DropTarget>> drop_across(
    App& app, POINT screen) {
  std::vector<Shell*> order;
  for (const std::unique_ptr<Shell>& shell : app.floats) order.push_back(shell.get());
  order.push_back(&app.main);

  for (Shell* shell : order) {
    if (shell->window == nullptr || shell->dock == nullptr) continue;

    POINT local = screen;
    ScreenToClient(shell->window, &local);

    RECT client{};
    GetClientRect(shell->window, &client);
    if (local.x < 0 || local.y < 0 || local.x >= client.right || local.y >= client.bottom) {
      continue;
    }

    // Over this window, so it answers — whether or not the answer is a drop.
    // A point over a caption must not fall through to the window behind it.
    const auto [ux, uy] = to_units(*shell, local);
    const auto target = shell->dock->drop_target(ux, uy);
    if (target.has_value()) return std::pair{shell, *target};
    return std::nullopt;
  }
  return std::nullopt;
}

/// Shows a drag in whichever window the pointer is over, and nowhere else.
void show_drag_across(App& app, const PanelId& panel, POINT screen) {
  const auto landed = drop_across(app, screen);

  for (Shell* shell : app.shells()) {
    if (shell->dock == nullptr) continue;

    if (landed.has_value() && landed->first == shell && !panel.empty()) {
      POINT local = screen;
      ScreenToClient(shell->window, &local);
      const auto [ux, uy] = to_units(*shell, local);
      shell->dock->set_drag(panel, ux, uy);
    } else {
      shell->dock->set_drag(std::nullopt, 0.0, 0.0);
    }
    shell->dirty = true;
    if (shell->window != nullptr) InvalidateRect(shell->window, nullptr, FALSE);
  }
}

/// Docks every panel of a torn-out window back into the main one.
///
/// What closing a floating window does. Throwing the panels away would be the
/// one action in the application with no way back, because there is no menu to
/// reopen a panel from yet.
void return_panels_home(App& app, const std::string& window_id) {
  const auto found =
      std::ranges::find(layout_of(app).floating, window_id, &cutline::ui::FloatingDock::id);
  if (found == layout_of(app).floating.end()) return;

  for (const PanelId& panel : cutline::ui::panels_in(found->root)) {
    const std::vector<PanelId> home = cutline::ui::panels_in(layout_of(app).root);
    if (home.empty()) {
      // Nothing to dock against: it becomes the main window's whole layout.
      cutline::ui::close_panel(layout_of(app), panel);
      cutline::ui::open_panel(layout_of(app), panel);
      continue;
    }
    cutline::ui::dock_panel(layout_of(app), panel, home.front(), cutline::ui::DockSide::Centre);
  }
  app.dock_stale = true;
}

/// Tears out whichever panel the pointer is over, without a drag.
///
/// The gesture is the drag; this is the same thing on a key. Worth having in
/// its own right — a drag across two windows is not a reachable action for
/// everyone — and it is also the only way any of this can be exercised without
/// a hand on a mouse.
void float_panel_under_cursor(App& app, Shell& shell) {
  if (shell.dock == nullptr || shell.window == nullptr) return;

  POINT screen{};
  GetCursorPos(&screen);
  POINT local = screen;
  ScreenToClient(shell.window, &local);

  const auto [ux, uy] = to_units(shell, local);
  for (const cutline::ui::DockGroup* group : shell.dock->groups()) {
    if (!group->bounds().contains(ux, uy)) continue;
    const PanelId panel = group->active_panel();
    if (panel.empty()) return;

    // On the screen, so in its pixels: a floating window is placed by Windows.
    const double s = shell.scale;
    const cutline::ui::Rect where{static_cast<double>(screen.x) - 130.0 * s,
                                  static_cast<double>(screen.y) - 12.0 * s, 520.0 * s, 380.0 * s};
    if (cutline::ui::float_panel(layout_of(app), panel, where)) app.dock_stale = true;
    return;
  }
}

/// The tab group holding a panel, in whichever window it is.
[[nodiscard]] const DockNode* group_holding(const DockLayout& layout, const PanelId& panel) {
  if (const DockNode* found = cutline::ui::group_of(layout.root, panel); found != nullptr) {
    return found;
  }
  for (const FloatingDock& window : layout.floating) {
    if (const DockNode* found = cutline::ui::group_of(window.root, panel); found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

/// Where a panel's group is on the screen, in pixels, for a window undocked
/// from it to open in the same place rather than wherever the pointer is.
[[nodiscard]] Rect group_on_screen(const Shell& shell, const PanelId& panel) {
  const double s = shell.scale;
  Rect where{0.0, 0.0, 520.0 * s, 380.0 * s};
  if (shell.dock == nullptr || shell.window == nullptr) return where;
  const cutline::ui::DockGroup* group = shell.dock->group_showing(panel);
  if (group == nullptr) return where;

  POINT corner{to_pixels(group->bounds().x, s), to_pixels(group->bounds().y, s)};
  ClientToScreen(shell.window, &corner);
  // No smaller than a torn-out panel, so undocking a sliver of a group does not
  // give a window too small to use.
  return Rect{static_cast<double>(corner.x), static_cast<double>(corner.y),
              std::max(group->bounds().width * s, 320.0 * s),
              std::max(group->bounds().height * s, 240.0 * s)};
}

/// What a panel's own menu adds after the entries every panel shares.
///
/// Premiere's panel menus are where a panel keeps its view options — how a
/// list is shown, what a monitor overlays — and the commands that act on what
/// the panel holds. Here they are the same commands the panel's toolbar and
/// right-click already run, so the menu is one more way to them rather than a
/// second implementation.
[[nodiscard]] std::vector<MenuEntry> panel_menu_extras(App& app, const PanelId& panel);

/// A panel's menu: the ≡ beside its tab's title, or a right-click on the tab.
///
/// The entries every panel shares come first and in Premiere's order, then the
/// panel's own. Opened on the window the panel is in, since a torn-out panel's
/// menu belongs over that window rather than over the editor behind it.
void open_panel_menu(App& app, Shell& shell, const PanelId& panel, const Rect& anchor) {
  if (shell.host == nullptr) return;

  DockLayout& layout = layout_of(app);
  const DockNode* group = group_holding(layout, panel);
  const std::vector<PanelId> siblings = group != nullptr ? group->panels : std::vector<PanelId>{};
  const bool floating = cutline::ui::is_floating(layout, panel);
  // Alone in a window already, there is nothing to undock it from.
  const bool alone_in_window = floating && siblings.size() == 1;
  const auto undock_to = group_on_screen(shell, panel);
  // A window holding this group and nothing else is already what undocking the
  // group would make.
  bool group_is_window = false;
  for (const FloatingDock& window : layout.floating) {
    if (cutline::ui::contains_panel(window.root, panel)) group_is_window = !window.root.is_split();
  }

  std::vector<MenuEntry> entries;
  entries.push_back(MenuEntry{.label = "Close Panel", .act = [&app, panel] {
    if (cutline::ui::close_panel(layout_of(app), panel)) app.dock_stale = true;
  }});
  entries.push_back(MenuEntry{.label = "Undock Panel", .enabled = !alone_in_window,
                              .act = [&app, panel, undock_to] {
    // A panel undocked while another fills the window would leave that window
    // showing what was maximised and nothing to come back to.
    if (cutline::ui::restore_maximised(layout_of(app))) app.dock_stale = true;
    if (cutline::ui::float_panel(layout_of(app), panel, undock_to)) app.dock_stale = true;
  }});
  entries.push_back(MenuEntry{.label = "Close Other Panels in Group",
                              .enabled = siblings.size() > 1,
                              .act = [&app, panel, siblings] {
    for (const PanelId& other : siblings) {
      if (other != panel && cutline::ui::close_panel(layout_of(app), other)) app.dock_stale = true;
    }
  }});

  std::vector<MenuEntry> group_settings;
  group_settings.push_back(MenuEntry{.label = "Close Panel Group", .act = [&app, siblings] {
    for (const PanelId& each : siblings) {
      if (cutline::ui::close_panel(layout_of(app), each)) app.dock_stale = true;
    }
  }});
  group_settings.push_back(MenuEntry{.label = "Undock Panel Group",
                                     .enabled = !group_is_window,
                                     .act = [&app, panel, siblings, undock_to] {
    DockLayout& now = layout_of(app);
    if (cutline::ui::restore_maximised(now)) app.dock_stale = true;
    // The panel the menu came from goes first and the rest join it, so the
    // group arrives in its window as it was, showing what it showed.
    if (!cutline::ui::float_panel(now, panel, undock_to)) return;
    for (const PanelId& other : siblings) {
      if (other != panel) {
        (void)cutline::ui::dock_panel(now, other, panel, cutline::ui::DockSide::Centre);
      }
    }
    (void)cutline::ui::activate_panel(now, panel);
    app.dock_stale = true;
  }});
  group_settings.push_back(MenuEntry{
      .label = "Maximise Panel Group",
      .shortcut = "`",
      .enabled = !floating,
      .checked = layout.maximised == panel,
      .act = [&app, panel] {
        if (cutline::ui::toggle_maximised(layout_of(app), panel)) app.dock_stale = true;
      }});
  entries.push_back(MenuEntry{.label = "Panel Group Settings", .submenu = std::move(group_settings)});

  if (std::vector<MenuEntry> extras = panel_menu_extras(app, panel); !extras.empty()) {
    entries.push_back(MenuEntry::divider());
    for (MenuEntry& extra : extras) entries.push_back(std::move(extra));
  }

  auto list = std::make_unique<MenuList>(std::move(entries));
  if (shell.host->keyboard_cues()) list->set_highlighted(0);
  shell.host->open_popup(std::move(list), anchor);
}

std::vector<MenuEntry> panel_menu_extras(App& app, const PanelId& panel) {
  using cutline::editor::Command;
  const auto plain = [](std::string label, std::function<void()> act, std::string keys = {}) {
    return action_entry(std::move(label), std::move(act), std::move(keys));
  };

  std::vector<MenuEntry> rows;
  if (panel == "project") {
    rows.push_back(plain("New Bin", [&app] { add_bin(app); }));
    rows.push_back(plain("New Title", [&app] { add_title(app); }));
    rows.push_back(plain("New Colour Matte", [&app] { add_matte(app); }));
    rows.push_back(plain("New Adjustment Layer", [&app] { add_adjustment(app); }));
    rows.push_back(MenuEntry::divider());
    rows.push_back(plain("Import...", [&app] { import_media(app); },
                         shortcut_of(&app, "file.import")));
    rows.push_back(plain("Remove from Project", [&app] { remove_from_pool(app); }));
    rows.push_back(plain("Link Media...", [&app] { relink_pool_entry(app); }));
    rows.push_back(MenuEntry::divider());
    // Premiere's List and Icon, as a pair of switches with one of them on.
    const bool icons =
        app.browser != nullptr && app.browser->view() == cutline::ui::BrowserView::Icons;
    rows.push_back(switch_entry("List", !icons, [&app] {
      set_pool_view(app, cutline::ui::BrowserView::List);
    }));
    rows.push_back(switch_entry("Icon", icons, [&app] {
      set_pool_view(app, cutline::ui::BrowserView::Icons);
    }));
  } else if (panel == "monitor") {
    rows.push_back(command_entry(app, "Mark In", Command::MarkIn));
    rows.push_back(command_entry(app, "Mark Out", Command::MarkOut));
    rows.push_back(command_entry(app, "Clear In and Out", Command::ClearMarks));
    rows.push_back(MenuEntry::divider());
    rows.push_back(switch_entry("Loop", app.looping, [&app] { toggle_looping(app); }));
    std::vector<MenuEntry> resolutions;
    for (const auto& [scale, name] : kPreviewScales) {
      resolutions.push_back(switch_entry(std::string(name), scale == app.preview_scale,
                                         [&app, scale] { choose_preview_scale(app, scale); }));
    }
    rows.push_back(MenuEntry{.label = "Playback Resolution", .submenu = std::move(resolutions)});
    rows.push_back(switch_entry("Safe Margins", app.settings.safe_margins_program, [&app] {
      app.settings.safe_margins_program = !app.settings.safe_margins_program;
      save_settings(app);
      show_safe_margins(app);
    }));
    // Premiere's Display Mode, with Comparison View among them as it is there.
    rows.push_back(MenuEntry{
        .label = "Display Mode",
        .submenu = {switch_entry("Composite Video",
                                 !app.program_alpha && !app.lumetri_comparing &&
                                     !app.multicam_view,
                                 [&app] {
                                   app.program_alpha = false;
                                   if (app.lumetri_comparing) toggle_comparison(app);
                                   if (app.multicam_view) toggle_multicam_view(app);
                                   show_display_options(app);
                                 }),
                    // Premiere's Multi-Camera: every camera beside the picture.
                    switch_entry("Multi-Camera", app.multicam_view,
                                 [&app] { toggle_multicam_view(app); }),
                    switch_entry("Alpha", app.program_alpha, [&app] {
                      app.program_alpha = true;
                      show_display_options(app);
                    }),
                    switch_entry("Comparison View", app.lumetri_comparing,
                                 [&app] { toggle_comparison(app); })}});
    rows.push_back(switch_entry("Transparency Grid", app.settings.transparency_grid_program,
                                [&app] {
                                  app.settings.transparency_grid_program =
                                      !app.settings.transparency_grid_program;
                                  save_settings(app);
                                  show_display_options(app);
                                }));
    rows.push_back(switch_entry("Multi-Camera Audio Follows Video",
                                app.settings.multicam_audio_follows, [&app] {
                                  app.settings.multicam_audio_follows =
                                      !app.settings.multicam_audio_follows;
                                  save_settings(app);
                                }));
    rows.push_back(MenuEntry::divider());
    rows.push_back(plain("Export Frame", [&app] { take_snapshot(app); }));
  } else if (panel == "source") {
    const bool has_source = source_media_of(app) != nullptr;
    MenuEntry in = plain("Mark In", [&app] { mark_source(app, false); },
                         shortcut_of(&app, "markers.mark_in"));
    MenuEntry out = plain("Mark Out", [&app] { mark_source(app, true); },
                          shortcut_of(&app, "markers.mark_out"));
    MenuEntry clear = plain("Clear In and Out", [&app] {
      if (const cutline::core::Media* media = source_media_of(app); media != nullptr) {
        app.session.apply(cutline::core::clear_source_marks(app.session.project(), media->id));
        refresh_all(app);
      }
    });
    // Premiere's clip markers, on the source itself.
    MenuEntry marker = plain("Add Marker", [&app] { mark_source_marker(app); },
                             shortcut_of(&app, "markers.add"));
    MenuEntry clear_markers = plain("Clear Markers", [&app] {
      if (const cutline::core::Media* media = source_media_of(app); media != nullptr) {
        app.session.apply(cutline::core::clear_source_markers(app.session.project(), media->id));
        refresh_all(app);
      }
    }, shortcut_of(&app, "markers.clear_all"));
    for (MenuEntry* each : {&in, &out, &clear, &marker, &clear_markers}) {
      each->enabled = has_source;
    }
    rows.push_back(std::move(in));
    rows.push_back(std::move(out));
    rows.push_back(std::move(clear));
    rows.push_back(std::move(marker));
    rows.push_back(std::move(clear_markers));
    rows.push_back(MenuEntry::divider());
    rows.push_back(switch_entry("Safe Margins", app.settings.safe_margins_source, [&app] {
      app.settings.safe_margins_source = !app.settings.safe_margins_source;
      save_settings(app);
      show_safe_margins(app);
    }));
    rows.push_back(MenuEntry{
        .label = "Display Mode",
        .submenu = {switch_entry("Composite Video", !app.source_alpha,
                                 [&app] {
                                   app.source_alpha = false;
                                   show_display_options(app);
                                 }),
                    switch_entry("Alpha", app.source_alpha, [&app] {
                      app.source_alpha = true;
                      show_display_options(app);
                    })}});
    rows.push_back(switch_entry("Transparency Grid", app.settings.transparency_grid_source,
                                [&app] {
                                  app.settings.transparency_grid_source =
                                      !app.settings.transparency_grid_source;
                                  save_settings(app);
                                  show_display_options(app);
                                }));
  } else if (panel == "timeline") {
    rows.push_back(command_entry(app, "Add Video Track", Command::AddVideoTrack));
    rows.push_back(command_entry(app, "Add Audio Track", Command::AddAudioTrack));
    rows.push_back(command_entry(app, "Remove Track", Command::RemoveTrack));
    rows.push_back(MenuEntry::divider());
    rows.push_back(switch_entry("Snap in Timeline", app.snapping, [&app] { toggle_snapping(app); },
                                "S"));
    rows.push_back(plain("Zoom to Sequence", [&app] {
      if (app.timeline == nullptr) return;
      app.timeline->zoom_to_fit();
      mark_dirty(app);
    }, "\\"));
  } else if (panel == "scopes") {
    for (const cutline::ui::ScopeKind kind :
         {cutline::ui::ScopeKind::Histogram, cutline::ui::ScopeKind::Waveform,
          cutline::ui::ScopeKind::Parade, cutline::ui::ScopeKind::Vectorscope}) {
      rows.push_back(switch_entry(std::string(cutline::ui::to_string(kind)), app.scope_kind == kind,
                                  [&app, kind] { choose_scope(app, kind); }));
    }
  } else if (panel == "library") {
    rows.push_back(plain("New Custom Bin", [&app] { new_bin(app); }));
  } else if (panel == "history") {
    rows.push_back(command_entry(app, "Step Backward", Command::Undo));
    rows.push_back(command_entry(app, "Step Forward", Command::Redo));
    rows.push_back(MenuEntry::divider());
    rows.push_back(action_entry("Clear History", [&app] {
      app.session.clear_history();
      refresh_title(app);
      mark_dirty(app);
    }, {}, app.session.can_undo() || app.session.can_redo()));
  }
  return rows;
}

/// Everything a dock reports, wired to the layout.
///
/// The same for the main window and for a torn-out one: which window a panel is
/// in is a fact about the layout, so the handlers are identical and only the
/// tree they were reported from differs.
void wire_dock(App* app, Shell* shell, DockView& dock) {
  dock.set_titles(panel_title);
  dock.set_content_factory(
      [app](const PanelId& id) -> std::unique_ptr<Widget> { return make_panel(app, id); });

  // Every one of these marks rather than rebuilds. What asked is a tab strip
  // inside the tree the rebuild replaces.
  dock.set_on_activate([app](PanelId panel) {
    if (app == nullptr) return;
    if (cutline::ui::activate_panel(layout_of(*app), panel)) app->dock_stale = true;
  });
  dock.set_on_close([app](PanelId panel) {
    if (app == nullptr) return;
    if (cutline::ui::close_panel(layout_of(*app), panel)) app->dock_stale = true;
  });
  dock.set_on_menu([app, shell](PanelId panel, Rect anchor) {
    if (app == nullptr || shell == nullptr) return;
    open_panel_menu(*app, *shell, panel, anchor);
  });
  dock.set_on_dock([app](PanelId panel, cutline::ui::DropTarget target) {
    if (app == nullptr) return;
    // Whatever was filling the window stops: a panel dropped into an
    // arrangement nobody can see is a move nobody can check.
    if (cutline::ui::restore_maximised(layout_of(*app))) app->dock_stale = true;
    const bool moved =
        target.at_edge ? cutline::ui::dock_panel_at_edge(layout_of(*app), panel, target.side)
                       : cutline::ui::dock_panel(layout_of(*app), panel, target.onto, target.side);
    if (moved) app->dock_stale = true;
  });

  dock.set_on_drag([app, shell](const PanelId& panel, double x, double y) {
    if (app == nullptr || shell == nullptr || shell->window == nullptr) return;
    POINT screen{to_pixels(x, shell->scale), to_pixels(y, shell->scale)};
    ClientToScreen(shell->window, &screen);
    show_drag_across(*app, panel, screen);
  });

  // Dropped somewhere this window's own dock did not want it. It may still be
  // over another window — a panel dragged out of a torn-out window and back
  // into the editor comes through here — and only if it is over nothing at all
  // does it become a window of its own.
  dock.set_on_tear_out([app, shell](PanelId panel, double x, double y) {
    if (app == nullptr || shell == nullptr || shell->window == nullptr) return;
    // The same reason a drop restores: a panel torn out of a window showing one
    // panel would leave that window showing nothing.
    if (cutline::ui::restore_maximised(layout_of(*app))) app->dock_stale = true;

    POINT screen{to_pixels(x, shell->scale), to_pixels(y, shell->scale)};
    ClientToScreen(shell->window, &screen);
    show_drag_across(*app, PanelId{}, screen);

    if (const auto landed = drop_across(*app, screen); landed.has_value()) {
      const cutline::ui::DropTarget& target = landed->second;
      const bool moved =
          target.at_edge ? cutline::ui::dock_panel_at_edge(layout_of(*app), panel, target.side)
                         : cutline::ui::dock_panel(layout_of(*app), panel, target.onto, target.side);
      if (moved) app->dock_stale = true;
      return;
    }

    // Placed so the caption lands near the cursor rather than the far corner,
    // because the tab being dragged was already under it.
    constexpr double kTornWidth = 520.0;
    constexpr double kTornHeight = 380.0;
    const cutline::ui::Rect where{static_cast<double>(screen.x) - kTornWidth * 0.25,
                                  static_cast<double>(screen.y) - 12.0, kTornWidth, kTornHeight};
    if (cutline::ui::float_panel(layout_of(*app), panel, where)) app->dock_stale = true;
  });

  // A divider is read back rather than rebuilt: nothing about the arrangement
  // changed, only its proportions, and a rebuild would be work for nothing.
  dock.set_on_resize([app, shell] {
    if (app == nullptr || shell == nullptr || shell->dock == nullptr) return;
    if (shell->is_main()) {
      shell->dock->read_fractions_into(layout_of(*app).root);
      return;
    }
    const auto found = std::ranges::find(layout_of(*app).floating, shell->floating_id,
                                         &cutline::ui::FloatingDock::id);
    if (found != layout_of(*app).floating.end()) shell->dock->read_fractions_into(found->root);
  });
}

/// Whether a panel is anywhere in the arrangement, docked or torn out.
[[nodiscard]] bool panel_is_open(App& app, const PanelId& panel) {
  const std::vector<PanelId> open = cutline::ui::panels_in(layout_of(app));
  return std::ranges::find(open, panel) != open.end();
}

/// What the Window menu does to a panel: opens a closed one, and puts a hidden
/// tab of an open one to the front.
///
/// Closing from here as well, which is what Premiere's Window menu does and
/// what makes the tick honest — a menu that can only ever add ticks is one
/// where the tick is decoration.
void toggle_panel(App& app, const PanelId& panel) {
  DockLayout& layout = layout_of(app);
  if (!panel_is_open(app, panel)) {
    if (cutline::ui::open_panel(layout, panel)) app.dock_stale = true;
    return;
  }
  // Open, but possibly behind another tab. Bringing it forward is the useful
  // answer when that is so, and closing it is the useful answer when it is
  // already the one showing.
  if (cutline::ui::activate_panel(layout, panel)) {
    app.dock_stale = true;
    return;
  }
  if (cutline::ui::close_panel(layout, panel)) app.dock_stale = true;
}

/// Moves to another named arrangement.
///
/// Nothing is copied: the layout being left *is* the workspace's, so whatever
/// it was dragged into stays with it and is there on the way back.
void set_workspace(App& app, std::string_view name) {
  if (!cutline::editor::activate_workspace(app.workspaces, name)) return;
  if (app.workspace_button != nullptr) {
    app.workspace_button->set_text("Workspace: " + app.workspaces.active);
  }
  app.dock_stale = true;
}

/// Cycles to the next arrangement. A menu is what this wants to be; a button
/// that does one useful thing is what it can be until there is one.
void next_workspace(App& app) {
  const auto& all = app.workspaces.named;
  if (all.size() < 2) return;

  const auto found = std::ranges::find(all, app.workspaces.active,
                                       &cutline::editor::Workspace::name);
  const auto next = (found == all.end() || found + 1 == all.end()) ? all.begin() : found + 1;
  set_workspace(app, next->name);
}

/// Puts the arrangement back to how it was defined, and says so when there is
/// nothing to go back to.
void reset_workspace(App& app) {
  if (cutline::editor::reset_workspace(app.workspaces, app.workspaces.active)) {
    app.dock_stale = true;
    return;
  }
  complain(app.main.window,
           app.workspaces.active + " is already as it was defined, or is one you made "
                                   "yourself and so has nothing to go back to.");
}

/// Writes the arrangements out. Quiet on success, and quiet on failure too
/// except at the point it matters — losing a layout is not worth a dialog in
/// the middle of an edit.
void save_workspaces(App& app, bool complain_on_failure) {
  const auto written = cutline::editor::write_workspaces(app.workspace_file, app.workspaces);
  if (!written.has_value() && complain_on_failure) {
    complain(app.main.window, "Could not save the workspace.\n\n" + written.error());
  }
}

/// Rebuilds the dock from the layout.
///
/// Panel contents are carried across by the view itself, so this costs a walk
/// of the tree rather than building a browser and a timeline again.
void refresh_dock(App& app) {
  // Windows first: a rearrangement may have emptied one or made another, and
  // rebuilding a view that is about to be destroyed is wasted work.
  reconcile_windows(app);

  // Forgotten before the panels are rebuilt, and set again by whichever build
  // makes one. A rearrangement can close the mixer altogether, and a pointer to
  // a panel that is no longer there is worse than no pointer: the next thing to
  // change a route would rebuild strips inside freed memory.
  app.mixer = nullptr;

  for (Shell* shell : app.shells()) {
    if (shell->dock == nullptr) continue;

    const DockNode* node = &layout_of(app).root;
    // One panel filling the main window, when something is maximised. Built
    // here rather than by rewriting the tree, which is what makes coming back
    // exact — the arrangement was never taken apart.
    DockNode only;
    if (!layout_of(app).maximised.empty() && shell->is_main()) {
      only = DockNode::tabs({layout_of(app).maximised});
      node = &only;
    } else if (!shell->is_main()) {
      const auto found =
          std::ranges::find(layout_of(app).floating, shell->floating_id, &FloatingDock::id);
      if (found == layout_of(app).floating.end()) continue;
      node = &found->root;
    }

    shell->dock->set_node(*node);
    if (shell->host != nullptr) shell->host->request_layout();
    shell->dirty = true;
    if (shell->window != nullptr) InvalidateRect(shell->window, nullptr, FALSE);
  }
  refresh_float_titles(app);
}

}  // namespace cutline::tool
