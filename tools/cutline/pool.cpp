/// The project panel's pool: what is in it, how it is sorted and filed into
/// bins, placing from it, relinking, and the proxies made for it.

#include "app.hpp"

namespace cutline::tool {

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
                      std::optional<cutline::ui::DropPoint> where) {
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
void place_from_pool(App& app, std::size_t index, std::optional<cutline::ui::DropPoint> where) {
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
      // Premiere's, for the clips of every camera selected together.
      rows.push_back(action_entry("Create Multi-Camera Source Sequence...",
                                  [&app] { open_multicam_dialog(app); }));
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

}  // namespace cutline::tool
