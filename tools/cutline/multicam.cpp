/// Premiere's multi-camera editing, as the application offers it: Create
/// Multi-Camera Source Sequence on the pool's selection, the Program Monitor's
/// Multi-Camera view with every camera beside the picture, cameras chosen by
/// a click or by their number, and Clip > Multi-Camera.
///
/// What a multicam is and how a switch cuts one is `core/multicam.hpp`; this
/// is where they meet the windows. Synchronising by sound is the one slow
/// step, so it runs on a thread of its own behind a progress window, as mask
/// tracking does.

#include "app.hpp"

#include "cutline/engine/multicam_sync.hpp"

namespace cutline::tool {
namespace {

/// The widest the camera grid is rendered. Each camera gets a cell of it, so
/// four cameras are 480 across each: enough to choose between, and a quarter
/// of the work of rendering them at full size.
constexpr int kGridWidth = 960;

/// The multicam the Multi-Camera view follows and a switch acts on: the one
/// under the playhead on the topmost targeted video track, or failing that on
/// the topmost video track that has one, as Premiere follows the targeted
/// track.
struct Following {
  std::string clip_id;
  std::string track_id;
  std::string media_id;
};

[[nodiscard]] std::optional<Following> following(const App& app) {
  const cutline::core::Project& project = app.session.project();
  const double t = app.session.playhead();
  std::optional<Following> fallback;
  for (const cutline::core::Track& track : project.sequence().tracks) {
    if (track.kind != cutline::core::TrackKind::Video) continue;
    const cutline::core::Clip* clip = cutline::core::clip_at_time(track, t);
    if (clip == nullptr || !cutline::core::shows_camera(project, *clip)) continue;
    const Following found{.clip_id = clip->id, .track_id = track.id, .media_id = clip->media_id};
    if (track.targeted) return found;
    if (!fallback.has_value()) fallback = found;
  }
  return fallback;
}

/// The Synchronizing window's bar, while that window is still the one showing.
[[nodiscard]] cutline::ui::ProgressBar* sync_bar_of(App& app) {
  if (app.multicam_sync_dialog == nullptr || app.main.host == nullptr ||
      app.main.host->popup() != app.multicam_sync_dialog) {
    return nullptr;
  }
  return app.multicam_sync_bar;
}

/// Makes the sequence, once the sources' start times are known.
void make_multicam(App& app, const std::vector<std::string>& sources,
                   const std::vector<double>& starts,
                   const cutline::core::MulticamOptions& options) {
  std::string made;
  app.session.name_next_edit("Create Multi-Camera Source Sequence");
  app.session.apply(cutline::core::create_multicam_sequence(app.session.project(), sources,
                                                            starts, options, &made));
  refresh_all(app);
  app.dock_stale = true;
  if (!made.empty() && app.browser != nullptr) app.browser->select_id(made);
}

/// One edit for the whole pass of switches made while playing: the first is
/// applied, and each after it amends it while the document is still the one
/// it left.
void apply_switch(App& app, cutline::core::Project next) {
  const bool continuing = app.multicam_pass.has_value() &&
                          *app.multicam_pass == app.session.document_revision();
  bool changed = false;
  if (continuing) {
    changed = app.session.amend(std::move(next));
  } else {
    app.session.name_next_edit("Multi-Camera Edit");
    changed = app.session.apply(std::move(next));
  }
  if (!changed) return;
  if (app.playing()) app.multicam_pass = app.session.document_revision();
  refresh_timeline(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  show_multicam(app);
  mark_dirty(app);
}

}  // namespace

// ------------------------------------------------------------- making one --

void open_multicam_dialog(App& app) {
  if (app.main.host == nullptr || app.browser == nullptr) return;
  std::vector<std::string> sources;
  for (const std::size_t row : app.browser->selected_rows()) {
    const cutline::ui::MediaItem& item = app.browser->items()[row];
    if (!cutline::editor::bin_of_row(item.id).empty()) continue;
    const auto media = std::ranges::find(app.session.project().media, item.id,
                                         &cutline::core::Media::id);
    if (media == app.session.project().media.end()) continue;
    if (cutline::core::is_nested_sequence(*media) || cutline::core::is_still_like(*media)) continue;
    sources.push_back(item.id);
  }
  const bool any_camera = std::ranges::any_of(sources, [&app](const std::string& id) {
    const auto media = std::ranges::find(app.session.project().media, id,
                                         &cutline::core::Media::id);
    return media != app.session.project().media.end() && media->has_video;
  });
  if (!any_camera) {
    complain(app.main.window,
             "Select the clips from each camera in the Project panel first, with Ctrl or Shift.");
    return;
  }

  auto dialog = std::make_unique<cutline::ui::Dialog>("Create Multi-Camera Source Sequence");
  Box& body = dialog->body();
  body.emplace<Label>(std::format("{} clips selected", sources.size())).set_small(true);

  auto& name_row = body.emplace<Box>(Axis::Horizontal);
  name_row.emplace<Label>("Name").set_small(true);
  name_row.emplace<Spacer>();
  const auto first = std::ranges::find(app.session.project().media, sources.front(),
                                       &cutline::core::Media::id);
  auto& name = name_row.emplace<TextField>(first->name + " Multicam");
  name.set_columns(24);

  body.emplace<Label>("Synchronize Point").set_small(true);
  auto& sync = body.emplace<RadioGroup>(
      std::vector<std::string>{"In Points", "Out Points", "Timecode", "Audio"}, 0u);
  auto& hours = body.emplace<Checkbox>("Ignore Hours", false);

  auto& audio_row = body.emplace<Box>(Axis::Horizontal);
  audio_row.emplace<Label>("Audio").set_small(true);
  audio_row.emplace<Spacer>();
  auto& audio = audio_row.emplace<Dropdown>(
      std::vector<std::string>{"Camera 1", "All Cameras", "Switch Audio"}, 0u);

  auto& names_row = body.emplace<Box>(Axis::Horizontal);
  names_row.emplace<Label>("Camera Names").set_small(true);
  names_row.emplace<Spacer>();
  auto& naming = names_row.emplace<Dropdown>(
      std::vector<std::string>{"Enumerate Cameras", "Use Clip Name"}, 0u);

  auto& processed = body.emplace<Checkbox>("Move source clips to Processed Clips bin", false);

  dialog->set_accept("OK", [&app, sources, name = &name, sync = &sync, hours = &hours,
                            audio = &audio, naming = &naming,
                            processed = &processed]() -> std::optional<std::string> {
    const cutline::core::MulticamOptions options{
        .name = name->text(),
        .sync = static_cast<cutline::core::MulticamSync>(std::min<std::size_t>(sync->selected(), 3)),
        .ignore_hours = hours->checked(),
        .audio = static_cast<cutline::core::MulticamAudio>(std::min<std::size_t>(audio->selected(), 2)),
        .naming = naming->selected() == 1 ? cutline::core::CameraNaming::ClipNames
                                          : cutline::core::CameraNaming::Enumerate,
        .move_to_processed = processed->checked()};

    if (options.sync != cutline::core::MulticamSync::Audio) {
      const auto starts = cutline::core::multicam_starts(app.session.project(), sources,
                                                         options.sync, options.ignore_hours);
      // Said, and the dialog stays, so another way of lining up can be chosen.
      if (!starts.has_value()) return starts.error();
      make_multicam(app, sources, *starts, options);
      return std::nullopt;
    }

    // By sound, on a thread of its own: every source is decoded to find it.
    if (app.multicam_sync_running.load()) return "Synchronizing is already under way.";
    if (app.multicam_sync_worker.joinable()) app.multicam_sync_worker.join();
    app.multicam_sync_running = true;
    app.multicam_sync_cancel = false;
    app.multicam_sync_done = 0;
    app.multicam_sync_total = static_cast<int>(sources.size());
    app.multicam_sync_sources = sources;
    app.multicam_sync_options = options;
    app.multicam_sync_result.reset();
    refresh_busy(app);
    const cutline::core::Project project = app.session.project();
    HWND window = app.main.window;
    app.multicam_sync_worker = std::thread([&app, project, sources, window] {
      auto starts = cutline::engine::multicam_audio_starts(
          project, sources, [&app, window](int done, int total) {
            app.multicam_sync_done = done;
            app.multicam_sync_total = total;
            PostMessageW(window, kMulticamSynced, 1, 0);
            return !app.multicam_sync_cancel.load();
          });
      {
        const std::scoped_lock lock(app.multicam_sync_lock);
        app.multicam_sync_result = std::move(starts);
      }
      app.multicam_sync_running = false;
      PostMessageW(window, kMulticamSynced, 0, 0);
    });
    // The Synchronizing window goes up once this one has closed, which it
    // does on the way out of here.
    PostMessageW(app.main.window, kMulticamSynced, 2, 0);
    return std::nullopt;
  });
  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

void show_multicam_sync(App& app) {
  if (!app.multicam_sync_running.load() || app.main.host == nullptr) return;
  auto progress = std::make_unique<cutline::ui::Dialog>("Synchronizing");
  auto& bar = progress->body().emplace<cutline::ui::ProgressBar>(0.0);
  progress->set_accept("", {});
  progress->set_cancel("Cancel", [&app] { app.multicam_sync_cancel = true; });
  app.multicam_sync_dialog = progress.get();
  app.multicam_sync_bar = &bar;
  app.main.host->open_popup(std::move(progress), settings_anchor());
  collect_multicam_sync(app, true);
}

void collect_multicam_sync(App& app, bool progress_only) {
  if (cutline::ui::ProgressBar* bar = sync_bar_of(app); bar != nullptr) {
    const int done = app.multicam_sync_done.load();
    const int total = std::max(app.multicam_sync_total.load(), 1);
    bar->set_fraction(static_cast<double>(done) / total);
    bar->set_text(std::format("Reading the sound of clip {} of {}", std::min(done + 1, total),
                              total));
    mark_dirty(app);
  }
  if (progress_only || app.multicam_sync_running.load()) return;
  if (app.multicam_sync_worker.joinable()) app.multicam_sync_worker.join();
  if (sync_bar_of(app) != nullptr) app.main.host->close_popup();
  app.multicam_sync_dialog = nullptr;
  app.multicam_sync_bar = nullptr;
  refresh_busy(app);

  std::optional<std::expected<std::vector<double>, std::string>> result;
  {
    const std::scoped_lock lock(app.multicam_sync_lock);
    result = std::move(app.multicam_sync_result);
    app.multicam_sync_result.reset();
  }
  if (!result.has_value() || app.multicam_sync_cancel.load()) return;
  if (!result->has_value()) {
    complain(app.main.window, "Could not synchronize the clips by their sound.\n\n" +
                                  result->error());
    return;
  }
  make_multicam(app, app.multicam_sync_sources, **result, app.multicam_sync_options);
}

// ---------------------------------------------------- the monitor's view --

void toggle_multicam_view(App& app) {
  app.multicam_view = !app.multicam_view;
  // The left half is one thing at a time: the grid, a comparison or trim.
  if (app.multicam_view && app.lumetri_comparing) toggle_comparison(app);
  show_multicam(app);
  refresh_preview(app);
}

void show_multicam(App& app) {
  if (app.monitor == nullptr) return;
  const std::optional<Following> now = app.multicam_view ? following(app) : std::nullopt;
  if (!now.has_value()) {
    app.monitor->set_angles(std::nullopt);
    app.monitor->set_on_angle({});
    if (!app.lumetri_comparing && !app.trim_point.has_value()) app.monitor->set_reference({});
  } else {
    const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), now->clip_id);
    const cutline::core::Sequence& source = *cutline::core::multicam_source(app.session.project(), *clip);
    const std::vector<std::string> names = cutline::core::camera_names(source);
    const int count = static_cast<int>(names.size());
    app.monitor->set_angles(cutline::ui::MonitorView::Angles{
        .count = count,
        .columns = cutline::core::camera_grid_columns(count),
        .active = clip->camera,
        .recording = app.playing(),
        .names = names});
    app.monitor->set_on_angle([&app](int camera) { select_camera(app, camera); });
  }
  for (Shell* shell : app.shells()) {
    if (shell->host != nullptr) shell->host->request_layout();
  }
  mark_dirty(app);
}

void refresh_angles([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (!app.multicam_view || app.trim_point.has_value() || !app.shares_device()) return;
  const std::optional<Following> now = following(app);
  show_multicam(app);
  if (!now.has_value()) return;
  cutline::app::PreviewEngine* engine =
      engine_for(app, app.angles_preview, app.angles_failed, "The Multi-Camera view");
  if (engine == nullptr) return;
  if (app.angles_snapshot == nullptr || app.angles_revision != app.session.document_revision() ||
      app.angles_media != now->media_id) {
    app.angles_snapshot = std::make_shared<const cutline::core::Project>(
        cutline::core::camera_grid(app.session.project(), now->media_id, kGridWidth));
    app.angles_revision = app.session.document_revision();
    app.angles_media = now->media_id;
  }
  const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), now->clip_id);
  // The source sequence's moment under the playhead, which every cut of the
  // same multicam agrees on.
  app.angles_time = cutline::core::source_time_at(*clip, app.session.playhead());
  engine->want(app.angles_snapshot, app.angles_time, app.playing() ? 1.0 : 0.0);
  poll_angles(app);
#endif
}

void poll_angles([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.angles_preview == nullptr || app.monitor == nullptr || !app.multicam_view ||
      app.trim_point.has_value() || app.monitor->angles() == std::nullopt) {
    return;
  }
  auto taken = app.angles_preview->take(app.angles_time);
  if (!taken.has_value()) {
    app.angles_failed = true;
    app.angles_preview.reset();
    complain(app.main.window, "Could not draw the cameras.\n\n" + taken.error());
    return;
  }
  if (taken->has_value()) {
    cutline::ui::TextureView grid = **taken;
    // In SDR beside the picture: one picture at a time is drawn for an HDR
    // display, and that is the programme.
    grid.hdr_texture = nullptr;
    app.monitor->set_reference_texture(grid);
    mark_dirty(app);
  }
#endif
}

// -------------------------------------------------------------- switching --

void select_camera(App& app, int camera) {
  const std::optional<Following> now = following(app);
  if (!now.has_value()) return;
  const bool follows = app.settings.multicam_audio_follows;
  if (app.playing()) {
    const std::vector<cutline::core::CameraSwitch> one{
        {.time = app.session.playhead(), .camera = camera}};
    apply_switch(app, cutline::core::record_camera_switches(app.session.project(), now->track_id,
                                                            one, follows));
    return;
  }
  apply_switch(app, cutline::core::switch_camera(app.session.project(), now->clip_id, camera,
                                                 follows));
}

void cut_to_camera(App& app, int camera) {
  const std::optional<Following> now = following(app);
  if (!now.has_value()) return;
  const std::vector<cutline::core::CameraSwitch> one{
      {.time = app.session.playhead(), .camera = camera}};
  apply_switch(app, cutline::core::record_camera_switches(app.session.project(), now->track_id,
                                                          one,
                                                          app.settings.multicam_audio_follows));
}

void end_multicam_pass(App& app) {
  app.multicam_pass.reset();
  if (app.multicam_view) show_multicam(app);
}

std::vector<MenuEntry> multicam_entries(App& app, const std::vector<std::string>& clips) {
  const cutline::core::Project& project = app.session.project();
  std::vector<std::string> multicams;
  bool enabled = false;
  const cutline::core::Sequence* source = nullptr;
  int camera = -1;
  for (const std::string& id : clips) {
    const cutline::core::Clip* clip = cutline::core::find_clip(project, id);
    if (clip == nullptr || cutline::core::multicam_source(project, *clip) == nullptr) continue;
    multicams.push_back(id);
    enabled = enabled || !clip->multicam_off;
    source = cutline::core::multicam_source(project, *clip);
    camera = camera == -1 || camera == clip->camera ? clip->camera : -2;
  }
  std::vector<MenuEntry> rows;
  rows.push_back(switch_entry("Enable", enabled, [&app, multicams, enabled] {
    app.session.apply(cutline::core::set_multicam_enabled(app.session.project(), multicams,
                                                          !enabled));
    refresh_timeline(app);
    invalidate_preview(app);
    show_multicam(app);
  }));
  if (source != nullptr) {
    rows.push_back(MenuEntry::divider());
    const std::vector<std::string> names = cutline::core::camera_names(*source);
    for (int k = 0; k < static_cast<int>(names.size()); ++k) {
      MenuEntry entry = switch_entry(names[static_cast<std::size_t>(k)], camera == k,
                                     [&app, multicams, k] {
        cutline::core::Project next = app.session.project();
        for (const std::string& id : multicams) {
          next = cutline::core::switch_camera(std::move(next), id, k,
                                              app.settings.multicam_audio_follows);
        }
        apply_switch(app, std::move(next));
      });
      entry.enabled = enabled;
      rows.push_back(std::move(entry));
    }
  }
  rows.push_back(MenuEntry::divider());
  MenuEntry flatten = action_entry("Flatten", [&app, multicams] {
    // Each clip's linked sound with it, as Premiere flattens the pair.
    std::vector<std::string> pairs;
    for (const std::string& id : multicams) {
      for (const std::string& member : cutline::core::group_members(app.session.project(), id)) {
        if (std::ranges::find(pairs, member) == pairs.end()) pairs.push_back(member);
      }
    }
    app.session.name_next_edit("Flatten Multi-Camera");
    app.session.apply(cutline::core::flatten_multicam(app.session.project(), pairs));
    refresh_timeline(app);
    invalidate_preview(app);
    app.inspector_stale = true;
    show_multicam(app);
  });
  flatten.enabled = enabled;
  rows.push_back(std::move(flatten));
  return rows;
}

}  // namespace cutline::tool
