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
  app.monitor->set_transform(
      selection.empty()
          ? std::nullopt
          : cutline::editor::monitor_box(document, selection.front(), app.session.playhead()));

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

/// The heading an ordering belongs to, or nothing for pool order — which has no
/// column, being the order things were imported in rather than a fact about any
/// of them.
[[nodiscard]] std::optional<cutline::ui::MediaColumn> sort_column(
    cutline::editor::BrowserSort sort) noexcept {
  using cutline::editor::BrowserSort;
  using cutline::ui::MediaColumn;
  switch (sort) {
    case BrowserSort::Pool: return std::nullopt;
    case BrowserSort::Name: return MediaColumn::Name;
    case BrowserSort::Kind: return MediaColumn::Kind;
    case BrowserSort::Duration: return MediaColumn::Duration;
    case BrowserSort::Uses: return MediaColumn::Uses;
  }
  return std::nullopt;
}

/// The ordering a heading asks for.
[[nodiscard]] cutline::editor::BrowserSort sort_for(cutline::ui::MediaColumn column) noexcept {
  using cutline::editor::BrowserSort;
  using cutline::ui::MediaColumn;
  switch (column) {
    case MediaColumn::Name: return BrowserSort::Name;
    case MediaColumn::Duration: return BrowserSort::Duration;
    case MediaColumn::Uses: return BrowserSort::Uses;
    case MediaColumn::Kind: return BrowserSort::Kind;
  }
  return BrowserSort::Name;
}

/// A heading clicked: order by it, or reverse it if it is already the one.
///
/// The third click on the same heading goes back to pool order rather than
/// round again. Pool order has no column of its own, so without this there
/// would be no way back to it once a heading had been touched — and the order
/// footage was brought in is the one people mean by "how it was".
void sort_by_column(App& app, cutline::ui::MediaColumn column) {
  const cutline::editor::BrowserSort wanted = sort_for(column);
  if (app.browser_sort != wanted) {
    app.browser_sort = wanted;
    app.browser_descending = false;
  } else if (!app.browser_descending) {
    app.browser_descending = true;
  } else {
    app.browser_sort = cutline::editor::BrowserSort::Pool;
    app.browser_descending = false;
  }
  save_settings(app);
  refresh_browser(app);
}

/// Rebuilds the media pool from the session.
///
/// Through the binding every time rather than being patched, for the same
/// reason the timeline is: an undo can put media back that no incremental
/// update would know to restore.
/// How many frames one pool tile is worth taking, across the whole source.
///
/// A tile is about 108 pixels wide and the pointer's position across it picks
/// the frame, so a dozen is already finer than anybody can aim. Taking them is
/// most of the cost of showing a pool of footage: 0.74 s for an eight-second 4K
/// source, decoded on the card.
constexpr int kPoolTileFrames = 12;

/// Asks for a few frames of every source the icon view is showing.
///
/// Only in the icon view, and deliberately *coarse*: the tile scrubs across the
/// whole file, but a handful of frames spread over it is what a 108-pixel
/// picture can distinguish anyway.
///
/// This asked for the whole of each source at the timeline's own fineness until
/// somebody read it — the comment above has said "a short stretch" since it was
/// written, and the line below asked for `0.0, media.duration`. Forty sources
/// was forty-eight seeks each. Worse than the cost, the coverage it recorded
/// claimed those seconds were done, so every clip of that source on the
/// timeline was stuck with one frame every twelve seconds for the rest of the
/// session.
void request_pool_pictures([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.browser == nullptr || app.browser->view() != cutline::ui::BrowserView::Icons) return;

  for (const cutline::core::Media& media : app.session.project().media) {
    if (!media.has_video || cutline::core::is_generated_media(media) || media.is_image) continue;
    if (media.path.empty() || media.duration <= 0.0) continue;
    // Spread across the whole source, however long it is. A ten-minute capture
    // and a ten-second one both cost twelve frames.
    app.filmstrips.request(media.id, media.path, 0.0, media.duration,
                           media.duration / kPoolTileFrames);
  }
#endif
}

void refresh_browser(App& app) {
  if (app.browser == nullptr) return;

  cutline::editor::BrowserOptions options;
  options.sort = app.browser_sort;
  options.descending = app.browser_descending;
  options.expanded = app.expanded_bins;
#if CUTLINE_HAVE_PREVIEW
  // Whatever the renderer could not open. It only knows once it has tried, so
  // a file that has moved shows as offline after the first render rather than
  // the moment the project opens.
  if (app.preview != nullptr) options.offline = app.preview->missing_media();
#endif

  std::vector<cutline::ui::MediaItem> rows =
      cutline::editor::browser_items(app.session.project(), options);
#if CUTLINE_HAVE_PREVIEW
  // Attached here rather than in the binding, because a filmstrip is a fact
  // about what has been decoded so far rather than about the project — and the
  // binding is tested without a decoder anywhere near it.
  if (app.browser->view() == cutline::ui::BrowserView::Icons) {
    for (cutline::ui::MediaItem& row : rows) {
      if (row.is_bin) continue;
      row.filmstrip = app.filmstrips.find(row.id);
    }
  }
#endif
  app.browser->set_items(std::move(rows));
  app.browser->set_sorted_by(sort_column(app.browser_sort), app.browser_descending);
  request_pool_pictures(app);
  mark_dirty(app);
}

/// Puts a pool entry on the timeline.
///
/// `where` is where it was dropped, or nothing for a double-click — which
/// lands at the playhead on the topmost video track, the way every editor's
/// "insert" does.
/// Puts a named media on the timeline, wherever the drop landed.
///
/// Shared by the pool and the source monitor, which are two ways of saying the
/// same thing about the same media — and a second copy of this arithmetic would
/// be a second chance for the two to disagree about where a clip goes.
void place_media_from(App& app, const std::string& media_id,
                      std::optional<cutline::ui::DropPoint> where = std::nullopt) {
  if (media_id.empty()) return;

  double at = app.session.playhead();
  std::string track_id;

  if (where.has_value() && app.timeline != nullptr) {
    at = cutline::core::snap_to_frame(where->time, app.session.project().sequence().fps);

    // Only a video track can be named here: `place_media` puts the audio
    // streams on lanes of its own choosing, and handing it an audio track as
    // the video one would put a picture where no picture can go.
    const cutline::ui::TimelineModel& model = app.timeline->model();
    if (where->track < model.tracks.size() && !model.tracks[where->track].audio) {
      track_id = model.tracks[where->track].id;
    }
  }

  // Dragged from the pool, so it lands as the source is marked — the same span
  // insert and overwrite would place, since it is the same decision.
  const auto range = cutline::core::source_range(app.session.project(), media_id);
  app.session.apply(
      cutline::core::place_media(app.session.project(), media_id, at, track_id, range));
  refresh_all(app);
}

/// The same, named by its row in the pool.
void place_from_pool(App& app, std::size_t index,
                     std::optional<cutline::ui::DropPoint> where = std::nullopt) {
  if (app.browser == nullptr || index >= app.browser->items().size()) return;
  const std::string& id = app.browser->items()[index].id;
  // A bin has nothing to put in a sequence. Reached by dragging a folder onto
  // the timeline, which is a thing somebody will do once.
  if (!cutline::editor::bin_of_row(id).empty()) return;
  place_media_from(app, id, where);
}

/// Shows where a row dragged out of the pool would land, while it is in the air.
///
/// The same arithmetic `place_from_pool` does on release — snapped to the frame
/// grid, aimed at the track under the pointer, the length the source's marks
/// give it — so what is drawn is a promise the drop keeps rather than an
/// approximation of it.
void refresh_drop_ghost(App& app) {
  if (app.timeline == nullptr) return;

  // Worked out first and applied once, so the repaint can be asked for exactly
  // when the answer changed. A ghost that redrew on every mouse move would cost
  // a full repaint per pixel of a gesture that mostly stands still.
  std::optional<cutline::ui::DropGhost> ghost;

  // Two ways a clip arrives from elsewhere: dragged out of the pool, or out of
  // the source monitor's picture. Both promise the same thing, so both are
  // answered here rather than by two nearly identical pieces of code.
  std::string dragged;
  double at_x = 0.0;
  double at_y = 0.0;
  if (app.browser != nullptr) {
    if (const std::optional<std::size_t> row = app.browser->dragging();
        row.has_value() && *row < app.browser->items().size()) {
      dragged = app.browser->items()[*row].id;
      at_x = app.browser->drag_x();
      at_y = app.browser->drag_y();
    }
  }
  if (dragged.empty() && app.source_monitor != nullptr && app.source_monitor->dragging_out()) {
    dragged = app.session.source_media();
    at_x = app.source_monitor->drag_x();
    at_y = app.source_monitor->drag_y();
  }

  if (!dragged.empty()) {
    // Nothing over the tracks means nothing to promise — over the headers, the
    // ruler, or off the panel entirely.
    if (const auto where = app.timeline->drop_at(at_x, at_y); where.has_value()) {
      const cutline::core::Project& project = app.session.project();
      const auto media = std::ranges::find(project.media, dragged, &cutline::core::Media::id);
      if (media != project.media.end()) {
        ghost = cutline::ui::DropGhost{
            .track = where->track,
            .start = cutline::core::snap_to_frame(where->time, project.sequence().fps),
            .duration = cutline::core::placed_length(
                *media, cutline::core::source_range(project, dragged)),
            .label = media->name};
      }
    }
  }

  if (app.timeline->drop_ghost() == ghost) return;
  app.timeline->set_drop_ghost(std::move(ghost));
  mark_dirty(app);
}

/// Takes an entry out of the pool, and every clip that used it with it.
/// The bin a row names, or empty when the row is media or there is no row.
[[nodiscard]] std::string bin_of_selection(const App& app) {
  if (app.browser == nullptr) return {};
  const cutline::ui::MediaItem* chosen = app.browser->selected();
  return chosen == nullptr ? std::string{} : cutline::editor::bin_of_row(chosen->id);
}

/// Shows or hides what is in a bin.
void toggle_bin(App& app, std::size_t row) {
  if (app.browser == nullptr || row >= app.browser->items().size()) return;
  const std::string bin = cutline::editor::bin_of_row(app.browser->items()[row].id);
  if (bin.empty()) return;

  const auto found = std::ranges::find(app.expanded_bins, bin);
  if (found == app.expanded_bins.end()) {
    app.expanded_bins.push_back(bin);
  } else {
    app.expanded_bins.erase(found);
  }
  // Not `refresh_all`: nothing about the project changed, so redrawing the
  // timeline and re-rendering the frame would be work for a folder opening.
  refresh_browser(app);
}

/// Puts a new bin inside whichever one is selected, or at the top level.
///
/// Inside the selection because that is where somebody who has just opened a
/// bin and pressed New expects it, and it is the only way to make a nested one
/// without dragging.
void add_bin(App& app) {
  const std::string parent = bin_of_selection(app);
  app.session.apply(cutline::core::create_bin(app.session.project(), "New Bin", parent));

  const std::string made = app.session.project().bins.back().id;
  // Opened, so that filing something into it is possible without a second
  // click — and so a bin made inside another is visible at all.
  if (!parent.empty() && std::ranges::find(app.expanded_bins, parent) == app.expanded_bins.end()) {
    app.expanded_bins.push_back(parent);
  }
  app.expanded_bins.push_back(made);

  refresh_browser(app);
  if (app.browser != nullptr) app.browser->select_id(cutline::editor::bin_row_id(made));
  // Straight into the rename, because "New Bin" is never the name anybody
  // wanted and a folder called that is worse than no folder.
  rename_pool_entry(app);
}

/// Renames whatever the pool has selected, bin or media.
///
/// A field in a popup rather than a modal prompt, which is the idiom every
/// other rename here already uses — the track headers and the effect bins.
void rename_pool_entry(App& app) {
  if (app.browser == nullptr || app.main.host == nullptr) return;
  const cutline::ui::MediaItem* chosen = app.browser->selected();
  if (chosen == nullptr) {
    complain(app.main.window, "Choose something in the project panel to rename first.");
    return;
  }

  const std::string row_id = chosen->id;
  const std::string bin = cutline::editor::bin_of_row(row_id);
  const std::optional<std::size_t> at = app.browser->selection();
  const Rect anchor = at.has_value() ? app.browser->row_rect(*at) : app.browser->bounds();

  auto popup = std::make_unique<Panel>();
  auto& row = popup->emplace<Box>(Axis::Horizontal);
  auto& field = row.emplace<TextField>(chosen->name);

  const auto commit = [&app, row_id, bin, control = &field] {
    if (app.main.host != nullptr) app.main.host->close_popup();
    const std::string name = control->text();
    if (name.empty()) return;

    app.session.apply(bin.empty()
                          ? cutline::editor::rename_media(app.session.project(), row_id, name)
                          : cutline::core::rename_bin(app.session.project(), bin, name));
    // The whole lot: a media entry's name is on its clips as well as its row.
    refresh_all(app);
  };
  field.set_on_commit([commit](const std::string&) { commit(); });
  row.emplace<Button>("Rename", commit);

  app.main.host->open_popup(std::move(popup), anchor);
  app.main.host->set_focus(&field);
}

/// Files a dragged row into a bin, or moves a bin inside another.
void file_into_bin(App& app, std::size_t from, std::size_t onto) {
  if (app.browser == nullptr) return;
  const std::vector<cutline::ui::MediaItem>& items = app.browser->items();
  if (from >= items.size() || onto >= items.size()) return;

  const std::string into = cutline::editor::bin_of_row(items[onto].id);
  if (into.empty()) return;

  const std::string moving = cutline::editor::bin_of_row(items[from].id);
  app.session.apply(moving.empty()
                        ? cutline::core::file_media(app.session.project(), items[from].id, into)
                        : cutline::core::move_bin(app.session.project(), moving, into));
  // Opened, or what was just filed disappears and reads as having been lost.
  if (std::ranges::find(app.expanded_bins, into) == app.expanded_bins.end()) {
    app.expanded_bins.push_back(into);
  }
  refresh_browser(app);
}

void remove_from_pool(App& app) {
  if (app.browser == nullptr) return;
  const cutline::ui::MediaItem* chosen = app.browser->selected();
  if (chosen == nullptr) return;

  if (const std::string bin = cutline::editor::bin_of_row(chosen->id); !bin.empty()) {
    // Premiere's behaviour, and the destructive one, so anything holding
    // something is asked about. An empty bin goes without a question — there is
    // nothing to lose and asking would train somebody to click through it.
    if (!cutline::core::bin_is_empty(app.session.project(), bin)) {
      const std::string message =
          chosen->name + " is not empty.\n\nDeleting it removes what is inside it, and any "
                         "clips of that media in the sequence.";
      if (MessageBoxA(app.main.window, message.c_str(), "Cutline",
                      MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
        return;
      }
    }
    app.session.apply(cutline::editor::remove_bin(app.session.project(), bin));
    std::erase(app.expanded_bins, bin);
    refresh_all(app);
    return;
  }

  // Asked rather than assumed. Removing media the sequence is built from is
  // undoable, but it is still not something to do on a mis-click.
  if (chosen->uses > 0) {
    const std::string message = chosen->name + " is used by " + std::to_string(chosen->uses) +
                                (chosen->uses == 1 ? " clip." : " clips.") +
                                "\n\nRemoving it takes them out of the sequence too.";
    if (MessageBoxA(app.main.window, message.c_str(), "Cutline", MB_OKCANCEL | MB_ICONWARNING) !=
        IDOK) {
      return;
    }
  }

  app.session.apply(cutline::editor::remove_media(app.session.project(), chosen->id));
  refresh_all(app);
}

/// Gives freshly imported entries the label their kind defaults to.
///
/// Only what has none already: importing the same file twice hands back the
/// entry that is there, and re-labelling it would undo a colour somebody set by
/// hand the first time.
///
/// A transform rather than an edit so that labelling can be folded into the
/// import that caused it. Bringing in forty files and then labelling them is
/// one thing that happened, and it should take one undo to take back.
cutline::core::Project labelled_new_media(const App& app, cutline::core::Project project,
                                          std::span<const std::string> media_ids) {
  for (const std::string& media_id : media_ids) {
    if (media_id.empty()) continue;
    const auto media = std::ranges::find(project.media, media_id, &cutline::core::Media::id);
    if (media == project.media.end() || !media->label_color.empty()) continue;

    const std::string_view colour =
        cutline::editor::label_default(app.settings, cutline::editor::media_kind(*media));
    if (colour.empty()) continue;
    project = cutline::core::set_media_label(std::move(project), media_id, std::string(colour));
  }
  return project;
}

/// The colours a pool entry can be labelled with.
///
/// The same palette a clip's label uses, because a label put on the source is
/// the one that ends up on every clip cut from it — two palettes would mean two
/// meanings for the same colour.
/// The colours a pool entry can be labelled, with the one it has ticked.
[[nodiscard]] std::vector<MenuEntry> media_label_entries(App& app,
                                                         const cutline::ui::MediaItem& chosen) {
  const auto choose = [&app, media_id = chosen.id](std::string color) {
    return [&app, media_id, color = std::move(color)] {
      app.session.apply(cutline::core::set_media_label(app.session.project(), media_id, color));
      // Only the pool. Clips already on the timeline keep whatever they were
      // given, which is the point of them taking a copy at placement.
      refresh_browser(app);
    };
  };

  std::vector<MenuEntry> rows;
  rows.push_back(switch_entry("None", chosen.label_color.empty(), choose({})));
  const std::span<const cutline::editor::ClipLabel> palette = cutline::editor::clip_labels();
  for (std::size_t i = 0; i < palette.size(); ++i) {
    rows.push_back(switch_entry(std::string(cutline::editor::label_name(app.settings, i)),
                                chosen.label_color == palette[i].color,
                                choose(std::string(palette[i].color))));
  }
  return rows;
}

/// An empty sequence beside the open one, taking its canvas and rate.
/// Not opened: adding and switching are two decisions — see `add_sequence`.
void new_sequence(App& app) {
  app.session.apply(cutline::core::add_sequence(app.session.project()));
  refresh_all(app);
  app.dock_stale = true;
}

/// The pool's own menu, from a right-click.
///
/// What a panel this narrow cannot hold as buttons. The row under the pointer is
/// already selected by the time this runs, so every entry acts on what was
/// clicked — and on empty space it acts on nothing, which is why New Bin is
/// first and always available.
void open_pool_menu(App& app, double x, double y) {
  if (app.main.host == nullptr || app.browser == nullptr) return;

  std::vector<MenuEntry> rows;
  rows.push_back(action_entry("New Bin", [&app] { add_bin(app); }));
  rows.push_back(action_entry("New Sequence", [&app] { new_sequence(app); }));

  if (const cutline::ui::MediaItem* chosen = app.browser->selected(); chosen != nullptr) {
    const bool is_bin = !cutline::editor::bin_of_row(chosen->id).empty();
    if (!is_bin) {
      // Premiere's New Sequence From Clip: a cut shaped to that footage, with
      // the clip already on it. It is not opened — see `add_sequence` — so this
      // does not take somebody away from what they were cutting.
      rows.push_back(action_entry("New Sequence From Clip", [&app, id = chosen->id] {
        app.session.apply(cutline::core::sequence_from_clip(app.session.project(), id));
        refresh_all(app);
        app.dock_stale = true;
      }));
    }
    rows.push_back(MenuEntry::divider());
    rows.push_back(action_entry("Rename...", [&app] { rename_pool_entry(app); }));
    rows.push_back(action_entry(is_bin ? "Delete Bin" : "Remove", [&app] { remove_from_pool(app); }));
    if (!is_bin) {
      rows.push_back(submenu_entry("Label", media_label_entries(app, *chosen)));
      rows.push_back(MenuEntry::divider());
      rows.push_back(action_entry("Link Media...", [&app] { relink_pool_entry(app); }));
      // Only for footage with a rate to conform *from*. A still has no frames
      // to re-time and a file whose rate never probed has no ratio to form —
      // offering it there would be offering a box that can only be cancelled.
      const auto asset = std::ranges::find(app.session.project().media, chosen->id,
                                           &cutline::core::Media::id);
      const bool interpretable = asset != app.session.project().media.end() &&
                                 cutline::core::can_interpret(*asset);
      rows.push_back(action_entry("Interpret Footage...",
                                  [&app, id = chosen->id] { open_interpret_dialog(app, id); },
                                  {}, interpretable));
      // Greyed where a row is not inside a bin: there is nothing to move it out
      // of.
      const bool filed = asset != app.session.project().media.end() &&
                         cutline::core::find_bin(app.session.project(), asset->bin) != nullptr;
      rows.push_back(action_entry("Move to Top Level", [&app, id = chosen->id] {
        app.session.apply(cutline::core::file_media(app.session.project(), id, ""));
        refresh_browser(app);
      }, {}, filed));
    }
  }

  app.main.host->open_popup(std::make_unique<MenuList>(std::move(rows)), Rect{x, y, 0.0, 0.0});
}

/// Points a pool entry at a file somewhere else.
///
/// What repairs a project whose footage has moved — a drive letter that
/// changed, a folder that was tidied. Clips name media by id, so repointing the
/// one entry repairs every clip that used it, however many there are.
void relink_pool_entry([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.browser == nullptr) return;
  const cutline::ui::MediaItem* chosen = app.browser->selected();
  // Said rather than ignored. A menu item that does nothing at all is
  // indistinguishable from one that is broken, and this is reached exactly when
  // something is already wrong and patience is short.
  if (chosen == nullptr) {
    complain(app.main.window,
             "Choose the media to relink in the project panel first.");
    return;
  }

  const cutline::core::Project& project = app.session.project();
  const auto media =
      std::ranges::find(project.media, chosen->id, &cutline::core::Media::id);
  if (media == project.media.end()) {
    // A bin, most likely. Said rather than ignored, for the same reason as
    // above: this is reached when something is already wrong.
    complain(app.main.window, "Choose the media to relink in the project panel first.");
    return;
  }
  // Generated media have no file to point anywhere.
  if (media->path.empty() || cutline::core::is_generated_media(*media)) {
    complain(app.main.window, media->name + " is made rather than read, so there is no file "
                                            "to relink it to.");
    return;
  }

  std::array<wchar_t, MAX_PATH> buffer{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = app.main.window;
  dialog.lpstrFilter =
      L"Media\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.wav;*.mp3;*.flac;*.png;*.jpg\0"
      L"All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  // Named in the title, because a project with several files missing is exactly
  // when this is used and "Open" would not say which one is being answered.
  const std::wstring title = L"Relink " + std::filesystem::path(media->name).wstring();
  dialog.lpstrTitle = title.c_str();
  if (GetOpenFileNameW(&dialog) == FALSE) return;

  const std::filesystem::path path{buffer.data()};
  const auto source = cutline::app::probe_source(path.string());
  if (!source.has_value()) {
    complain(app.main.window, "Could not read that file.\n\n" + source.error());
    return;
  }

  app.session.apply(cutline::editor::relink_media(app.session.project(), chosen->id, *source,
                                                 app.settings.still_length));
  // The renderer is holding a decoder for the old path, and the browser is
  // holding the old file's filmstrip and envelope. All three would go on
  // describing a file that is no longer what this entry means.
  if (app.preview != nullptr) app.preview->release_sources();
  app.filmstrips.clear();
  app.waveforms.clear();
  refresh_all(app);
  invalidate_preview(app);
#endif
}

/// Queues proxies for the sources that have not got one.
///
/// Everything in the pool rather than whatever is selected, which is the one
/// place this deliberately parts company with Premiere's per-clip menu. A proxy
/// is wanted when a machine cannot keep up with the footage, and that is a fact
/// about the whole cut — being asked to select the right files first, and to
/// remember which ones were done, is work the application can do itself.
void make_proxies([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  int asked = 0;
  for (const cutline::core::Media& media : app.session.project().media) {
    // Generated media has no file to transcode, and a source that already has a
    // proxy is not made again — rebuilding one is what taking it away first is
    // for.
    if (media.path.empty() || cutline::core::is_generated_media(media)) continue;
    if (!media.proxy_path.empty()) continue;
    if (!media.has_video) continue;  // audio is read from the original either way

    app.proxies.request(media.id, media.name, media.path,
                        cutline::media::default_proxy_path(media.path,
                                                           app.settings.proxy_folder),
                        app.settings.proxy_height);
    ++asked;
  }

  // Said rather than silent. The work is on a worker and the only sign of it is
  // a line in the corner, so a press that queued nothing has to say why or it
  // reads as one that did nothing at all.
  if (asked == 0) {
    const std::size_t have = cutline::core::proxy_count(app.session.project());
    complain(app.main.window,
             have == 0 ? "There is nothing in this project to make a proxy of."
                       : "Every source in this project already has a proxy.");
    return;
  }
  refresh_busy(app);
#endif
}

/// Attaches the proxies that have finished, and reports the ones that did not.
///
/// Called from the frame loop, because this is where a worker's answer becomes
/// part of the project — and the project may only be touched from the thread
/// that owns it.
void collect_proxies([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  const auto finished = app.proxies.take_finished();
  if (!finished.empty()) {
    cutline::core::Project project = app.session.project();
    for (const cutline::app::FinishedProxy& made : finished) {
      project = cutline::core::set_proxy_path(std::move(project), made.media_id, made.path);
    }
    // Applied as one edit however many arrived, so a batch of proxies is one
    // step to undo rather than forty.
    app.session.apply(std::move(project));
    // Only when the project is already reading from proxies: attaching one to a
    // source the renderer has open would otherwise leave it on the original
    // until something else happened to reopen it.
    if (app.session.project().use_proxies && app.preview != nullptr) {
      app.preview->release_sources();
      invalidate_preview(app);
    }
    refresh_all(app);
  }

  const auto failures = app.proxies.take_failures();
  if (!failures.empty()) {
    // One dialog for however many arrived together, because several failing at
    // once is usually one cause — a drive gone, a folder that cannot be written
    // to — and a modal per file during a long batch would be unusable. Said at
    // all, though: a proxy that never appears is indistinguishable from one
    // still being made, and that difference is a session spent waiting.
    std::string message = "Could not make proxies for:";
    for (const cutline::app::FailedProxy& failed : failures) {
      message += "\n\n" + failed.name + "\n" + failed.message;
    }
    complain(app.main.window, message);
  }
#endif
}

/// Abandons the queue and the transcode in progress.
void stop_making_proxies([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.proxies.pending() == 0) {
    complain(app.main.window, "No proxies are being made.");
    return;
  }
  // What has already finished is kept. Those files are written and attached,
  // and throwing them away because the rest was stopped would mean transcoding
  // them again.
  app.proxies.cancel_all();
  refresh_busy(app);
#endif
}

/// Turns reading from proxies on or off.
void toggle_use_proxies([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  const cutline::core::Project& project = app.session.project();
  const bool wanted = !project.use_proxies;
  if (wanted && cutline::core::proxy_count(project) == 0) {
    complain(app.main.window,
             "Nothing in this project has a proxy yet. Make some first, from the Project menu.");
    return;
  }

  app.session.apply(cutline::core::set_use_proxies(project, wanted));
  // The renderer is holding decoders open on the other file. Dropping them is
  // what makes the switch take effect on the next frame rather than the next
  // time something happened to reopen a source.
  if (app.preview != nullptr) app.preview->release_sources();
  refresh_all(app);
  invalidate_preview(app);
#endif
}

/// Makes a monitor's render thread on first use, on the windows' own device so
/// its pictures can be handed over without leaving the card.
///
/// Null when it cannot be made, and then `failed` is set so it is not tried on
/// every frame. A machine with no device will not acquire one.
[[nodiscard]] cutline::app::PreviewEngine* engine_for(
    App& app, std::unique_ptr<cutline::app::PreviewEngine>& engine, bool& failed,
    const char* what) {
#if CUTLINE_HAVE_PREVIEW
  if (engine != nullptr) return engine.get();
  if (failed) return nullptr;

  std::shared_ptr<cutline::gpu::Device> device = app.device;
  if (device == nullptr) {
    auto made = cutline::gpu::Device::create({.allow_software = true});
    if (!made.has_value()) {
      failed = true;
      complain(app.main.window, std::string(what) + " is unavailable.\n\n" + made.error());
      return nullptr;
    }
    device = std::move(*made);
  }

  HWND window = app.main.window;
  auto made = cutline::app::PreviewEngine::create(
      std::move(device), {.on_ready = [window] { PostMessageW(window, kPreviewReady, 0, 0); }});
  if (!made.has_value()) {
    failed = true;
    complain(app.main.window, std::string(what) + " is unavailable.\n\n" + made.error());
    return nullptr;
  }
  engine = std::move(*made);
  engine->set_hdr_display(app.hdr_white);
  return engine.get();
#else
  (void)app;
  (void)engine;
  (void)failed;
  (void)what;
  return nullptr;
#endif
}

/// The project the program monitor shows, as one held pointer.
///
/// The same document at the same preview scale is the same pointer, which is
/// what lets the engine keep the frames it rendered ahead of the playhead. A
/// gesture in flight is a new project on every move, because it is one.
[[nodiscard]] std::shared_ptr<const cutline::core::Project> preview_project(App& app) {
  const bool gesture = app.live_project.has_value();
  if (!gesture && app.preview_snapshot != nullptr &&
      app.preview_snapshot_revision == app.session.document_revision() &&
      app.preview_snapshot_scale == app.preview_scale) {
    return app.preview_snapshot;
  }
  const cutline::core::Project& document = app.showing();
  app.preview_snapshot = std::make_shared<const cutline::core::Project>(
      app.preview_scale == 1.0 ? document
                               : cutline::render::scaled_canvas(document, app.preview_scale));
  // A gesture's project is never reused, so it must not be mistaken for the
  // document's at this revision once the gesture ends.
  app.preview_snapshot_revision = gesture ? ~std::uint64_t{0} : app.session.document_revision();
  app.preview_snapshot_scale = app.preview_scale;
  return app.preview_snapshot;
}

/// Asks for the frame under the playhead.
///
/// Asks, and returns: the picture is rendered on the program monitor's own
/// thread and arrives through `poll_preview`. It used to be rendered here, in
/// the paint, and every seek and every cut held the window still for as long
/// as the decode took.
void refresh_preview(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.monitor == nullptr || app.preview_failed) return;

  // Nothing to show until something has been imported. Rendering an empty
  // project would replace the colour bars with a black rectangle and look
  // like a fault.
  bool has_media = false;
  for (const cutline::core::Track& track : app.session.project().sequence().tracks) {
    if (!track.clips.empty()) has_media = true;
  }
  if (!has_media) return;

  cutline::app::PreviewEngine* engine =
      engine_for(app, app.preview, app.preview_failed, "Preview");
  if (engine == nullptr) return;

  const std::shared_ptr<const cutline::core::Project> project = preview_project(app);
  app.monitor->set_canvas_size(project->sequence().canvas_w, project->sequence().canvas_h);

  // Two ways to the same picture. Sharing a device means the frame is already
  // in memory the window can draw from, so it is handed over as a texture;
  // without one the two have nothing in common and it has to come back through
  // memory, which waits — that machine was always going to be the slow one.
  if (app.shares_device() && std::getenv("CUTLINE_CPU_PREVIEW") == nullptr) {
    // A shuttle is the playhead moving too, at its own rate and either way.
    engine->want(project, app.session.playhead(), app.playing() ? 1.0 : app.shuttle);
    poll_preview(app);
    return;
  }

  auto frame = engine->capture(project, app.session.playhead());
  if (!frame.has_value()) {
    app.preview_failed = true;
    complain(app.main.window, "Could not render the preview.\n\n" + frame.error());
    return;
  }
  app.preview_pixels = std::move(*frame);
  app.monitor->set_frame(cutline::ui::ImageView{.pixels = app.preview_pixels.pixels.data(),
                                                .width = app.preview_pixels.width,
                                                .height = app.preview_pixels.height});
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Puts up whatever the program monitor's thread has finished that is better
/// than what is showing. Cheap when there is nothing: one lock and a look.
void poll_preview([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.preview == nullptr || app.monitor == nullptr || app.preview_failed) return;

  auto taken = app.preview->take(app.session.playhead());
  if (!taken.has_value()) {
    app.preview_failed = true;
    complain(app.main.window, "Could not render the preview.\n\n" + taken.error());
    return;
  }
  if (taken->has_value()) {
    app.monitor->set_texture(**taken);
    app.shown_preview = **taken;
    mark_dirty(app);
  }

  // A source that will not open is only known once the renderer has tried,
  // and that happens on the other thread now. The pool's offline marks follow
  // it here rather than waiting for the next edit to redraw them.
  std::vector<std::string> missing = app.preview->missing_media();
  if (missing != app.preview_missing) {
    app.preview_missing = std::move(missing);
    refresh_browser(app);
  }
#endif
}

/// The source monitor's sequence: one video track holding one clip, the whole
/// of the media, at the media's own shape and rate.
///
/// A source monitor *is* a sequence of one clip, so it can be shown by handing
/// the ordinary renderer a project built on the spot. Nothing new decodes,
/// nothing new composites, and what the source monitor shows is by construction
/// what the sequence would show — which is the property that makes it worth
/// trusting when deciding what to place.
[[nodiscard]] cutline::core::Project source_project(const cutline::core::Media& media) {
  cutline::core::Project project;
  project.sequence().canvas_w = std::max(1, media.width.value_or(1280));
  project.sequence().canvas_h = std::max(1, media.height.value_or(720));
  // The rate it is *played* at, so a conformed source steps and scrubs on the
  // grid it will actually be cut on rather than on the file's.
  const std::optional<double> rate = cutline::core::playback_fps(media);
  project.sequence().fps = rate.value_or(0.0) > 0.0 ? *rate : 30.0;
  project.media = {media};

  cutline::core::Track track;
  track.id = "v1";
  track.kind = cutline::core::TrackKind::Video;

  cutline::core::Clip clip;
  clip.id = "c1";
  clip.media_id = media.id;
  clip.kind = cutline::core::TrackKind::Video;
  clip.source_in = 0.0;
  // The whole of it, marks and all. The marks say what will be *placed*; the
  // monitor still shows the entire source, because a mark you cannot see past
  // is one you cannot move.
  clip.source_out = std::max(media.duration, 1.0 / project.sequence().fps);
  clip.start = 0.0;
  track.clips.push_back(std::move(clip));

  project.sequence().tracks.push_back(std::move(track));
  return project;
}

/// The media the source monitor is showing, or null when there is none.
[[nodiscard]] const cutline::core::Media* source_media_of(const App& app) {
  const std::string& id = app.session.source_media();
  if (id.empty()) return nullptr;
  const cutline::core::Project& project = app.session.project();
  const auto found = std::ranges::find(project.media, id, &cutline::core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// Whether the keyboard is somewhere inside the source monitor.
///
/// Premiere routes the transport and the marking keys to whichever monitor is
/// focused, and this is that question asked of the widget tree rather than of a
/// flag: a flag has to be cleared by everything that could take the focus away,
/// and the one that forgets is the one that leaves `I` marking the wrong thing.
[[nodiscard]] bool source_has_focus(const App& app) {
  if (app.source_panel == nullptr || app.main.host == nullptr) return false;
  for (const Widget* at = app.main.host->focused(); at != nullptr; at = at->parent()) {
    if (at == app.source_panel) return true;
  }
  return false;
}

/// Moves the source monitor's playhead, in frames of the source itself.
void step_source(App& app, double frames) {
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) return;

  const std::optional<double> rate = cutline::core::playback_fps(*media);
  const double fps = rate.value_or(0.0) > 0.0 ? *rate : 30.0;
  const double at = app.source_playhead + frames / fps;
  app.source_playhead =
      std::clamp(cutline::core::snap_to_frame(at, fps), 0.0, std::max(0.0, media->duration));
  refresh_source(app);
}

/// Marks the source at its own playhead, and takes the mark away when it is
/// already there — the same rule the sequence's own `I` and `O` follow.
void mark_source(App& app, bool out_point) {
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) return;

  const std::optional<double>& already = out_point ? media->out_point : media->in_point;
  const std::optional<double> rate = cutline::core::playback_fps(*media);
  const double fps = rate.value_or(0.0) > 0.0 ? *rate : 30.0;
  const bool here =
      already.has_value() && std::abs(*already - app.source_playhead) < 0.5 / fps;

  const std::optional<double> to = here ? std::nullopt
                                        : std::optional<double>(app.source_playhead);
  app.session.apply(out_point
                        ? cutline::core::set_source_out_point(app.session.project(),
                                                              media->id, to)
                        : cutline::core::set_source_in_point(app.session.project(),
                                                             media->id, to));
  refresh_all(app);
}

/// Stops the source playing, and says so on the button.
void stop_source_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_player != nullptr) app.source_player->pause();
  if (app.source_play_button != nullptr) app.source_play_button->set_text("Play");
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Plays the source, or stops it.
void toggle_source_playback([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_playing()) {
    stop_source_playback(app);
    return;
  }

  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr || media->path.empty()) return;
  if (app.player_failed) return;

  // One sound card, so one thing playing. Stopping the sequence rather than
  // refusing: pressing play on a source is a clear statement about which of the
  // two is being watched.
  if (app.playing()) stop_playback(app);
  if (app.shuttling()) set_shuttle(app, 0.0);

  if (app.source_player != nullptr && app.source_player_for != media->id) {
    app.source_player.reset();
  }
  if (app.source_player == nullptr) {
    auto made = cutline::engine::Player::create(source_project(*media),
                                               {.device_id = app.settings.audio_device});
    if (!made.has_value()) {
      app.player_failed = true;
      complain(app.main.window, "Playback is unavailable.\n\n" + made.error());
      return;
    }
    app.source_player = std::move(*made);
    app.source_player_for = media->id;
  }

  app.source_player->seek(app.source_playhead);
  app.source_player->play();
  // The same reason the sequence does it: without this `Sleep(1)` in the loop
  // really sleeps about 15 ms, which is most of a frame at 60 Hz.
  timeBeginPeriod(1);
  if (app.source_play_button != nullptr) app.source_play_button->set_text("Stop");
  mark_dirty(app);
#endif
}

/// One turn of source playback. The sound card says where time is, exactly as
/// it does for the sequence, and the picture follows.
void advance_source_playback([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (!app.source_playing()) return;

  if (!app.source_player->error().empty()) {
    const std::string message = app.source_player->error();
    stop_source_playback(app);
    complain(app.main.window, "Playback stopped.\n\n" + message);
    return;
  }

  const double at = app.source_player->position();
  if (app.source_player->finished()) {
    app.source_playhead = at;
    stop_source_playback(app);
    refresh_source(app);
    return;
  }

  app.source_playhead = at;
  refresh_source(app);
#endif
}

/// Shows a source in the source monitor, remembering it as recently looked at.
///
/// The one way in, so the pool's selection, the dropdown and anything later all
/// leave the same state behind: the list in the order last looked at, and the
/// playhead back at the start when the source itself changed.
void show_source(App& app, const std::string& media_id) {
  if (media_id != app.session.source_media()) app.source_playhead = 0.0;
  app.session.set_source_media(media_id);

  // Newest first, and only ever *added* to the front — something already in the
  // list stays where it is.
  //
  // Reordering on every look was the first attempt and it fought the dropdown
  // it feeds: choosing the second entry moved that entry to the top, so the
  // index the control had just acted on named something else, and the list
  // settled showing the name of the source you had switched away from. A list
  // whose items move while you are choosing from them is the wrong shape for a
  // control you choose from.
  if (!media_id.empty() && std::ranges::find(app.source_recent, media_id) ==
                               app.source_recent.end()) {
    app.source_recent.insert(app.source_recent.begin(), media_id);
    // Long enough to go back and forth between a handful of takes, short enough
    // that the list is still something to glance at rather than read.
    constexpr std::size_t kRecentSources = 10;
    if (app.source_recent.size() > kRecentSources) app.source_recent.resize(kRecentSources);
  }
  refresh_source(app);
  mark_dirty(app);
}

/// Fills the dropdown from what has been looked at, dropping anything the
/// project no longer has — media can be removed, and a list naming something
/// gone would offer a source that cannot be shown.
void refresh_source_list(App& app) {
  if (app.source_choice == nullptr) return;

  const cutline::core::Project& project = app.session.project();
  std::erase_if(app.source_recent, [&](const std::string& id) {
    return std::ranges::find(project.media, id, &cutline::core::Media::id) == project.media.end();
  });

  std::vector<std::string> names;
  names.reserve(app.source_recent.size());
  for (const std::string& id : app.source_recent) {
    const auto found = std::ranges::find(project.media, id, &cutline::core::Media::id);
    names.push_back(found == project.media.end() ? id : found->name);
  }
  if (names.empty()) names.emplace_back("No source");

  if (names != app.source_choice->options()) app.source_choice->set_options(std::move(names));

  const auto at = std::ranges::find(app.source_recent, app.session.source_media());
  app.source_choice->set_selected(at == app.source_recent.end()
                                      ? 0
                                      : static_cast<std::size_t>(at - app.source_recent.begin()));
}

/// Renders the source monitor, and keeps its scrub bar in step.
void refresh_source([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_monitor == nullptr) return;

  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr || cutline::core::is_generated_media(*media) || media->path.empty()) {
    // Nothing chosen, or something with no file behind it. The panel keeps its
    // placeholder rather than showing the last source, which would be a picture
    // of something nobody is pointing at.
    app.source_monitor->clear_frame();
    app.shown_source = {};
    if (app.source_waveform != nullptr) app.source_waveform->set_visible(false);
    if (app.source_scrub != nullptr) {
      app.source_scrub->set_duration(0.0);
      app.source_scrub->set_marks(std::nullopt, std::nullopt);
    }
    refresh_source_list(app);
    app.source_snapshot.reset();
    return;
  }

  refresh_source_list(app);
  if (app.source_scrub != nullptr) {
    app.source_scrub->set_duration(media->duration);
    app.source_scrub->set_marks(media->in_point, media->out_point);
    app.source_scrub->set_playhead(app.source_playhead);
  }

  // A source with no picture is shown as its shape instead. Which of the two
  // widgets is in the layout at all is decided here, so the panel never holds
  // an empty rectangle where the other one would be.
  const bool has_picture = media->has_video;
  if (app.source_waveform != nullptr) {
    app.source_waveform->set_visible(!has_picture);
    if (!has_picture) {
      app.waveforms.request(media->id, media->path, 0);
      app.source_waveform->set_waveform(app.waveforms.find(media->id, 0));
      app.source_waveform->set_duration(media->duration);
      app.source_waveform->set_playhead(app.source_playhead);
    }
  }
  app.source_monitor->set_visible(has_picture);
  if (!has_picture) {
    app.source_monitor->clear_frame();
    app.shown_source = {};
    mark_dirty(app);
    return;
  }

  cutline::app::PreviewEngine* engine =
      engine_for(app, app.source_preview, app.source_failed, "The source monitor");
  if (engine == nullptr) return;

  // The same one-clip sequence for as long as the source is the same, so the
  // engine can render ahead of a playing source rather than starting over at
  // every frame. Compared by value, since the media itself can change — a
  // relink, a conform — and that is a different picture.
  cutline::core::Project wanted = source_project(*media);
  if (app.source_snapshot == nullptr || *app.source_snapshot != wanted) {
    app.source_snapshot = std::make_shared<const cutline::core::Project>(std::move(wanted));
  }
  const cutline::core::Project& project = *app.source_snapshot;
  app.source_monitor->set_canvas_size(project.sequence().canvas_w, project.sequence().canvas_h);

  const double at = std::clamp(app.source_playhead, 0.0, std::max(0.0, media->duration));
  if (app.shares_device()) {
    engine->want(app.source_snapshot, at, app.source_playing() ? 1.0 : 0.0);
    poll_source(app);
    return;
  }

  auto frame = engine->capture(app.source_snapshot, at);
  if (!frame.has_value()) {
    app.source_failed = true;
    complain(app.main.window, "Could not render the source.\n\n" + frame.error());
    return;
  }
  app.source_pixels = std::move(*frame);
  app.source_monitor->set_frame(cutline::ui::ImageView{.pixels = app.source_pixels.pixels.data(),
                                                       .width = app.source_pixels.width,
                                                       .height = app.source_pixels.height});
  mark_dirty(app);
#endif
}

/// The source monitor's `poll_preview`.
void poll_source([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_preview == nullptr || app.source_monitor == nullptr || app.source_failed) return;
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr || !media->has_video) return;

  const double at = std::clamp(app.source_playhead, 0.0, std::max(0.0, media->duration));
  auto taken = app.source_preview->take(at);
  if (!taken.has_value()) {
    app.source_failed = true;
    complain(app.main.window, "Could not render the source.\n\n" + taken.error());
    return;
  }
  if (taken->has_value()) {
    app.source_monitor->set_texture(**taken);
    app.shown_source = **taken;
    mark_dirty(app);
  }
#endif
}

/// Writes the frame at the playhead to a PNG.
///
/// Rendered again rather than read back off the monitor: what is on screen is
/// letterboxed into whatever the panel happens to be, and a snapshot is a frame
/// of the *sequence* at its own size. This is the same call export makes per
/// frame, so the file is what the exported movie would contain.
void take_snapshot([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.preview == nullptr) {
    complain(app.main.window, "There is no preview to take a snapshot of yet.");
    return;
  }

  // The sequence's own size, whatever the preview is set to — a snapshot is a
  // frame of the cut, not of the proxy of it that is being scrubbed. The
  // monitor no longer has to let go of anything first: what it shows is a
  // copy the render thread cannot touch, whatever size it renders at next.
  const auto frame = app.preview->capture(
      std::make_shared<const cutline::core::Project>(app.session.project()),
      app.session.playhead());
  invalidate_preview(app);
  if (!frame.has_value()) {
    complain(app.main.window, "Could not render the frame.\n\n" + frame.error());
    return;
  }
  if (frame->empty()) {
    complain(app.main.window, "There is nothing at the playhead to save.");
    return;
  }

  // Asked for after the frame is in hand. A dialog that opens and then reports
  // that there was nothing to save wastes the answer it just collected.
  const auto path = choose_image_file(app.main.window);
  if (!path.has_value()) return;

  // Through Skia, which this already links for the window's surface. The
  // alternative was the uncompressed writer `render_frame` carries for its own
  // use, and a 4K snapshot stored raw is thirty megabytes.
  const SkImageInfo info = SkImageInfo::Make(frame->width, frame->height,
                                             kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  const SkPixmap pixels(info, frame->pixels.data(), static_cast<std::size_t>(frame->width) * 4);

  SkDynamicMemoryWStream out;
  if (!SkPngEncoder::Encode(&out, pixels, SkPngEncoder::Options{})) {
    complain(app.main.window, "Could not encode the snapshot.");
    return;
  }

  const sk_sp<SkData> encoded = out.detachAsData();
  std::ofstream file(*path, std::ios::binary);
  if (!file) {
    complain(app.main.window, "Could not write " + path->string());
    return;
  }
  file.write(static_cast<const char*>(encoded->data()),
             static_cast<std::streamsize>(encoded->size()));
  if (!file) complain(app.main.window, "Could not write " + path->string());
#else
  complain(app.main.window, "This build has no preview to take a snapshot of.");
#endif
}

/// How wide a frame is measured at.
///
/// A scope is a statistical picture, and a couple of hundred columns tell the
/// same story as four thousand for a fortieth of the work — which matters when
/// it happens every time the playhead moves. It is also about the width the
/// panel is drawn at, so the waveform is not being asked to describe detail
/// finer than a pixel of graph.
constexpr int kScopeWidth = 320;


/// A sequence size in words: the preset's name when it is one of them, and the
/// plain numbers when it is not. A sequence somebody typed a size into should
/// show that size rather than the word "Custom", which says nothing.
[[nodiscard]] std::string canvas_label(const cutline::core::Project& project) {
  for (const CanvasPreset& preset : kCanvasPresets) {
    if (preset.width == project.sequence().canvas_w && preset.height == project.sequence().canvas_h) {
      return std::string(preset.name);
    }
  }
  return std::to_string(project.sequence().canvas_w) + "x" + std::to_string(project.sequence().canvas_h);
}


/// What a monitor zoom level is called. Fit is a word rather than a percentage
/// because it is not one: it is whatever percentage the panel happens to be.
[[nodiscard]] std::string zoom_label(double zoom) {
  if (zoom <= cutline::ui::kMonitorFit) return "Fit";
  return std::to_string(std::lround(zoom * 100.0)) + "%";
}

/// The zoom dropdown that goes under a monitor's picture.
///
/// Both monitors get one and they are independent, so this takes the level and
/// what to do with a new one rather than reaching for either monitor by name.
void add_zoom_control(Box& transport, MonitorView& picture, double chosen,
                      std::function<void(double)> on_choose) {
  std::vector<std::string> names;
  std::size_t at = 0;
  for (std::size_t i = 0; i < cutline::ui::kMonitorZooms.size(); ++i) {
    if (cutline::ui::kMonitorZooms[i] == chosen) at = i;
    names.push_back(zoom_label(cutline::ui::kMonitorZooms[i]));
  }

  auto& zoom = transport.emplace<Dropdown>(std::move(names), at);
  zoom.set_tooltip("How far into the picture to look. Drag with the middle button, or scroll, to pan");
  zoom.set_on_change([on_choose = std::move(on_choose)](std::size_t index) {
    if (index < cutline::ui::kMonitorZooms.size()) on_choose(cutline::ui::kMonitorZooms[index]);
  });

  // Pushed rather than left to the widget's default, because the panel is
  // rebuilt by a rearrangement and the level is remembered by the application.
  picture.set_zoom(chosen);
}

/// Resizes the sequence and brings everything that depends on its size along.
///
/// The preview's renderer is sized from the project on every frame, so it
/// follows without being told; what does not is the monitor's letterbox while
/// there is no picture yet, and the export dialog's "same as sequence", which
/// is showing a number that has just changed.
void apply_canvas(App& app, int width, int height) {
  if (!app.session.apply(cutline::core::set_canvas(app.session.project(), width, height))) {
    return;
  }
  if (app.monitor != nullptr) {
    const cutline::core::Project& project = app.session.project();
    app.monitor->set_canvas_size(project.sequence().canvas_w, project.sequence().canvas_h);
  }
  refresh_title(app);
  invalidate_preview(app);
  app.inspector_stale = true;
}

/// Sets the sequence's frame rate and tells what depends on it.
void apply_fps(App& app, double fps) {
  if (!app.session.apply(cutline::core::set_fps(app.session.project(), fps))) return;
  refresh_timeline(app);
  refresh_title(app);
  invalidate_preview(app);
  // The page that changed it shows what the rate makes possible: drop-frame
  // appears at 29.97 and vanishes at 30, and nothing else would rebuild it.
  if (app.settings_window != nullptr && app.settings_kind == SettingsKind::Project) {
    app.settings_page_stale = true;
  }
}


/// A frame rate written the way a rate is written: 23.976 keeps its decimals
/// and 30 does not get three zeroes it never had.
[[nodiscard]] std::string fps_label(double fps) {
  const std::string text = std::format("{:.3f}", fps);
  const std::size_t last = text.find_last_not_of('0');
  const std::size_t cut = text[last] == '.' ? last : last + 1;
  return text.substr(0, cut);
}

void choose_preview_scale(App& app, double scale) {
  if (app.preview_scale == scale) return;
  app.preview_scale = scale;
  save_settings(app);
  // Kept in step for the callers that are not the control itself — the
  // dropdown has already moved when it is the one asking, and setting it back
  // to where it is costs nothing.
  if (app.preview_scale_choice != nullptr) {
    using Entry = std::pair<double, std::string_view>;
    const auto at = std::ranges::find(kPreviewScales, scale, &Entry::first);
    if (at != kPreviewScales.end()) {
      app.preview_scale_choice->set_selected(
          static_cast<std::size_t>(at - kPreviewScales.begin()));
    }
  }
  invalidate_preview(app);
}

/// The reading under the master fader.
///
/// Silence rather than "-36 dB" at the bottom stop: that is what the scale's
/// floor means everywhere else in the application, and a number there would
/// suggest the mix was merely quiet.
void show_master_gain(App& app, double gain) {
  if (app.master_reading == nullptr) return;
  const double db = cutline::ui::gain_to_fader_db(gain);
  app.master_reading->set_text(db <= cutline::ui::kGainFloorDb
                                   ? std::string("Silent")
                                   : std::format("{:+.1f} dB", db));
}

/// Copies the player's levels into the meter, once a frame.
///
/// Polled rather than pushed: levels are produced every few milliseconds on the
/// render thread and the meter is drawn at frame rate, so a message per block
/// would be several wake-ups for each thing drawn. A meter is also the one
/// display where a missed update is invisible — the next one is milliseconds
/// away and the ballistics carry across it.
void refresh_meter([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  // A player that has not been made yet reads as silence, which is true: there
  // is nothing playing.
  if (app.meter != nullptr) {
    app.meter->set_levels(app.player == nullptr ? cutline::audio::MeterReading{}
                                                : app.player->levels());
  }

  // And every strip's own meter. Polled here rather than pushed from the mixer
  // for the same reason the master's is: this runs at frame rate, a level is a
  // few dozen bytes, and a display where a missed update is invisible does not
  // need a notification.
  for (const App::TrackMeter& strip : app.track_meters) {
    if (strip.view == nullptr) continue;
    strip.view->set_levels(app.player == nullptr
                               ? cutline::audio::MeterReading{}
                               : app.player->track_levels(static_cast<int>(strip.index)));
  }
#endif
}

/// Measures the frame at the playhead, if the scopes are on show and stale.
///
/// Through `frame_at` rather than off the monitor's texture: a scope measures
/// the *frame*, and what the monitor holds may be a texture on the graphics
/// card that would have to come down anyway. Downscaled by sampling rather than
/// averaging — a scope counts pixels that exist, and an averaged pixel is one
/// the frame does not contain.
void refresh_scopes([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.scopes == nullptr || !app.scopes_stale) return;
  app.scopes_stale = false;

  if (app.preview == nullptr) return;

  // At the preview's scale, which is the picture actually on screen — and the
  // size the preview is already built for.
  //
  // It used to ask for the sequence's own size. That was wrong twice over: a
  // scope is a measurement of what is being looked at, and asking for a
  // different size than the monitor just asked for made every frame rebuild
  // the compositor, then rebuild it again for the next picture. The rebuild
  // freed the texture the monitor was showing, which is a crash rather than a
  // slow scope.
  // An HDR sequence is measured as its signal, in its own PQ or HLG code
  // values, rather than as the tone-mapped picture an SDR display is shown.
  const cutline::core::WorkingColour colour = app.session.project().sequence().colour;
  const auto frame = app.preview->capture(preview_project(app), app.session.playhead(),
                                          static_cast<cutline::gpu::WorkingColour>(colour));
  if (!frame.has_value() || frame->empty()) return;

  const int step = std::max(1, frame->width / kScopeWidth);
  const int width = std::max(1, frame->width / step);
  const int height = std::max(1, frame->height / step);

  // Not `small`: <rpcndr.h>, which arrives with windows.h, defines that as
  // `char`, and the error it produces names neither.
  std::vector<std::uint8_t> sampled(static_cast<std::size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* row = frame->pixels.data() +
                              static_cast<std::ptrdiff_t>(y) * step * frame->width * 4;
    for (int x = 0; x < width; ++x) {
      const std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * step * 4;
      const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4;
      sampled[at] = pixel[0];
      sampled[at + 1] = pixel[1];
      sampled[at + 2] = pixel[2];
      sampled[at + 3] = pixel[3];
    }
  }

  const cutline::render::ScopeImage image{
      .pixels = sampled.data(), .width = width, .height = height};

  auto readings = std::make_shared<cutline::ui::ScopeReadings>();
  readings->histogram = cutline::render::compute_histogram(image);
  readings->waveform =
      cutline::render::compute_waveform(image, cutline::render::ScopeChannel::Luma);
  readings->parade = cutline::render::compute_parade(image);
  readings->vectorscope = cutline::render::compute_vectorscope(image);
  readings->measured = true;
  readings->signal = colour == cutline::core::WorkingColour::Rec2100Pq    ? cutline::ui::ScopeSignal::Pq
                     : colour == cutline::core::WorkingColour::Rec2100Hlg ? cutline::ui::ScopeSignal::Hlg
                                                                          : cutline::ui::ScopeSignal::Sdr;

  app.scope_readings = std::move(readings);
  app.scopes->set_readings(app.scope_readings);
  mark_dirty(app);
#endif
}

/// Says the measurements no longer describe what is on screen.
void invalidate_scopes(App& app) {
  app.scopes_stale = true;
  mark_dirty(app);
}

/// Shows one of the four, and lights its tab.
void choose_scope(App& app, cutline::ui::ScopeKind kind) {
  app.scope_kind = kind;
  if (app.scopes != nullptr) app.scopes->set_kind(kind);

  // Relabelled here rather than by rebuilding the row, which would destroy the
  // button whose click is still running — the same trap the theme switcher has.
  if (app.scope_button != nullptr) {
    app.scope_button->set_text(std::string(cutline::ui::to_string(kind)));
  }
  mark_dirty(app);
}

/// Marks the picture as no longer matching the playhead or the project.
void invalidate_preview(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // The gesture's project outlives the gesture only by accident, and an
  // accident here is invisible: the preview goes on rendering a project the
  // document no longer has, and looks merely out of date. Anything asking for
  // a fresh picture while no handle is being dragged is asking for the
  // document's.
  if (!app.dragging_handle && !app.live_gesture) app.live_project.reset();

  app.preview_stale = true;
  // The scopes measure the picture, so anything that changes the picture
  // changes them. Here rather than at every call site, because every one of
  // those already says "the picture is out of date" and would otherwise have to
  // remember to say it twice.
  app.scopes_stale = true;
  mark_dirty(app);
#else
  (void)app;
#endif
}

// ---------------------------------------------------------------- playback --

/// Stops playback and puts the transport back the way it looks when idle.
///
/// Separate from pausing because it is also what happens at the end of the
/// timeline and when the document changes underneath a playing project.
void stop_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // Cleared before the early return, so a player that has gone away cannot
  // leave the application believing playback is still wanted.
  app.playback_wanted = false;
  app.playhead_from_playback = -1.0;
  if (app.player == nullptr) return;
  if (app.player->playing()) {
    app.player->pause();
    // Windows' scheduler tick goes back to its lazy default. Asking for a
    // millisecond costs power across the whole machine, so it is held only
    // while something is actually being paced by it.
    timeEndPeriod(1);
  }
  if (app.play_button != nullptr) app.play_button->set_text("Play");
  app.shown_frame = -1;
  // Once more with the sound stopped, or the bars stay where the last thing
  // playing left them and the meter goes on claiming there is audio.
  refresh_meter(app);

  // The pass ends when the sequence does. Applied here rather than as it was
  // recorded, so riding a fader through a passage is one edit and one undo
  // rather than one per displayed frame.
  commit_automation(app);
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Rebuilds the player for a document that changed under it, and carries on.
///
/// A player holds a plan of the sequence and a decode of the sound it needs, so
/// an edit leaves it playing something the document no longer describes. It
/// used to stop, which was honest and is not what an editor should do: cutting
/// while watching is most of what cutting *is*, and a sequence that halted
/// every time it was touched had to be started again by hand after every cut,
/// every level, every trim.
///
/// So it is rebuilt and resumed from where it had got to. That costs about
/// 25 ms — a gap rather than a stop — and it is flat in the size of the cut,
/// because building a player now decodes the audio at the playhead rather than
/// all of it. Before that it would have been a third of a second at sixteen
/// cuts and worse after, which is why stopping was the right answer then.
///
/// Seamless would be better still, and means replanning the mixer in place
/// while its device keeps running rather than building another one. That is a
/// change to something the audio thread is reading, and it deserves to be done
/// on its own rather than folded in here.
///
/// Nothing is resumed if it was not playing: an edit made while stopped has
/// nothing to carry on.
void rebuild_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.player == nullptr) return;
  if (!app.playing()) {
    invalidate_playback(app);
    return;
  }

  // Where the *sound* has got to, which is where time is during playback. The
  // session's playhead follows it a frame behind.
  const double at = app.player->position();

  // Dropped before the replacement is built, so two players never hold the
  // output device at once.
  app.player.reset();

  auto made = cutline::engine::Player::create(
      app.session.project(),
      {.start_at = at, .device_id = app.settings.audio_device});
  if (!made.has_value()) {
    app.player_failed = true;
    stop_playback(app);
    complain(app.main.window, "Playback stopped.\n\n" + made.error());
    return;
  }

  app.player = std::move(*made);
  app.player_project = app.session.project();
  app.player_revision = app.session.revision();
  app.player->seek(at);
  app.player->play();
  app.playhead_from_playback = app.session.playhead();
  // The picture is drawn from whichever frame the playhead names, and the one
  // on screen is from the document as it was. Forgetting it is what makes the
  // next turn draw the edit rather than leave the old frame up until the
  // playhead happens to cross into another.
  app.shown_frame = -1;
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Forgets the player, because what it decoded no longer matches the document.
void invalidate_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.player == nullptr) return;
  stop_playback(app);
  app.player.reset();
#else
  (void)app;
#endif
}

void toggle_looping(App& app) {
  app.looping = !app.looping;
  save_settings(app);
  if (app.loop_button != nullptr) app.loop_button->set_selected(app.looping);
  mark_dirty(app);
}

/// The speeds J and L step through, in each direction.
///
/// Premiere's: press again and it goes up. Four is as fast as a picture is
/// still worth looking at — past that the frames a decoder can produce are so
/// far apart that scrubbing the ruler is quicker.
constexpr std::array kShuttleRates{1.0, 2.0, 4.0};

/// The next speed in `direction` from whatever is running now.
///
/// Changing direction starts again at 1 rather than counting down through the
/// speeds: pressing J while running forwards means "go back", not "go forwards
/// a bit less".
[[nodiscard]] double next_shuttle(double current, double direction) noexcept {
  if (current == 0.0 || (current > 0.0) != (direction > 0.0)) return direction;
  const double speed = std::abs(current);
  for (const double step : kShuttleRates) {
    if (step > speed + 1e-9) return direction * step;
  }
  return current;
}

void set_shuttle(App& app, double rate) {
#if CUTLINE_HAVE_PREVIEW
  // Ordinary playback is a shuttle of exactly one: it is the only rate the
  // sound card can play, so it is the only one that gets sound. Everything
  // else moves the picture and stays silent — which is what J and L are for,
  // since what they are used for is finding a moment by eye.
  if (rate == 1.0) {
    app.shuttle = 0.0;
    if (!app.playing()) toggle_playback(app);
    return;
  }

  if (app.playing()) stop_playback(app);
  app.shuttle = rate;
  app.shuttled_at = std::chrono::steady_clock::now();
  app.shuttle_at = app.session.playhead();
  // So the first turn draws rather than deciding it is on the frame already
  // showing and sleeping instead.
  app.shown_frame = -1;
  if (app.play_button != nullptr) {
    app.play_button->set_text(app.shuttling() ? "Stop" : "Play");
  }
  mark_dirty(app);
#else
  (void)app;
  (void)rate;
#endif
}

/// One turn of a shuttle: moves the playhead by however much time has really
/// passed, times the rate.
///
/// By elapsed time rather than a fixed step per turn, because the loop's turn
/// rate depends on how long a frame took to decode — and a shuttle that ran
/// slower on heavy footage would be a shuttle nobody could aim with.
void advance_shuttle(App& app) {
  if (!app.shuttling()) return;

  const auto now = std::chrono::steady_clock::now();
  // Capped, and the cap is what stops one slow turn making the next one worse.
  //
  // A shuttle that advances by however long the last turn took will, after a
  // hitch of a quarter of a second, jump fifteen frames of a 60fps source — and
  // fifteen frames is half the run of decoded frames the renderer keeps, so the
  // next stretch has half as many turns to read ahead in, and stalls sooner.
  // Driven, that showed as hitches arriving four seconds apart and then bunching
  // up: 10832, 11396, 11881, 12941, 13493 milliseconds, closing in.
  //
  // Nothing is out of sync as a result. J and L are a *silent* shuttle — the
  // sound card only plays at its own rate — so there is no clock this has to
  // agree with, and playing a moment slow after a hitch is invisible where the
  // hitch itself is not.
  constexpr double kMaxShuttleStep = 0.1;
  const double elapsed =
      std::min(std::chrono::duration<double>(now - app.shuttled_at).count(), kMaxShuttleStep);
  app.shuttled_at = now;

  const double duration = cutline::core::timeline_duration(app.session.project());
  app.shuttle_at += elapsed * app.shuttle;

  // Stopping at the ends rather than wrapping, unless looping is on — the same
  // rule playback follows, and the same answer to "what does the end mean".
  const bool past_the_end = app.shuttle_at <= 0.0 ||
                            (duration > 0.0 && app.shuttle_at >= duration);
  if (past_the_end && app.looping) {
    app.shuttle_at = app.shuttle > 0.0 ? 0.0 : std::max(0.0, duration);
  }
  app.shuttle_at = std::clamp(app.shuttle_at, 0.0, std::max(0.0, duration));
  app.session.set_playhead(app.shuttle_at);
  if (past_the_end && !app.looping) set_shuttle(app, 0.0);

  // Once per frame of the sequence, the same rule playback follows: the
  // playhead moves continuously and the picture does not have to. Without this
  // a shuttle rebuilt the timeline and asked for a render as fast as the loop
  // could turn, which is a busy core for pictures nobody sees.
  const double fps = app.session.project().sequence().fps > 0.0 ? app.session.project().sequence().fps : 30.0;
  // Rounded, not truncated: the playhead is already on the frame grid, and
  // frame k times the rate can come out a hair under k, which truncation
  // turns into the frame before -- one already shown, so this one was never
  // drawn.
  const auto frame = std::llround(app.session.playhead() * fps);
  if (frame == app.shown_frame) {
    Sleep(1);
    return;
  }
  app.shown_frame = frame;

  show_playhead(app);
  follow_playhead(app);
  refresh_timeline(app);
  scroll_to_playhead(app);
  invalidate_preview(app);
}

void toggle_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // A shuttle is playback of a sort, so the space bar stops it.
  if (app.shuttling()) {
    set_shuttle(app, 0.0);
    return;
  }
  if (app.playing()) {
    stop_playback(app);
    return;
  }
  if (app.player_failed) return;

  // A player that decoded a different project is no use. Dropped here rather
  // than at every edit, so there is one place that decides and it is the place
  // that knows what the player was made from.
  if (app.player != nullptr && app.player_project != app.session.project()) {
    app.player.reset();
  }

  if (app.player == nullptr) {
    // Told where it is about to start, so only the audio that is about to be
    // heard is decoded before this returns. Without it, building a player read
    // every clip in the sequence — which is what made a press of Play cost
    // longer the further into the cut somebody got.
    auto made = cutline::engine::Player::create(
        app.session.project(),
        {.start_at = app.session.playhead(), .device_id = app.settings.audio_device});
    if (!made.has_value()) {
      // Once and no more: a machine with no output device will not acquire one,
      // and a dialog on every press of Play would be worse than the silence.
      app.player_failed = true;
      complain(app.main.window, "Playback is unavailable.\n\n" + made.error());
      return;
    }
    app.player = std::move(*made);
    app.player_project = app.session.project();
  }
  app.player_revision = app.session.revision();

  // From wherever the playhead is, not from wherever playback last stopped —
  // the user may have scrubbed since.
  app.player->seek(app.session.playhead());
  app.player->play();
  app.playback_wanted = true;
  // Armed here, so the first turn compares against where playback was asked to
  // begin rather than against whatever the last run left behind.
  app.playhead_from_playback = app.session.playhead();
  // Pressing play is a statement about what is being watched, so the view goes
  // back to following the playhead even if it was scrolled somewhere else while
  // the last run was going.
  if (app.timeline != nullptr) app.timeline->follow_playhead_again();
  // Without this, `Sleep(1)` in the playback loop really sleeps about 15 ms,
  // which is most of a frame at 60 Hz.
  timeBeginPeriod(1);

  if (app.play_button != nullptr) app.play_button->set_text("Pause");
  app.shown_frame = -1;
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Everything a new frame of playback costs the interface, short of painting.
///
/// Apart from `advance_playback` so `--benchmark` can time exactly this: the
/// part of a playback turn that grows with the length of the timeline.
void show_playback_frame(App& app, double at) {
  app.session.set_playhead(at);
  // Read back rather than stored from `at`: the setter snaps to the frame grid,
  // and comparing next turn against a value that was never written would make
  // every turn look like somebody had moved the playhead.
  app.playhead_from_playback = app.session.playhead();
  show_playhead(app);
  follow_playhead(app);
  refresh_timeline(app);
  // After the refresh, which is what puts the new time on the view.
  scroll_to_playhead(app);
  invalidate_preview(app);
}

/// One turn of playback: follows the sound card and redraws when the frame it
/// is pointing at changes.
///
/// The playhead is *read*, never advanced here. The sound card decides where
/// time is, because it is the one that cannot be nudged — a repeated or dropped
/// picture frame goes unnoticed, while a gap of the same length in audio is a
/// click. So the picture is what adapts.
void advance_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (!app.playback_live()) return;

  if (!app.player->error().empty()) {
    const std::string message = app.player->error();
    stop_playback(app);
    complain(app.main.window, "Playback stopped.\n\n" + message);
    return;
  }

  // An edit while the sound is running leaves the player playing audio the
  // document no longer describes. Stopping is the honest answer: it decoded
  // what it needs up front, so there is nothing to update in place.
  //
  // Guarded by the revision so the project comparison happens when something
  // changed rather than thirty times a second. The revision also moves for a
  // change of selection, which is why the projects are then compared rather
  // than trusted — clicking a clip should not stop playback.
  if (app.session.revision() != app.player_revision) {
    app.player_revision = app.session.revision();

    // The master fader is the one edit playback survives, and it has to: it is
    // set by ear against what is playing, and stopping the sound the moment the
    // button came up would mean it could only ever be set against silence. The
    // mixer takes it live, and nothing that was decoded depends on it — so the
    // change is handed over and then discounted from the comparison below.
    const double master = app.session.project().sequence().master_gain;
    if (master != app.player_project.sequence().master_gain) {
      app.player->set_master_gain(master);
      app.player_project.sequence().master_gain = master;
    }

    if (app.player_project != app.session.project()) {
      rebuild_playback(app);
      return;
    }
  }

  // Before the frame check below: a meter is the one thing here that has
  // something new to say between two pictures, and it costs a copy of a dozen
  // doubles to say it.
  refresh_meter(app);
  mark_dirty(app);

  // And a point from every fader that is writing. Also before the frame check,
  // because a pass is a shape drawn by hand and the hand does not wait for a
  // new picture to be decoded.
  record_automation(app);

  // Something else moved the playhead since the last turn: a transport key, a
  // jump to a marker, the timecode field. Take the sound to it rather than
  // dragging the playhead back to wherever the sound happens to be.
  //
  // Noticed here rather than by seeking at each place that can move the
  // playhead. There are several, more get added, and the ones that forget fail
  // silently — the playhead moves for one frame and is pulled back, which is
  // indistinguishable from the key not being bound.
  if (app.playhead_from_playback >= 0.0 &&
      app.session.playhead() != app.playhead_from_playback) {
    app.player->seek(app.session.playhead());
    app.playhead_from_playback = app.session.playhead();
    // The picture is drawn once per frame of the sequence and only when that
    // frame changes; without this the jump waits for the next one.
    app.shown_frame = -1;
  }

  const double at = app.player->position();
  if (app.player->finished()) {
    // Round again from the start of whatever is marked, rather than stopping.
    // The marked range and not the whole sequence, because marking a passage
    // and looping it is one gesture: it is how a cut gets watched twenty times
    // while a level is set against it.
    if (app.looping) {
      const double from = cutline::core::playback_span(app.session.project(), app.settings.preroll,
                                                       app.settings.postroll)
                              .start;
      app.session.set_playhead(from);
      app.player->seek(from);
      app.player->play();
      app.shown_frame = -1;
      refresh_timeline(app);
      invalidate_preview(app);
      return;
    }
    app.session.set_playhead(at);
    refresh_timeline(app);
    invalidate_preview(app);
    stop_playback(app);
    return;
  }

  // The end of the marked range is the loop's end. The player only knows where
  // the *sequence* stops, so this is the one place that can notice.
  //
  // Widened by the preroll and postroll, so a loop shows what leads into the
  // marked passage and what it lands in. With both at zero this is the marked
  // span exactly, which is what it was before they existed.
  if (app.looping) {
    const cutline::core::MarkedSpan span = cutline::core::playback_span(
        app.session.project(), app.settings.preroll, app.settings.postroll);
    const double end = span.start + span.duration;
    if (span.duration > 0.0 && at >= end) {
      app.session.set_playhead(span.start);
      app.player->seek(span.start);
      app.shown_frame = -1;
      refresh_timeline(app);
      invalidate_preview(app);
      return;
    }
  }

  // Once per frame of the sequence, not once per turn of the loop. Rendering
  // faster than the project's frame rate would draw the same picture twice and
  // charge a decode for it.
  const double fps = app.session.project().sequence().fps > 0.0 ? app.session.project().sequence().fps : 30.0;
  const auto frame = static_cast<long long>(at * fps);
  if (frame == app.shown_frame) {
    // Nothing due yet. Sleeping rather than spinning, because the audio thread
    // wants a core more than this loop does.
    Sleep(1);
    return;
  }
  app.shown_frame = frame;
  // The start of the frame the sound is in, not `at` itself. The playhead's
  // setter rounds to the *nearest* frame, and the test above truncates: a turn
  // late in one frame rounded up into the next, the turn after found that next
  // frame "new" and drew it again, and one frame in four was never shown.
  show_playback_frame(app, static_cast<double>(frame) / fps);
#else
  (void)app;
#endif
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
void refresh_all(App& app) {
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

/// Brings files into the project's pool.
///
/// Several at once, because a day's rushes arrive as a folder rather than as a
/// file: the dialog allows a multiple selection and everything picked is
/// probed, filed and labelled as a single edit.
void import_media(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // Room for many names rather than for one path. A multi-select dialog packs
  // the directory and every chosen filename into this one buffer, and a
  // selection too large for it is refused outright rather than truncated — so
  // the size is the real limit on how much can be brought in at once, and
  // MAX_PATH would have made that limit one file.
  std::vector<wchar_t> buffer(64 * 1024, L'\0');
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = app.main.window;
  dialog.lpstrFilter =
      L"Media\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.wav;*.mp3;*.flac;*.png;*.jpg\0"
      L"All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  // OFN_EXPLORER is named because it is what separates the chosen names with
  // nulls. Without it a multiple selection comes back separated by spaces,
  // which cannot be taken apart again once a filename contains one.
  dialog.Flags =
      OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
  if (GetOpenFileNameW(&dialog) == FALSE) return;

  const std::vector<std::filesystem::path> paths = cutline::editor::chosen_paths(buffer);
  if (paths.empty()) return;

  // Probed before anything is applied, so a folder with one unreadable file in
  // it still brings in the rest. The alternative — stopping at the first
  // refusal — makes one bad file cost the whole selection.
  std::vector<cutline::editor::MediaSource> sources;
  sources.reserve(paths.size());
  std::vector<std::string> refused;
  for (const std::filesystem::path& path : paths) {
    auto source = cutline::app::probe_source(path.string());
    if (source.has_value()) {
      sources.push_back(std::move(*source));
    } else {
      refused.push_back(path.filename().string() + " — " + source.error());
    }
  }

  // Into the pool and nowhere else. Importing is filing something away, not
  // editing with it — and a file that lands on the timeline the moment it is
  // read is one that has to be undone before the sequence can be looked at.
  // Placing it is a gesture of its own: a drag onto a track, or a drop.
  std::vector<std::string> ids;
  if (!sources.empty()) {
    cutline::core::Project next = cutline::editor::import_media(
        app.session.project(), sources, &ids, app.settings.still_length);
    app.session.apply(labelled_new_media(app, std::move(next), ids));
    refresh_all(app);
  }

  // Selected in the pool, so it is obvious where it went — a browser that has
  // silently grown by one is how somebody imports the same file twice. The
  // first of a batch rather than the last, because that is where what arrived
  // begins and scrolling down from it shows the rest.
  const auto first = std::ranges::find_if(ids, [](const std::string& id) { return !id.empty(); });
  if (app.browser != nullptr && first != ids.end()) {
    const auto& items = app.browser->items();
    const auto found = std::ranges::find(items, *first, &cutline::ui::MediaItem::id);
    if (found != items.end()) {
      app.browser->select(static_cast<std::size_t>(found - items.begin()));
    }
    // And into the source monitor, because selecting a row from here does not
    // go through the browser's own callback — it is set, not chosen. Without
    // this the pool showed the new file highlighted and the source monitor
    // said "No source", which is two panels disagreeing about what is selected.
    show_source(app, *first);
  }

  // Said last, so the complaint is about what did not arrive rather than
  // standing between the selection and everything that did. Listed in full up
  // to a point and then counted, because a dialog taller than the screen has
  // an OK button nobody can reach.
  if (!refused.empty()) {
    constexpr std::size_t kMostNamed = 8;
    std::string message = refused.size() == 1 ? "Could not read that file.\n\n"
                                              : "Could not read " + std::to_string(refused.size()) +
                                                    " of the files chosen.\n\n";
    for (std::size_t i = 0; i < refused.size() && i < kMostNamed; ++i) {
      message += refused[i] + "\n";
    }
    if (refused.size() > kMostNamed) {
      message += "and " + std::to_string(refused.size() - kMostNamed) + " more.";
    }
    complain(app.main.window, message);
  }
#else
  complain(app.main.window, "This build has no media layer, so there is nothing to import with.");
#endif
}

/// Makes a title, drops it at the playhead, and selects it.
///
/// Selected because the next thing anyone wants is to type: the inspector shows
/// a title's own text and styling for whatever is selected, so creating one and
/// leaving the selection elsewhere would mean making a card and then having to
/// go and find it.
void add_title(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_title_at(app.session.project(),
                                                  cutline::editor::default_title_spec(),
                                                  app.session.playhead(), {}, &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

void add_matte(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_color_matte_at(
      app.session.project(), cutline::editor::MatteFill{}, app.session.playhead(), {},
      &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

void add_adjustment(App& app) {
  std::string clip_id;
  app.session.apply(cutline::editor::add_adjustment_layer_at(app.session.project(),
                                                             app.session.playhead(), {},
                                                             &clip_id));
  if (!clip_id.empty()) app.session.select_one(clip_id);
  refresh_all(app);
}

/// The system's open or save dialog.
[[nodiscard]] std::optional<std::filesystem::path> choose_file(HWND owner, bool saving) {
  std::array<wchar_t, MAX_PATH> buffer{};

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  // Doubly terminated, and embedded nulls separate the pairs, which is why
  // this is a raw literal rather than anything that counts its own length.
  dialog.lpstrFilter = L"Cutline project\0*.cutline\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"cutline";
  dialog.Flags = saving ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                        : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);

  const BOOL chosen = saving ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
  if (chosen == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

/// The system's folder picker, for the one setting that names a directory.
///
/// `IFileDialog` with `FOS_PICKFOLDERS` rather than the old
/// `SHBrowseForFolder`, which draws a tree from Windows 2000 and cannot be
/// typed into.
///
/// COM is initialised here rather than at startup because this is the only
/// thing on this thread that needs it, and it is undone on the way out unless
/// somebody else had already claimed the thread for another apartment — in
/// which case the dialog still works and the claim is not ours to release.
[[nodiscard]] std::optional<std::filesystem::path> choose_folder(HWND owner) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool ours = SUCCEEDED(com);
  const auto finish = [ours] {
    if (ours) CoUninitialize();
  };

  IFileDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) {
    finish();
    return std::nullopt;
  }
  const auto release = [&] {
    dialog->Release();
    finish();
  };

  DWORD options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
  }
  dialog->SetTitle(L"Where proxies go");

  if (FAILED(dialog->Show(owner))) {
    release();
    return std::nullopt;
  }

  IShellItem* item = nullptr;
  if (FAILED(dialog->GetResult(&item))) {
    release();
    return std::nullopt;
  }

  PWSTR path = nullptr;
  std::optional<std::filesystem::path> chosen;
  if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
    chosen = std::filesystem::path(path);
    CoTaskMemFree(path);
  }
  item->Release();
  release();
  return chosen;
}

/// Where an export should go. Its own dialog rather than a parameter on the
/// one above, because the filters and the default extension are the whole of
/// what that function does and threading them through would leave neither
/// caller readable.
[[nodiscard]] std::optional<std::filesystem::path> choose_movie_file(HWND owner) {
  std::array<wchar_t, MAX_PATH> buffer{};

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"MP4 video\0*.mp4\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"mp4";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

  if (GetSaveFileNameW(&dialog) == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

/// Where a snapshot should go. A third dialog for the same reason the second
/// exists: the filter and the default extension are the whole of what these
/// functions do.
[[nodiscard]] std::optional<std::filesystem::path> choose_image_file(HWND owner) {
  std::array<wchar_t, MAX_PATH> buffer{};

  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"PNG image\0*.png\0All files\0*.*\0";
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrDefExt = L"png";
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

  if (GetSaveFileNameW(&dialog) == FALSE) return std::nullopt;
  return std::filesystem::path(buffer.data());
}

void complain(HWND owner, const std::string& message) {
  MessageBoxA(owner, message.c_str(), "Cutline", MB_OK | MB_ICONWARNING);
}

/// Plays what the person's Windows sound scheme says an event sounds like,
/// when they have asked for sounds at all.
///
/// By alias, and with no default: an event the scheme leaves silent stays
/// silent rather than falling back to the default beep, and nothing Microsoft
/// recorded is shipped with this. Asynchronous, so a menu never waits on a
/// sound to open.
void play_sound(const App& app, cutline::ui::Sound sound) {
  if (!app.settings.sounds) return;
  const wchar_t* alias = nullptr;
  switch (sound) {
    case cutline::ui::Sound::MenuPopup: alias = L"MenuPopup"; break;
    case cutline::ui::Sound::MenuCommand: alias = L"MenuCommand"; break;
    case cutline::ui::Sound::Refused: alias = L".Default"; break;
    case cutline::ui::Sound::Done: alias = L"SystemNotification"; break;
    case cutline::ui::Sound::Failed: alias = L"SystemHand"; break;
  }
  if (alias != nullptr) PlaySoundW(alias, nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

void give_sounds(App& app, WidgetHost& host) {
  host.set_sound_player([&app](cutline::ui::Sound sound) { play_sound(app, sound); });
}

/// Writes a recovery copy if one is due. Called from the autosave timer.
///
/// Silent about failure, and deliberately: this is a background courtesy, and a
/// message box interrupting an edit to say that a file the user never asked for
/// could not be written would be worse than the thing it is warning about. It
/// simply tries again at the next tick.
void poll_autosave(App& app) {
  const std::uint64_t revision = app.session.revision();
  if (!cutline::editor::autosave_due(app.autosave, app.session.modified(), revision,
                                     std::chrono::steady_clock::now(),
                                     std::chrono::seconds(app.settings.autosave_seconds))) {
    return;
  }

  if (cutline::editor::write_autosave(app.session.path(), app.session.project(),
                                      app.settings.autosave_versions)) {
    app.autosave = {.written_at = std::chrono::steady_clock::now(),
                    .written_revision = revision,
                    .ever_written = true};
  }
}

/// Throws away the recovery copy and forgets there was one.
///
/// After a save, and on the way out. What is left in the recovery directory is
/// then exactly what was never recovered from, which is what makes finding one
/// at startup mean something.
void clear_autosave(App& app, const std::filesystem::path& document) {
  cutline::editor::discard_autosave(document);
  app.autosave = {};
}

/// Offers back a recovery copy of `document`, if there is one newer than the
/// document itself. Returns whether it was taken.
///
/// Asked rather than restored silently. A recovery copy is a guess about what
/// somebody wanted, and one that opens a different project from the one they
/// double-clicked without saying so is a guess made too confidently.
[[nodiscard]] bool offer_recovery(App& app, const std::filesystem::path& document) {
  const auto found = cutline::editor::find_recovery(document);
  if (!found.has_value()) return false;

  const std::string what =
      document.empty() ? std::string("an unsaved project")
                       : document.filename().string();
  const std::string message =
      "Cutline has unsaved changes to " + what +
      " from a session that did not close normally.\n\nRecover them?";
  if (MessageBoxA(app.main.window, message.c_str(), "Cutline",
                  MB_YESNO | MB_ICONQUESTION) != IDYES) {
    // Refused once is refused: keeping it would offer the same changes again
    // every time the project was opened.
    clear_autosave(app, document);
    return false;
  }

  const auto loaded = cutline::editor::read_project(found->path);
  if (!loaded.has_value()) {
    complain(app.main.window, "Could not read the recovered project.\n\n" + loaded.error());
    return false;
  }

  // Opened against the *document's* path, so saving writes where the project
  // lives rather than into the recovery directory. It stays modified, because
  // it is: what is on disk is still the older version.
  app.session.reset(loaded->project, document);
  app.session.mark_unsaved();
  refresh_all(app);
  return true;
}

/// Asks whether there is a newer version.
///
/// Only ever from the button. Nothing checks on its own: an editor that phones
/// home the moment it opens is one that has decided on the user's behalf that
/// it may, and this one has no telemetry and no reason to start.
void check_for_updates(App& app) {
  using State = cutline::app::Updater::State;
  switch (app.updater.state()) {
    case State::Available:
      // Already found; pressing again gets on with it.
      app.updater.download();
      break;
    case State::Ready:
      settle_update(app);
      break;
    case State::Checking:
    case State::Downloading:
      break;
    default:
      app.updater.check(cutline::editor::Version{cutline::core::kVersionMajor,
                                                 cutline::core::kVersionMinor,
                                                 cutline::core::kVersionPatch});
      if (app.version_button != nullptr) app.version_button->set_text("Checking...");
      mark_dirty(app);
      break;
  }
}

/// Acts on whatever the updater has just decided.
///
/// Every branch that offers something asks first. An editor that downloaded and
/// ran an installer because somebody pressed a button labelled with a version
/// number would be doing rather more than it said it would.
void settle_update(App& app) {
  using State = cutline::app::Updater::State;
  const std::string running = std::string("v") + std::string(cutline::core::kVersion);

  switch (app.updater.state()) {
    case State::UpToDate:
      if (app.version_button != nullptr) app.version_button->set_text(running);
      MessageBoxA(app.main.window, ("Cutline " + running + " is the latest version.").c_str(),
                  "Cutline", MB_OK | MB_ICONINFORMATION);
      break;

    case State::Available: {
      const cutline::editor::Release found = app.updater.found();
      const std::string message =
          "Cutline " + found.version.to_string() + " is available.\n\n" +
          (found.notes.empty() ? std::string("") : found.notes + "\n\n") + "Download it now?";
      if (app.version_button != nullptr) {
        app.version_button->set_text("Update to " + found.version.to_string());
      }
      if (MessageBoxA(app.main.window, message.c_str(), "Cutline",
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
        app.updater.download();
      }
      break;
    }

    case State::Ready: {
      // The installer replaces the running program, so the editor has to go
      // first — and anything unsaved has to be dealt with before it does.
      if (!confirm_discard(app)) break;
      if (MessageBoxA(app.main.window,
                      "The update is ready.\n\nCutline will close while it installs.", "Cutline",
                      MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
        break;
      }

      const std::wstring path = app.updater.installer().wstring();
      const auto started = reinterpret_cast<INT_PTR>(
          ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
      // Anything at or below 32 is a failure code rather than an instance
      // handle, which is this API's way of saying so.
      if (started <= 32) {
        complain(app.main.window, "The installer would not start. It is at\n\n" +
                                      app.updater.installer().string());
        break;
      }
      PostMessageW(app.main.window, WM_CLOSE, 0, 0);
      break;
    }

    case State::Failed:
      if (app.version_button != nullptr) app.version_button->set_text(running);
      complain(app.main.window, "Could not check for updates.\n\n" + app.updater.error());
      break;

    case State::Downloading:
      if (app.version_button != nullptr) {
        app.version_button->set_text(
            std::format("{:.0f}%", app.updater.progress() * 100.0));
      }
      break;

    case State::Idle:
      break;
  }
  mark_dirty(app);
}

/// Opens a named project, having already established that it is wanted.
///
/// Split from the menu command so the same path serves a file named on the
/// command line — which is what "Open with", a shortcut with an argument, and a
/// project dragged onto the executable all amount to. Without it the
/// application read `--check` and `--benchmark` and silently ignored everything
/// else, so every one of those opened an empty editor.
void open_project_at(App& app, const std::filesystem::path& path) {
  if (offer_recovery(app, path)) return;

  const auto loaded = cutline::editor::read_project(path);
  if (!loaded.has_value()) {
    complain(app.main.window, "Could not open that project.\n\n" + loaded.error());
    return;
  }

  app.session.reset(loaded->project, path);
  refresh_all(app);

  // Warnings are not failures: a project whose footage has moved still opens,
  // and saying so is more use than refusing it.
  if (!loaded->warnings.empty()) {
    std::string message = "The project opened with warnings:\n";
    for (const std::string& warning : loaded->warnings) message += "\n" + warning;
    complain(app.main.window, message);
  }
}

void open_project(App& app) {
  // Before the file dialog, so somebody who decides to save first is not asked
  // to pick a file twice.
  if (!confirm_discard(app)) return;

  const auto path = choose_file(app.main.window, false);
  if (!path.has_value()) return;

  open_project_at(app, *path);
}

/// Acts on a file named on the command line.
///
/// A project opens; anything the importer recognises goes into the pool, which
/// is what "Open with" on a video should reasonably do and is the same place
/// importing puts it. Anything else is said out loud rather than ignored — a
/// mistyped name that opens an empty editor looks exactly like a crash on
/// startup.
void open_from_command_line(App& app, const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    complain(app.main.window, "There is no file at\n\n" + path.string());
    return;
  }

  if (cutline::editor::has_project_extension(path)) {
    open_project_at(app, path);
    return;
  }

#if CUTLINE_HAVE_PREVIEW
  if (cutline::editor::looks_like_media(path.string())) {
    const auto source = cutline::app::probe_source(path.string());
    if (!source.has_value()) {
      complain(app.main.window, "Could not read that file.\n\n" + source.error());
      return;
    }
    std::string id;
    cutline::core::Project next = cutline::editor::import_media(
        app.session.project(), *source, &id, app.settings.still_length);
    const std::array<std::string, 1> ids{id};
    app.session.apply(labelled_new_media(app, std::move(next), ids));
    refresh_all(app);
    if (app.browser != nullptr && !id.empty()) app.browser->select_id(id);
    show_source(app, id);
    return;
  }
#endif

  complain(app.main.window,
           path.filename().string() + " is not a project or a media file.");
}

/// Returns whether it was written, so a cancelled dialog is not mistaken for
/// a successful save.
bool save_project(App& app, bool ask_where) {
  std::filesystem::path path = app.session.path();
  if (ask_where || path.empty()) {
    const auto chosen = choose_file(app.main.window, true);
    if (!chosen.has_value()) return false;
    path = cutline::editor::with_project_extension(*chosen);
  }

  const auto written = cutline::editor::write_project(path, app.session.project());
  if (!written.has_value()) {
    complain(app.main.window, "Could not save the project.\n\n" + written.error());
    return false;
  }

  // The old document's recovery copy goes before the new path is recorded, or
  // "Save As" would leave one behind under the name it used to have.
  clear_autosave(app, app.session.path());
  app.session.mark_saved(path);
  clear_autosave(app, path);
  refresh_title(app);
  return true;
}

/// Asks what to do about unsaved work, and reports whether to carry on.
///
/// Three answers, not two. "Are you sure?" with only Yes and No makes
/// cancelling and discarding the same button, and one of those throws away an
/// afternoon — so Save writes and continues, Discard continues, and Cancel does
/// not. A save that is cancelled at the file dialog cancels the whole thing,
/// which is the only reading of it that cannot lose anything.
[[nodiscard]] bool confirm_discard(App& app) {
  if (!app.session.modified()) return true;

  // The file's own name, not `document_title` — that one carries the asterisk
  // that *means* unsaved, and "unsaved changes to Untitled *" says it twice and
  // reads like a filename with a typo in it.
  const std::string name = app.session.path().empty()
                               ? std::string("this project")
                               : app.session.path().filename().string();
  const int answer = MessageBoxA(
      app.main.window,
      ("There are unsaved changes to " + name + ".\n\nSave them before closing?").c_str(),
      "Cutline", MB_YESNOCANCEL | MB_ICONWARNING);

  if (answer == IDCANCEL) return false;
  if (answer == IDYES) return save_project(app, false);
  return true;
}

void new_project(App& app) {
  if (!confirm_discard(app)) return;
  clear_autosave(app, app.session.path());
  app.session.reset(new_project_model());
  refresh_all(app);
}

/// A key bound to an editing command.
struct Binding {
  Key key;
  bool control = false;
  bool shift = false;
  cutline::editor::Command command;
  /// Rarely set, and last so the common bindings stay readable. What it is for
  /// is the destructive twin of a harmless key: M drops a marker and
  /// Ctrl+Alt+M throws every one of them away, which is Premiere's arrangement
  /// and a good one — the awkward chord is the point.
  bool alt = false;
};

/// Taken before the widget tree sees them. Nothing should be able to swallow
/// undo, and a control-modified key is never something a control wants.
constexpr std::array kApplicationKeys{
    Binding{Key::Z, true, false, cutline::editor::Command::Undo},
    Binding{Key::Z, true, true, cutline::editor::Command::Redo},
    Binding{Key::Y, true, false, cutline::editor::Command::Redo},
    Binding{Key::A, true, false, cutline::editor::Command::SelectAll},
    Binding{Key::K, true, false, cutline::editor::Command::Split},
    // Premiere's Add Edit to All Tracks.
    Binding{Key::K, true, true, cutline::editor::Command::SplitAllTracks},
};

/// Offered only after the widget tree has declined them.
///
/// These are unmodified keys that a control may legitimately want — the arrows
/// belong to a focused slider before they belong to the playhead, and taking
/// them first would make the inspector unusable from the keyboard.
constexpr std::array kTransportKeys{
    // Premiere's I and O, and its Ctrl+Shift+X to throw both away.
    Binding{Key::I, false, false, cutline::editor::Command::MarkIn},
    Binding{Key::O, false, false, cutline::editor::Command::MarkOut},
    Binding{Key::X, true, true, cutline::editor::Command::ClearMarks},
    // And Premiere's markers: M drops one, shift walks to the next, control
    // and shift together walk back.
    Binding{Key::M, false, false, cutline::editor::Command::AddMarker},
    Binding{Key::M, false, true, cutline::editor::Command::NextMarker},
    Binding{Key::M, true, true, cutline::editor::Command::PreviousMarker},
    Binding{Key::M, true, false, cutline::editor::Command::ClearMarkers, true},
    Binding{Key::Left, false, false, cutline::editor::Command::PreviousFrame},
    Binding{Key::Right, false, false, cutline::editor::Command::NextFrame},
    Binding{Key::Home, false, false, cutline::editor::Command::GoToStart},
    Binding{Key::End, false, false, cutline::editor::Command::GoToEnd},
    // Premiere's comma and full stop, which are insert and overwrite there and
    // were nudge here. Nudge moves to alt and the arrows, which is where
    // Premiere keeps it — the two keys next to each other on the bottom row are
    // worth more to the edit that needs a hand on the keyboard.
    Binding{Key::Comma, false, false, cutline::editor::Command::Insert},
    Binding{Key::Period, false, false, cutline::editor::Command::Overwrite},
    Binding{Key::Left, false, false, cutline::editor::Command::NudgeLeft, true},
    Binding{Key::Right, false, false, cutline::editor::Command::NudgeRight, true},
    // Premiere's Q and W: trim the edit point before the playhead up to it, and
    // the one after it back. Both ripple, which is what makes them the fastest
    // way to tighten a cut — the sequence closes over what went.
    Binding{Key::Q, false, false, cutline::editor::Command::TrimPreviousToPlayhead},
    Binding{Key::W, false, false, cutline::editor::Command::TrimNextToPlayhead},
    Binding{Key::Delete, false, false, cutline::editor::Command::Delete},
    Binding{Key::Delete, false, true, cutline::editor::Command::RippleDelete},
    Binding{Key::Escape, false, false, cutline::editor::Command::SelectNone},
    // Premiere's Ctrl+L, and shift for the other direction. Not one key that
    // toggles: a selection with some clips linked and some not has no honest
    // answer to which way it is going, and guessing would quietly unlink what
    // somebody meant to gather up.
    Binding{Key::L, true, false, cutline::editor::Command::LinkClips},
    Binding{Key::L, true, true, cutline::editor::Command::UnlinkClips},
    // The clipboard. Here rather than among the application keys on purpose:
    // the keyframe lanes have their own Ctrl+C and Ctrl+V for the keyframes in
    // them, and taking these first would mean the panel you are working in
    // could never have a clipboard of its own.
    Binding{Key::C, true, false, cutline::editor::Command::Copy},
    Binding{Key::X, true, false, cutline::editor::Command::Cut},
    Binding{Key::V, true, false, cutline::editor::Command::Paste},
    Binding{Key::V, true, true, cutline::editor::Command::PasteInsert},
    // Premiere's Paste Attributes, on Premiere's chord.
    Binding{Key::V, true, false, cutline::editor::Command::PasteAttributes, true},
};

/// How a key is written in a menu: the character itself where it has one, as
/// Windows writes "Ctrl+," rather than "Ctrl+Comma".
[[nodiscard]] std::string key_label(Key key) {
  const auto value = static_cast<std::uint16_t>(key);
  if (value > ' ' && value < 127) return std::string(1, static_cast<char>(value));
  if (key == Key::Escape) return "Esc";
  if (key == Key::Delete) return "Del";
  return std::string(cutline::ui::to_string(key));
}

/// The shortcut a command is bound to, written as a menu shows it, or empty.
///
/// Read from the same tables the keys are handled from, so a menu can never
/// claim a key that does something else.
[[nodiscard]] std::string shortcut_for(cutline::editor::Command command) {
  const auto written = [](const Binding& binding) {
    std::string text;
    // Windows' order, which is the order its own menus write them in.
    if (binding.control) text += "Ctrl+";
    if (binding.alt) text += "Alt+";
    if (binding.shift) text += "Shift+";
    return text + key_label(binding.key);
  };
  for (const Binding& binding : kApplicationKeys) {
    if (binding.command == command) return written(binding);
  }
  for (const Binding& binding : kTransportKeys) {
    if (binding.command == command) return written(binding);
  }
  return {};
}

void run_command(App& app, cutline::editor::Command command);

/// A menu row for an editing command: its key beside it, greyed when it has
/// nothing to act on, and running exactly what the key runs.
[[nodiscard]] MenuEntry command_entry(App& app, std::string label,
                                      cutline::editor::Command command) {
  return MenuEntry{.label = std::move(label),
                   .shortcut = shortcut_for(command),
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

/// The transport and marking keys, when the source monitor is the one in hand.
///
/// Reports whether it took the key, so the sequence's own bindings see only
/// what this did not want. The set is deliberately the same as the sequence's —
/// these are the keys everybody's fingers already know, and which of the two
/// monitors they act on is the whole question a focused panel answers.
bool handle_source_key(App& app, Key key, const Modifiers& modifiers) {
  if (!source_has_focus(app)) return false;
  if (source_media_of(app) == nullptr) return false;
  if (modifiers.control || modifiers.alt) return false;

  const cutline::core::Media* media = source_media_of(app);
  switch (key) {
    case Key::I:
      if (modifiers.shift) return false;
      mark_source(app, false);
      return true;
    case Key::O:
      if (modifiers.shift) return false;
      mark_source(app, true);
      return true;
    case Key::Left:
      // Shift for a bigger step, as the sequence's nudge does.
      step_source(app, modifiers.shift ? -5.0 : -1.0);
      return true;
    case Key::Right:
      step_source(app, modifiers.shift ? 5.0 : 1.0);
      return true;
    case Key::Home:
      app.source_playhead = 0.0;
      refresh_source(app);
      return true;
    case Key::End:
      app.source_playhead = std::max(0.0, media->duration);
      refresh_source(app);
      return true;
    case Key::Space:
      if (modifiers.shift) return false;
      toggle_source_playback(app);
      return true;
    case Key::K:
      stop_source_playback(app);
      return true;
    default:
      return false;
  }
}

/// Runs whichever command the key is bound to, if any.
bool run_binding(App& app, std::span<const Binding> bindings, Key key,
                 const Modifiers& modifiers) {
  for (const Binding& binding : bindings) {
    if (binding.key != key) continue;
    if (binding.control != modifiers.control || binding.shift != modifiers.shift ||
        binding.alt != modifiers.alt) {
      continue;
    }
    run_command(app, binding.command);
    return true;
  }
  return false;
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

[[nodiscard]] std::unique_ptr<Widget> make_project_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& tools = panel->emplace<Box>(Axis::Horizontal);
  tools.emplace<Button>("Import", [app] {
    if (app != nullptr) import_media(*app);
  }).set_tooltip("Bring media into the project (Ctrl+I)");
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

    picture.set_on_transform_change([app](const cutline::ui::MonitorBox& box) {
      const auto selection = app->session.selection();
      if (selection.empty()) return;
      app->dragging_handle = true;
      // Against the session's project every time rather than against the last
      // preview: the box the view reports is where the layer should be, not how
      // far it has come, and compounding them would send it off across the
      // canvas at several times the speed of the pointer.
      app->live_project = cutline::editor::apply_monitor_box(
          app->session.project(), selection.front(), box, app->session.playhead());
      invalidate_preview(*app);
    });
    picture.set_on_transform_commit([app](const cutline::ui::MonitorBox& box) {
      const auto selection = app->session.selection();
      app->dragging_handle = false;
      app->live_project.reset();
      if (selection.empty()) return;

      app->session.apply(cutline::editor::apply_monitor_box(
          app->session.project(), selection.front(), box, app->session.playhead()));
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
  }).set_tooltip("Mark in at the playhead, or clear it (I)");
  transport.emplace<Button>("Mark Out", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::MarkOut);
  }).set_tooltip("Mark out at the playhead, or clear it (O)");
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
  play.set_tooltip("Play or pause (Space)");
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
  }).set_tooltip("Undo (Ctrl+Z)");
  tools.emplace<Button>("Redo", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::Redo);
  }).set_tooltip("Redo (Ctrl+Y)");
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
    button.set_tooltip(entry.hint);
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
  link.set_tooltip("Link the selected clips (Ctrl+L)");
  auto& unlink = tools.emplace<Button>("Unlink", [app] {
    if (app != nullptr) run_command(*app, cutline::editor::Command::UnlinkClips);
  });
  unlink.set_tooltip("Unlink them (Ctrl+Shift+L)");
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
  snap.set_tooltip("Snap edges together (S)");
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
          app.session.project(), *clip_id, *kind, app.settings.transition_length);
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
                   : cutline::editor::apply_library_entry(std::move(next), clip_id, id);
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
  back.set_tooltip("Back one frame (Left)");
  back.set_narrow(true);
  auto& forward = row.emplace<IconButton>(IconButton::Icon::ArrowRight, [app] {
    if (app != nullptr) step_source(*app, 1.0);
  });
  forward.set_tooltip("On one frame (Right)");
  // A pair that belongs together, so they take about the room one control
  // would — which is what buys the Play button its place on the row.
  forward.set_narrow(true);

  auto& play = row.emplace<Button>("Play", [app] {
    if (app != nullptr) toggle_source_playback(*app);
  });
  play.set_tooltip("Play the source, with its sound (Space)");
  if (app != nullptr) app->source_play_button = &play;

  // "In" and "Out" rather than "Mark In" and "Mark Out": this panel is a third
  // of the width the program monitor's row has, and with the two step buttons
  // beside them `--check` found fourteen controls squeezed and clipped in three
  // themes of four. The tooltips carry the whole name.
  auto& mark_in = row.emplace<Button>("In", [app] {
    if (app != nullptr) mark_source(*app, false);
  });
  mark_in.set_tooltip("Mark in on the source (I)");
  auto& mark_out = row.emplace<Button>("Out", [app] {
    if (app != nullptr) mark_source(*app, true);
  });
  mark_out.set_tooltip("Mark out on the source (O)");
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

  // A layout naming a panel this build does not have. Saying so beats an empty
  // rectangle, which looks like a panel that failed to draw.
  auto missing = std::make_unique<Panel>();
  missing->emplace<Label>("There is no panel called " + id).set_small(true);
  return missing;
}

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
    rows.push_back(plain("Import...", [&app] { import_media(app); }, "Ctrl+I"));
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
    rows.push_back(MenuEntry::divider());
    rows.push_back(plain("Export Frame", [&app] { take_snapshot(app); }));
  } else if (panel == "source") {
    const bool has_source = source_media_of(app) != nullptr;
    MenuEntry in = plain("Mark In", [&app] { mark_source(app, false); }, "I");
    MenuEntry out = plain("Mark Out", [&app] { mark_source(app, true); }, "O");
    MenuEntry clear = plain("Clear In and Out", [&app] {
      if (const cutline::core::Media* media = source_media_of(app); media != nullptr) {
        app.session.apply(cutline::core::clear_source_marks(app.session.project(), media->id));
        refresh_all(app);
      }
    });
    for (MenuEntry* each : {&in, &out, &clear}) each->enabled = has_source;
    rows.push_back(std::move(in));
    rows.push_back(std::move(out));
    rows.push_back(std::move(clear));
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

/// Points both derived-media caches at wherever the preference says.
///
/// One function rather than two calls at each site, because the two must never
/// disagree: a waveform found in one place and a filmstrip written to another
/// is a cache somebody empties and finds still full.
void apply_media_cache([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  const std::filesystem::path dir =
      cutline::app::media_cache_dir(app.settings.media_cache_folder);
  app.waveforms.set_cache_dir(dir);
  app.filmstrips.set_cache_dir(dir);
#endif
}

/// Gathers what the person has chosen, and writes it.
///
/// Called from every place that changes one of these rather than on a timer or
/// at shutdown. A setting written only on a clean exit is a setting lost by
/// every crash, and the file is a few hundred bytes — the cost of writing it on
/// each change is nothing next to a preference that does not stick.
void save_settings(App& app) {
  // Started from what is already held rather than from a fresh struct, so a
  // setting with no live field of its own — the labels, the durations — is
  // carried through instead of being dropped. Building this from scratch and
  // naming the fields to copy back is a bug waiting for the next setting: the
  // labels were wiped on every save for exactly that reason until it was found
  // by renaming one and watching it not stick.
  cutline::editor::Settings settings = app.settings;
  settings.theme = std::string(built_in_themes()[app.theme].name);
  settings.snapping = app.snapping;
  settings.looping = app.looping;
  settings.aspect_locked = app.aspect_locked;
  settings.preview_scale = app.preview_scale;
  settings.pool_sort = app.browser_sort;
  settings.pool_descending = app.browser_descending;
  settings.pool_view =
      app.browser == nullptr ? cutline::ui::BrowserView::List : app.browser->view();
  app.settings = settings;

  // Quietly. Losing a preference is not worth a dialog in the middle of an
  // edit, and the one place it would matter — a settings directory that cannot
  // be written at all — would complain once per keystroke.
  (void)cutline::editor::write_settings(app.settings_file, settings);
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
      WS_EX_TOOLWINDOW, kWindowClass, L"Cutline", WS_POPUP | WS_THICKFRAME,
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
    default: return Key::None;
  }
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
  }
  if (app.mixer_stale) {
    app.mixer_stale = false;
    refresh_audio_panel(app);
  }
  if (app.settings_page_stale) {
    app.settings_page_stale = false;
    refresh_settings_page(app);
  }
  // Compared rather than flagged: there are well over a hundred places that
  // apply an edit, and a flag set at each would be a list that has to stay
  // complete for ever. Comparing a hundred short names is nothing.
  refresh_history(app);
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
  // that is a gradient stays one.
  canvas->clear(SK_ColorBLACK);
  canvas->save();
  canvas->scale(static_cast<SkScalar>(shell.scale), static_cast<SkScalar>(shell.scale));
  paint_surface(*painter, client, theme.style(cutline::ui::Part::Window));
  shell.host->paint(*painter, theme);
  canvas->restore();

  shell.surface->present();
}

void set_theme(App& app, std::size_t index) {
  if (index >= built_in_themes().size() || index == app.theme) return;
  app.theme = index;
  save_settings(app);

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

      if (!editing && wparam >= '1' && wparam <= '9') {
        set_theme(*app, static_cast<std::size_t>(wparam - '1'));
        return 0;
      }

      // Application shortcuts, taken before the widget tree sees them: undo is
      // not something any one control should be able to swallow.
      const Modifiers held = modifiers_now();

      // The tools, on bare letters. Before the tree for the same reason the
      // theme digits are: no control wants a V, and a tool that only works when
      // nothing happens to be focused is a tool nobody trusts.
      if (!editing && held.none()) {
        const Key pressed = key_from_win32(wparam);
        const auto tool = std::ranges::find(kTools, pressed, &ToolEntry::key);
        if (tool != kTools.end()) {
          choose_tool(*app, tool->tool);
          return 0;
        }
        // The zoom, with the tools and for the same reason: it changes the
        // view rather than the document. Premiere's keys — `=` and `-` about
        // the playhead, and `\` for the whole sequence.
        if (app->timeline != nullptr &&
            (pressed == Key::Equal || pressed == Key::Minus || pressed == Key::Backslash)) {
          if (pressed == Key::Backslash) {
            app->timeline->zoom_to_fit();
          } else {
            app->timeline->zoom_about_playhead(pressed == Key::Equal);
          }
          mark_dirty(*app);
          return 0;
        }

        // Premiere's S. Here with the tools rather than in the command table
        // because it changes the view and not the document, so there is nothing
        // for `run` to report and nothing for undo to put back.
        if (pressed == Key::S) {
          toggle_snapping(*app);
          return 0;
        }

        // J, K and L: back, stop, forward, pressing again to go faster. The
        // same reasoning as the tool keys — these are muscle memory from every
        // editor there has ever been, and one that only worked when nothing
        // happened to be focused would be one nobody trusted.
        if (pressed == Key::J) {
          set_shuttle(*app, next_shuttle(app->shuttle, -1.0));
          return 0;
        }
        if (pressed == Key::K) {
          set_shuttle(*app, 0.0);
          if (app->playing()) stop_playback(*app);
          return 0;
        }
        if (pressed == Key::L) {
          set_shuttle(*app, app->playing() ? next_shuttle(1.0, 1.0)
                                           : next_shuttle(app->shuttle, 1.0));
          return 0;
        }
      }
      if (held.control && wparam == 'O') {
        open_project(*app);
        return 0;
      }
      if (held.control && wparam == 'S') {
        save_project(*app, held.shift);
        return 0;
      }
      if (held.control && wparam == 'N') {
        new_project(*app);
        return 0;
      }
      if (held.control && wparam == 'I') {
        import_media(*app);
        return 0;
      }
      if (held.control && held.shift && wparam == 'F') {
        float_panel_under_cursor(*app, *shell);
        return 0;
      }
      // Space, before the tree sees it, because a focused button would
      // otherwise take it as a press. Every editor in the world plays with it.
      if (!editing && wparam == VK_SPACE && !held.control && !held.alt) {
        toggle_playback(*app);
        return 0;
      }
      const KeyEvent event{.key = key_from_win32(wparam),
                           .modifiers = held,
                           .repeat = (lparam & (1 << 30)) != 0};

      // While typing, the tree goes first. Ctrl+A means every clip normally and
      // every character inside a field, and a shortcut that reaches past the
      // keyboard focus to select the timeline instead is simply wrong there.
      // Undo still works: a field does not handle Ctrl+Z, so it falls through.
      if (editing && shell->host->key_down(event)) {
        mark_dirty(*app);
        return 0;
      }

      // Before the tree: nothing should be able to swallow undo.
      if (run_binding(*app, kApplicationKeys, event.key, held)) return 0;

      if (event.key == Key::Tab) {
        shell->host->focus_next(event.modifiers.shift);
        mark_dirty(*app);
        return 0;
      }

      // Premiere's `~`: the panel under the pointer fills the window, and again
      // puts it back. Under the *pointer* rather than whatever has the keyboard,
      // because it is used to get a closer look at something being watched, and
      // watching a panel does not focus it.
      if (event.key == Key::Backtick && held.none() && shell->is_main()) {
        if (shell->dock != nullptr) {
          POINT at{};
          GetCursorPos(&at);
          ScreenToClient(window, &at);
          const auto [ux, uy] = to_units(*shell, at);
          const auto over = shell->dock->panel_at(ux, uy);
          const PanelId panel =
              over.value_or(layout_of(*app).maximised);
          if (cutline::ui::toggle_maximised(layout_of(*app), panel)) app->dock_stale = true;
        }
        return 0;
      }

      // After the tree, and only if it did not want the key. A focused slider
      // owns the arrows before the playhead does.
      if (!shell->host->key_down(event)) {
        if (shell->is_main() && menu_bar_key(*app, event.key)) {
          mark_dirty(*app);
          return 0;
        }
        // Premiere sends the transport and the marking keys to whichever
        // monitor is focused, and the source is a monitor. Before the sequence
        // bindings, because these are the same keys meaning the same thing
        // about a different thing — an `I` with the source in hand is a mark on
        // the source, not on the timeline behind it.
        if (!handle_source_key(*app, event.key, held)) {
          run_binding(*app, kTransportKeys, event.key, held);
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
      CreateWindowExW(0, kWindowClass, L"Cutline", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
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
    if (app.playback_live() || app.shuttling() || app.source_playing() || app.exporting() ||
        app.export_job.finished.load() || animating) {
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
        advance_source_playback(app);
        poll_export(app);
        // Nothing is due this instant when only the export is running, and the
        // encoder wants the core far more than this loop does.
        if (!app.playback_live() && !app.shuttling() && !app.source_playing()) {
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
