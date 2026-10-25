/// The menu bar's menus, in Premiere's order.
///
/// Each is built as it drops, from the application as it is at that moment,
/// so what is greyed and what is ticked is always true.

#include "app.hpp"

namespace cutline::tool {

// ------------------------------------------------------------- the menus --
//
// Premiere's menu bar, in Premiere's order, and every command the editor has
// somewhere on it: a command reachable only from a key or a button is one
// nobody finds. Each menu is built as it drops, so what is greyed and what is
// ticked is what is true at that moment rather than what was true at startup.
//
// The keys written beside the rows are read from the keymap, so a menu always
// says the key that runs it now -- including one somebody changed.

[[nodiscard]] std::vector<MenuEntry> file_menu(App& app) {
  std::vector<MenuEntry> rows;
  rows.push_back(submenu_entry("New", {
      action_entry("Project", [&app] { new_project(app); }, shortcut_of(&app, "file.new")),
      action_entry("Sequence", [&app] { new_sequence(app); }),
      MenuEntry::divider(),
      action_entry("Bin", [&app] { add_bin(app); }),
      action_entry("Title", [&app] { add_title(app); }),
      action_entry("Colour Matte", [&app] { add_matte(app); }),
      action_entry("Adjustment Layer", [&app] { add_adjustment(app); }),
  }));
  rows.push_back(action_entry("Open Project...", [&app] { open_project(app); },
                              shortcut_of(&app, "file.open")));
  rows.push_back(MenuEntry::divider());
  rows.push_back(action_entry("Save", [&app] { save_project(app, false); },
                              shortcut_of(&app, "file.save")));
  rows.push_back(action_entry("Save As...", [&app] { save_project(app, true); },
                              shortcut_of(&app, "file.save_as")));
  rows.push_back(MenuEntry::divider());
  rows.push_back(action_entry("Import...", [&app] { import_media(app); },
                              shortcut_of(&app, "file.import")));
  // Premiere's name for it. On the pool's selection, like Premiere's.
  rows.push_back(action_entry("Link Media...", [&app] { relink_pool_entry(app); }));
  rows.push_back(MenuEntry::divider());
  rows.push_back(submenu_entry("Export", {
      action_entry("Media...", [&app] { open_export_dialog(app); },
                   shortcut_of(&app, "file.export")),
      action_entry("Frame", [&app] { take_snapshot(app); }),
  }));
  rows.push_back(MenuEntry::divider());
  rows.push_back(action_entry("Project Settings...", [&app] {
    open_settings_dialog(app, SettingsKind::Project);
  }));
  rows.push_back(MenuEntry::divider());
  rows.push_back(action_entry("Exit", [&app] {
    if (app.main.window != nullptr) PostMessageW(app.main.window, WM_CLOSE, 0, 0);
  }, "Alt+F4"));
  return rows;
}

[[nodiscard]] std::vector<MenuEntry> edit_menu(App& app) {
  using cutline::editor::Command;
  std::vector<MenuEntry> rows;
  rows.push_back(command_entry(app, "Undo", Command::Undo));
  rows.push_back(command_entry(app, "Redo", Command::Redo));
  rows.push_back(MenuEntry::divider());
  rows.push_back(command_entry(app, "Cut", Command::Cut));
  rows.push_back(command_entry(app, "Copy", Command::Copy));
  rows.push_back(command_entry(app, "Paste", Command::Paste));
  rows.push_back(command_entry(app, "Paste Insert", Command::PasteInsert));
  rows.push_back(command_entry(app, "Paste Attributes...", Command::PasteAttributes));
  rows.push_back(MenuEntry::divider());
  rows.push_back(command_entry(app, "Clear", Command::Delete));
  rows.push_back(command_entry(app, "Ripple Delete", Command::RippleDelete));
  rows.push_back(MenuEntry::divider());
  rows.push_back(command_entry(app, "Select All", Command::SelectAll));
  rows.push_back(command_entry(app, "Deselect All", Command::SelectNone));
  rows.push_back(MenuEntry::divider());
  // Where Premiere keeps both on Windows.
  rows.push_back(action_entry("Keyboard Shortcuts...", [&app] { open_keyboard_dialog(app); },
                              shortcut_of(&app, "edit.keyboard_shortcuts")));
  rows.push_back(action_entry("Preferences...", [&app] {
    open_settings_dialog(app, SettingsKind::Application);
  }));
  return rows;
}

[[nodiscard]] std::vector<MenuEntry> clip_menu(App& app) {
  using cutline::editor::Command;
  std::vector<MenuEntry> rows = clip_entries(app);
  rows.push_back(MenuEntry::divider());
  rows.push_back(command_entry(app, "Insert", Command::Insert));
  rows.push_back(command_entry(app, "Overwrite", Command::Overwrite));
  // It opens the box rather than acting, because the answer is a judgement
  // about the cut — Fit to Fill is only the first of five.
  rows.push_back(action_entry("Fit Clip...", [&app] { open_fit_dialog(app); }, {},
                              cutline::editor::can_run(app.session, Command::FitToFill)));
  rows.push_back(MenuEntry::divider());
  // The two halves kept apart, as Premiere keeps them: making proxies is a job
  // that runs for minutes, and reading from them is a switch.
  rows.push_back(submenu_entry("Proxy", {
      action_entry("Make Proxies", [&app] { make_proxies(app); }),
      // A tick rather than a label that changes, so the row says both what it
      // is and what it is set to.
      switch_entry("Use Proxies", app.session.project().use_proxies,
                   [&app] { toggle_use_proxies(app); }),
      // Reachable at all, which matters more than where: a transcode queued
      // over a card of footage is an hour of the machine.
      action_entry("Stop Making Proxies", [&app] { stop_making_proxies(app); }),
  }));
  return rows;
}

[[nodiscard]] std::vector<MenuEntry> sequence_menu(App& app) {
  using cutline::editor::Command;
  std::vector<MenuEntry> rows;
  rows.push_back(command_entry(app, "Add Edit", Command::Split));
  rows.push_back(command_entry(app, "Add Edit to All Tracks", Command::SplitAllTracks));
  rows.push_back(command_entry(app, "Trim Previous Edit to Playhead",
                               Command::TrimPreviousToPlayhead));
  rows.push_back(command_entry(app, "Trim Next Edit to Playhead", Command::TrimNextToPlayhead));
  rows.push_back(command_entry(app, "Nudge Left One Frame", Command::NudgeLeft));
  rows.push_back(command_entry(app, "Nudge Right One Frame", Command::NudgeRight));
  rows.push_back(MenuEntry::divider());
  rows.push_back(switch_entry("Snap in Timeline", app.snapping, [&app] { toggle_snapping(app); },
                              shortcut_of(&app, "sequence.snap")));
  rows.push_back(MenuEntry::divider());
  rows.push_back(command_entry(app, "Add Video Track", Command::AddVideoTrack));
  rows.push_back(command_entry(app, "Add Audio Track", Command::AddAudioTrack));
  rows.push_back(command_entry(app, "Remove Track", Command::RemoveTrack));
  rows.push_back(MenuEntry::divider());
  // The programme as a whole is what has a delivery number, so loudness is a
  // property of the sequence rather than of a clip.
  rows.push_back(action_entry("Normalise Loudness...", [&app] { open_loudness_dialog(app); }));
  rows.push_back(MenuEntry::divider());
  // Every cut, with the open one ticked. The strip of tabs over the timeline
  // only appears once there are two, so this is the one place that always
  // says which is open.
  std::vector<MenuEntry> cuts;
  const cutline::core::Project& document = app.session.project();
  for (std::size_t i = 0; i < document.sequences.size(); ++i) {
    const cutline::core::Sequence& sequence = document.sequences[i];
    cuts.push_back(switch_entry(sequence.name.empty() ? std::string("Sequence") : sequence.name,
                                i == document.open, [&app, id = sequence.id] {
      if (!app.session.open_sequence(id)) return;
      refresh_all(app);
      app.dock_stale = true;
    }));
  }
  rows.push_back(submenu_entry("Sequences", std::move(cuts)));
  return rows;
}

[[nodiscard]] std::vector<MenuEntry> markers_menu(App& app) {
  using cutline::editor::Command;
  std::vector<MenuEntry> rows;
  rows.push_back(command_entry(app, "Mark In", Command::MarkIn));
  rows.push_back(command_entry(app, "Mark Out", Command::MarkOut));
  rows.push_back(command_entry(app, "Clear In and Out", Command::ClearMarks));
  rows.push_back(MenuEntry::divider());
  rows.push_back(command_entry(app, "Add Marker", Command::AddMarker));
  rows.push_back(command_entry(app, "Go to Next Marker", Command::NextMarker));
  rows.push_back(command_entry(app, "Go to Previous Marker", Command::PreviousMarker));
  rows.push_back(command_entry(app, "Clear All Markers", Command::ClearMarkers));
  return rows;
}

[[nodiscard]] std::vector<MenuEntry> view_menu(App& app) {
  using cutline::editor::Command;
  std::vector<MenuEntry> rows;

  std::vector<MenuEntry> resolutions;
  for (const auto& [scale, name] : kPreviewScales) {
    resolutions.push_back(switch_entry(std::string(name), scale == app.preview_scale,
                                       [&app, scale] { choose_preview_scale(app, scale); }));
  }
  rows.push_back(submenu_entry("Playback Resolution", std::move(resolutions)));
  rows.push_back(switch_entry("Loop", app.looping, [&app] { toggle_looping(app); }));
  rows.push_back(MenuEntry::divider());

  // The transport. Premiere leaves these to the keys alone; here they are on
  // the menu as well, so that every one of them can be found.
  rows.push_back(action_entry("Play/Pause", [&app] { toggle_playback(app); },
                              shortcut_of(&app, "playback.play")));
  rows.push_back(action_entry("Shuttle Backward", [&app] {
    set_shuttle(app, next_shuttle(app.shuttle, -1.0));
  }, shortcut_of(&app, "playback.shuttle_left")));
  rows.push_back(action_entry("Stop", [&app] {
    set_shuttle(app, 0.0);
    if (app.playing()) stop_playback(app);
  }, shortcut_of(&app, "playback.stop")));
  rows.push_back(action_entry("Shuttle Forward", [&app] {
    set_shuttle(app, app.playing() ? next_shuttle(1.0, 1.0) : next_shuttle(app.shuttle, 1.0));
  }, shortcut_of(&app, "playback.shuttle_right")));
  rows.push_back(command_entry(app, "Go to Start", Command::GoToStart));
  rows.push_back(command_entry(app, "Go to End", Command::GoToEnd));
  rows.push_back(command_entry(app, "Previous Frame", Command::PreviousFrame));
  rows.push_back(command_entry(app, "Next Frame", Command::NextFrame));
  rows.push_back(MenuEntry::divider());

  const bool timeline = app.timeline != nullptr;
  rows.push_back(action_entry("Zoom In", [&app] {
    if (app.timeline == nullptr) return;
    app.timeline->zoom_about_playhead(true);
    mark_dirty(app);
  }, shortcut_of(&app, "sequence.zoom_in"), timeline));
  rows.push_back(action_entry("Zoom Out", [&app] {
    if (app.timeline == nullptr) return;
    app.timeline->zoom_about_playhead(false);
    mark_dirty(app);
  }, shortcut_of(&app, "sequence.zoom_out"), timeline));
  rows.push_back(action_entry("Zoom to Sequence", [&app] {
    if (app.timeline == nullptr) return;
    app.timeline->zoom_to_fit();
    mark_dirty(app);
  }, shortcut_of(&app, "sequence.zoom_fit"), timeline));
  rows.push_back(MenuEntry::divider());

  // The tools by name, with the one in hand ticked.
  std::vector<MenuEntry> tools;
  for (const ToolEntry& entry : kTools) {
    tools.push_back(switch_entry(entry.name, app.tool == entry.tool,
                                 [&app, tool = entry.tool] { choose_tool(app, tool); },
                                 shortcut_of(&app, entry.action)));
  }
  rows.push_back(submenu_entry("Tools", std::move(tools)));

  std::vector<MenuEntry> themes;
  const std::span<const Theme> all = built_in_themes();
  constexpr std::array<std::string_view, 4> kThemeActions{"view.theme.slate", "view.theme.luna",
                                                           "view.theme.aero", "view.theme.phosphor"};
  for (std::size_t i = 0; i < all.size(); ++i) {
    themes.push_back(switch_entry(std::string(all[i].name), i == app.theme,
                                  [&app, i] { set_theme(app, i); },
                                  i < kThemeActions.size() ? shortcut_of(&app, kThemeActions[i])
                                                           : std::string{}));
  }
  rows.push_back(submenu_entry("Theme", std::move(themes)));
  return rows;
}

[[nodiscard]] std::vector<MenuEntry> window_menu(App& app) {
  std::vector<MenuEntry> rows;

  // The named arrangements, the one in use ticked, and Premiere's way back to
  // how the one in use was defined.
  std::vector<MenuEntry> spaces;
  for (const cutline::editor::Workspace& workspace : app.workspaces.named) {
    spaces.push_back(switch_entry(workspace.name, workspace.name == app.workspaces.active,
                                  [&app, name = workspace.name] { set_workspace(app, name); }));
  }
  spaces.push_back(MenuEntry::divider());
  spaces.push_back(action_entry("Reset to Saved Layout", [&app] { reset_workspace(app); }));
  rows.push_back(submenu_entry("Workspaces", std::move(spaces)));
  rows.push_back(MenuEntry::divider());

  // Every panel this build has, ticked where it is open. This is the only way
  // back to a panel whose tab has been closed; without it closing one is
  // permanent short of resetting the whole workspace.
  for (const auto& [id, title] : kPanels) {
    rows.push_back(switch_entry(std::string(title), panel_is_open(app, PanelId(id)),
                                [&app, panel = PanelId(id)] { toggle_panel(app, panel); }));
  }
  return rows;
}

[[nodiscard]] std::vector<MenuEntry> help_menu(App& app) {
  return {action_entry("Check for Updates...", [&app] { check_for_updates(app); })};
}

/// The Window menu on its own, for `--check`, which measures its ticks.
[[nodiscard]] std::unique_ptr<Widget> build_window_menu(App& app) {
  return std::make_unique<MenuList>(window_menu(app));
}

}  // namespace cutline::tool
