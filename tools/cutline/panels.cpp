/// The panels: what `make_panel` builds for each name the layout holds --
/// the project pool, the two monitors, the timeline with its menus, effect
/// controls, the effects library, History and the scopes.

#include "app.hpp"

namespace cutline::tool {

/// What a drag of the monitor's handles does: moves the graphic layer chosen
/// in Essential Graphics when there is one, and the clip otherwise.
[[nodiscard]] cutline::core::Project apply_handles(const App& app, const std::string& clip_id,
                                                  const cutline::ui::MonitorBox& box) {
  if (!app.graphic_layer.empty() &&
      cutline::editor::graphic_layer_box(app.session.project(), clip_id, app.graphic_layer,
                                         app.session.playhead(), layer_measurer())
          .has_value()) {
    return cutline::editor::apply_graphic_layer_box(app.session.project(), clip_id,
                                                    app.graphic_layer, box,
                                                    app.session.playhead(), layer_measurer());
  }
  return cutline::editor::apply_monitor_box(app.session.project(), clip_id, box,
                                            app.session.playhead());
}

[[nodiscard]] std::unique_ptr<Widget> make_project_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& tools = panel->emplace<Box>(Axis::Horizontal);
  tools.emplace<Button>("Import", [app] {
    if (app != nullptr) import_media(*app);
  }).set_tooltip(with_keys(app, "Bring media into the project", "file.import"));
  tools.emplace<Button>("Remove", [app] {
    if (app != nullptr) remove_from_pool(*app);
  }).set_tooltip("Take the selected media out, and every clip of it");
  // One button and a menu rather than three buttons. Three would not fit — the
  // first attempt put New Title, Matte and Adjustment in this row and the last
  // of them was cut off by the edge of the panel — and a fourth generator would
  // not fit either.
  auto& generate = tools.emplace<Button>("New");
  generate.set_on_click([app, control = &generate] {
    if (app == nullptr || app->main.host == nullptr) return;

    auto list = std::make_unique<MenuList>(
        std::vector<std::string>{"Bin", "Title", "Colour Matte", "Adjustment Layer"});
    list->set_on_choose([app](std::size_t index) {
      if (app->main.host != nullptr) app->main.host->close_popup();
      switch (index) {
        // First, because it is the one wanted often and the others are wanted
        // once a project.
        case 0: add_bin(*app); break;
        case 1: add_title(*app); break;
        case 2: add_matte(*app); break;
        case 3: add_adjustment(*app); break;
        default: break;
      }
    });
    app->main.host->open_popup(std::move(list), control->bounds());
  });
  tools.emplace<Spacer>();

  // Premiere puts this at the bottom of the panel as two small icons. A button
  // saying which view it would switch to is one control instead of two, and it
  // reads without anybody having to learn what the icons mean.
  auto& view_choice = tools.emplace<Button>("Icons", [app] {
    if (app == nullptr || app->browser == nullptr) return;
    const bool icons = app->browser->view() == cutline::ui::BrowserView::Icons;
    set_pool_view(*app, icons ? cutline::ui::BrowserView::List : cutline::ui::BrowserView::Icons);
  });
  view_choice.set_tooltip("Show the pool as pictures or as a list");

  auto& pool = panel->emplace<MediaBrowser>();

  // Double-click or Enter drops it at the playhead; dragging it out puts it
  // where it was released. Both go through the same placement, so a clip
  // arrives the same way however it was asked for.
  pool.set_on_activate([app](std::size_t index) {
    if (app != nullptr) place_from_pool(*app, index);
  });
  // What an insert or an overwrite would place. Premiere's source monitor holds
  // this; there is none here yet, so the pool's selection is the source, which
  // is also what a double-click already places.
  pool.set_on_select([app](std::optional<std::size_t> index) {
    if (app == nullptr || app->browser == nullptr) return;
    const std::vector<cutline::ui::MediaItem>& items = app->browser->items();
    std::string id = index.has_value() && *index < items.size() ? items[*index].id : std::string{};
    // Selecting a bin leaves the source monitor showing whatever it had, rather
    // than clearing it: a folder is not a source, and emptying the monitor
    // because somebody clicked a folder loses the thing they were about to cut.
    if (!cutline::editor::bin_of_row(id).empty()) return;
    show_source(*app, std::move(id));
  });
  // The headings order the list now. The button that used to cycle through the
  // orderings has gone with them: it said what the order was in a place nobody
  // looks, and the column it applies to is the obvious place to say it.
  pool.set_on_sort([app](cutline::ui::MediaColumn column) {
    if (app != nullptr) sort_by_column(*app, column);
  });
  pool.set_on_toggle([app](std::size_t index) {
    if (app != nullptr) toggle_bin(*app, index);
  });
  pool.set_on_file([app](std::size_t from, std::size_t onto) {
    if (app != nullptr) file_into_bin(*app, from, onto);
  });
  pool.set_on_context_menu([app](double x, double y) {
    if (app != nullptr) open_pool_menu(*app, x, y);
  });
  pool.set_on_drop([app](std::size_t index, double x, double y) {
    if (app == nullptr) return;
    // The promise is kept or it is withdrawn; either way it stops being shown.
    if (app->timeline != nullptr) app->timeline->set_drop_ghost(std::nullopt);
    const auto where = app->timeline == nullptr ? std::nullopt : app->timeline->drop_at(x, y);
    // A drag that ended anywhere but over a track is one that was thought
    // better of, not one that meant the playhead.
    if (where.has_value()) place_from_pool(*app, index, where);
  });

  if (app != nullptr) {
    app->browser = &pool;
    app->view_button = &view_choice;
    // Whatever the settings asked for, applied now that there is a widget to
    // ask it of. The button is named for what it would do next, so it says the
    // opposite of the view being shown.
    pool.set_view(app->pool_view);
    view_choice.set_text(app->pool_view == cutline::ui::BrowserView::Icons ? "List" : "Icons");
    refresh_browser(*app);
  } else {
    // The headless check builds these with no session behind them. Filling the
    // pool anyway is what makes it lay out and paint real rows in every theme,
    // which is the only thing that would catch a row too tall for the one
    // theme whose lists are roomier.
    // Both bins open, so the check sees the deepest indent the panel can draw
    // rather than two closed folders.
    const std::vector<std::string> open{"bin_pictures", "bin_sound"};
    pool.set_items(cutline::editor::browser_items(check_project(), {.expanded = open}));
  }

  // Under the pool rather than in the toolbar above it. It was in the toolbar
  // first, and `--check` reported two controls cut in half by the edge of the
  // panel — the same trap a third generator button fell into. This row has room
  // and this is a thing you look at rather than reach for.
  //
  // The version doubles as the button: a label saying what is running and a
  // separate control asking about newer would be two where one will do, and
  // putting the number on the button is what makes "there is a newer one" mean
  // anything.
  auto& footer = panel->emplace<Box>(Axis::Horizontal);
  auto& version = footer.emplace<Button>(
      std::string("v") + std::string(cutline::core::kVersion), [app] {
        if (app != nullptr) check_for_updates(*app);
      });
  if (app != nullptr) app->version_button = &version;
  footer.emplace<Spacer>();

  // At the far end of the row the version sits on, which is where Premiere puts
  // its progress too. Small and quiet: it is reassurance that something is
  // happening, not a thing to watch.
  auto& busy = footer.emplace<Label>(std::string{});
  busy.set_small(true);
  busy.set_visible(false);
  if (app != nullptr) app->busy_label = &busy;

  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_monitor_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& picture = panel->emplace<MonitorView>();
  if (app != nullptr) {
    app->monitor = &picture;
    show_comparison(*app);
    show_safe_margins(*app);
#if !CUTLINE_HAVE_PREVIEW
    // Only where nothing can be decoded at all. In a build that renders, an
    // empty sequence shows an empty frame — colour bars there read as content
    // somebody put on the timeline rather than as "there is nothing here yet".
    picture.set_frame(app->pattern.view());
#else
    if (!app->shown_preview.empty()) {
      picture.set_texture(app->shown_preview);
    } else if (!app->preview_pixels.pixels.empty()) {
      picture.set_frame(cutline::ui::ImageView{.pixels = app->preview_pixels.pixels.data(),
                                               .width = app->preview_pixels.width,
                                               .height = app->preview_pixels.height});
    }
#endif
    // Pushed rather than defaulted, for the same reason snapping is: the panel
    // is rebuilt by a rearrangement and the setting is not.
    picture.set_aspect_locked(app->aspect_locked);

    // Live on every pixel so the preview follows the drag, and once on release
    // so the whole gesture is one entry in the undo stack — the same split the
    // timeline and every slider use.
    // A mask dragged on the picture. The same bargain as everything else
    // dragged here: rendered against a copy while the button is down, written
    // once on release, so the whole gesture is one undo entry.
    picture.set_on_mask_change([app](std::size_t index, const cutline::ui::MaskOverlay& shape) {
      if (index >= app->mask_effects.size()) return;
      const auto selection = app->session.selection();
      if (selection.empty()) return;

      app->live_gesture = true;
      app->live_project = cutline::editor::apply_mask_overlay(
          app->session.project(), selection.front(), app->mask_effects[index], shape,
          app->session.playhead());
      invalidate_preview(*app);
    });
    picture.set_on_mask_commit([app](std::size_t index, const cutline::ui::MaskOverlay& shape) {
      if (index >= app->mask_effects.size()) return;
      const auto selection = app->session.selection();
      if (selection.empty()) return;

      app->session.apply(cutline::editor::apply_mask_overlay(
          app->session.project(), selection.front(), app->mask_effects[index], shape,
          app->session.playhead()));
      app->live_gesture = false;
      invalidate_preview(*app);
      app->inspector_stale = true;
    });

    picture.set_on_picture_press([app](double x, double y, int clicks) {
      return press_on_picture(*app, x, y, clicks);
    });
    picture.set_box_drawing(app->tool == cutline::ui::Tool::Rectangle ||
                            app->tool == cutline::ui::Tool::Ellipse);
    picture.set_on_box_drawn([app](const cutline::ui::MonitorBox& box, bool done) {
      box_drawn_on_picture(*app, box, done);
    });
    picture.set_on_transform_change([app](const cutline::ui::MonitorBox& box) {
      const auto selection = app->session.selection();
      if (selection.empty()) return;
      app->dragging_handle = true;
      // Against the session's project every time rather than against the last
      // preview: the box the view reports is where the layer should be, not how
      // far it has come, and compounding them would send it off across the
      // canvas at several times the speed of the pointer.
      app->live_project = apply_handles(*app, selection.front(), box);
      invalidate_preview(*app);
    });
    picture.set_on_transform_commit([app](const cutline::ui::MonitorBox& box) {
      const auto selection = app->session.selection();
      app->dragging_handle = false;
      app->live_project.reset();
      if (selection.empty()) return;

      app->session.apply(apply_handles(*app, selection.front(), box));
      refresh_timeline(*app);
      invalidate_preview(*app);
      // The inspector's Motion rows are showing the same numbers, and a
      // transform dragged on the picture that left them stale would be two
      // controls disagreeing about one value. On the commit only — rebuilding
      // it per mouse move would destroy and remake dozens of widgets a second.
      app->inspector_stale = true;
    });
    refresh_handles(*app);
  }

  auto& transport = panel->emplace<Box>(Axis::Horizontal);
  // Both toggle: marking where a mark already is takes it away, which is how
  // one is removed without a third button that exists only to undo the other
  // two. `run` is what decides that, so the button and the I key cannot drift.
  transport.emplace<Button>("Mark In", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::MarkIn);
  }).set_tooltip(with_keys(app, "Mark in at the playhead, or clear it", "markers.mark_in"));
  transport.emplace<Button>("Mark Out", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::MarkOut);
  }).set_tooltip(with_keys(app, "Mark out at the playhead, or clear it", "markers.mark_out"));
  // The frame at the playhead, written out as a PNG. Under the monitor rather
  // than in the project panel, which the spec suggests: that row is already
  // full, and `--check` said so by finding two controls cut in half by the edge
  // of it. This is also where the frame being saved is on show.
  transport.emplace<Button>("Snapshot", [app] {
    if (app != nullptr) take_snapshot(*app);
  }).set_tooltip("Write the frame at the playhead out as a PNG");

  // The sequence's size used to be a button here. It is a property of the
  // project, and it lives in Project Settings now — the monitor showing it was
  // a second way in from before there was a menu bar to hold the first.

  // A dropdown rather than a button that cycles. Cycling is fine for two
  // states; with three it hides two of them behind the one showing, so choosing
  // a quality means clicking until the right one comes round and there is no
  // way to see what the choices are without doing it.
  {
    std::vector<std::string> names;
    std::size_t chosen = 0;
    for (const auto& [scale, name] : kPreviewScales) {
      if (app != nullptr && scale == app->preview_scale) chosen = names.size();
      names.emplace_back(name);
    }
    auto& quality = transport.emplace<Dropdown>(std::move(names), chosen);
    quality.set_tooltip("How much of the picture to render while scrubbing");
    quality.set_on_change([app](std::size_t index) {
      if (app == nullptr || index >= kPreviewScales.size()) return;
      choose_preview_scale(*app, kPreviewScales[index].first);
    });
    if (app != nullptr) app->preview_scale_choice = &quality;
  }

  if (app != nullptr) {
    add_zoom_control(transport, picture, app->monitor_zoom, [app](double zoom) {
      app->monitor_zoom = zoom;
      if (app->monitor != nullptr) app->monitor->set_zoom(zoom);
      mark_dirty(*app);
    });
  }

  transport.emplace<Spacer>();
  auto& loop = transport.emplace<Button>("Loop", [app] {
    if (app != nullptr) toggle_looping(*app);
  });
  loop.set_tooltip("Play the marked span over and over");
  if (app != nullptr) {
    app->loop_button = &loop;
    loop.set_selected(app->looping);
  }

  auto& play = transport.emplace<Button>("Play", [app] {
    if (app != nullptr) toggle_playback(*app);
  });
  play.set_tooltip(with_keys(app, "Play or pause", "playback.play"));
  if (app != nullptr) {
    app->play_button = &play;
    // The panel may have been rebuilt mid-playback — a rearrangement, a theme
    // change — and a button that says Play while the sound is running is worse
    // than one that does nothing.
    if (app->playing()) play.set_text("Pause");
  }
  transport.emplace<Button>("Export", [app] {
    if (app != nullptr) open_export_dialog(*app);
  }).set_tooltip("Render the sequence to a file");

  // Trim mode's buttons, Premiere's: back five, back one, the default
  // transitions, forward one, forward five. Shown only while it is on.
  auto& trim = panel->emplace<Box>(Axis::Horizontal);
  trim.set_visible(app != nullptr && app->trim_point.has_value());
  auto& label = trim.emplace<Label>("");
  label.set_small(true);
  trim.emplace<Spacer>();
  for (const auto& [text, frames, action] :
       {std::tuple{"-5", -5.0, "sequence.trim_backward_many"},
        std::tuple{"-1", -1.0, "sequence.trim_backward"}}) {
    trim.emplace<Button>(text, [app, frames] {
      if (app != nullptr) trim_by(*app, frames);
    }).set_tooltip(with_keys(app, frames < -1.0 ? "Trim backward five frames"
                                                : "Trim backward a frame",
                             action));
  }
  trim.emplace<Button>("Transitions", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::ApplyDefaultTransitions);
  }).set_tooltip("Apply the default transitions to the cut");
  for (const auto& [text, frames, action] :
       {std::tuple{"+1", 1.0, "sequence.trim_forward"},
        std::tuple{"+5", 5.0, "sequence.trim_forward_many"}}) {
    trim.emplace<Button>(text, [app, frames] {
      if (app != nullptr) trim_by(*app, frames);
    }).set_tooltip(with_keys(app, frames > 1.0 ? "Trim forward five frames"
                                               : "Trim forward a frame",
                             action));
  }
  trim.emplace<Spacer>();
  if (app != nullptr) {
    app->trim_row = &trim;
    app->trim_label = &label;
    // Trim mode shows its outgoing frame where a comparison would, so it is
    // put back once the rest of the panel is.
    if (app->trim_point.has_value()) show_trim(*app);
  }
  return panel;
}

/// The colours a clip can be labelled with, as a menu.
///
/// Its own popup rather than eight more rows on the clip menu, which is long
/// enough already. Ticked where the selection already carries one, and "None"
/// first because getting a label off is as ordinary as putting one on.
/// The colours clips can be labelled. A mixed selection ticks nothing, which
/// is honest: there is no one answer to what colour these are.
[[nodiscard]] std::vector<MenuEntry> clip_label_entries(App& app,
                                                        const std::vector<std::string>& clips) {
  std::string current;
  bool agreed = true;
  bool first = true;
  for (const std::string& id : clips) {
    const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), id);
    if (clip == nullptr) continue;
    if (first) {
      current = clip->label_color;
      first = false;
    } else if (clip->label_color != current) {
      agreed = false;
    }
  }

  const auto choose = [&app, clips](std::string color) {
    return [&app, clips, color = std::move(color)] {
      app.session.apply(cutline::core::set_clips_label(app.session.project(), clips, color));
      refresh_timeline(app);
      mark_dirty(app);
    };
  };

  std::vector<MenuEntry> rows;
  rows.push_back(switch_entry("None", agreed && current.empty(), choose({})));
  const std::span<const cutline::editor::ClipLabel> palette = cutline::editor::clip_labels();
  for (std::size_t i = 0; i < palette.size(); ++i) {
    rows.push_back(switch_entry(std::string(cutline::editor::label_name(app.settings, i)),
                                agreed && current == palette[i].color, choose(std::string(palette[i].color))));
  }
  return rows;
}

/// Whether any of the clips is held on one frame.
[[nodiscard]] bool any_held(const App& app, const std::vector<std::string>& clips) {
  return std::ranges::any_of(clips, [&](const std::string& id) {
    const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), id);
    return clip != nullptr && clip->hold.has_value();
  });
}

/// Holds the clips on the frame under the playhead, or lets them go when any
/// of them is held already.
///
/// At the playhead, which is Premiere's default and the only choice that means
/// anything: the frame you are looking at is the one you want held. Each clip
/// clamps it into its own trim, so a selection spread across the sequence
/// still freezes on a frame it owns.
void toggle_frame_hold(App& app, const std::vector<std::string>& clips) {
  app.session.apply(cutline::core::set_clips_hold(
      app.session.project(), clips,
      any_held(app, clips) ? std::nullopt : std::optional<double>{app.session.playhead()}));
  refresh_timeline(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  mark_dirty(app);
}

/// Scales the clips to fit inside the frame, or to cover it.
void frame_clips(App& app, const std::vector<std::string>& clips, cutline::editor::FrameFit fit) {
  app.session.apply(cutline::editor::scale_to_frame(app.session.project(), clips, fit,
                                                    app.session.playhead()));
  refresh_timeline(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  mark_dirty(app);
}

/// Whether every one of the clips is switched on.
[[nodiscard]] bool all_enabled(const App& app, const std::vector<std::string>& clips) {
  return std::ranges::all_of(clips, [&](const std::string& id) {
    const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), id);
    return clip != nullptr && !clip->disabled;
  });
}

/// Switches the clips off when they are all on, and on otherwise.
void toggle_clips_enabled(App& app, const std::vector<std::string>& clips) {
  app.session.apply(
      cutline::core::set_clips_enabled(app.session.project(), clips, !all_enabled(app, clips)));
  refresh_timeline(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  stop_playback(app);
  mark_dirty(app);
}

void open_speed_dialog(App& app, std::span<const std::string> clips);

/// What can be done to the clips selected: Premiere's Clip menu, less the
/// editing that lives under Edit and Sequence.
///
/// One function for the menu bar and the timeline's right-click, so the two
/// cannot drift. Greyed rather than left out when nothing is selected, so the
/// menu keeps its shape and says what it would do.
[[nodiscard]] std::vector<MenuEntry> clip_entries(App& app) {
  using cutline::editor::Command;
  const std::vector<std::string> selected = app.session.selected_group();
  const bool any = !selected.empty();

  std::vector<MenuEntry> rows;
  rows.push_back(action_entry("Speed/Duration...",
                              [&app, selected] { open_speed_dialog(app, selected); }, {}, any));
  // Ticked, because a hold is a state a clip is in rather than something to do
  // to it — and the row that turns it off has to be the same row.
  rows.push_back(switch_entry("Frame Hold", any && any_held(app, selected),
                              [&app, selected] { toggle_frame_hold(app, selected); }, {}, any));
  rows.push_back(action_entry("Fit to Frame", [&app, selected] {
    frame_clips(app, selected, cutline::editor::FrameFit::Fit);
  }, {}, any));
  rows.push_back(action_entry("Fill Frame", [&app, selected] {
    frame_clips(app, selected, cutline::editor::FrameFit::Fill);
  }, {}, any));
  rows.push_back(MenuEntry::divider());
  // Premiere's Modify ▸ Audio Channels, and the answer to a camera with a
  // lapel on one channel and a shotgun on the other.
  rows.push_back(action_entry("Audio Channels...",
                              [&app, selected] { open_channel_dialog(app, selected); }, {}, any));
  rows.push_back(action_entry("Essential Sound...",
                              [&app, selected] { open_essential_sound(app, selected); }, {}, any));
  rows.push_back(MenuEntry::divider());
  rows.push_back(switch_entry("Enable", any && all_enabled(app, selected),
                              [&app, selected] { toggle_clips_enabled(app, selected); }, {}, any));
  rows.push_back(command_entry(app, "Link", Command::LinkClips));
  rows.push_back(command_entry(app, "Unlink", Command::UnlinkClips));
  rows.push_back(command_entry(app, "Nest...", Command::Nest));
  // Premiere's Multi-Camera, for clips of a multi-camera source sequence.
  {
    std::vector<MenuEntry> cameras = multicam_entries(app, selected);
    const bool multicam = std::ranges::any_of(selected, [&app](const std::string& id) {
      const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), id);
      return clip != nullptr && cutline::core::multicam_source(app.session.project(), *clip);
    });
    MenuEntry entry = submenu_entry("Multi-Camera", multicam ? std::move(cameras)
                                                             : std::vector<MenuEntry>{});
    entry.enabled = multicam;
    rows.push_back(std::move(entry));
  }
  rows.push_back(MenuEntry::divider());
  rows.push_back(submenu_entry("Label", any ? clip_label_entries(app, selected)
                                            : std::vector<MenuEntry>{}));
  return rows;
}

/// The timeline's right-click: the clipboard and the cuts, then the Clip menu.
///
/// Over empty track there is no clip, and the clip rows are left off rather
/// than greyed: a right-click is about what is under the pointer, and a column
/// of greyed clip commands over a gap says nothing. The tracks are offered
/// there instead.
[[nodiscard]] std::vector<MenuEntry> timeline_context_entries(App& app) {
  using cutline::editor::Command;
  const bool any = !app.session.selected_group().empty();

  std::vector<MenuEntry> rows;
  if (any) {
    rows.push_back(command_entry(app, "Cut", Command::Cut));
    rows.push_back(command_entry(app, "Copy", Command::Copy));
  }
  rows.push_back(command_entry(app, "Paste", Command::Paste));
  rows.push_back(command_entry(app, "Paste Insert", Command::PasteInsert));
  if (any) rows.push_back(command_entry(app, "Paste Attributes...", Command::PasteAttributes));
  rows.push_back(MenuEntry::divider());
  if (any) rows.push_back(command_entry(app, "Clear", Command::Delete));
  rows.push_back(command_entry(app, "Ripple Delete", Command::RippleDelete));
  rows.push_back(command_entry(app, "Add Edit", Command::Split));
  rows.push_back(command_entry(app, "Add Edit to All Tracks", Command::SplitAllTracks));
  rows.push_back(MenuEntry::divider());
  if (any) {
    for (MenuEntry& row : clip_entries(app)) rows.push_back(std::move(row));
  } else {
    rows.push_back(command_entry(app, "Add Video Track", Command::AddVideoTrack));
    rows.push_back(command_entry(app, "Add Audio Track", Command::AddAudioTrack));
  }
  return rows;
}

/// Renaming a track: a popup with a field in it, hung under the header.
///
/// A popup rather than a field the timeline holds: the view draws its headers
/// and builds no widgets in them, and giving it one for this alone would mean
/// it owning focus, a caret and a commit rule it has no other use for.
void rename_track(App& app, std::size_t track) {
  if (app.timeline == nullptr || app.main.host == nullptr) return;
  const auto& rows = app.timeline->model().tracks;
  if (track >= rows.size()) return;

  const std::string id = rows[track].id;
  auto popup = std::make_unique<Panel>();
  auto& row = popup->emplace<Box>(Axis::Horizontal);
  auto& field = row.emplace<TextField>(rows[track].name);

  const auto commit = [&app, id, control = &field] {
    if (app.main.host != nullptr) app.main.host->close_popup();
    app.session.apply(
        cutline::core::set_track_label(app.session.project(), id, control->text()));
    refresh_timeline(app);
  };
  // Both, because a field is finished either by pressing the button or by
  // pressing return, and somebody who typed a name expects both to work.
  field.set_on_commit([commit](const std::string&) { commit(); });
  row.emplace<Button>("Rename", commit);

  app.main.host->open_popup(std::move(popup), app.timeline->header_rect(track));
  app.main.host->set_focus(&field);
}

/// A track's own menu, from a right-click on its header.
///
/// Premiere has one and this had none: a right-click on a header opened the
/// clip menu, which offered Cut and Copy over a place where there is no clip.
///
/// Every row that is a *state* is ticked rather than named for what it would
/// become, so the menu reads as what is true rather than as a list of verbs —
/// which is also the only way sync lock, the one switch with no letter in the
/// header, is reachable at all.
void open_track_menu(App& app, std::size_t track, double x, double y) {
  if (app.main.host == nullptr || app.timeline == nullptr) return;
  const std::vector<cutline::ui::TimelineTrack>& rows = app.timeline->model().tracks;
  if (track >= rows.size()) return;

  const std::string id = rows[track].id;
  const cutline::core::Track* held =
      std::ranges::find(app.session.project().sequence().tracks, id, &cutline::core::Track::id) !=
              app.session.project().sequence().tracks.end()
          ? &*std::ranges::find(app.session.project().sequence().tracks, id, &cutline::core::Track::id)
          : nullptr;
  if (held == nullptr) return;

  const bool audio = held->kind == cutline::core::TrackKind::Audio;

  /// One row: what it says, whether it is a switch and on, and what taking it
  /// does. A row with no label is a rule between groups.
  struct Row {
    std::string label;
    std::optional<bool> ticked;
    std::function<void()> act;
  };

  const auto patched = [&app, id](const cutline::core::TrackPropsPatch& patch) {
    app.session.apply(cutline::core::update_track(app.session.project(), id, patch));
    refresh_timeline(app);
    invalidate_preview(app);
    stop_playback(app);
    mark_dirty(app);
  };

  std::vector<Row> rows_out;
  rows_out.push_back(Row{.label = "Rename...", .act = [&app, track] {
                           if (app.timeline != nullptr) rename_track(app, track);
                         }});
  rows_out.push_back(Row{.label = "Target",
                         .ticked = held->targeted,
                         .act = [patched, was = held->targeted] {
                           patched({.targeted = !was});
                         }});
  rows_out.push_back(Row{.label = "Sync Lock",
                         .ticked = held->sync_locked,
                         .act = [patched, was = held->sync_locked] {
                           patched({.sync_locked = !was});
                         }});
  rows_out.push_back(Row{.label = "Lock",
                         .ticked = held->locked,
                         .act = [patched, was = held->locked] { patched({.locked = !was}); }});

  if (audio) {
    rows_out.push_back(Row{.label = "Mute",
                           .ticked = held->muted,
                           .act = [patched, was = held->muted] { patched({.muted = !was}); }});
    rows_out.push_back(Row{.label = "Solo",
                           .ticked = held->solo,
                           .act = [patched, was = held->solo] { patched({.solo = !was}); }});
  } else {
    rows_out.push_back(Row{.label = "Hide",
                           .ticked = held->hidden,
                           .act = [patched, was = held->hidden] { patched({.hidden = !was}); }});
  }

  rows_out.push_back(Row{});
  rows_out.push_back(Row{.label = "Reset Height", .act = [&app, id] {
                           app.session.apply(cutline::core::set_track_height(
                               app.session.project(), id, std::nullopt));
                           refresh_timeline(app);
                           mark_dirty(app);
                         }});
  rows_out.push_back(Row{});
  rows_out.push_back(Row{.label = "Add Video Track", .act = [&app] {
                           run_command(app, cutline::editor::Command::AddVideoTrack);
                         }});
  rows_out.push_back(Row{.label = "Add Audio Track", .act = [&app] {
                           run_command(app, cutline::editor::Command::AddAudioTrack);
                         }});
  // A bus, rather than a lane. Nothing can be dropped on it: it is fed by the
  // tracks routed into it, which is what the output dropdown on every mixer
  // strip is for.
  rows_out.push_back(Row{.label = "Add Submix Track", .act = [&app] {
                           // Named on arrival, which Premiere does too. A lane
                           // number says nothing about a bus — "A4" for the
                           // dialogue bus has to be looked up every time it is
                           // read — and the name is what both the timeline head
                           // and the mixer strip show.
                           std::size_t count = 0;
                           for (const cutline::core::Track& t : app.session.project().sequence().tracks) {
                             if (t.submix) ++count;
                           }
                           app.session.apply(cutline::core::add_submix_track(
                               app.session.project(),
                               "Submix " + std::to_string(count + 1)));
                           refresh_all(app);
                           stop_playback(app);
                           mark_dirty(app);
                         }});
  rows_out.push_back(Row{});
  rows_out.push_back(Row{.label = "Delete Track", .act = [&app, id] {
                           app.session.apply(
                               cutline::core::remove_track(app.session.project(), id));
                           refresh_timeline(app);
                           invalidate_preview(app);
                           stop_playback(app);
                           mark_dirty(app);
                         }});

  std::vector<MenuEntry> entries;
  for (Row& row : rows_out) {
    if (row.label.empty()) {
      entries.push_back(MenuEntry::divider());
      continue;
    }
    entries.push_back(MenuEntry{.label = std::move(row.label), .checked = row.ticked,
                                .act = std::move(row.act)});
  }

  app.main.host->open_popup(std::make_unique<MenuList>(std::move(entries)), Rect{x, y, 0.0, 0.0});
}

[[nodiscard]] std::unique_ptr<Widget> make_timeline_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& tools = panel->emplace<Box>(Axis::Horizontal);
  // Through `run_command` rather than calling the session directly, so the
  // button and Ctrl+Z do the same thing. They did not: these two refreshed the
  // timeline and the pool and forgot the preview, so undoing a transform with
  // the button left the picture showing the edit that had just been undone,
  // while the same undo from the keyboard was fine. Anything with two ways to
  // reach it needs one place that does it.
  tools.emplace<Button>("Undo", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::Undo);
  }).set_tooltip(with_keys(app, "Undo", "edit.undo"));
  tools.emplace<Button>("Redo", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::Redo);
  }).set_tooltip(with_keys(app, "Redo", "edit.redo"));
  // The tool palette, between the history buttons and the timecode. Premiere
  // floats it over the timeline; a row in the toolbar is the same thing without
  // a second window to keep track of, and it cannot be lost behind anything.
  auto& palette = tools.emplace<Box>(Axis::Horizontal);
  palette.set_spacing(Space::Tight);
  if (app != nullptr) app->tool_buttons.clear();
  for (const ToolEntry& entry : kTools) {
    auto& button = palette.emplace<IconButton>(entry.icon, [app, tool = entry.tool] {
      if (app != nullptr) choose_tool(*app, tool);
    });
    button.set_name(std::string("tool.") + std::string(cutline::ui::to_string(entry.tool)));
    button.set_tooltip(with_keys(app, entry.name, entry.action));
    if (app != nullptr) {
      button.set_selected(app->tool == entry.tool);
      app->tool_buttons.push_back(&button);
    }
  }

  // Linking, and the track operations. Both act through the command table
  // rather than calling the model here, so the button and the shortcut cannot
  // come to mean different things — and `can_run` is what greys them out, which
  // is the same answer a menu would ask for.
  auto& link = tools.emplace<Button>("Link", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::LinkClips);
  });
  link.set_tooltip(with_keys(app, "Link the selected clips", "clip.link"));
  auto& unlink = tools.emplace<Button>("Unlink", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::UnlinkClips);
  });
  unlink.set_tooltip(with_keys(app, "Unlink them", "clip.unlink"));
  if (app != nullptr) {
    app->link_button = &link;
    app->unlink_button = &unlink;
  }

  // A menu rather than three more buttons, for the reason the project panel's
  // New menu already found out: the row runs out of width before the commands
  // run out.
  auto& track_menu = tools.emplace<Button>("Track");
  track_menu.set_tooltip("Add or remove a track");
  track_menu.set_on_click([app, control = &track_menu] {
    if (app == nullptr || app->main.host == nullptr) return;

    auto list = std::make_unique<MenuList>(
        std::vector<std::string>{"Add Video Track", "Add Audio Track", "Remove Track"});
    list->set_on_choose([app](std::size_t index) {
      if (app->main.host != nullptr) app->main.host->close_popup();
      using cutline::editor::Command;
      switch (index) {
        case 0: run_command(*app, Command::AddVideoTrack); break;
        case 1: run_command(*app, Command::AddAudioTrack); break;
        case 2: run_command(*app, Command::RemoveTrack); break;
        default: break;
      }
    });
    app->main.host->open_popup(std::move(list), control->bounds());
  });

  // Snapping, and the zoom that fits the whole sequence. Both are properties of
  // the *view* rather than of the document, which is why neither goes through
  // the command table: undoing a zoom is not a thing anybody wants.
  auto& snap = tools.emplace<Button>("Snap", [app] {
    if (app != nullptr) toggle_snapping(*app);
  });
  snap.set_tooltip(with_keys(app, "Snap edges together", "sequence.snap"));
  if (app != nullptr) app->snap_button = &snap;

  tools.emplace<Button>("Fit", [app] {
    if (app == nullptr || app->timeline == nullptr) return;
    app->timeline->zoom_to_fit();
    mark_dirty(*app);
  }).set_tooltip("Zoom to the whole sequence (\\)");

  tools.emplace<Spacer>();
  // Eleven columns: "00:00:00:00" is what a timecode is and it is never any
  // other length, so the field asks for exactly that and leaves the rest of the
  // row to the buttons.
  auto& readout = tools.emplace<TextField>("00:00:00:00");
  readout.set_columns(11);
  readout.set_tooltip("Where the playhead is. Type a time to go there");
  readout.set_on_commit([app](const std::string& typed) {
    if (app == nullptr) return;
    const std::optional<double> at =
        cutline::core::timecode_to_seconds(typed, app->session.project().sequence().fps,
                                           app->session.project().sequence().drop_frame);
    // Nothing that parses leaves the playhead where it is rather than sending
    // it to zero, which is what a mistyped character would otherwise do.
    if (!at.has_value()) {
      show_playhead(*app);
      return;
    }
    go_to_time(*app, *at);
    // The keyboard goes back to the editor. A field that keeps it after Enter
    // swallows every single-letter shortcut there is — pressing N for the roll
    // tool types an N into the timecode instead, which is a confusing way to
    // find out where the focus went.
    if (app->main.host != nullptr) app->main.host->set_focus(nullptr);
  });
  // However the edit ends, the field goes back to saying where the playhead
  // actually is — including after an escape, and after a half-typed time the
  // keyboard simply left.
  readout.set_on_finish([app] {
    if (app != nullptr) show_playhead(*app);
  });
  if (app != nullptr) app->readout = &readout;

  // The sequences, as a strip of tabs above the tracks.
  //
  // Only when there is more than one. A project with a single cut has nothing
  // to switch between, and a row holding one tab is a row of chrome explaining
  // a choice nobody has — Premiere shows the strip always because a Premiere
  // project is a bin of sequences from the start, and this one is not.
  if (app != nullptr && app->session.project().sequences.size() > 1) {
    auto& strip = panel->emplace<Box>(Axis::Horizontal);
    strip.set_spacing(Space::Tight);
    const cutline::core::Project& document = app->session.project();
    for (std::size_t i = 0; i < document.sequences.size(); ++i) {
      const cutline::core::Sequence& sequence = document.sequences[i];
      const std::string label = sequence.name.empty() ? std::string("Sequence") : sequence.name;
      auto& tab = strip.emplace<Button>(label, [app, id = sequence.id] {
        if (app == nullptr) return;
        if (!app->session.open_sequence(id)) return;
        // Everything is showing a different cut now.
        refresh_all(*app);
        app->dock_stale = true;
      });
      tab.set_selected(i == document.open);
      tab.set_tooltip(label);
    }
    strip.emplace<Spacer>();
  }

  auto& tracks = panel->emplace<TimelineView>();
  tracks.set_scale(TimeScale{.pixels_per_second = 60.0});
  if (app != nullptr) {
    app->timeline = &tracks;
    // The panel is new; the tool and the snap setting are not. A rearrangement
    // or a theme change must not quietly put the selection tool back, or turn
    // snapping on again after somebody deliberately turned it off.
    tracks.set_tool(app->tool);
    show_snapping(*app);
  }

  tracks.set_on_scrub([app](double at) {
    if (app == nullptr) return;
    app->session.set_playhead(at);
    show_playhead(*app);
    follow_playhead(*app);
#if CUTLINE_HAVE_PREVIEW
    // Dragging the playhead while it is playing takes the sound with it.
    // Without this the audio carries on from where it was and the picture is
    // somewhere else, which is worse than either.
    if (app->playing()) {
      app->player->seek(at);
      // As in `go_to_time`: the loop must not seek again behind this one.
      app->playhead_from_playback = app->session.playhead();
      app->shown_frame = -1;
    }
#endif
    invalidate_preview(*app);
  });

  tracks.set_on_select([app](std::span<const cutline::ui::BlockRef> refs) {
    if (app == nullptr || app->timeline == nullptr) return;
    if (refs.empty()) {
      app->session.clear_selection();
      app->inspector_stale = true;
      refresh_timeline(*app);
      return;
    }

    // Each block, expanded to the whole linked group it belongs to. Clicking a
    // video clip and seeing its audio stay unhighlighted says the two are
    // separate when the point of linking them is that they are not — and every
    // edit was already going to reach both, so showing one selected was the
    // interface disagreeing with what it was about to do.
    std::vector<std::string> ids;
    for (const cutline::ui::BlockRef& ref : refs) {
      const auto id = cutline::editor::block_clip_id(app->timeline->model(), ref);
      if (!id.has_value()) continue;
      for (std::string& member :
           cutline::core::group_members(app->session.project(), *id)) {
        // A sweep across a linked pair reaches both blocks and each names the
        // whole group, so without this the same clip arrives twice.
        if (std::ranges::find(ids, member) == ids.end()) ids.push_back(std::move(member));
      }
    }

    app->session.select(std::move(ids));
    // The view highlighted what was swept; the session may say more, because a
    // group reaches clips the rectangle never touched. Rebuilt rather than
    // patched, for the usual reason: which blocks are in a group is the model's
    // answer, not the view's.
    refresh_timeline(*app);
    app->inspector_stale = true;
  });

  // The drag goes back through the model's own operations, so a move that a
  // neighbour would not allow is refused there rather than being allowed here
  // and looking different once the view is rebuilt.
  tracks.set_on_track_toggle([app](cutline::ui::TrackControlRef which) {
    if (app == nullptr || app->timeline == nullptr) return;
    if (which.track >= app->timeline->model().tracks.size()) return;

    app->session.apply(cutline::editor::toggle_track_switch(
        app->session.project(), app->timeline->model().tracks[which.track].id, which.control));
    // A mute silences the track; a solo silences every other one. Either way
    // the headers no longer say what they should, so the whole view is rebuilt
    // rather than the one switch that was pressed.
    refresh_timeline(*app);
    // Hiding a video track changes the picture. Muting does not, and asking for
    // a frame that has not changed costs a comparison.
    invalidate_preview(*app);
    // The sound was decoded from a project that is not this one any more.
    stop_playback(*app);
  });

  // The right-click. Premiere's clip menu, which is where most of what can be
  // done to a clip is actually reached from — the toolbar holds the handful of
  // things that are worth a permanent button and nothing else fits there. The
  // rows are the Clip menu's own; see `timeline_context_entries`.
  tracks.set_on_context_menu([app](double x, double y) {
    if (app == nullptr || app->main.host == nullptr || app->timeline == nullptr) return;

    // A right-click on a header is about the *track*, not about a clip. It used
    // to open the clip menu, which offered Cut and Copy over a place where
    // there is no clip to cut.
    if (const auto header = app->timeline->header_at(x, y); header.has_value()) {
      open_track_menu(*app, *header, x, y);
      return;
    }

    // At the pointer: there is no widget under a right-click to hang a menu
    // from, only a place.
    app->main.host->open_popup(std::make_unique<MenuList>(timeline_context_entries(*app)),
                               Rect{x, y, 0.0, 0.0});
  });

  // A lane dragged taller or shorter. The view has already resized itself so
  // the drag could be aimed; this writes it down, once, at the end.
  tracks.set_on_track_resize([app](std::size_t track, std::optional<double> height) {
    if (app == nullptr || app->timeline == nullptr) return;
    const auto& rows = app->timeline->model().tracks;
    if (track >= rows.size()) return;

    app->session.apply(
        cutline::core::set_track_height(app->session.project(), rows[track].id, height));
    // Not rebuilt: the view is already showing the height that was dragged to,
    // and rebuilding would take the same numbers back out of the project it
    // has just put them into.
    mark_dirty(*app);
  });

  // Renaming, from a double-click on a header. A popup with a field in it,
  // rather than a field the timeline holds: the view draws its headers and
  // builds no widgets in them, and giving it one for this alone would mean it
  // owning focus, a caret and a commit rule it has no other use for.
  tracks.set_on_marker_activate([app](std::size_t marker) {
    if (app != nullptr) open_marker_dialog(*app, marker);
  });

  tracks.set_on_track_rename([app](std::size_t track) {
    if (app != nullptr) rename_track(*app, track);
  });

  tracks.set_on_edit([app](const cutline::ui::TimelineEdit& edit) {
    if (app == nullptr || app->timeline == nullptr) return;
    const auto id = cutline::editor::block_clip_id(app->timeline->model(), edit.block);
    if (!id.has_value()) return;

    // The selection goes with it, so a move carries every clip that is
    // highlighted rather than only the one under the pointer. `selected_group`
    // rather than the raw selection, so linked partners travel too.
    const std::vector<std::string> carried = app->session.selected_group();
    std::vector<std::string> made;
    app->session.apply(cutline::editor::apply_timeline_edit(app->session.project(), *id, edit,
                                                            carried, &made));
    // An alt-drag leaves the originals where they were and the copies under the
    // pointer. Selecting the copies is what makes the nudge or the second drag
    // that usually follows act on what was just put down rather than on what it
    // came from.
    if (!made.empty()) app->session.select(made);
    // Rebuilt whether or not the edit applied: when it did not, the view is
    // showing where the pointer went rather than where the clip is allowed to
    // be, and it has to snap back.
    refresh_timeline(*app);
    // A cut leaves two clips where the panel was showing one, and a rate
    // stretch changes the speed the panel reads out. Neither is visible from
    // the block alone, so the inspector cannot be trusted to still be right.
    app->inspector_stale = true;
    invalidate_preview(*app);
  });

  if (app != nullptr) {
    refresh_timeline(*app);
  } else {
    // The headless check builds this with no session behind it, and an empty
    // timeline paints no clips at all — so a block too tall for its track in
    // one theme, or a label running past a block's edge, would never be seen.
    // The same reasoning as the pool's fallback above.
    tracks.set_model(cutline::editor::timeline_model(check_project(), {}, {}));
  }
  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_effects_panel(App* app) {
  // Only somewhere to put the controls: they are rebuilt from whatever is
  // selected, and a clip has more parameters than fit in a panel.
  auto panel = std::make_unique<Panel>();
  auto& scroll = panel->emplace<ScrollView>(Axis::Vertical);
  auto& rows = scroll.set_content(std::make_unique<Box>(Axis::Vertical));

  if (app != nullptr) {
    app->inspector = static_cast<Box*>(&rows);
    // Filled the first time the tab is shown, which may be long after startup.
    app->inspector_stale = true;
  }
  return panel;
}

/// The clip under a point, if dropping `id` there would do anything.
///
/// Works in the *main window's* coordinates. A drag that began in the library
/// keeps the pointer captured, so the moves arrive at the browser however far
/// away the cursor is — and the timeline it has to hit is a different panel
/// entirely. Nothing below this knows about both, which is why the answer is
/// worked out here.
///
/// Nothing when the pointer is not over a clip, or over one the entry could not
/// be applied to: a video effect over a waveform is a drop that has to decline
/// rather than land somewhere approximate.
/// Whether an entry could be applied to a clip, presets included.
///
/// `library_entry_fits` answers for the catalogue and has never seen a preset,
/// so this asks the saved set for those — and a preset carrying nothing the
/// clip could take is refused, exactly as an audio effect over a picture is.
[[nodiscard]] bool drop_fits(const App& app, const std::string& clip_id,
                             const std::string& id) {
  const std::string_view name = cutline::editor::preset_name_of(id);
  if (name.empty()) {
    return cutline::editor::library_entry_fits(app.session.project(), clip_id, id);
  }

  const cutline::editor::EffectPreset* saved =
      cutline::editor::find_preset(app.presets, name);
  if (saved == nullptr) return false;
  return cutline::editor::apply_preset(app.session.project(), clip_id, *saved) !=
         app.session.project();
}

/// What a drop on the program monitor would land on: whatever the monitor is
/// showing at the playhead.
///
/// Premiere's rule, and it is the one that makes the gesture worth having —
/// what you are looking at is what you are dropping onto, and there is nothing
/// to aim at because the picture is the target. The topmost video clip under
/// the playhead, because that is the one on screen.
///
/// Nothing when the pointer is not over the monitor, when nothing is under the
/// playhead, or when the entry could not be applied to what is: a transition
/// dropped on the picture has no join to sit on and declines rather than
/// landing somewhere approximate.
[[nodiscard]] std::optional<std::string> monitor_drop_target(const App& app,
                                                             const std::string& id, double x,
                                                             double y) {
  if (app.monitor == nullptr || id.empty()) return std::nullopt;
  if (!app.monitor->bounds().contains(x, y)) return std::nullopt;

  const cutline::core::Project& project = app.session.project();
  const double t = app.session.playhead();

  std::optional<std::string> found;
  for (const cutline::core::Track& track : project.sequence().tracks) {
    if (track.kind != cutline::core::TrackKind::Video || track.hidden) continue;
    for (const cutline::core::Clip& clip : track.clips) {
      if (clip.disabled) continue;
      const double end = clip.start + cutline::core::clip_duration(clip);
      if (t < clip.start || t >= end) continue;
      // Later tracks are drawn over earlier ones, so the last match is the one
      // on screen.
      found = clip.id;
    }
  }

  if (!found.has_value()) return std::nullopt;
  if (!drop_fits(app, *found, id)) return std::nullopt;
  return found;
}

[[nodiscard]] std::optional<std::string> library_drop_target(const App& app,
                                                             const std::string& id, double x,
                                                             double y) {
  // The monitor first: it sits above the timeline and a drag crossing one on
  // the way to the other should land on whichever it is actually over.
  if (const std::optional<std::string> shown = monitor_drop_target(app, id, x, y);
      shown.has_value()) {
    return shown;
  }

  if (app.timeline == nullptr || id.empty()) return std::nullopt;

  const std::optional<cutline::ui::BlockRef> block = app.timeline->block_at(x, y);
  if (!block.has_value()) return std::nullopt;

  const cutline::ui::TimelineModel& model = app.timeline->model();
  if (block->track >= model.tracks.size()) return std::nullopt;
  const auto& blocks = model.tracks[block->track].blocks;
  if (block->block >= blocks.size()) return std::nullopt;

  const std::string& clip_id = blocks[block->block].id;
  if (!drop_fits(app, clip_id, id)) return std::nullopt;
  return clip_id;
}

/// The bin a point in the library would drop into, declared here because the
/// outline below has to know and is written above it.
[[nodiscard]] std::string library_bin_at(const App& app, double x, double y);

/// Outlines the clip a drop would land on, or clears the outline.
///
/// An empty `id` means the drag is over, which is how a drop that lands on
/// nothing still tidies up after itself.
void show_library_drop(App& app, const std::string& id, double x, double y) {
  if (app.timeline == nullptr) return;

  // Cleared up front and set again at the end, so none of the several ways out
  // of this function can leave one drawn over a cut nothing is being held over.
  app.timeline->set_transition_ghost(std::nullopt);

  // Back inside the library, over a bin: the drop gathers rather than applies,
  // so the bin is outlined and nothing on the timeline is.
  const std::string bin = id.empty() ? std::string{} : library_bin_at(app, x, y);
  if (app.library != nullptr) {
    app.library->set_drop_folder(bin.empty() ? std::string{}
                                             : cutline::editor::bin_folder(bin));
  }
  if (!bin.empty()) {
    app.timeline->set_drop_target(std::nullopt);
    if (app.monitor != nullptr) app.monitor->set_drop_lit(false);
    return;
  }

  // Over the monitor, the whole picture is the target, so the timeline's
  // outline comes off and the monitor lights instead. Two outlines at once
  // would say the drop was going to two places.
  const bool over_monitor = !id.empty() && monitor_drop_target(app, id, x, y).has_value();
  if (app.monitor != nullptr) app.monitor->set_drop_lit(over_monitor);
  if (over_monitor) {
    app.timeline->set_drop_target(std::nullopt);
    return;
  }

  if (id.empty()) {
    app.timeline->set_drop_target(std::nullopt);
    return;
  }

  // Through `library_drop_target` rather than through `block_at` directly, so
  // the outline appears on exactly the clips a drop would be accepted by. An
  // outline on a clip that then refused the drop would be a lie.
  const std::optional<std::string> clip_id = library_drop_target(app, id, x, y);
  if (!clip_id.has_value()) {
    app.timeline->set_drop_target(std::nullopt);
    app.timeline->set_transition_ghost(std::nullopt);
    return;
  }
  app.timeline->set_drop_target(app.timeline->block_at(x, y));

  // A transition lands on the *join*, not on the clip, so an outline round the
  // clip says nothing about the thing anybody actually wants to know before
  // letting go: how much of the cut it will cover. Drawn at the length it would
  // be given, which is the same length the drop will ask for.
  std::optional<cutline::ui::TransitionGhost> ghost;
  if (const std::optional<cutline::core::TransitionKind> kind =
          cutline::editor::transition_from_id(id);
      kind.has_value()) {
    const cutline::editor::TransitionRow row =
        cutline::editor::clip_transition(app.session.project(), *clip_id);
    const std::optional<cutline::ui::BlockRef> block = app.timeline->block_at(x, y);
    if (row.joins && block.has_value()) {
      const double length = cutline::editor::default_transition_length(
          app.session.project(), *clip_id, *kind,
          app.settings.transition_lengths().for_kind(*kind));
      if (length > 0.0) {
        ghost = cutline::ui::TransitionGhost{
            .track = block->track,
            .block = block->block,
            .duration = length,
            .label = std::string(cutline::editor::transition_name(*kind))};
      }
    }
  }
  app.timeline->set_transition_ghost(std::move(ghost));
}

/// The bin a point in the library would drop into, or empty.
///
/// A point over a bin's heading or over anything already gathered in it counts,
/// which is what makes the whole folder a target rather than one row of it.
[[nodiscard]] std::string library_bin_at(const App& app, double x, double y) {
  if (app.library == nullptr || !app.library->bounds().contains(x, y)) return {};
  return std::string(cutline::editor::bin_of_folder(app.library->folder_at(y)));
}

/// A name nobody has used yet, for a bin made without one typed.
///
/// Premiere makes "Custom Bin 01" and lets you rename it; the same idea, because
/// a button that refuses to do anything until a field is filled is a button that
/// looks broken.
[[nodiscard]] std::string unused_bin_name(const App& app) {
  for (int n = 1;; ++n) {
    std::string name = "Bin " + std::to_string(n);
    if (cutline::editor::find_bin(app.bins, name) == nullptr) return name;
  }
}

/// Makes a bin, opens it, and leaves the name field empty for the next one.
void new_bin(App& app) {
  const std::string name = app.bin_name.empty() ? unused_bin_name(app) : app.bin_name;
  if (!cutline::editor::create_bin(app.bins, name)) {
    complain(app.main.window, "There is already a bin called " + name + ".");
    return;
  }

  app.bin_name.clear();
  if (app.library != nullptr) {
    // Opened, and its parent with it. A folder made where nothing can see it is
    // one somebody makes twice.
    app.library->set_open(std::string(cutline::editor::kBinFolder), true);
    app.library->set_open(cutline::editor::bin_folder(name), true);
  }
  keep_bins(app);
  if (app.library_name != nullptr) app.library_name->set_text({});
}

/// The menu a right-click in the library opens.
///
/// What it offers depends on what is under the pointer, which is the whole
/// reason it is a menu rather than a row of buttons: a bin's operations only
/// mean anything while a bin is in front of you.
void open_library_menu(App& app, double x, double y) {
  if (app.main.host == nullptr || app.library == nullptr) return;

  const std::string folder = app.library->folder_at(y);
  const std::string bin(cutline::editor::bin_of_folder(folder));
  const std::string id = app.library->selected();
  // An entry only counts as "in this bin" when the pointer is on it rather than
  // on the heading, or removing would take whatever happened to be selected.
  const std::size_t row = app.library->row_at(y);
  const bool on_entry = !bin.empty() && row < app.library->rows().size() &&
                        !app.library->rows()[row].id.empty();

  struct Item {
    std::string label;
    std::function<void()> act;
  };
  std::vector<Item> items;

  items.push_back(Item{"New Bin", [&app] { new_bin(app); }});
  if (!bin.empty()) {
    if (!app.bin_name.empty()) {
      // Renamed to what is typed in the field. There is no modal text prompt in
      // this application, and a bin name is not the place to introduce one.
      items.push_back(Item{"Rename Bin to \"" + app.bin_name + "\"", [&app, bin] {
                             if (!cutline::editor::rename_bin(app.bins, bin, app.bin_name)) {
                               complain(app.main.window,
                                        "There is already a bin called " + app.bin_name + ".");
                               return;
                             }
                             // The tree remembers what is open by path, so a
                             // renamed bin would close itself — carry it over,
                             // or renaming one you are working in hides it.
                             if (app.library != nullptr) {
                               const std::string was = cutline::editor::bin_folder(bin);
                               const bool open = app.library->is_open(was);
                               app.library->set_open(was, false);
                               app.library->set_open(
                                   cutline::editor::bin_folder(app.bin_name), open);
                             }
                             app.bin_name.clear();
                             if (app.library_name != nullptr) app.library_name->set_text({});
                             keep_bins(app);
                           }});
    }
    // Quoted, because "Delete Bin Bin 1" is what naming it without them reads
    // as the first time somebody makes a bin and does not name it.
    items.push_back(Item{"Delete Bin \"" + bin + "\"", [&app, bin] {
                           cutline::editor::remove_bin(app.bins, bin);
                           keep_bins(app);
                         }});
    if (on_entry) {
      items.push_back(Item{"Remove from Bin", [&app, bin, id] {
                             cutline::editor::remove_from_bin(app.bins, bin, id);
                             keep_bins(app);
                           }});
    }
  }

  std::vector<MenuEntry> entries;
  for (Item& item : items) {
    entries.push_back(action_entry(std::move(item.label), std::move(item.act)));
    // New Bin apart from what acts on the bin under the pointer.
    if (entries.size() == 1 && items.size() > 1) entries.push_back(MenuEntry::divider());
  }
  app.main.host->open_popup(std::make_unique<MenuList>(std::move(entries)), Rect{x, y, 0.0, 0.0});
}

/// The effects library: everything that can be applied, searchable.
///
/// Premiere has a panel for this, and it is the right shape. A menu was the
/// right answer while there were eleven effects and no panel machinery; it
/// stopped being the right answer as soon as transitions wanted to live beside
/// them, and it was already why the video and audio stacks needed separate
/// buttons opening separate menus.
[[nodiscard]] std::unique_ptr<Widget> make_library_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  // The search field above the tree, which is where every search field in every
  // application of this kind is.
  auto& search = panel->emplace<TextField>();
  search.set_placeholder("Search effects");

  auto& browser = panel->emplace<cutline::ui::EffectsBrowser>();

  browser.set_items([app] {
    if (app != nullptr) return library_entries(*app);

    std::vector<cutline::ui::EffectEntry> entries;
    for (const cutline::editor::LibraryEntry& entry : cutline::editor::effect_library()) {
      entries.push_back(cutline::ui::EffectEntry{
          .id = entry.id, .name = entry.name, .folder = entry.folder});
    }
    return entries;
  }());

  // As you type rather than on Enter. A library is read by narrowing it, and
  // having to commit each guess makes finding something a slower job than
  // scrolling would have been.
  search.set_on_change([control = &browser](const std::string& text) {
    control->set_filter(text);
  });

  // The bin bar along the bottom, which is where Premiere keeps its New Custom
  // Bin button. The field names the bin; empty, one is named for you.
  auto& bins = panel->emplace<Box>(Axis::Horizontal);
  auto& bin_name = bins.emplace<TextField>();
  bin_name.set_placeholder("Bin name");
  auto& new_bin_button = bins.emplace<Button>("New Bin", [app] {
    if (app != nullptr) new_bin(*app);
  });
  if (app == nullptr) new_bin_button.set_enabled(false);

  if (app != nullptr) {
    app->library = &browser;
    app->library_name = &bin_name;
    refresh_library(*app);

    // Only as it is typed. Committing would also fire when the keyboard left
    // the field, and a bin made by clicking away from a half-typed name is a
    // bin nobody asked for — the button is the gesture.
    bin_name.set_on_change([app](const std::string& text) { app->bin_name = text; });

    browser.set_on_context_menu([app](double x, double y) { open_library_menu(*app, x, y); });

    // Every clip named, against one project, applied once.
    //
    // Applying per clip would put one entry in the undo stack for each, so
    // undoing "blur these six" would be six presses. And each one is still
    // refused on its own terms: an audio effect on a picture, or a transition
    // where nothing abuts the clip, declines rather than appearing to work — so
    // a mixed selection takes the effect on the clips it suits and leaves the
    // rest alone, which is what Premiere does too.
    const auto apply_to = [app](std::span<const std::string> clip_ids, const std::string& id) {
      // A preset is applied from the saved set rather than by the binding: the
      // binding knows what an effect *type* means and has never seen a preset,
      // which is what keeps a settings file out of a pure layer.
      const std::string_view preset = cutline::editor::preset_name_of(id);
      const cutline::editor::EffectPreset* saved =
          preset.empty() ? nullptr : cutline::editor::find_preset(app->presets, preset);

      cutline::core::Project next = app->session.project();
      for (const std::string& clip_id : clip_ids) {
        next = saved != nullptr
                   ? cutline::editor::apply_preset(std::move(next), clip_id, *saved)
                   : cutline::editor::apply_library_entry(std::move(next), clip_id, id,
                                                          app->settings.transition_lengths());
      }
      app->session.apply(std::move(next));
      refresh_timeline(*app);
      invalidate_preview(*app);
      app->inspector_stale = true;
    };

    browser.set_on_choose([app, apply_to](const std::string& id) {
      // The whole selection, not the first of it. Selecting six clips and
      // double-clicking an effect meant five of them were quietly skipped.
      const auto selection = app->session.selection();
      if (selection.empty()) return;
      const std::vector<std::string> clip_ids(selection.begin(), selection.end());
      apply_to(clip_ids, id);
    });

    // Dragged onto a clip, which is how Premiere applies one and the only way
    // that does not need the right thing selected first.
    browser.set_on_drag([app](const std::string& id, double x, double y) {
      show_library_drop(*app, id, x, y);
    });
    browser.set_on_drop([app, apply_to](const std::string& id, double x, double y) {
      // Into a bin, when the drop landed back inside the library. Gathering
      // rather than applying: the same drag, and where it ends says which.
      const std::string bin = library_bin_at(*app, x, y);
      show_library_drop(*app, {}, 0.0, 0.0);
      if (!bin.empty()) {
        if (cutline::editor::add_to_bin(app->bins, bin, id)) {
          // Opened, so what has just been gathered can be seen. Dropping into a
          // shut folder and being shown nothing looks exactly like a drop that
          // was refused.
          app->library->set_open(cutline::editor::bin_folder(bin), true);
          keep_bins(*app);
        }
        return;
      }

      const std::optional<std::string> clip_id = library_drop_target(*app, id, x, y);
      if (!clip_id.has_value()) return;

      // Dropped onto one of several selected clips, it takes all of them —
      // Premiere's rule, and the one that makes dragging onto a selection worth
      // doing. Onto a clip outside the selection it takes only that clip, which
      // is what aiming at it meant.
      const auto selection = app->session.selection();
      const bool inside = std::ranges::find(selection, *clip_id) != selection.end();
      if (inside) {
        apply_to(std::vector<std::string>(selection.begin(), selection.end()), id);
      } else {
        apply_to(std::vector{*clip_id}, id);
      }
    });
  }
  return panel;
}

/// The scopes, with a row of tabs to choose between them.
/// Goes to the state after `steps` edits, as a click in the History panel does,
/// and brings everything that shows the document along.
void step_history_to(App& app, std::size_t steps) {
  if (!app.session.step_to(steps)) return;
  refresh_all(app);
  show_playhead(app);
  mark_dirty(app);
}

/// Fills the History panel from the session, when its steps have changed.
///
/// The first row is the project as it was opened, Premiere's "New/Open"; each
/// row after it is the state an edit left. The current one is highlighted and
/// the ones undone are faded but can still be clicked, which redoes back to
/// them — Premiere's arrangement, and the reason this is a list and not a pair
/// of buttons.
void refresh_history(App& app) {
  if (app.history_list == nullptr) return;
  std::vector<std::string> names = app.session.undo_names();
  const std::size_t at = names.size();
  for (std::string& name : app.session.redo_names()) names.push_back(std::move(name));
  if (names == app.history_shown && at == app.history_at) return;
  app.history_shown = names;
  app.history_at = at;

  std::vector<MenuEntry> rows;
  rows.reserve(names.size() + 1);
  rows.push_back(MenuEntry{.label = "New/Open", .act = [&app] { step_history_to(app, 0); }});
  for (std::size_t i = 0; i < names.size(); ++i) {
    rows.push_back(MenuEntry{.label = names[i].empty() ? std::string("Edit") : names[i],
                             .faded = i >= at,
                             .act = [&app, steps = i + 1] { step_history_to(app, steps); }});
  }
  app.history_list->set_entries(std::move(rows));
  app.history_list->set_current(at);
  mark_dirty(app);
}

[[nodiscard]] std::unique_ptr<Widget> make_history_panel(App* app) {
  auto panel = std::make_unique<Panel>();
  // Scrolled: a hundred steps is the default depth, and a panel tall enough for
  // all of them is not one anybody has.
  auto& scroll = panel->emplace<ScrollView>(Axis::Vertical);
  auto& list = static_cast<MenuList&>(scroll.set_content(std::make_unique<MenuList>()));
  // A click is a step, and a step is the panel's whole purpose; the list stays
  // where it is rather than closing like a menu would.
  list.set_on_dismiss([] {});
  list.set_fills_height(true);
  if (app != nullptr) {
    app->history_list = &list;
    app->history_shown.clear();
    app->history_at = static_cast<std::size_t>(-1);
    refresh_history(*app);
  }
  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_scopes_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& tabs = panel->emplace<Box>(Axis::Horizontal);
  auto& view = panel->emplace<cutline::ui::ScopesView>();
  if (app != nullptr) {
    app->scopes = &view;
    view.set_kind(app->scope_kind);
    // Whatever was last measured. The panel may be rebuilt by a rearrangement
    // or a theme change long after the frame it is showing was rendered, and a
    // scope that went blank because a tab moved would be a puzzle.
    view.set_readings(app->scope_readings);
  }

  // One button that cycles rather than four tabs. Four names as long as
  // "Vectorscope" do not fit a panel docked in a narrow column — the first
  // attempt lost two of them off the edge — and this is the idiom the sort
  // button already established for a short list in a tight row.
  auto& choice = tabs.emplace<Button>(
      std::string(cutline::ui::to_string(app == nullptr ? cutline::ui::ScopeKind::Histogram
                                                        : app->scope_kind)),
      [app] {
        if (app == nullptr) return;
        constexpr std::array kOrder{
            cutline::ui::ScopeKind::Histogram, cutline::ui::ScopeKind::Waveform,
            cutline::ui::ScopeKind::Parade, cutline::ui::ScopeKind::Vectorscope};
        const auto at = std::ranges::find(kOrder, app->scope_kind);
        const std::size_t next =
            at == kOrder.end() ? 0 : (static_cast<std::size_t>(at - kOrder.begin()) + 1) %
                                         kOrder.size();
        choose_scope(*app, kOrder[next]);
      });
  tabs.emplace<Spacer>();
  if (app != nullptr) app->scope_button = &choice;

  if (app != nullptr) invalidate_scopes(*app);
  return panel;
}

/// The source monitor: one source, its own playhead, and the marks that decide
/// which part of it gets placed.
[[nodiscard]] std::unique_ptr<Widget> make_source_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  // The name of what is showing, and the way to the others. One control rather
  // than a label and a list, which is what Premiere does and for the reason
  // that the name *is* the thing you press to change it.
  //
  // The zoom sits at the other end of the same row rather than down beside the
  // transport, where the program monitor keeps its own and where Premiere keeps
  // both. That row is a third the width of the program monitor's and has twice
  // been cut down to fit — `--check` said 5 clipped and 5 squeezed in every
  // theme the moment a seventh control went on it. This row holds one control
  // and the whole width of the panel, and "what is showing" and "how closely
  // you are looking at it" are the same kind of statement about it.
  auto& header = panel->emplace<Box>(Axis::Horizontal);
  auto& choice = header.emplace<Dropdown>(std::vector<std::string>{"No source"});
  if (app != nullptr) {
    app->source_choice = &choice;
    choice.set_on_change([app](std::size_t index) {
      if (index >= app->source_recent.size()) return;
      show_source(*app, app->source_recent[index]);
    });
  }
  header.emplace<Spacer>();

  auto& picture = panel->emplace<MonitorView>();
  picture.set_placeholder("Select something in the project panel.");
  if (app != nullptr) {
    app->source_monitor = &picture;
    show_safe_margins(*app);
    if (!app->shown_source.empty()) picture.set_texture(app->shown_source);
    // Premiere's shortest statement of "use this": what is in the monitor is
    // what is about to be placed, so drag it where it goes. The marked span
    // comes with it, exactly as it does from the pool.
    picture.set_on_drag_out([app](double x, double y) {
      if (app->timeline == nullptr) return;
      app->timeline->set_drop_ghost(std::nullopt);
      const auto where = app->timeline->drop_at(x, y);
      if (!where.has_value()) return;  // released somewhere that means nothing
      place_media_from(*app, app->session.source_media(), where);
    });
    add_zoom_control(header, picture, app->source_zoom, [app](double zoom) {
      app->source_zoom = zoom;
      if (app->source_monitor != nullptr) app->source_monitor->set_zoom(zoom);
      mark_dirty(*app);
    });
  }

  auto& envelope = panel->emplace<WaveformView>();
  envelope.set_visible(false);
  if (app != nullptr) app->source_waveform = &envelope;

  auto& bar = panel->emplace<ScrubBar>();
  // So a click on it takes the keyboard, which is what makes the transport and
  // the marking keys land here rather than on the sequence.
  bar.set_focusable(true);
  if (app != nullptr) {
    app->source_panel = panel.get();
    app->source_scrub = &bar;
    // The bar does not move its own playhead. This decides where the playhead
    // really is — snapped to the source's own frame grid — and sets it back, so
    // the picture and the mark that follows it agree on which frame is meant.
    bar.on_scrub = [app](double time) {
      const cutline::core::Media* media = source_media_of(*app);
      const double fps =
          media != nullptr ? cutline::core::playback_fps(*media).value_or(0.0) : 0.0;
      app->source_playhead = fps > 0.0 ? cutline::core::snap_to_frame(time, fps) : time;
      refresh_source(*app);
    };
  }

  auto& row = panel->emplace<Box>(Axis::Horizontal);

  // A frame either way. The buttons and the arrow keys go through the same
  // call, so the two can never disagree about what a frame is.
  //
  // Drawn icons rather than "<" and ">", which is what the rest of the
  // application uses and — the reason it changed — half the width. Spelled out,
  // this row and a Play button together were fourteen controls squeezed in two
  // themes of four.
  auto& back = row.emplace<IconButton>(IconButton::Icon::ArrowLeft, [app] {
    if (app != nullptr) step_source(*app, -1.0);
  });
  back.set_tooltip(with_keys(app, "Back one frame", "playback.step_back"));
  back.set_narrow(true);
  auto& forward = row.emplace<IconButton>(IconButton::Icon::ArrowRight, [app] {
    if (app != nullptr) step_source(*app, 1.0);
  });
  forward.set_tooltip(with_keys(app, "On one frame", "playback.step_forward"));
  // A pair that belongs together, so they take about the room one control
  // would — which is what buys the Play button its place on the row.
  forward.set_narrow(true);

  auto& play = row.emplace<Button>("Play", [app] {
    if (app != nullptr) toggle_source_playback(*app);
  });
  play.set_tooltip(with_keys(app, "Play the source, with its sound", "playback.play"));
  if (app != nullptr) app->source_play_button = &play;

  // "In" and "Out" rather than "Mark In" and "Mark Out": this panel is a third
  // of the width the program monitor's row has, and with the two step buttons
  // beside them `--check` found fourteen controls squeezed and clipped in three
  // themes of four. The tooltips carry the whole name.
  auto& mark_in = row.emplace<Button>("In", [app] {
    if (app != nullptr) mark_source(*app, false);
  });
  mark_in.set_tooltip(with_keys(app, "Mark in on the source", "markers.mark_in"));
  auto& mark_out = row.emplace<Button>("Out", [app] {
    if (app != nullptr) mark_source(*app, true);
  });
  mark_out.set_tooltip(with_keys(app, "Mark out on the source", "markers.mark_out"));
  // A cross rather than the word, which is the last thing Aero's roomier chrome
  // had room for — spelled out it was fourteen controls squeezed, in that theme
  // alone. Taking both marks away is a rarer thing to want than setting either.
  auto& clear = row.emplace<IconButton>(IconButton::Icon::Cross, [app] {
    if (app == nullptr) return;
    if (const cutline::core::Media* media = source_media_of(*app); media != nullptr) {
      app->session.apply(
          cutline::core::clear_source_marks(app->session.project(), media->id));
      refresh_all(*app);
    }
  });
  clear.set_tooltip("Take both of the source's marks away");
  row.emplace<Spacer>();

  return panel;
}

[[nodiscard]] std::unique_ptr<Widget> make_panel(App* app, const PanelId& id) {
  if (id == "scopes") return make_scopes_panel(app);
  if (id == "history") return make_history_panel(app);
  if (id == "source") return make_source_panel(app);
  if (id == "project") return make_project_panel(app);
  if (id == "monitor") return make_monitor_panel(app);
  if (id == "timeline") return make_timeline_panel(app);
  if (id == "effects") return make_effects_panel(app);
  if (id == "library") return make_library_panel(app);
  if (id == "audio") return make_audio_panel(app);
  if (id == "queue") return make_queue_panel(app);
  if (id == "graphics") return make_graphics_panel(app);
  if (id == "lumetri") return make_lumetri_panel(app);

  // A layout naming a panel this build does not have. Saying so beats an empty
  // rectangle, which looks like a panel that failed to draw.
  auto missing = std::make_unique<Panel>();
  missing->emplace<Label>("There is no panel called " + id).set_small(true);
  return missing;
}

}  // namespace cutline::tool
