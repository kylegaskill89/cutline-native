/// A window to actually look at the interface in.
///
/// Everything under `src/ui` is tested without a window on purpose, and that
/// catches a great deal — but not whether the result is something a person
/// would want to use. This puts the real widget tree on screen, in each of the
/// built-in themes, so the claim that they differ in *chrome* rather than in
/// colour can be checked by looking at it.
///
/// The chrome is drawn by Skia onto a Direct3D 12 swapchain. It used to go into
/// a CPU raster surface and be blitted with GDI, on the argument that interface
/// drawing is a few hundred rounded rectangles a frame and the GPU would be no
/// measurable gain. That argument was made without measuring and it was wrong:
/// `--benchmark` puts an Aero frame at 1080p at well over half a second on the
/// CPU and under fifty milliseconds on the GPU, because every glass surface
/// wants its own blurred layer and filling pixels is precisely what a GPU is
/// for. Video frames still have their own path.
///
/// Not the editor. A harness for the parts of it that exist.

#include "app.hpp"
#include "gallery.hpp"

namespace cutline::tool {

/// What a new document is: an empty sequence with somewhere to put things.
///
/// One video track and one audio track rather than none. A timeline with no
/// tracks has nowhere to drop the first import, and "add a track before you can
/// do anything" is a step no editor asks for. More are a menu away.
[[nodiscard]] cutline::core::Project new_project_model() {
  cutline::core::Project project;
  project.sequence().fps = 30.0;
  project.sequence().tracks = {
      cutline::core::Track{.id = "v1", .kind = cutline::core::TrackKind::Video},
      cutline::core::Track{.id = "a1", .kind = cutline::core::TrackKind::Audio},
  };
  return project;
}

/// A project with something in it, for the headless check only.
///
/// `--check` lays out and paints every panel in every theme, and a panel with
/// nothing in it proves nothing: an empty pool has no rows to be too tall, and
/// an empty timeline has no clip to be clipped by the edge of its track. This
/// is the only caller — the application itself starts empty, the way every
/// other editor does.
[[nodiscard]] cutline::core::Project check_project() {
  using namespace cutline::core;

  Project project;
  // 29.97, so the headless pass lays out the drop-frame control at all — it is
  // only shown where it means something, and at 30 it means nothing.
  project.sequence().fps = 30000.0 / 1001.0;
  project.media = {
      // With a rate on it, because real footage has one and because Interpret
      // Footage is only offered where there is a rate to conform *from* — so
      // without this the check cannot reach that box at all.
      Media{.id = "wide", .name = "wide.mp4", .duration = 120.0, .has_video = true,
            .fps = 60.0},
      Media{.id = "close", .name = "close.mp4", .duration = 120.0, .has_video = true},
      Media{.id = "title", .name = "title.png", .duration = 10.0, .has_video = true,
            .is_image = true},
      Media{.id = "dialogue", .name = "dialogue.wav", .duration = 120.0,
            .audio_stream_count = 1},
      Media{.id = "score", .name = "score.wav", .duration = 120.0, .audio_stream_count = 1},
  };
  // Two levels of bin with something filed in each, so the headless check
  // paints an indented tree in every theme rather than the flat list it would
  // otherwise only ever see. A row too tall or a name cut off by an indent is
  // exactly what this catches and nothing else would.
  project.bins = {Bin{.id = "bin_pictures", .name = "Pictures"},
                  Bin{.id = "bin_sound", .name = "Sound", .parent = "bin_pictures"}};
  project.media[0].bin = "bin_pictures";
  project.media[3].bin = "bin_sound";
  // One labelled entry, so the check paints the stripe as well as the tree.
  project.media[1].label_color = "#8f7bb8";

  const auto clip = [](std::string id, std::string media, double start, double length,
                       TrackKind kind = TrackKind::Video) {
    return Clip{.id = std::move(id), .media_id = std::move(media), .kind = kind,
                .source_in = 0.0, .source_out = length, .start = start};
  };

  Track upper{.id = "v2", .kind = TrackKind::Video};
  upper.clips = {clip("t1", "title", 6.0, 5.5), clip("t2", "title", 24.0, 5.0)};

  Track lower{.id = "v1", .kind = TrackKind::Video};
  lower.clips = {clip("c1", "wide", 0.0, 6.2), clip("c2", "close", 6.2, 7.8),
                 clip("c3", "wide", 14.0, 8.5), clip("c4", "close", 22.5, 15.5)};

  Track dialogue{.id = "a1", .kind = TrackKind::Audio};
  dialogue.clips = {clip("d1", "dialogue", 0.0, 22.5, TrackKind::Audio),
                    clip("d2", "dialogue", 22.5, 15.5, TrackKind::Audio)};

  Track music{.id = "a2", .kind = TrackKind::Audio, .muted = true};
  music.clips = {clip("m1", "score", 2.0, 36.0, TrackKind::Audio)};

  project.sequence().tracks = {std::move(upper), std::move(lower), std::move(dialogue),
                    std::move(music)};

  // A second sequence, so the headless pass lays out the tab strip. The strip
  // is only drawn when there is something to switch between, which means a
  // single-sequence scene never measures it at all — and it is the one part of
  // this feature that cannot be checked any other way.
  project.sequence().id = "seq-main";
  project.sequence().name = "Main";
  project = add_sequence(std::move(project), "Alternate");
  return project;
}

/// The panels this build has, which is what a saved arrangement is measured
/// against: one naming a panel that has since gone shows a tab nothing can
/// fill, and one written before a panel existed leaves it unreachable.
[[nodiscard]] std::vector<PanelId> known_panels() {
  std::vector<PanelId> panels;
  for (const auto& [id, title] : kPanels) panels.emplace_back(id);
  return panels;
}

[[nodiscard]] std::string panel_title(const PanelId& id) {
  using Entry = std::pair<std::string_view, std::string_view>;
  const auto found = std::ranges::find(kPanels, id, &Entry::first);
  return found == kPanels.end() ? id : std::string(found->second);
}

/// The arrangement a window with no session behind it shows. Only the headless
/// check has one of those; everything else reads it from the active workspace.
[[nodiscard]] DockLayout default_layout() {
  return cutline::editor::built_in_workspaces().front().layout;
}

/// What a window's display asks of it, as a scale.
[[nodiscard]] double scale_of(HWND window) {
  const UINT dpi = window != nullptr ? GetDpiForWindow(window) : 0;
  return dpi == 0 ? 1.0 : static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI;
}

/// A size in interface units, as pixels on a window at this scale.
[[nodiscard]] int to_pixels(double units, double scale) {
  return static_cast<int>(std::lround(units * scale));
}

/// A point in a window's client pixels, in the units its tree is laid out in.
[[nodiscard]] std::pair<double, double> to_units(const Shell& shell, POINT local) {
  return {static_cast<double>(local.x) / shell.scale, static_cast<double>(local.y) / shell.scale};
}

/// Where the panels are: the active workspace's layout.
///
/// There is exactly one of these at a time and it is owned by the workspace, so
/// dragging a panel is remembered against the arrangement it was dragged in.
[[nodiscard]] DockLayout& layout_of(App& app) {
  DockLayout* showing = app.workspaces.current();
  // `settle` guarantees there is always one. This is the last resort so that a
  // programming mistake is an empty window rather than a crash.
  static DockLayout nothing;
  return showing == nullptr ? nothing : *showing;
}

/// Marks every window as needing to be drawn again.
///
/// Most changes are to the document or the theme, and those show in whichever
/// windows happen to be open rather than in one of them.
void mark_dirty(App& app) {
  for (Shell* shell : app.shells()) shell->dirty = true;
}

/// A menu row that runs something, with the keys that do the same written
/// beside it. Only words: whoever handles the keys is elsewhere.
[[nodiscard]] MenuEntry action_entry(std::string label, std::function<void()> act,
                                     std::string keys, bool enabled) {
  return MenuEntry{.label = std::move(label), .shortcut = std::move(keys), .enabled = enabled,
                   .act = std::move(act)};
}

/// A menu row that is a switch, ticked when it is on.
[[nodiscard]] MenuEntry switch_entry(std::string label, bool on, std::function<void()> act,
                                     std::string keys, bool enabled) {
  return MenuEntry{.label = std::move(label), .shortcut = std::move(keys), .enabled = enabled,
                   .checked = on, .act = std::move(act)};
}

/// A menu row that opens another beside it, greyed when that would be empty.
[[nodiscard]] MenuEntry submenu_entry(std::string label, std::vector<MenuEntry> rows) {
  const bool any = !rows.empty();
  return MenuEntry{.label = std::move(label), .enabled = any, .submenu = std::move(rows)};
}

/// Asks for any envelope or filmstrip the project needs and does not have.
///
/// Here rather than in the importer because that is not the only way a source
/// arrives: an undo, a paste, and a project opened from disk all bring media
/// that never went past it. Requests already known or already queued cost
/// nothing, which is what makes calling this on every rebuild reasonable.
void request_media([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  const cutline::core::Project& project = app.session.project();
  for (const cutline::core::Track& track : project.sequence().tracks) {
    const bool audio = track.kind == cutline::core::TrackKind::Audio;
    for (const cutline::core::Clip& clip : track.clips) {
      const auto media =
          std::ranges::find(project.media, clip.media_id, &cutline::core::Media::id);
      if (media == project.media.end()) continue;

      if (audio) {
        app.waveforms.request(media->id, media->path, clip.audio_stream);
        continue;
      }
      // A still has one frame that never changes and a generated source has no
      // file, so neither is a filmstrip — asking would be an error reported
      // once a frame for as long as the clip is on screen.
      if (!media->has_video || cutline::core::is_generated_media(*media) || media->is_image) {
        continue;
      }

      // Only the stretch of the source that is actually on screen, translated
      // from timeline time through the clip's own trim and speed. A ten-minute
      // capture at an ordinary zoom shows about thirteen seconds of itself, and
      // asking for the whole file to draw those was 48 seeks across a 4K
      // capture — the hundred seconds of processor time that made dropping a
      // clip feel like a crash.
      if (app.timeline == nullptr) continue;
      const cutline::ui::TimeScale& view = app.timeline->scale();
      const double left = view.start;
      const double right = left + view.visible_duration(app.timeline->time_area().width);

      const double ends = cutline::core::clip_end(clip);
      if (ends <= left || clip.start >= right) continue;  // not on screen at all

      // Clamped to the clip, then carried into the source. `source_in` is where
      // the clip starts in the file, and a clip playing at speed covers more of
      // the source than it occupies on the timeline.
      const double speed = cutline::core::clip_speed(clip);
      const double from_edge = std::max(0.0, left - clip.start);
      const double to_edge = std::min(ends, right) - clip.start;

      // A margin either side, so scrolling gently does not ask for a new
      // stretch on every frame of the movement.
      const double margin = (to_edge - from_edge) * 0.5;
      const double from = clip.source_in + std::max(0.0, from_edge - margin) * speed;
      const double to = clip.source_in + (to_edge + margin) * speed;
      app.filmstrips.request(media->id, media->path, from, std::min(to, media->duration));
    }
  }
#endif
}

/// Greys out the commands that have nothing to act on.
///
/// Through `can_run` rather than by asking the selection here, so a button is
/// enabled exactly when pressing it would do something — the two cannot drift,
/// because there is only one answer.
void refresh_command_buttons(App& app) {
  if (app.link_button != nullptr) {
    app.link_button->set_enabled(cutline::editor::can_run(app.session,
                                                          cutline::editor::Command::LinkClips));
  }
  if (app.unlink_button != nullptr) {
    app.unlink_button->set_enabled(
        cutline::editor::can_run(app.session, cutline::editor::Command::UnlinkClips));
  }
}

/// Says what the workers are still reading, or nothing when they are idle.
///
/// Called from the timeline refresh, which is where the sources get asked for,
/// and again when one arrives — the two ends of the work. Polling it on a timer
/// instead would mean the label appeared a beat after the machine got busy,
/// which is the beat that matters.
void refresh_busy(App& app) {
  if (app.busy_label == nullptr) return;
#if CUTLINE_HAVE_PREVIEW
  const std::size_t left = app.waveforms.pending() + app.filmstrips.pending();
  // Counted rather than a bare "working": four audio streams and a filmstrip is
  // five things, and a number that comes down is the difference between waiting
  // and wondering.
  std::string says = left == 0 ? std::string{} : std::format("Reading media ({})", left);
  if (analysing_stabilizer(app)) says = "Analyzing for Warp Stabilizer";
  if (detecting_scenes(app)) says = "Detecting scene edits";
  if (remixing(app)) says = "Analyzing music for Remix";
  if (analysing_reframe(app)) says = "Analyzing for Auto Reframe";
  if (analysing_morph(app)) says = "Analyzing for Morph Cut";
  if (tracking_mask(app)) says = "Tracking mask";
  // Rendering previews comes before reading, which it outlasts.
  if (rendering_previews(app)) {
    says = std::format("Rendering previews {} of {}", app.preview_render_done.load() + 1,
                       app.preview_render_total);
  }

  // Proxies take precedence over the reading, because they take minutes where
  // the reading takes seconds — and a line that flickered between the two would
  // say less than either. Named and given a percentage for the same reason the
  // rest is counted: something that will run for five minutes has to show it is
  // moving, or it is indistinguishable from something stuck.
  if (const auto making = app.proxies.progress()) {
    // The name is elided rather than let run. This label sits in the corner of
    // the project panel with the version badge at the other end, and a camera
    // file called A001_C003_0410XX_001.R3D would push it off the edge — which
    // `--check` cannot catch, because nothing is being transcoded when it runs.
    constexpr std::size_t kLongestName = 20;
    std::string name = making->name;
    if (name.size() > kLongestName) name = name.substr(0, kLongestName - 3) + "...";

    says = std::format("Proxy: {} {}%", name, static_cast<int>(making->done * 100.0));
    if (making->queued > 0) says += std::format(" (+{})", making->queued);
  }
#else
  std::string says;
#endif
  if (says == app.busy_label->text()) return;
  // Hidden rather than merely blank when there is nothing to say. `--check`
  // counts a widget that takes up room and draws nothing as a fault, and it is
  // right to: an idle editor should have an empty corner, not an empty label
  // holding a space open for one.
  app.busy_label->set_visible(!says.empty());
  app.busy_label->set_text(std::move(says));
  mark_dirty(app);
}

/// Rebuilds what the timeline draws from the session.
///
/// Everything goes through here rather than being patched in place, which is
/// what keeps the view from drifting out of step with the document — an undo
/// changes the project in ways no incremental update could follow.
void refresh_timeline(App& app) {
  refresh_command_buttons(app);
  if (app.timeline == nullptr) return;

  request_media(app);
  refresh_busy(app);
  refresh_render_bar(app);
  refresh_stabilizer_analysis(app);
  show_stabilizer_banner(app);
  refresh_reframe_analysis(app);
  refresh_morph_analysis(app);
  app.timeline->set_model(cutline::editor::timeline_model(
      app.session.project(), app.session.selection(), app.timeline_media));
  app.timeline->set_playhead(app.session.playhead());
  refresh_handles(app);
  mark_dirty(app);
}

/// Puts the handles on whatever is selected, or takes them away.
///
/// Driven from the same refresh as the timeline, because that is where a change
/// of selection or of playhead lands, and both move the box: a keyframed
/// transform is a different rectangle at a different moment.
///
/// The one exception is while a handle is being dragged. The view is showing
/// the box the pointer is describing and the model is a frame behind it, so
/// writing the model's answer back mid-gesture would fight the drag — which
/// looked, on screen, like the layer sticking every few pixels.
///
/// Reads `showing()` rather than the document, so that scrubbing Position in
/// the inspector moves the box in the monitor. The two are the same picture
/// described twice, and a box that stayed put while the number went by would be
/// the more convincing of the two — it is drawn over the frame itself.
void refresh_handles(App& app) {
  if (app.monitor == nullptr) return;
  if (app.dragging_handle) return;

  const cutline::core::Project& document = app.showing();
  const auto selection = app.session.selection();
  // A layer of a graphic, when one is chosen in Essential Graphics: the handles
  // go round it, as Premiere's do, rather than round the whole frame-sized
  // graphic.
  std::optional<cutline::ui::MonitorBox> box;
  if (!selection.empty() && !app.graphic_layer.empty()) {
    box = cutline::editor::graphic_layer_box(document, selection.front(), app.graphic_layer,
                                             app.session.playhead(), layer_measurer());
  }
  if (!box.has_value() && !selection.empty()) {
    box = cutline::editor::monitor_box(document, selection.front(), app.session.playhead());
  }
  app.monitor->set_transform(box);

  // The masks on whatever is selected, drawn where they actually fall on the
  // frame. Which effect each belongs to is remembered here, because the widget
  // knows only that it has some shapes to draw.
  app.mask_effects.clear();
  std::vector<cutline::ui::MaskOverlay> shapes;
  if (!selection.empty()) {
    for (const cutline::editor::MaskOverlayRef& mask : cutline::editor::mask_overlays(
             document, selection.front(), app.session.playhead())) {
      app.mask_effects.push_back(mask.effect);
      shapes.push_back(mask.overlay);
    }
  }
  app.monitor->set_masks(std::move(shapes));
}

/// Puts the document's name where it can be seen — in the caption the theme
/// draws, and in the window text the taskbar reads.
void refresh_title(App& app) {
  const std::string title = app.session.document_title() + " - Cutline";
  if (app.main.title_bar != nullptr) app.main.title_bar->set_title(title);
  if (app.main.window != nullptr) SetWindowTextA(app.main.window, title.c_str());
  mark_dirty(app);
}

/// Everything a view needs told after the document has been replaced.
void show_safe_margins(App& app) {
  const cutline::core::SafeAreas& areas = app.session.project().safe_areas;
  const cutline::ui::MonitorView::SafeMargins margins{.title_x = areas.title_width / 100.0,
                                                      .title_y = areas.title_height / 100.0,
                                                      .action_x = areas.action_width / 100.0,
                                                      .action_y = areas.action_height / 100.0};
  // Halved: Premiere's percentages are of the whole width, split between the
  // two sides.
  cutline::ui::MonitorView::SafeMargins sides = margins;
  sides.title_x /= 2.0;
  sides.title_y /= 2.0;
  sides.action_x /= 2.0;
  sides.action_y /= 2.0;
  if (app.monitor != nullptr) {
    app.monitor->set_safe_margins(app.settings.safe_margins_program
                                      ? std::optional(sides)
                                      : std::nullopt);
  }
  if (app.source_monitor != nullptr) {
    app.source_monitor->set_safe_margins(app.settings.safe_margins_source
                                             ? std::optional(sides)
                                             : std::nullopt);
  }
  mark_dirty(app);
}

void show_display_options(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.preview != nullptr) {
    app.preview->set_display_options(
        {.mode = app.program_alpha ? cutline::gpu::DisplayMode::Alpha
                                   : cutline::gpu::DisplayMode::Composite,
         .transparency_grid = app.settings.transparency_grid_program});
  }
  if (app.source_preview != nullptr) {
    app.source_preview->set_display_options(
        {.mode = app.source_alpha ? cutline::gpu::DisplayMode::Alpha
                                  : cutline::gpu::DisplayMode::Composite,
         .transparency_grid = app.settings.transparency_grid_source});
  }
#endif
  invalidate_preview(app);
  mark_dirty(app);
}

void refresh_all(App& app) {
  show_safe_margins(app);
  // After an undo, or anything else, the cut may have moved or gone.
  if (app.trim_point.has_value()) show_trim(app);
  refresh_timeline(app);
  refresh_browser(app);
  refresh_title(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  // The readout is a *timecode*, so how the project counts them is part of what
  // it says. Nothing else here would repaint it: `refresh_timeline` redraws the
  // ruler and leaves the field alone, which is right while somebody is typing
  // into it and wrong the moment the counting changes underneath.
  show_playhead(app);
  // The mixer describes the tracks, so anything that changes them changes it:
  // a track added or removed, a route sent somewhere else, a stack given its
  // first effect. Deferred, because what asked is often a control inside a
  // strip about to be rebuilt.
  app.mixer_stale = true;
  // Project Settings shows the project, so a change to the project is a change
  // to that page — setting the rate to 29.97 is exactly what makes the
  // drop-frame choice appear, and it appeared nowhere until this was written.
  // Deferred like the inspector, since what asked is usually a control inside
  // the page about to be rebuilt.
  if (app.settings_window != nullptr && app.settings_kind == SettingsKind::Project) {
    app.settings_page_stale = true;
  }
  // The master fader belongs to the document, so a loaded project moves it.
  if (app.master_fader != nullptr) {
    app.master_fader->set_value(
        cutline::ui::gain_to_fader_db(app.session.project().sequence().master_gain));
    show_master_gain(app, app.session.project().sequence().master_gain);
  }
  if (app.monitor != nullptr) {
    const cutline::core::Project& project = app.session.project();
    app.monitor->set_canvas_size(project.sequence().canvas_w, project.sequence().canvas_h);
  }
  // After the browser, which is what may have changed which source is chosen.
  refresh_source(app);
  if (app.main.host != nullptr) app.main.host->request_layout();
}

std::string shortcut_of(const App* app, std::string_view action) {
  static const cutline::editor::Keymap premiere =
      cutline::editor::preset_keymap(cutline::editor::kPremierePreset);
  const cutline::editor::Keymap& keys = app != nullptr ? app->keys : premiere;
  const auto chords = keys.chords(action);
  return chords.empty() ? std::string{} : cutline::editor::to_text(chords.front());
}

std::string shortcut_for(const App* app, cutline::editor::Command command) {
  const cutline::editor::Action* action = cutline::editor::action_for(command);
  return action == nullptr ? std::string{} : shortcut_of(app, action->id);
}

std::string with_keys(const App* app, std::string text, std::string_view action) {
  const std::string keys = shortcut_of(app, action);
  return keys.empty() ? text : text + " (" + keys + ")";
}

void save_keys(App& app) {
  if (const auto written = cutline::editor::write_keymap(cutline::editor::default_keys_path(),
                                                         app.keys);
      !written.has_value()) {
    complain(app.main.window, "Could not save the keyboard shortcuts.\n\n" + written.error());
  }
}

void run_command(App& app, cutline::editor::Command command);

/// A menu row for an editing command: its key beside it, greyed when it has
/// nothing to act on, and running exactly what the key runs.
[[nodiscard]] MenuEntry command_entry(App& app, std::string label,
                                      cutline::editor::Command command) {
  return MenuEntry{.label = std::move(label),
                   .shortcut = shortcut_for(&app, command),
                   .enabled = cutline::editor::can_run(app.session, command),
                   .act = [&app, command] { run_command(app, command); }};
}

/// Pushes the snap setting into the view and lights the button for it.
///
/// The setting lives on the application rather than on the view, because the
/// view is destroyed and rebuilt by a rearrangement or a theme change — and
/// snapping turning itself back on when a panel moved would be a setting
/// nobody could rely on.
void show_snapping(App& app) {
  if (app.timeline != nullptr) app.timeline->set_snapping(app.snapping);
  if (app.snap_button != nullptr) app.snap_button->set_selected(app.snapping);
  mark_dirty(app);
}

void toggle_snapping(App& app) {
  app.snapping = !app.snapping;
  save_settings(app);
  show_snapping(app);
}

/// Picks a tool, from the palette or from a key.
///
/// The buttons are lit here rather than rebuilt, so the one that was just
/// clicked is still alive when its handler returns.
void choose_tool(App& app, cutline::ui::Tool tool) {
  app.tool = tool;
  if (app.timeline != nullptr) app.timeline->set_tool(tool);
  // The shape tools draw on the picture, which is a mode the monitor holds.
  if (app.monitor != nullptr) {
    app.monitor->set_box_drawing(tool == cutline::ui::Tool::Rectangle ||
                                 tool == cutline::ui::Tool::Ellipse);
  }
  for (std::size_t i = 0; i < app.tool_buttons.size() && i < kTools.size(); ++i) {
    app.tool_buttons[i]->set_selected(kTools[i].tool == tool);
  }
  mark_dirty(app);
}

/// Runs a command and brings the interface back into line with it.
///
/// One place, so a key and a button that name the same command cannot end up
/// refreshing different things. The command decides whether it applies —
/// nothing here needs to know when a razor has anything to cut.
void run_command(App& app, cutline::editor::Command command) {
  // The one command that is a question rather than an edit. `run` refuses it
  // because the editor layer has no way to ask, and this is the layer that
  // does — so it is caught here rather than being given a second route in
  // through the menu, which is how a key and a menu entry come to do slightly
  // different things.
  if (command == cutline::editor::Command::PasteAttributes) {
    open_paste_attributes_dialog(app);
    return;
  }

  const std::uint64_t document_before = app.session.document_revision();
  if (cutline::editor::run(app.session, command)) {
    if (app.session.document_revision() != document_before) {
      // The document changed, so everything that shows it does — including the
      // source monitor and the mixer, which this handler used to leave out
      // while repeating the rest of `refresh_all` by hand. Undoing an import
      // then went on showing a frame of media the project no longer had, and
      // undoing a new track left the mixer with a strip for it.
      refresh_all(app);
    } else {
      // A transport key or a selection. Rebuilding the mixer on every arrow
      // press would be work for nothing, so this is what it always did.
      refresh_timeline(app);
      refresh_browser(app);
      refresh_title(app);
      invalidate_preview(app);
      app.inspector_stale = true;
    }

    // The playhead may have moved: every transport command moves it, and so do
    // the jumps to the next and previous marker. Here rather than in each of
    // them — which is also why the timecode used to sit still while the arrow
    // keys walked the playhead along.
    show_playhead(app);
    follow_playhead(app);
  }
  mark_dirty(app);
}

/// Moves the playhead somewhere and brings everything that follows it along.
///
/// What the scrub handler does, in a function, because the timecode field
/// needs the same thing and a second copy of it is how the two come to disagree
/// about whether a jump takes the sound with it.
void go_to_time(App& app, double at) {
  app.session.set_playhead(at);
  show_playhead(app);
  follow_playhead(app);
  refresh_timeline(app);
#if CUTLINE_HAVE_PREVIEW
  // A jump while it is playing takes the sound with it. Without this the audio
  // carries on from where it was and the picture is somewhere else, which is
  // worse than either.
  if (app.playing()) {
    app.player->seek(app.session.playhead());
    // Recorded, so the playback loop sees the playhead and the sound already
    // agreeing and does not seek a second time — every seek stops and resets
    // the output device, and doing it twice per jump is heard.
    app.playhead_from_playback = app.session.playhead();
    app.shown_frame = -1;
  }
#endif
  invalidate_preview(app);
  mark_dirty(app);
}

/// What a key's action does.
///
/// The editing commands go through `run_command`, like their buttons and menu
/// rows; the rest -- the file, the transport, the tools, the view -- are here.
/// The transport and marking keys go to the source monitor when it is the one
/// in hand, as Premiere sends them to whichever monitor is focused: an `I`
/// with the source in hand is a mark on the source, not on the timeline
/// behind it.
void run_action(App& app, Shell& shell, std::string_view id) {
  const cutline::editor::Action* action = cutline::editor::find_action(id);
  if (action == nullptr) return;

  if (source_has_focus(app)) {
    if (const cutline::core::Media* media = source_media_of(app); media != nullptr) {
      bool taken = true;
      if (id == "markers.mark_in") {
        mark_source(app, false);
      } else if (id == "markers.mark_out") {
        mark_source(app, true);
      } else if (id == "markers.add") {
        mark_source_marker(app);
      } else if (id == "markers.next") {
        jump_source_marker(app, true);
      } else if (id == "markers.previous") {
        jump_source_marker(app, false);
      } else if (id == "markers.clear_all") {
        app.session.apply(cutline::core::clear_source_markers(app.session.project(), media->id));
        refresh_all(app);
      } else if (id == "playback.step_back") {
        step_source(app, -1.0);
      } else if (id == "playback.step_forward") {
        step_source(app, 1.0);
      } else if (id == "playback.step_back_five") {
        step_source(app, -5.0);
      } else if (id == "playback.step_forward_five") {
        step_source(app, 5.0);
      } else if (id == "playback.go_to_start") {
        app.source_playhead = source_reach(*media).first;
        refresh_source(app);
      } else if (id == "playback.go_to_end") {
        app.source_playhead = source_reach(*media).second;
        refresh_source(app);
      } else if (id == "playback.play") {
        // A shuttle is playback of a sort, so the space bar stops it too.
        if (app.source_shuttling()) {
          set_source_shuttle(app, 0.0);
        } else {
          toggle_source_playback(app);
        }
      } else if (id == "playback.stop") {
        set_source_shuttle(app, 0.0);
        stop_source_playback(app);
      } else if (id == "playback.shuttle_left") {
        set_source_shuttle(app, next_shuttle(app.source_shuttle, -1.0));
      } else if (id == "playback.shuttle_right") {
        set_source_shuttle(app, app.source_playing() ? next_shuttle(1.0, 1.0)
                                                     : next_shuttle(app.source_shuttle, 1.0));
      } else {
        taken = false;
      }
      if (taken) {
        mark_dirty(app);
        return;
      }
    }
  }

  // Escape leaves trim mode, or stops a render, before it deselects anything.
  if (id == "edit.deselect_all" && app.trim_point.has_value()) {
    exit_trim_mode(app);
    return;
  }
  if (id == "edit.deselect_all" && rendering_previews(app)) {
    cancel_preview_render(app);
    return;
  }

  if (action->command.has_value()) {
    run_command(app, *action->command);
    return;
  }

  const auto zoom = [&app](auto&& change) {
    if (app.timeline == nullptr) return;
    change(*app.timeline);
  };
  const auto step = [&app](double frames) {
    const double fps = app.session.project().sequence().fps;
    if (fps <= 0.0) return;
    go_to_time(app, std::max(0.0, app.session.playhead() + frames / fps));
  };

  // Premiere's camera keys, numbered from 1 as the cameras are.
  if (id.starts_with("multicam.select_camera_") || id.starts_with("multicam.cut_to_camera_")) {
    const int camera = id.back() - '1';
    if (id.starts_with("multicam.select")) {
      select_camera(app, camera);
    } else {
      cut_to_camera(app, camera);
    }
    return;
  }

  if (id == "file.new") {
    new_project(app);
  } else if (id == "file.open") {
    open_project(app);
  } else if (id == "file.save") {
    save_project(app, false);
  } else if (id == "file.save_as") {
    save_project(app, true);
  } else if (id == "file.import") {
    import_media(app);
  } else if (id == "file.export") {
    open_export_dialog(app);
  } else if (id == "edit.keyboard_shortcuts") {
    open_keyboard_dialog(app);
  } else if (id == "clip.make_subclip") {
    open_make_subclip_dialog(app);
  } else if (id == "sequence.render_in_to_out") {
    render_in_to_out(app);
  } else if (id == "sequence.trim_edit") {
    toggle_trim_mode(app);
  } else if (id == "sequence.toggle_trim_type") {
    toggle_trim_type(app);
  } else if (id == "sequence.trim_backward") {
    trim_by(app, -1.0);
  } else if (id == "sequence.trim_forward") {
    trim_by(app, 1.0);
  } else if (id == "sequence.trim_backward_many") {
    trim_by(app, -5.0);
  } else if (id == "sequence.trim_forward_many") {
    trim_by(app, 5.0);
  } else if (id == "sequence.snap") {
    toggle_snapping(app);
  } else if (id == "sequence.zoom_in") {
    // Premiere's keys: `=` and `-` about the playhead, and `\` for the whole.
    zoom([](TimelineView& view) { view.zoom_about_playhead(true); });
  } else if (id == "sequence.zoom_out") {
    zoom([](TimelineView& view) { view.zoom_about_playhead(false); });
  } else if (id == "sequence.zoom_fit") {
    zoom([](TimelineView& view) { view.zoom_to_fit(); });
  } else if (id == "playback.play") {
    toggle_playback(app);
  } else if (id == "playback.shuttle_left") {
    // J, K and L: back, stop, forward, pressing again to go faster.
    set_shuttle(app, next_shuttle(app.shuttle, -1.0));
  } else if (id == "playback.stop") {
    set_shuttle(app, 0.0);
    if (app.playing()) stop_playback(app);
  } else if (id == "playback.shuttle_right") {
    set_shuttle(app, app.playing() ? next_shuttle(1.0, 1.0) : next_shuttle(app.shuttle, 1.0));
  } else if (id == "playback.step_back_five") {
    step(-5.0);
  } else if (id == "playback.step_forward_five") {
    step(5.0);
  } else if (id == "window.float_panel") {
    float_panel_under_cursor(app, shell);
  } else if (id == "window.maximise_frame") {
    // Premiere's `~`: the panel under the pointer fills the window, and again
    // puts it back. Under the *pointer* rather than whatever has the keyboard,
    // because it is used to get a closer look at something being watched, and
    // watching a panel does not focus it.
    if (shell.is_main() && shell.dock != nullptr) {
      POINT at{};
      GetCursorPos(&at);
      ScreenToClient(shell.window, &at);
      const auto [ux, uy] = to_units(shell, at);
      const auto over = shell.dock->panel_at(ux, uy);
      const PanelId panel = over.value_or(layout_of(app).maximised);
      if (cutline::ui::toggle_maximised(layout_of(app), panel)) app.dock_stale = true;
    }
  } else if (id.starts_with("tool.")) {
    const auto tool = std::ranges::find(kTools, id, &ToolEntry::action);
    if (tool != kTools.end()) choose_tool(app, tool->tool);
  } else if (id.starts_with("view.theme.")) {
    // In the order the themes are built: Slate, Luna, Aero, Phosphor.
    constexpr std::array<std::string_view, 4> kThemes{"view.theme.slate", "view.theme.luna",
                                                       "view.theme.aero", "view.theme.phosphor"};
    const auto at = std::ranges::find(kThemes, id);
    if (at != kThemes.end()) set_theme(app, static_cast<std::size_t>(at - kThemes.begin()));
  }
  mark_dirty(app);
}

// ------------------------------------------------------------------ panels --
//
// Each panel is built once, by name, and then moved around by the dock rather
// than rebuilt. `app` may be null, so the headless check can build every panel
// with no window and no session behind it.

/// Shows the pool as a list or as pictures, and remembers which.
void set_pool_view(App& app, cutline::ui::BrowserView view) {
  if (app.browser == nullptr) return;
  app.browser->set_view(view);
  save_settings(app);
  // Named for what it would do next, so the button says where pressing it
  // goes rather than where you already are.
  if (app.view_button != nullptr) {
    app.view_button->set_text(view == cutline::ui::BrowserView::Icons ? "List" : "Icons");
  }
  refresh_browser(app);
  if (app.main.host != nullptr) app.main.host->request_layout();
}

/// The window's own widget tree: a caption, a theme switcher, and the dock.
///
/// `app` may be null, so the headless check can build the same tree with no
/// window behind it.
[[nodiscard]] std::unique_ptr<Widget> build_interface(App* app) {
  auto shell = std::make_unique<Box>(Axis::Vertical);
  shell->set_spacing(0.0);

  // The window's own caption. The system one cannot be themed, so it is turned
  // off in `WM_NCCALCSIZE` and this is drawn in its place.
  auto& caption = shell->emplace<TitleBar>("Cutline");
  // Only here. A torn-out panel's caption is a title bar too, and stamping the
  // product mark on every one of them turns branding into wallpaper.
  caption.set_mark(true);
  if (app != nullptr) app->main.title_bar = &caption;

  caption.emplace<CaptionButton>(CaptionButton::Kind::Minimise, [app] {
    if (app != nullptr && app->main.window != nullptr) ShowWindow(app->main.window, SW_MINIMIZE);
  });
  auto& maximise = caption.emplace<CaptionButton>(CaptionButton::Kind::Maximise, [app] {
    if (app == nullptr || app->main.window == nullptr) return;
    ShowWindow(app->main.window, IsZoomed(app->main.window) ? SW_RESTORE : SW_MAXIMIZE);
  });
  caption.emplace<CaptionButton>(CaptionButton::Kind::Close, [app] {
    if (app != nullptr && app->main.window != nullptr) PostMessageW(app->main.window, WM_CLOSE, 0, 0);
  });
  if (app != nullptr) app->main.maximise = &maximise;

  // A menu bar, where a Windows application keeps the things that are done to
  // the document rather than with it: opening and saving, what the sequence is,
  // and what the application looks like.
  //
  // Built out of buttons that open popups rather than out of a menu widget of
  // its own, because that is what every other menu here already is — the
  // effects list, the track menu, the right-click on a clip. One idiom.
  auto& bar = shell->emplace<Box>(Axis::Horizontal);
  bar.set_padding(Space::Tight);

  if (app != nullptr) app->menu_buttons.clear();
  const auto menu = [app, &bar](const char* title, std::vector<MenuEntry> (*build)(App&)) {
    auto& button = bar.emplace<Button>(title);
    button.set_mnemonic(true);
    const std::size_t index = app != nullptr ? app->menu_buttons.size() : 0;
    if (app != nullptr) app->menu_buttons.push_back(&button);
    // A menu bar, not a row of buttons: words until the pointer is over one,
    // which is what every menu bar on the platform looks like and what
    // Premiere's looks like too.
    button.set_part(cutline::ui::Part::MenuItem);
    button.set_on_click([app, index, control = &button, build] {
      if (app == nullptr || app->main.host == nullptr) return;

      auto list = std::make_unique<MenuList>(build(*app));
      list->set_on_dismiss([app] {
        if (WidgetHost* host = app->main.host.get(); host != nullptr) {
          // Out of the menus altogether when they were opened from the bar on
          // the keyboard, as Windows leaves them, rather than back onto the bar.
          host->close_popup(!std::ranges::contains(app->menu_buttons, host->focus_before_popup()));
        }
      });
      // Opened from the keyboard, it starts on its first entry, so Enter does
      // something at once; opened by the mouse it starts on none.
      if (app->main.host->keyboard_cues()) list->set_highlighted(0);
      app->open_menu_list = list.get();
      app->open_menu = index;
      // Under the button rather than at the pointer, which is where a menu bar
      // drops its menus and what makes two of them line up.
      app->main.host->open_popup(std::move(list), control->bounds());
    });
  };

  menu("File", file_menu);
  menu("Edit", edit_menu);
  menu("Clip", clip_menu);
  menu("Sequence", sequence_menu);
  menu("Markers", markers_menu);
  menu("View", view_menu);
  menu("Window", window_menu);
  menu("Help", help_menu);

  bar.emplace<Spacer>();

  auto& workspace_choice = bar.emplace<Button>("Workspace: Editing", [app] {
    if (app != nullptr) next_workspace(*app);
  });
  if (app != nullptr) {
    app->workspace_button = &workspace_choice;
    workspace_choice.set_text("Workspace: " + app->workspaces.active);
  }
  bar.emplace<Button>("Reset", [app] {
    if (app != nullptr) reset_workspace(*app);
  });

  // Everything under the toolbar is the dock. Which panel is where is data now
  // rather than structure: this builds a view over a layout, and dragging a tab
  // is what changes the layout.
  auto& dock = shell->emplace<DockView>();
  if (app != nullptr) app->main.dock = &dock;

  wire_dock(app, app == nullptr ? nullptr : &app->main, dock);
  dock.set_node(app == nullptr ? default_layout().root : layout_of(*app).root);
  return shell;
}

// ------------------------------------------------------------------ windows --

/// The tree a torn-out window gets: a caption it can be dragged by, and its
/// dock. No theme switcher and no menu — it is one or two panels, not a second
/// copy of the editor.
[[nodiscard]] std::unique_ptr<Widget> build_floating_interface(App* app, Shell* shell) {
  auto root = std::make_unique<Box>(Axis::Vertical);
  root->set_spacing(0.0);

  auto& caption = root->emplace<TitleBar>("Cutline");
  shell->title_bar = &caption;

  caption.emplace<CaptionButton>(CaptionButton::Kind::Close, [app, shell] {
    // The panels go home rather than being thrown away. There is no menu to
    // reopen one from yet, so closing a window would otherwise be the one
    // action in the application that cannot be undone by any means.
    if (app != nullptr) return_panels_home(*app, shell->floating_id);
  });

  auto& dock = root->emplace<DockView>();
  shell->dock = &dock;
  wire_dock(app, shell, dock);
  return root;
}

/// Opens a real window for a floating dock.
void open_float_window(App& app, const cutline::ui::FloatingDock& floating) {
  auto shell = std::make_unique<Shell>();
  shell->app = &app;
  shell->floating_id = floating.id;
  shell->host = std::make_unique<WidgetHost>(build_floating_interface(&app, shell.get()));
  give_sounds(app, *shell->host);
  shell->dock->set_node(floating.root);

  // `WS_POPUP | WS_THICKFRAME` is a window with no caption that can still be
  // resized by its edges, which is what a drawn caption needs. Owned by the
  // main window so it stays above it and goes away with it.
  const HWND window = CreateWindowExW(
      WS_EX_TOOLWINDOW | kWindowExStyle, kWindowClass, L"Cutline", WS_POPUP | WS_THICKFRAME,
      static_cast<int>(floating.bounds.x), static_cast<int>(floating.bounds.y),
      static_cast<int>(floating.bounds.width), static_cast<int>(floating.bounds.height),
      app.main.window, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (window == nullptr) return;

  shell->window = window;
  shell->scale = scale_of(window);
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shell.get()));

  const MARGINS shadow{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

  // Shown without taking the keyboard: the panel was dragged out, not asked
  // to become the thing being typed into.
  ShowWindow(window, SW_SHOWNOACTIVATE);
  app.floats.push_back(std::move(shell));
}

void reconcile_windows(App& app) {
  // Windows whose dock has gone, because its last panel was docked elsewhere.
  for (auto it = app.floats.begin(); it != app.floats.end();) {
    const bool alive =
        std::ranges::find(layout_of(app).floating, (*it)->floating_id,
                          &cutline::ui::FloatingDock::id) != layout_of(app).floating.end();
    if (alive) {
      ++it;
      continue;
    }

    const HWND doomed = (*it)->window;
    // Unhooked before it is destroyed. `DestroyWindow` sends `WM_DESTROY`
    // straight back here, and the shell it would look up is about to be freed.
    if (doomed != nullptr) SetWindowLongPtrW(doomed, GWLP_USERDATA, 0);
    it = app.floats.erase(it);
    if (doomed != nullptr) DestroyWindow(doomed);
  }

  // Docks with no window yet, because something was just torn out.
  for (const cutline::ui::FloatingDock& floating : layout_of(app).floating) {
    const bool built = std::ranges::any_of(app.floats, [&](const std::unique_ptr<Shell>& shell) {
      return shell->floating_id == floating.id;
    });
    if (!built) open_float_window(app, floating);
  }
}

/// A torn-out window is named after whatever it is showing.
void refresh_float_titles(App& app) {
  for (const std::unique_ptr<Shell>& shell : app.floats) {
    if (shell->title_bar == nullptr || shell->dock == nullptr) continue;

    std::string title = "Cutline";
    if (const auto showing = shell->dock->node().active_panel(); showing.has_value()) {
      title = panel_title(*showing);
    } else if (const std::vector<PanelId> inside = cutline::ui::panels_in(shell->dock->node());
               !inside.empty()) {
      title = panel_title(inside.front());
    }

    shell->title_bar->set_title(title);
    if (shell->window != nullptr) SetWindowTextA(shell->window, title.c_str());
  }
}

/// Win32 virtual keys to the ones the interface knows about. Only the keys
/// that are actually bound; the rest arrive as `Key::None` and go nowhere.
[[nodiscard]] Key key_from_win32(WPARAM vk) {
  if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(vk);
  if (vk >= '0' && vk <= '9') return static_cast<Key>(vk);
  switch (vk) {
    case VK_SPACE: return Key::Space;
    case VK_ESCAPE: return Key::Escape;
    case VK_TAB: return Key::Tab;
    case VK_RETURN: return Key::Enter;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    // The zoom keys. `VK_OEM_PLUS` is the unshifted key, so this is the one
    // labelled `=` on a keyboard and `+` in every editor's menu.
    case VK_OEM_PLUS: return Key::Equal;
    case VK_OEM_MINUS: return Key::Minus;
    case VK_OEM_5: return Key::Backslash;
    // Insert and overwrite. `Key::Comma` and `Key::Period` have been bound
    // since the nudge was written and have never once arrived: the virtual key
    // for a comma is `VK_OEM_COMMA`, not the character, so the translation
    // dropped it and the binding sat there doing nothing.
    case VK_OEM_COMMA: return Key::Comma;
    case VK_OEM_PERIOD: return Key::Period;
    // Maximise. The same trap as the comma above, and it caught this too: the
    // binding was written, the key was pressed, and nothing happened because
    // the backtick arrives as `VK_OEM_3` rather than as its character.
    case VK_OEM_3: return Key::Backtick;
    // And the rest of the keys a shortcut can be put on, which the keyboard in
    // the shortcuts window offers and which would otherwise arrive as nothing.
    case VK_INSERT: return Key::Insert;
    case VK_OEM_1: return Key::Semicolon;
    case VK_OEM_2: return Key::Slash;
    case VK_OEM_4: return Key::LeftBracket;
    case VK_OEM_6: return Key::RightBracket;
    default: break;
  }
  if (vk >= VK_F1 && vk <= VK_F12) {
    return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(vk - VK_F1));
  }
  return Key::None;
}

[[nodiscard]] Modifiers modifiers_now() {
  return Modifiers{
      .shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0,
      .control = (GetKeyState(VK_CONTROL) & 0x8000) != 0,
      .alt = (GetKeyState(VK_MENU) & 0x8000) != 0,
  };
}

[[nodiscard]] MouseEvent mouse_from(const Shell& shell, LPARAM lparam, MouseButton button,
                                    int clicks = 1) {
  const auto [x, y] = to_units(shell, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
  return MouseEvent{
      .x = x,
      .y = y,
      .button = button,
      .modifiers = modifiers_now(),
      .click_count = clicks,
  };
}

/// The device every window here draws on, made once and kept.
///
/// Empty when there is no compositor in this build, or when no device could be
/// made — in both cases the window falls back to creating its own, which is
/// what it did before there was anything to share.
[[nodiscard]] cutline::ui::AdoptedDevice shared_device(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.device == nullptr && !app.device_failed) {
    // The preference is read here and nowhere later. Everything that draws is
    // built on this device, so the choice is settled once per run — which is
    // why the page that offers it says a restart is needed rather than
    // pretending the tick took effect.
    auto made = cutline::gpu::Device::create(
        {.force_software = app.settings.software_renderer});
    if (made.has_value()) {
      app.device = std::move(*made);
    } else {
      // Not worth interrupting anybody over: everything still works, just with
      // a copy in the middle. The preview says so if it cannot start at all.
      app.device_failed = true;
    }
  }
  if (app.device == nullptr) return {};
  return cutline::ui::AdoptedDevice{.adapter = app.device->native_adapter(),
                                    .device = app.device->native_device(),
                                    .queue = app.device->native_queue()};
#else
  (void)app;
  return {};
#endif
}

bool App::animating() noexcept {
  for (Shell* shell : shells()) {
    if (shell->host != nullptr && shell->host->animating()) return true;
  }
  return false;
}

/// How big a dialog window opens: as it was last left, or as designed.
[[nodiscard]] std::pair<int, int> dialog_size(const App& app, const std::string& name, int width,
                                              int height) {
  const auto found = app.settings.dialog_sizes.find(name);
  if (found == app.settings.dialog_sizes.end()) return {width, height};
  // Never smaller than designed: the design is what fits its contents, and
  // --check measures it at exactly that.
  return {std::max(width, found->second.first), std::max(height, found->second.second)};
}

/// Keeps a dialog window's size for next time.
void remember_dialog_size(App& app, const Shell& shell) {
  if (shell.window == nullptr || shell.size_key.empty()) return;
  RECT box{};
  if (!GetWindowRect(shell.window, &box)) return;
  // In units, so a size left on one display opens the same size on another.
  app.settings.dialog_sizes[shell.size_key] = {
      static_cast<int>(std::lround((box.right - box.left) / shell.scale)),
      static_cast<int>(std::lround((box.bottom - box.top) / shell.scale))};
  save_settings(app);
}

/// Asks each window's display whether it is showing HDR, puts each window in
/// the mode its display wants, and tells the preview engines whether anybody
/// needs HDR pictures.
void refresh_hdr(App& app) {
  std::optional<float> any;
  for (Shell* shell : app.shells()) {
    if (shell->window == nullptr) continue;
    shell->monitor = MonitorFromWindow(shell->window, MONITOR_DEFAULTTONEAREST);
    const std::optional<float> wanted = cutline::ui::SkiaWindow::hdr_white_for(shell->window);
    if (!any) any = wanted;
    if (shell->surface == nullptr || shell->surface->hdr() == wanted) continue;
    if (auto ok = shell->surface->set_hdr(wanted); !ok && !shell->warned) {
      shell->warned = true;
      complain(shell->window, "Could not show HDR on this display.\n\n" + ok.error());
    }
    shell->dirty = true;
  }
  if (any == app.hdr_white) return;
  app.hdr_white = any;
#if CUTLINE_HAVE_PREVIEW
  if (app.preview != nullptr) app.preview->set_hdr_display(any);
  if (app.source_preview != nullptr) app.source_preview->set_hdr_display(any);
  app.preview_stale = true;
#endif
}

/// Makes or resizes the window's swapchain.
///
/// A failure is said once and then the window simply does not draw. There is
/// nothing sensible to fall back to: the raster path this replaced was slower
/// by an order of magnitude, and the themes with a blur behind their chrome
/// were not usable on it at all.
void resize_surface(Shell& shell, int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (shell.surface != nullptr && width == shell.width && height == shell.height) return;

  shell.width = width;
  shell.height = height;

  if (shell.surface != nullptr) {
    if (const auto resized = shell.surface->resize(width, height); !resized.has_value()) {
      shell.surface.reset();
      if (!shell.warned) {
        shell.warned = true;
        complain(shell.window, "The window stopped drawing.\n\n" + resized.error());
      }
    }
    return;
  }

  auto made = cutline::ui::SkiaWindow::create(shell.window, width, height,
                                              shared_device(*shell.app));
  if (!made.has_value()) {
    if (!shell.warned) {
      shell.warned = true;
      complain(shell.window, "Could not draw on this display.\n\n" + made.error());
    }
    return;
  }
  shell.surface = std::move(*made);
  // A new surface starts SDR; the display decides.
  refresh_hdr(*shell.app);
  apply_backdrop(shell, shell.app->current());
}

void apply_backdrop(Shell& shell, const Theme& theme) {
  if (shell.window == nullptr) return;
  // Spelled out, for an SDK older than the Windows 11 that added them.
  constexpr DWORD kSystemBackdropType = 38;  // DWMWA_SYSTEMBACKDROP_TYPE
  constexpr int kNoBackdrop = 1;             // DWMSBT_NONE
  constexpr int kAcrylic = 3;                // DWMSBT_TRANSIENTWINDOW
  constexpr DWORD kDarkMode = 20;            // DWMWA_USE_IMMERSIVE_DARK_MODE

  // The glass is tinted to match: light acrylic under a light theme.
  const BOOL dark = theme.dark ? TRUE : FALSE;
  DwmSetWindowAttribute(shell.window, kDarkMode, &dark, sizeof dark);

  const bool wanted = theme.metrics.frame_glass && shell.surface != nullptr &&
                      shell.surface->translucent();
  const int backdrop = wanted ? kAcrylic : kNoBackdrop;
  // Fails where the system has no backdrops to give, before Windows 11's
  // second year; the window then stays opaque and the title bar's own fill
  // stands in for the glass.
  const bool given = SUCCEEDED(
      DwmSetWindowAttribute(shell.window, kSystemBackdropType, &backdrop, sizeof backdrop));
  shell.glass = wanted && given;

  // The glass covers the whole window, under everything the frame paints; a
  // frame that paints all of it opaque hides it everywhere but where it
  // leaves clear. Otherwise one pixel of frame handed back to the compositor,
  // which is enough to keep the drop shadow and the rounded corners.
  const MARGINS margins = shell.glass ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(shell.window, &margins);
  shell.dirty = true;
}

/// The work that belongs to the document rather than to any one window.
///
/// Done once a frame, from whichever window is drawing first. Each of these
/// rebuilds part of the interface, so none of them can happen where they are
/// asked for — the thing asking is usually inside what the rebuild replaces.
void settle(App& app) {
  // A gesture lasts exactly as long as the pointer is held, and the host is the
  // only thing that knows that. See `App::live_gesture` for why the control
  // cannot be trusted to say when it is finished.
  if (app.live_gesture && app.main.host != nullptr && app.main.host->captured() == nullptr) {
    app.live_gesture = false;
    app.live_project.reset();
    app.preview_stale = true;
    refresh_handles(app);
  }

  // Typing on the picture ends with its field, however the field went: OK and
  // Cancel say so themselves, but a click elsewhere closes the popup with
  // neither, and the picture must not go on showing words nobody kept.
  if (app.typing_on_picture && app.main.host != nullptr && app.main.host->popup() == nullptr) {
    app.typing_on_picture = false;
    app.live_project.reset();
    invalidate_preview(app);
    refresh_handles(app);
  }

  // The arrangement first, because rebuilding it is what creates a panel that
  // has never been shown before — and that panel then wants filling in this
  // same frame rather than staying blank until something else changes.
  if (app.dock_stale) {
    app.dock_stale = false;
    refresh_dock(app);
  }
  if (app.inspector_stale) {
    app.inspector_stale = false;
    refresh_inspector(app);
    // Whatever changed what the inspector shows changed what these show.
    app.graphics_stale = true;
    app.lumetri_stale = true;
  }
  if (app.graphics_stale) {
    app.graphics_stale = false;
    refresh_graphics_panel(app);
  }
  if (app.lumetri_stale) {
    app.lumetri_stale = false;
    refresh_lumetri_panel(app);
  }
  if (app.mixer_stale) {
    app.mixer_stale = false;
    refresh_audio_panel(app);
  }
  if (app.settings_page_stale) {
    app.settings_page_stale = false;
    refresh_settings_page(app);
  }
  if (app.keyboard_stale) {
    app.keyboard_stale = false;
    refresh_keyboard_dialog(app);
  }
  // Compared rather than flagged: there are well over a hundred places that
  // apply an edit, and a flag set at each would be a list that has to stay
  // complete for ever. Comparing a hundred short names is nothing.
  refresh_history(app);
  refresh_text_panel(app);
#if CUTLINE_HAVE_PREVIEW
  // Once per frame at most, however many mouse moves a scrub produced. A
  // read-back stalls the GPU, so rendering on each one would make the drag
  // itself the slow part.
  if (app.preview_stale) {
    app.preview_stale = false;
    refresh_preview(app);
  }
  // After the preview, and only when the panel is open. Measuring costs a
  // second render of the frame, so a scope nobody is looking at costs nothing.
  refresh_scopes(app);
#endif
}

/// Draws one window's tree and puts the pixels on it.
void render(Shell& shell) {
  if (shell.app == nullptr || shell.host == nullptr || shell.surface == nullptr) return;
  App& app = *shell.app;

  // The swapchain's next buffer, once the GPU has finished with it. Null means
  // the swapchain is not usable, and the window goes unpainted rather than
  // being drawn somewhere nothing will look.
  auto* canvas = static_cast<SkCanvas*>(shell.surface->begin_frame());
  if (canvas == nullptr) return;

  const Theme& theme = app.current();
  // The window's texture cache goes with the painter: a wrapper for somebody
  // else's GPU frame has to outlive the paint that drew it, and the painter
  // does not.
  const std::unique_ptr<SkiaPainter> painter =
      SkiaPainter::create(canvas, shell.surface->textures());
  if (painter == nullptr) {
    shell.surface->present();
    return;
  }

  const Rect client{0.0, 0.0, static_cast<double>(shell.width) / shell.scale,
                    static_cast<double>(shell.height) / shell.scale};
  const LayoutContext context{theme, *painter};

  settle(app);

  // The one place a theme and a text measurer are both in hand, which is
  // exactly why layout is deferred to here rather than done where the size
  // changed or the drag happened.
  if (shell.resized) {
    shell.host->resize(client, context);
    shell.resized = false;
  } else {
    shell.host->update_layout(context);
  }

  // Through the painter rather than a canvas clear, so a themed background
  // that is a gradient stays one. With the system's glass behind the window,
  // the frame starts clear and the background stops short of the title bar,
  // so the glass shows through the bar's tint and nowhere else.
  canvas->clear(shell.glass ? SK_ColorTRANSPARENT : SK_ColorBLACK);
  canvas->save();
  canvas->scale(static_cast<SkScalar>(shell.scale), static_cast<SkScalar>(shell.scale));
  Rect background = client;
  if (shell.glass && shell.title_bar != nullptr) {
    const double below = shell.title_bar->bounds().bottom();
    background = Rect{client.x, below, client.width, std::max(0.0, client.bottom() - below)};
  }
  paint_surface(*painter, background, theme.style(cutline::ui::Part::Window));
  shell.host->paint(*painter, theme);
  canvas->restore();

  shell.surface->present();
}

void set_theme(App& app, std::size_t index) {
  if (index >= built_in_themes().size() || index == app.theme) return;
  app.theme = index;
  save_settings(app);
  for (Shell* shell : app.shells()) apply_backdrop(*shell, app.current());

  // Metrics change with the theme, so everything has to be measured again —
  // in every window, not only the one whose button was pressed.
  for (Shell* shell : app.shells()) {
    if (shell->host != nullptr) shell->host->request_layout();
    shell->dirty = true;
    if (shell->window != nullptr) InvalidateRect(shell->window, nullptr, FALSE);
  }
}

// ----------------------------------------------------------------- cursors --

/// A tool cursor, drawn from the same art as the tool's button.
///
/// Rendered rather than shipped as a resource, and rendered from
/// `ui::draw_icon` rather than from a second drawing of the same shape: a razor
/// that looked one way in the palette and another under the pointer would be
/// two razors, and the second one would drift the first time either was
/// touched.
///
/// Thirty-two pixels, which is what Windows expects and what a hot spot in the
/// middle of leaves room around. The mark is drawn white with a black halo
/// under it, because a cursor has to be visible over a dark timeline and over a
/// light one, and the alternative — a cursor that takes the theme's colours —
/// is invisible over its own panel half the time.
constexpr int kCursorSize = 32;

/// The mark a tool cursor is made of, drawn onto a transparent surface.
///
/// Split from making the cursor itself so `--check` can look at the pixels. A
/// mark that comes out empty is a cursor that vanishes over the timeline, and
/// nothing else in the application would notice.
[[nodiscard]] sk_sp<SkSurface> draw_cursor_art(cutline::ui::IconButton::Icon icon,
                                               double scale) {
  constexpr double kReach = 9.0;

  const sk_sp<SkSurface> surface =
      SkSurfaces::Raster(SkImageInfo::MakeN32Premul(to_pixels(kCursorSize, scale),
                                                    to_pixels(kCursorSize, scale)));
  if (surface == nullptr) return nullptr;
  surface->getCanvas()->clear(SK_ColorTRANSPARENT);
  // Drawn in the same units as the interface and scaled the same way, so a
  // cursor grows with its display as Windows' own do, and its strokes land on
  // the same pixel grid as the buttons it came from.
  surface->getCanvas()->scale(static_cast<SkScalar>(scale), static_cast<SkScalar>(scale));

  const std::unique_ptr<cutline::ui::SkiaPainter> painter =
      cutline::ui::SkiaPainter::create(surface->getCanvas());
  if (painter == nullptr) return nullptr;

  const Rect area{0.0, 0.0, static_cast<double>(kCursorSize),
                  static_cast<double>(kCursorSize)};
  // The halo first, as the same mark drawn fatter underneath. Cheaper than an
  // outline and it cannot come apart at a corner the way a traced one does.
  cutline::ui::draw_icon(*painter, icon, area, cutline::ui::Color{0.0F, 0.0F, 0.0F, 0.85F},
                         kReach, 5.0);
  cutline::ui::draw_icon(*painter, icon, area, cutline::ui::Color{1.0F, 1.0F, 1.0F, 1.0F},
                         kReach, 2.0);
  return surface;
}

[[nodiscard]] HCURSOR render_tool_cursor(cutline::ui::IconButton::Icon icon, double scale) {
  const int kSize = to_pixels(kCursorSize, scale);
  const sk_sp<SkSurface> surface = draw_cursor_art(icon, scale);
  if (surface == nullptr) return nullptr;

  // Into a DIB, because that is what an icon is made of. Top-down, so the rows
  // are in the order Skia wrote them rather than upside down.
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(BITMAPV5HEADER);
  header.bV5Width = kSize;
  header.bV5Height = -kSize;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bits = nullptr;
  const HDC screen = GetDC(nullptr);
  const HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header),
                                          DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (colour == nullptr || bits == nullptr) return nullptr;

  const SkImageInfo info = SkImageInfo::Make(kSize, kSize, kBGRA_8888_SkColorType,
                                             kUnpremul_SkAlphaType);
  if (!surface->readPixels(info, bits, kSize * 4, 0, 0)) {
    DeleteObject(colour);
    return nullptr;
  }

  // The mask is ignored for a 32-bit icon with an alpha channel, but one has to
  // be there or `CreateIconIndirect` refuses.
  const HBITMAP mask = CreateBitmap(kSize, kSize, 1, 1, nullptr);
  ICONINFO icon_info{};
  icon_info.fIcon = FALSE;
  // The middle. Every one of these marks is symmetrical about its own centre,
  // and a hot spot anywhere else would mean the razor cut somewhere other than
  // where its blade was drawn.
  icon_info.xHotspot = kSize / 2;
  icon_info.yHotspot = kSize / 2;
  icon_info.hbmMask = mask;
  icon_info.hbmColor = colour;

  const HCURSOR made = reinterpret_cast<HCURSOR>(CreateIconIndirect(&icon_info));
  DeleteObject(colour);
  DeleteObject(mask);
  return made;
}

/// What each cursor is on this platform.
///
/// The tool ones are made once and kept: rendering thirty-two pixels is cheap,
/// and doing it on every mouse move over a timeline is thirty-two pixels sixty
/// times a second for no reason.
[[nodiscard]] HCURSOR system_cursor(cutline::ui::Cursor cursor, double scale) {
  using cutline::ui::Cursor;
  using Icon = cutline::ui::IconButton::Icon;

  switch (cursor) {
    case Cursor::Text: return LoadCursorW(nullptr, IDC_IBEAM);
    case Cursor::ResizeWE: return LoadCursorW(nullptr, IDC_SIZEWE);
    case Cursor::ResizeNS: return LoadCursorW(nullptr, IDC_SIZENS);
    case Cursor::Move: return LoadCursorW(nullptr, IDC_SIZEALL);
    case Cursor::Arrow: return LoadCursorW(nullptr, IDC_ARROW);

    case Cursor::Razor:
    case Cursor::RateStretch:
    case Cursor::Remix:
    case Cursor::Slip:
    case Cursor::Slide:
    case Cursor::Ripple:
    case Cursor::Roll: break;
  }

  // One per display scale as well as per tool: a window on each of two
  // displays wants the same razor at two sizes.
  static std::map<std::pair<cutline::ui::Cursor, int>, HCURSOR> made;
  const std::pair key{cursor, to_pixels(kCursorSize, scale)};
  if (const auto found = made.find(key); found != made.end()) {
    return found->second != nullptr ? found->second : LoadCursorW(nullptr, IDC_ARROW);
  }

  const Icon icon = cursor == Cursor::Razor         ? Icon::Razor
                    : cursor == Cursor::RateStretch ? Icon::RateStretch
                    : cursor == Cursor::Remix       ? Icon::Remix
                    : cursor == Cursor::Slip        ? Icon::Slip
                    : cursor == Cursor::Slide       ? Icon::Slide
                    : cursor == Cursor::Ripple      ? Icon::Ripple
                                                    : Icon::Roll;
  const HCURSOR drawn = render_tool_cursor(icon, scale);
  made.emplace(key, drawn);
  return drawn != nullptr ? drawn : LoadCursorW(nullptr, IDC_ARROW);
}

/// The arrows and Escape on the menu bar, and the arrows across an open menu:
/// Left and Right move from one menu to the next, Down opens the one with the
/// keyboard, and Escape leaves the bar.
[[nodiscard]] bool menu_bar_key(App& app, Key key) {
  WidgetHost* host = app.main.host.get();
  const std::size_t count = app.menu_buttons.size();
  if (host == nullptr || count == 0) return false;
  const auto step = [count](std::size_t from, Key towards) {
    return towards == Key::Right ? (from + 1) % count : (from + count - 1) % count;
  };

  // A menu the bar dropped is open: the next one drops in its place.
  if (host->popup() != nullptr && host->popup() == app.open_menu_list &&
      (key == Key::Left || key == Key::Right) && app.open_menu < count) {
    // The title first, so closing the new menu with Escape comes back to it
    // rather than to the one the keyboard started on.
    Button* next = app.menu_buttons[step(app.open_menu, key)];
    if (std::ranges::contains(app.menu_buttons, host->focus_before_popup())) host->set_focus(next);
    next->fire();
    return true;
  }

  const auto on_bar = std::ranges::find(app.menu_buttons, host->focused());
  if (on_bar == app.menu_buttons.end()) return false;
  const auto at = static_cast<std::size_t>(on_bar - app.menu_buttons.begin());
  switch (key) {
    case Key::Left:
    case Key::Right:
      host->set_focus(app.menu_buttons[step(at, key)]);
      return true;
    case Key::Down:
      app.menu_buttons[at]->fire();
      return true;
    case Key::Escape:
      host->set_focus(nullptr);
      return true;
    default:
      return false;
  }
}

/// Everything a message does. Wrapped by `window_proc`, which is the boundary
/// an exception must not cross.
LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* shell = reinterpret_cast<Shell*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (shell == nullptr || shell->app == nullptr || shell->host == nullptr) {
    return DefWindowProcW(window, message, wparam, lparam);
  }
  App* app = shell->app;

  switch (message) {
    case WM_NCCALCSIZE: {
      // The whole point: with no non-client area, there is no system caption
      // and no system border, and the interface owns every pixel.
      if (wparam == FALSE) break;
      auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);

      if (IsZoomed(window)) {
        // A maximised window's rectangle deliberately overhangs the monitor by
        // the frame thickness. Without putting that back, the top of the
        // interface — the caption itself — sits off the screen.
        const UINT dpi = GetDpiForWindow(window);
        const int frame = GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                          GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        params->rgrc[0].left += frame;
        params->rgrc[0].right -= frame;
        params->rgrc[0].top += frame;
        params->rgrc[0].bottom -= frame;
      }
      return 0;
    }

    case WM_NCHITTEST: {
      POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &at);

      RECT client{};
      GetClientRect(window, &client);

      // The resize edges have to be answered by hand now that there is no
      // system frame to do it. A maximised window has none.
      if (!IsZoomed(window)) {
        const UINT dpi = GetDpiForWindow(window);
        const LONG grab = GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                          GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const bool left = at.x < grab;
        const bool right = at.x >= client.right - grab;
        const bool top = at.y < grab;
        const bool bottom = at.y >= client.bottom - grab;

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
      }

      // The widget tree decides what drags the window: the caption itself
      // does, and anything sitting on it — a button — does not. Asking the
      // tree rather than comparing against a height is what keeps the two from
      // drifting apart.
      const auto [ux, uy] = to_units(*shell, at);
      if (shell->title_bar != nullptr && shell->host->root().at(ux, uy) == shell->title_bar) {
        return HTCAPTION;
      }
      return HTCLIENT;
    }

    case WM_DISPLAYCHANGE:
      refresh_hdr(*app);
      return 0;

    case WM_DPICHANGED: {
      // Dragged onto a display at another scale, or the scale changed under
      // it. Windows suggests the rectangle that keeps the window the same
      // size to the eye; taking it is what stops a window doubling or halving
      // as it crosses from one display to the next.
      shell->scale = static_cast<double>(HIWORD(wparam)) / USER_DEFAULT_SCREEN_DPI;
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      SetWindowPos(window, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left, suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      shell->host->request_layout();
      shell->resized = true;
      shell->dirty = true;
      return 0;
    }

    case WM_WINDOWPOSCHANGED:
      // Only when it has crossed onto another display: a drag sends one of
      // these for every step.
      if (MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != shell->monitor) refresh_hdr(*app);
      break;

    case WM_SIZE:
      resize_surface(*shell, LOWORD(lparam), HIWORD(lparam));
      shell->resized = true;
      shell->dirty = true;
      // Maximising swaps which glyph the middle caption button shows.
      if (shell->maximise != nullptr) {
        shell->maximise->set_kind(IsZoomed(window) ? CaptionButton::Kind::Restore
                                                 : CaptionButton::Kind::Maximise);
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT paint;
      BeginPaint(window, &paint);
      render(*shell);
      EndPaint(window, &paint);
      // Something still fading wants the next frame too.
      shell->dirty = shell->host->animating();
      shell->host->clear_paint();
      return 0;
    }

    case WM_MOUSEMOVE: {
      // Asked for once per entry, so WM_MOUSELEAVE arrives and hover clears
      // when the pointer goes somewhere else entirely.
      TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
      TrackMouseEvent(&track);
      shell->host->mouse_move(mouse_from(*shell, lparam, MouseButton::Left));

      // After the move, so the browser has been told how far the drag has got.
      // Only the main window has both a pool and a timeline in it; a floating
      // one may have either, and asking costs nothing when it has neither.
      if (app != nullptr) refresh_drop_ghost(*app);

      // Any movement takes the tooltip away and starts the wait again. A box
      // that stayed up while the pointer moved on would be labelling the wrong
      // thing, and one that never restarted would only ever appear once.
      shell->host->hide_tooltip();
      SetTimer(window, kTooltipTimer, kTooltipDelayMs, nullptr);
      // Not unconditionally: the pointer crossing a panel that does not care
      // leaves the picture exactly as it was, and a full repaint costs
      // milliseconds. `--benchmark` is where that number comes from.
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    }
    case WM_SETCURSOR: {
      // Only over the client area. The frame's own edges and buttons are the
      // system's to draw a cursor for, and taking those would leave a window
      // that cannot be resized by its border without guessing where it is.
      if (LOWORD(lparam) != HTCLIENT) break;
      SetCursor(system_cursor(shell->host->cursor(), shell->scale));
      return TRUE;
    }

    case WM_MOUSELEAVE:
      KillTimer(window, kTooltipTimer);
      shell->host->hide_tooltip();
      shell->host->mouse_exit();
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;

    case WM_LBUTTONDOWN:
      KillTimer(window, kTooltipTimer);
      shell->host->hide_tooltip();
      SetCapture(window);
      shell->host->mouse_down(mouse_from(*shell, lparam, MouseButton::Left));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    case WM_LBUTTONDBLCLK:
      shell->host->mouse_down(mouse_from(*shell, lparam, MouseButton::Left, 2));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    case WM_LBUTTONUP:
      ReleaseCapture();
      shell->host->mouse_up(mouse_from(*shell, lparam, MouseButton::Left));
      // A drag released anywhere — over the tracks, over a panel, off the
      // window — is a drag that is over, and the promise goes with it. The
      // browser has already forgotten the gesture by now, so this works the
      // answer out the same way every other move does and finds there is none.
      if (app != nullptr) refresh_drop_ghost(*app);
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;

    // The right button, which until now reached nothing at all: `MouseEvent`
    // has carried a button since the first widget and this layer only ever
    // translated the left one, so every context menu in the application was
    // blocked on three lines that were never written.
    //
    // No `SetCapture`. A right-click is a single event rather than a gesture —
    // there is no right-drag anywhere — and holding the pointer for one would
    // only mean the release goes somewhere surprising. The host does not
    // capture on it either, for the same reason.
    case WM_RBUTTONDOWN:
      shell->host->mouse_down(mouse_from(*shell, lparam, MouseButton::Right));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    case WM_RBUTTONUP:
      shell->host->mouse_up(mouse_from(*shell, lparam, MouseButton::Right));
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;

    // Swallowed. Windows sends this after the release, and letting it through
    // to `DefWindowProc` puts the system's own menu up over ours.
    case WM_CONTEXTMENU:
      return 0;

    case WM_MOUSEWHEEL: {
      // Wheel messages carry screen coordinates, unlike every other mouse
      // message, so they have to be brought back into the client area.
      POINT at{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &at);
      const double notches =
          -static_cast<double>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
      const auto [ux, uy] = to_units(*shell, at);
      shell->host->wheel(WheelEvent{.x = ux,
                                  .y = uy,
                                  .delta_y = notches,
                                  .modifiers = modifiers_now()});
      if (shell->host->needs_paint()) shell->dirty = true;
      return 0;
    }

    // A typed character. Nothing delivered these until there was something to
    // type into, which is why a field could be focused, show a caret, and take
    // nothing but the arrow keys.
    case WM_CHAR: {
      const auto unit = static_cast<char16_t>(wparam);

      // UTF-16 arrives a code unit at a time, so anything outside the basic
      // plane comes as a surrogate pair across two messages. Holding the high
      // half is what turns those back into one code point.
      static char16_t pending_high = 0;
      char32_t codepoint = unit;
      if (unit >= 0xD800 && unit <= 0xDBFF) {
        pending_high = unit;
        return 0;
      }
      if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (pending_high == 0) return 0;  // a low half on its own is nothing
        codepoint = 0x10000 + ((static_cast<char32_t>(pending_high - 0xD800) << 10) |
                               static_cast<char32_t>(unit - 0xDC00));
        pending_high = 0;
      } else {
        pending_high = 0;
      }

      if (shell->host->text(codepoint)) mark_dirty(*app);
      return 0;
    }

    case WM_KEYDOWN: {
      // Everything a field could mean belongs to the field. A digit switches
      // theme and a space plays the timeline — but not while someone is typing,
      // where they are a digit and a space.
      const Widget* typing = shell->host->focused();
      const bool editing = typing != nullptr && typing->wants_text();

      // Escape closes the settings window, which is what every modal on the
      // platform does — and this one is genuinely modal, so without it the only
      // way out is the one small button in its corner. Not while a field has
      // the keyboard: there, Escape abandons the edit.
      if (wparam == VK_ESCAPE && !editing && shell->is_dialog && shell->host != nullptr) {
        if (cutline::ui::Dialog* dialog = cutline::ui::find_dialog(shell->host->root());
            dialog != nullptr) {
          dialog->cancel();
          return 0;
        }
      }

      const Modifiers held = modifiers_now();
      const cutline::editor::Chord chord{.key = key_from_win32(wparam), .control = held.control,
                                         .shift = held.shift, .alt = held.alt};
      const KeyEvent event{.key = chord.key,
                           .modifiers = held,
                           .repeat = (lparam & (1 << 30)) != 0};
      using cutline::editor::Scope;

      // The keymap's three moments. Before the tree: what nothing should be
      // able to swallow -- undo, save -- and the bare keys no control wants,
      // the tools and the transport. While somebody is typing the field goes
      // first, so Ctrl+A selects its text rather than every clip, and the bare
      // keys are not offered at all: there they are letters and a space.
      if (editing && shell->host->key_down(event)) {
        mark_dirty(*app);
        return 0;
      }
      if (const auto* action = app->keys.resolve(chord, Scope::Always); action != nullptr) {
        run_action(*app, *shell, action->id);
        return 0;
      }
      if (!editing) {
        if (const auto* action = app->keys.resolve(chord, Scope::Bare); action != nullptr) {
          run_action(*app, *shell, action->id);
          return 0;
        }
      }

      if (event.key == Key::Tab) {
        shell->host->focus_next(event.modifiers.shift);
        mark_dirty(*app);
        return 0;
      }

      // After the tree, and only if it did not want the key. A focused slider
      // owns the arrows before the playhead does.
      if (!shell->host->key_down(event)) {
        if (shell->is_main() && menu_bar_key(*app, event.key)) {
          mark_dirty(*app);
          return 0;
        }
        if (const auto* action = app->keys.resolve(chord, Scope::Fallback); action != nullptr) {
          run_action(*app, *shell, action->id);
        }
      }
      mark_dirty(*app);
      return 0;
    }

    case WM_SYSKEYDOWN: {
      // Alt shows the menus' access letters straight away, as Windows does,
      // and only the menus' own letters are taken: Alt+F4 and Alt+Space go on
      // to Windows.
      if (wparam == VK_MENU) {
        if ((lparam & (1 << 30)) == 0) shell->alt_alone = true;
        shell->host->show_keyboard_cues();
        mark_dirty(*app);
        return 0;
      }
      shell->alt_alone = false;
      if (shell->is_main() && wparam == VK_F10 && !app->menu_buttons.empty()) {
        shell->host->show_keyboard_cues();
        shell->host->set_focus(app->menu_buttons.front());
        mark_dirty(*app);
        return 0;
      }
      if (shell->is_main() && wparam >= 'A' && wparam <= 'Z') {
        for (Button* title : app->menu_buttons) {
          if (title->text().empty() ||
              std::toupper(static_cast<unsigned char>(title->text().front())) !=
                  static_cast<int>(wparam)) {
            continue;
          }
          shell->host->show_keyboard_cues();
          title->fire();
          mark_dirty(*app);
          return 0;
        }
      }
      // Any other chord with Alt in it is the keymap's, wherever it is offered:
      // no control takes Alt, and Windows sends these here rather than as key
      // presses, which is why Alt+Left never nudged until this did. A control
      // taking text goes first -- the shortcuts window waiting for a key.
      {
        const Modifiers held = modifiers_now();
        if (const Widget* typing = shell->host->focused();
            typing != nullptr && typing->wants_text() &&
            shell->host->key_down(KeyEvent{.key = key_from_win32(wparam), .modifiers = held})) {
          mark_dirty(*app);
          return 0;
        }
        const cutline::editor::Chord chord{.key = key_from_win32(wparam), .control = held.control,
                                           .shift = held.shift, .alt = true};
        if (const auto* action = app->keys.holder(chord); action != nullptr) {
          run_action(*app, *shell, action->id);
          return 0;
        }
      }
      break;
    }

    case WM_SYSKEYUP:
      // Without this, Windows takes a lone Alt as the start of its own menu
      // loop, and the next key pressed is quietly eaten by a menu that is not
      // there.
      if (wparam == VK_MENU || wparam == VK_F10) {
        if (wparam == VK_MENU && shell->alt_alone && shell->is_main() &&
            !app->menu_buttons.empty()) {
          // Again leaves the bar, which is the other half of what Alt does.
          const bool on_bar =
              std::ranges::contains(app->menu_buttons, shell->host->focused());
          shell->host->set_focus(on_bar ? nullptr : app->menu_buttons.front());
          mark_dirty(*app);
        }
        shell->alt_alone = false;
        return 0;
      }
      break;

    case WM_SYSCHAR:
      // The letter of an Alt combination, which Windows would otherwise answer
      // with a beep for a menu bar it cannot see. Alt+Space is left for the
      // window menu.
      if (wparam != VK_SPACE) return 0;
      break;

    case WM_ERASEBKGND:
      return 1;  // every pixel is painted, so erasing only causes a flash

    case WM_TIMER:
      if (wparam == kTooltipTimer) {
        // Once. The pointer has rested; nothing more is due until it moves
        // again, which is what restarts the wait.
        KillTimer(window, kTooltipTimer);
        if (std::string say = shell->host->tooltip_at_pointer(); !say.empty()) {
          shell->host->show_tooltip(std::move(say));
          if (shell->host->needs_paint()) shell->dirty = true;
        }
        return 0;
      }
      if (wparam == kAutosaveTimer) {
        poll_autosave(*app);
        return 0;
      }
      if (wparam == kDisplayTimer) {
        refresh_hdr(*app);
        return 0;
      }
      break;

    case kUpdateChanged:
      settle_update(*app);
      return 0;

#if CUTLINE_HAVE_PREVIEW
    case kMediaReady: {
      // Rebuilt rather than patched: what arrives belongs to a source, and
      // which blocks show it is the binding's answer, not this one's. Both
      // flags are taken before the test, so a burst of arrivals is one rebuild
      // and neither cache is left holding a flag the other consumed.
      const bool waves = app->waveforms.take_arrival();
      const bool strips = app->filmstrips.take_arrival();
      if (waves || strips) refresh_timeline(*app);
      // And the source monitor, when what it shows *is* a waveform. It asks for
      // the peaks and takes whatever the cache has at that moment, which for a
      // source just opened is nothing — so an mp3 opened there drew an empty
      // panel and went on drawing one, the arrival never being passed on. Only
      // for a sound: a picture there has no use for peaks, and re-rendering it
      // for each arrival would decode a frame for nothing.
      if (waves) {
        const cutline::core::Media* shown = source_media_of(*app);
        if (shown != nullptr && !shown->has_video) refresh_source(*app);
      }
      // And the pool, which draws from the same cache when it is showing
      // pictures. Without this a tile stays blank until something else happens
      // to rebuild the panel.
      if (strips && app->browser != nullptr &&
          app->browser->view() == cutline::ui::BrowserView::Icons) {
        refresh_browser(*app);
      }
      // Outside the test as well: the last arrival of a burst brings the count
      // to zero, and a label still saying "Reading media (1)" over an idle
      // machine is worse than no label at all.
      refresh_busy(*app);
      return 0;
    }

    case kPreviewRendered:
      collect_rendered_previews(*app, wparam == 1);
      return 0;

    case kMaskTracked:
      collect_tracked_mask(*app, wparam == 1);
      return 0;

    case kStabilizerAnalysed:
      collect_stabilizer_analysis(*app, wparam == 1);
      return 0;

    case kMulticamSynced:
      if (wparam == 2) {
        show_multicam_sync(*app);
      } else {
        collect_multicam_sync(*app, wparam == 1);
      }
      return 0;

    case kScenesDetected:
      if (wparam == 2) {
        show_scene_detection(*app);
      } else {
        collect_scene_detection(*app, wparam == 1);
      }
      return 0;

    case kRemixHeard:
      collect_remix(*app);
      return 0;

    case kReframeAnalysed:
      collect_reframe_analysis(*app, wparam == 1);
      return 0;

    case kMorphAnalysed:
      collect_morph_analysis(*app, wparam == 1);
      return 0;

    case kPreviewReady:
      // A monitor's thread has a picture. Taken now rather than at the next
      // paint, because nothing may be due to paint: a scrub that has stopped
      // moving is exactly when its last picture arrives.
      poll_preview(*app);
      poll_source(*app);
      return 0;

    case kProxyChanged:
      // Both, and in this order: the ones that finished are attached first, so
      // the line in the corner is redrawn from what is actually left.
      collect_proxies(*app);
      refresh_busy(*app);
      return 0;
#endif

    case WM_CLOSE:
#if CUTLINE_HAVE_PREVIEW
      // A dialog holds nothing but itself, so it simply goes. Which one it is
      // decides what has to be undone with it — the settings window leaves the
      // main window disabled until it is closed properly.
      if (shell->is_dialog) {
        if (app->settings_window.get() == shell) {
          close_settings_dialog(*app);
        } else if (app->keyboard_window.get() == shell) {
          close_keyboard_dialog(*app);
        } else {
          close_export_dialog(*app);
        }
        return 0;
      }
#endif
      // Closing a torn-out window sends its panels home rather than destroying
      // it here. The window goes when the layout stops having a dock for it,
      // which is what keeps the two from ever disagreeing about what exists.
      if (!shell->is_main()) {
        return_panels_home(*app, shell->floating_id);
        return 0;
      }
      // The editor itself, with something unsaved. Three answers rather than
      // two, because "are you sure" with only yes and no makes cancelling and
      // discarding the same button — and one of those loses an afternoon.
      if (app->session.modified() && !confirm_discard(*app)) return 0;
      // Renders still to come go with the editor, so it asks. Media Encoder
      // would go on without Premiere; this queue cannot.
      if (!confirm_abandon_queue(*app)) return 0;
      break;

    case WM_DESTROY:
      // Only the editor's own window ends the session. A floating one closing
      // is a rearrangement.
      if (shell->is_main()) PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

/// The window procedure, and the one place an exception is caught.
///
/// A window procedure is a callback the *kernel* invokes, and an exception
/// leaving one is `STATUS_FATAL_USER_CALLBACK_EXCEPTION`: the process is
/// terminated on the spot, with no message, no chance to save, and nothing in
/// the log but a fault offset inside `KERNELBASE`. That is how resizing a
/// torn-out window looked like the application simply vanishing.
///
/// Caught rather than left to it, and said out loud. Swallowing a fault is not
/// the point — the point is that whatever went wrong is worth one dialogue and
/// a chance to save the project, and that a bug in one message must not take
/// the document with it.
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) try {
  return handle_message(window, message, wparam, lparam);
} catch (const std::exception& failure) {
  complain(window, std::string("Something went wrong handling a window message.\n\n") +
                       failure.what() +
                       "\n\nThe editor is still running. Save your work.");
  return 0;
} catch (...) {
  complain(window, "Something went wrong handling a window message.\n\n"
                   "The editor is still running. Save your work.");
  return 0;
}

}  // namespace cutline::tool

using namespace cutline::tool;

int main(int argc, char** argv) {
  // Each window draws at its own display's resolution and follows it from one
  // display to another. Without this Windows draws the interface at 96 DPI and
  // stretches the bitmap, which on a 125% display blurs every edge in it.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // `--check [dir]`: the second argument, when there is one, is where each
  // theme's frame is written so it can be looked at.
  if (argc > 1 && std::string_view(argv[1]) == "--check") {
    return self_check(argc > 2 ? argv[2] : "");
  }
  // `--gallery <dir>`: a sheet of every control in every state per theme, for
  // matching the period themes against captures of the real thing.
  if (argc > 2 && std::string_view(argv[1]) == "--gallery") {
    return cutline::tool::write_gallery(argv[2]);
  }
#if CUTLINE_HAVE_PREVIEW
  // The Direct3D 11 video device, built on a thread of its own while the window
  // is still being put together. Every hardware decoder borrows the one device,
  // so this is paid once — and left alone it is paid on the render thread at
  // the first frame of the first clip, which is exactly where somebody notices
  // a sixth of a second going missing.
  //
  // Detached because there is nothing to wait for: a decoder that gets there
  // first simply builds it itself, and the two are serialised inside.
  std::thread(cutline::media::warm_hardware_decoding).detach();
#endif

  // Both of these make windows now -- the benchmark a hidden one for its
  // swapchain -- so the class has to exist first.
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(WNDCLASSEXW);
  window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  // Deliberately null. With a class cursor Windows resets the pointer to it on
  // every move over the client area, and `WM_SETCURSOR` is where the interface
  // gets to say what it should be instead.
  window_class.hCursor = nullptr;
  // The application's own icon, for the taskbar, Alt+Tab and the window's own
  // corner. `hIcon` is the large one and `hIconSm` the small one; naming both
  // is what stops Windows scaling the 256-pixel image down to sixteen and
  // producing a grey smudge where the mark should be, since the .ico carries a
  // drawn-for-the-size version of each.
  const HINSTANCE self = window_class.hInstance;
  window_class.hIcon = static_cast<HICON>(LoadImageW(self, MAKEINTRESOURCEW(kIconResource),
                                                     IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
  window_class.hIconSm = static_cast<HICON>(
      LoadImageW(self, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON,
                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
  window_class.lpszClassName = kWindowClass;
  RegisterClassExW(&window_class);

  if (argc > 1 && std::string_view(argv[1]) == "--benchmark") {
    return argc > 2 ? benchmark_playback(argv[2]) : benchmark();
  }

  App app;
  app.main.app = &app;

  // The preferences first of all, and before the tree is built: the theme
  // decides every metric the layout is measured against, so reading it
  // afterwards would mean laying the whole window out twice — and the first of
  // those would be visible.
  if (auto read = cutline::editor::read_settings(app.settings_file); read.has_value()) {
    app.settings = *read;
    app.session.set_transition_lengths(app.settings.transition_lengths());
    const cutline::editor::Settings& settings = app.settings;
    // By name, and quietly ignored when it names one this build does not have.
    const auto theme = std::ranges::find(built_in_themes(), settings.theme, &Theme::name);
    if (theme != built_in_themes().end()) {
      app.theme = static_cast<std::size_t>(theme - built_in_themes().begin());
    }
    app.snapping = settings.snapping;
    app.looping = settings.looping;
    app.aspect_locked = settings.aspect_locked;
    app.preview_scale = settings.preview_scale;
    app.browser_sort = settings.pool_sort;
    app.browser_descending = settings.pool_descending;
    app.pool_view = settings.pool_view;
    app.session.set_undo_depth(static_cast<std::size_t>(settings.undo_depth));
  }
  // After the settings are read and before anything asks for a waveform, so the
  // first request of the session already looks on disk.
  apply_media_cache(app);

  // Before the tree is built, so the first menu and the first tooltip already
  // say the person's own keys.
  if (auto read = cutline::editor::read_keymap(cutline::editor::default_keys_path());
      read.has_value()) {
    app.keys = std::move(*read);
  }
  // A presets file that cannot be read costs the saved presets, not the
  // built-ins, and not the start.
  if (auto read = cutline::editor::read_export_presets(cutline::editor::default_export_presets_path());
      read.has_value()) {
    app.export_presets = std::move(*read);
  }

  // Read before the tree is built, so the window opens in the arrangement it
  // was last left in rather than flashing the default and then moving.
  if (auto read = cutline::editor::read_workspaces(app.workspace_file); read.has_value()) {
    app.workspaces = std::move(*read);
  }
  // Reconciled against the panels this build has, whatever the file said.
  cutline::editor::settle(app.workspaces, known_panels());

  // And the saved presets, before the library is built, so they are in the tree
  // from the first frame rather than appearing after it.
  if (auto read = cutline::editor::read_presets(cutline::editor::default_presets_path());
      read.has_value()) {
    app.presets = std::move(*read);
  }
  if (auto read = cutline::editor::read_bins(cutline::editor::default_bins_path());
      read.has_value()) {
    app.bins = std::move(*read);
  }

  app.main.host = std::make_unique<WidgetHost>(build_interface(&app));
  give_sounds(app, *app.main.host);

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const HWND window =
      CreateWindowExW(kWindowExStyle, kWindowClass, L"Cutline", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                      CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance, nullptr);
  if (window == nullptr) {
    std::println("could not create a window");
    return 1;
  }

  app.main.window = window;
  // Made at 1280 by 800 pixels before anything knew which display it would be
  // on; now that it does, 1280 by 800 units of it.
  app.main.scale = scale_of(window);
  if (app.main.scale != 1.0) {
    SetWindowPos(window, nullptr, 0, 0, to_pixels(1280.0, app.main.scale),
                 to_pixels(800.0, app.main.scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  // The shell rather than the app: a floating window's proc needs to know
  // which window it is, and every one of them finds its app through this.
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app.main));
  refresh_title(app);

#if CUTLINE_HAVE_PREVIEW
  app.timeline_media.waveforms = [&app](std::string_view media_id, int stream) {
    return app.waveforms.find(media_id, stream);
  };
  app.timeline_media.filmstrips = [&app](std::string_view media_id) {
    return app.filmstrips.find(media_id);
  };
  // Set once the window exists, since posting to one that does not is the
  // message going nowhere. Runs on a worker's thread, so each does the one
  // thing that is safe from there and leaves the rest to the message handler.
  app.waveforms.set_on_arrival([window] { PostMessageW(window, kMediaReady, 0, 0); });
  app.filmstrips.set_on_arrival([window] { PostMessageW(window, kMediaReady, 0, 0); });
  app.proxies.set_on_change([window] { PostMessageW(window, kProxyChanged, 0, 0); });
#endif
  // Same shape, and for the same reason: the answer arrives on a worker while
  // the loop is blocked on its queue.
  app.updater.set_on_change([window] { PostMessageW(window, kUpdateChanged, 0, 0); });

  // One pixel of frame handed back to the compositor, which is enough to keep
  // the drop shadow and the rounded corners the system draws around a window.
  // Without it a borderless window looks like a rectangle pasted on the screen.
  const MARGINS shadow{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);

  // Forces the frame to be recalculated now that WM_NCCALCSIZE will answer
  // differently, rather than at whatever moment the window is first moved.
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);

  // A saved arrangement can have had panels torn out of it. Those windows are
  // made here rather than at the first rearrangement, or they would not come
  // back until something else moved.
  reconcile_windows(app);
  refresh_float_titles(app);

  // Anything left over from a session that did not close cleanly. Only the
  // never-saved document is offered here; a recovery copy of a project on disk
  // is offered when that project is opened, which is where somebody is
  // expecting to be asked about it.
  (void)offer_recovery(app, {});

  // A file named on the command line, which is what "Open with", a shortcut
  // with an argument, and a project dropped on the executable all come through
  // as. After the recovery offer, so a crashed session is still asked about
  // first — and after the window is up, because everything here may need to
  // complain, and a message box with no owner appears behind everything.
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    open_from_command_line(app, std::filesystem::path(argv[1]));
  }

  SetTimer(window, kAutosaveTimer, kAutosaveTickMs, nullptr);
  SetTimer(window, kDisplayTimer, kDisplayTickMs, nullptr);

  // Two loops in one, and which is running is the difference between an
  // interface and a video player.
  //
  // Sitting still, this blocks in `GetMessage` and does nothing at all until
  // something happens: an interface that repaints at sixty hertz while nothing
  // moves is heat for nothing. Playing, it cannot block — the playhead moves
  // whether or not the mouse does — so it drains what has arrived and then asks
  // playback whether a new frame is due.
  MSG message{};
  bool running = true;
  while (running) {
    // Playing or exporting, the loop cannot block: the playhead moves and the
    // progress bar fills whether or not anything is being clicked.
    //
    // `finished` is in here as well as `exporting`, and it matters. The worker
    // clears `running` and sets `finished` as its last two acts, so a loop
    // watching only the first would go back to blocking with a thread still to
    // join and a bar stopped short of the end — which worked, whenever the
    // timing happened to fall the right way.
    //
    // `playback_live` rather than `playing` for the same reason, and it is the
    // same bug twice: a player that has reached the end has already stopped
    // running, so a loop watching only that goes back to blocking with the
    // end still to handle — and a loop that was meant to round again never
    // does, because nothing wakes to notice.
    const bool animating = app.animating();
    if (app.playback_live() || app.shuttling() || app.source_shuttling() ||
        app.source_playing() || app.exporting() ||
        animating) {
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
          running = false;
          break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (running) {
        advance_playback(app);
        advance_shuttle(app);
        advance_source_shuttle(app);
        advance_source_playback(app);
        poll_export(app);
        // Nothing is due this instant when only the export is running, and the
        // encoder wants the core far more than this loop does.
        if (!app.playback_live() && !app.shuttling() && !app.source_shuttling() &&
            !app.source_playing()) {
          // A fade is paced by the swapchain's wait for the screen; a
          // millisecond here is only so a hidden window cannot spin.
          Sleep(animating && !app.exporting() ? 1 : 8);
        }
      }
    } else if (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    } else {
      running = false;
    }

    // Asked of every window, because most changes are to the document and show
    // in whichever ones happen to be open.
    for (Shell* shell : app.shells()) {
      if (shell->dirty && shell->window != nullptr) {
        InvalidateRect(shell->window, nullptr, FALSE);
      }
    }
  }

  KillTimer(window, kAutosaveTimer);

  // Closing now asks about unsaved work, so anything still unsaved here was
  // deliberately discarded — and the recovery copy goes with it. What is left
  // in the recovery directory is exactly what was never saved and never
  // answered for, which is to say: what a crash took.
  clear_autosave(app, app.session.path());

  // Before the windows go, so neither the audio thread nor the export thread is
  // still reading things that are being torn down.
  invalidate_playback(app);
  settle_export(app);
#if CUTLINE_HAVE_PREVIEW
  // Both render threads, while the windows they post to and the device they
  // draw on are certainly still there.
  app.preview.reset();
  app.source_preview.reset();
#endif

  // Where the panels were left, so the next session opens the same way. On the
  // way out rather than on every drag: a rearrangement is a dozen small moves
  // and writing a file for each of them would be silly.
  save_workspaces(app, false);

  // The floating windows are owned by the main one and are already gone, but
  // their shells still hold host pointers that a stray message would follow.
  app.floats.clear();
  return 0;
}
