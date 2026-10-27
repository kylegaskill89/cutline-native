/// The render queue: exports waiting their turn, the one rendering, and the
/// panel that shows them.
///
/// Premiere sends a queued export to Media Encoder, which renders it while the
/// editor goes on being used. This is that, without the second application:
/// the jobs run one after another on a worker thread, each with the project as
/// it was when it was sent, and the panel shows each one's progress.

#include "app.hpp"

namespace cutline::tool {

std::string_view to_string(RenderState state) noexcept {
  switch (state) {
    case RenderState::Rendering: return "Rendering";
    case RenderState::Done: return "Done";
    case RenderState::Failed: return "Failed";
    case RenderState::Cancelled: return "Cancelled";
    case RenderState::Waiting: break;
  }
  return "Waiting";
}

App::RenderJob* find_job(App& app, std::uint64_t id) {
  for (const auto& job : app.render_queue) {
    if (job->id == id) return job.get();
  }
  return nullptr;
}

namespace {

[[nodiscard]] bool finished(RenderState state) noexcept {
  return state == RenderState::Done || state == RenderState::Failed ||
         state == RenderState::Cancelled;
}

[[nodiscard]] std::size_t unfinished_jobs(const App& app) {
  return static_cast<std::size_t>(std::ranges::count_if(app.render_queue, [](const auto& job) {
    return !finished(job->state.load());
  }));
}

#if CUTLINE_HAVE_PREVIEW

[[nodiscard]] cutline::media::VideoCodec codec_of(cutline::editor::ExportFormat format) {
  switch (format) {
    case cutline::editor::ExportFormat::Hevc:
    case cutline::editor::ExportFormat::Hevc10: return cutline::media::VideoCodec::Hevc;
    case cutline::editor::ExportFormat::ProRes: return cutline::media::VideoCodec::ProRes;
    case cutline::editor::ExportFormat::DnxHr: return cutline::media::VideoCodec::DnxHr;
    case cutline::editor::ExportFormat::H264: break;
  }
  return cutline::media::VideoCodec::H264;
}

[[nodiscard]] cutline::engine::ExportSettings settings_of(const App::RenderJob& job) {
  const cutline::editor::ExportPlan& plan = job.plan;
  cutline::engine::ExportSettings settings;
  settings.path = job.path.string();
  settings.codec = codec_of(plan.format);
  settings.quality = plan.quality;
  settings.colour = plan.colour;
  settings.bit_depth = plan.bit_depth;
  settings.fps = plan.fps;
  settings.width = plan.width;
  settings.height = plan.height;
  settings.audio = plan.audio;
  settings.audio_channels = plan.channels;
  settings.separate_audio = plan.separate_audio;
  if (job.span.has_value()) {
    settings.start = job.span->start;
    settings.duration = job.span->duration;
  }
  return settings;
}

/// Sets a job rendering on the worker.
///
/// Its own `gpu::Device` as well, deliberately. The compositor keeps one
/// command list and one allocator, so two threads compositing on the same
/// device would be a race on both. The window keeps drawing on the shared
/// device throughout; the export gets its own and they never meet.
void begin_job(App& app, App::RenderJob& job) {
  job.cancel = false;
  job.fraction = 0.0;
  job.outcome.clear();
  job.state = RenderState::Rendering;
  app.rendering = &job;

  // Captured by value: the worker outlives this call, and reaching into the
  // application's settings from another thread to ask again would be reading a
  // field the interface may be writing.
  const bool software = app.settings.software_renderer;
  const cutline::engine::ExportSettings settings = settings_of(job);

  app.render_worker = std::thread([&job, settings, software] {
    // The export has to make the same choice the preview did. Somebody who
    // picked the software renderer because their driver produces wrong pixels
    // would otherwise get a preview they trust and a file they cannot.
    auto device = cutline::gpu::Device::create({.force_software = software});
    if (!device.has_value()) {
      job.outcome = "No usable graphics device: " + device.error();
      job.state = RenderState::Failed;
      return;
    }

    const auto result = cutline::engine::export_project(
        *device, job.project, settings, [&job](const cutline::engine::ExportProgress& progress) {
          if (progress.total > 0) {
            job.fraction = static_cast<double>(progress.frame) /
                           static_cast<double>(progress.total);
          }
          // Returning false is how the exporter is told to stop, which leaves a
          // truncated file rather than a complete one.
          return !job.cancel.load();
        });

    // The outcome before the state: the interface reads them the other way.
    if (job.cancel.load()) {
      job.outcome = "Cancelled.";
      job.state = RenderState::Cancelled;
    } else if (!result.has_value()) {
      job.outcome = result.error();
      job.state = RenderState::Failed;
    } else {
      job.outcome = std::format("Done — {} frames in {:.1f}s ({:.1f} fps) via {}",
                                result->frames, result->elapsed_seconds,
                                result->frames_per_second(), result->encoder);
      // Smart rendering said out loud, because a render that took a second
      // where the last took a minute looks like one that went wrong.
      if (result->copied_frames > 0) {
        job.outcome += std::format(", {} copied unchanged", result->copied_frames);
      }
      job.state = RenderState::Done;
    }
  });
}

/// Starts the next waiting job, if the queue is going and nothing is
/// rendering. A queue with nothing left to do stops, so a job queued later
/// waits to be started rather than setting off on its own.
void advance_queue(App& app) {
  if (app.rendering != nullptr || !app.queue_running) return;
  for (const auto& job : app.render_queue) {
    if (job->state.load() == RenderState::Waiting) {
      begin_job(app, *job);
      return;
    }
  }
  app.queue_running = false;
}

#else
void advance_queue(App& app) { app.queue_running = false; }
#endif

/// The line a job's row says how it is going on.
[[nodiscard]] std::string status_of(const App::RenderJob& job) {
  const RenderState state = job.state.load();
  switch (state) {
    case RenderState::Rendering:
      return std::format("Rendering, {:.0f}%", job.fraction.load() * 100.0);
    case RenderState::Waiting: return "Waiting";
    case RenderState::Done:
    case RenderState::Failed:
    case RenderState::Cancelled: return job.outcome;
  }
  return {};
}

/// Opens Explorer with the file selected, which is where somebody wants to
/// be the moment a render finishes: about to send it somewhere.
void show_in_explorer(const std::filesystem::path& path) {
  const std::wstring arguments = L"/select,\"" + path.wstring() + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

/// Fills the panel's list afresh. Only when a job comes, goes or changes state;
/// the progress in between is set on the rows that are already there.
void rebuild_queue_rows(App& app) {
  app.queue_rows.clear();
  app.queue_list->clear_children();
  if (app.render_queue.empty()) {
    app.queue_list->emplace<Label>("Nothing queued. Queue in the Export window adds a render here.")
        .set_small(true);
    return;
  }
  for (const auto& job : app.render_queue) {
    const RenderState state = job->state.load();
    auto& row = app.queue_list->emplace<Box>(Axis::Vertical);
    row.set_spacing(2.0);

    auto& top = row.emplace<Box>(Axis::Horizontal);
    top.emplace<Label>(job->path.filename().string());
    top.emplace<Spacer>();
    auto& preset = top.emplace<Label>(job->preset);
    preset.set_small(true);
    if (state == RenderState::Done) {
      auto& show = top.emplace<Button>("Show", [path = job->path] { show_in_explorer(path); });
      show.set_tooltip("Show the file in Explorer");
    }
    // One button for both, as Media Encoder has: what taking a job away means
    // depends on whether it has started.
    auto& remove = top.emplace<IconButton>(IconButton::Icon::Cross,
                                           [&app, id = job->id] { remove_job(app, id); });
    remove.set_tooltip(state == RenderState::Rendering ? "Stop this render"
                                                       : "Take this out of the queue");

    auto& progress = row.emplace<ProgressBar>(finished(state) && state == RenderState::Done
                                                  ? 1.0
                                                  : job->fraction.load());
    auto& status = row.emplace<Label>(status_of(*job));
    status.set_small(true);
    app.queue_rows.push_back({.id = job->id, .state = state, .status = &status,
                              .progress = &progress});
  }
}

}  // namespace

std::expected<std::uint64_t, std::string> queue_export(App& app, bool start) {
  App::ExportSetup& setup = app.export_setup;
  if (setup.path.empty()) return std::unexpected("Choose where the file goes first.");

  const cutline::core::Project& project = app.session.project();
  auto plan = cutline::editor::plan_for(setup.preset, project);
  if (!plan.has_value()) return std::unexpected(plan.error());

  // The container follows the format, as it does in Premiere: a ProRes file
  // is a .mov whatever the name typed was.
  if (setup.path.extension() != plan->extension) setup.path.replace_extension(plan->extension);

  // Two renders of one file would be two writers to one file, and whichever
  // finished last would be what was left.
  for (const auto& job : app.render_queue) {
    if (!finished(job->state.load()) && job->path == setup.path) {
      return std::unexpected(setup.path.filename().string() + " is already in the queue.");
    }
  }

  auto job = std::make_unique<App::RenderJob>();
  job->id = app.next_job_id++;
  job->path = setup.path;
  job->preset = cutline::editor::matching_preset_name(app.export_presets, setup.preset);
  job->project = project;
  job->plan = *plan;
  if (setup.marked_only && cutline::core::has_marks(project)) {
    job->span = cutline::core::marked_span(project);
  }
  const std::uint64_t id = job->id;
  app.render_queue.push_back(std::move(job));
  if (start) start_queue(app);
  refresh_queue(app);
  return id;
}

void start_queue(App& app) {
  app.queue_running = true;
  advance_queue(app);
  refresh_queue(app);
  refresh_export_dialog(app);
}

void pause_queue(App& app) {
  app.queue_running = false;
  refresh_queue(app);
}

void remove_job(App& app, std::uint64_t id) {
  App::RenderJob* job = find_job(app, id);
  if (job == nullptr) return;
  if (job == app.rendering) {
    // It stops at the next frame it finishes, and leaves the queue then.
    job->cancel = true;
    return;
  }
  std::erase_if(app.render_queue, [id](const auto& entry) { return entry->id == id; });
  refresh_queue(app);
  refresh_export_dialog(app);
}

void clear_finished_jobs(App& app) {
  std::erase_if(app.render_queue,
                [](const auto& job) { return finished(job->state.load()); });
  refresh_queue(app);
}

void refresh_queue(App& app) {
  if (app.queue_start != nullptr) {
    const bool waiting = std::ranges::any_of(app.render_queue, [](const auto& job) {
      return job->state.load() == RenderState::Waiting;
    });
    app.queue_start->set_text(app.queue_running ? "Pause Queue" : "Start Queue");
    app.queue_start->set_enabled(app.queue_running || waiting);
  }
  if (app.queue_clear != nullptr) {
    app.queue_clear->set_enabled(std::ranges::any_of(
        app.render_queue, [](const auto& job) { return finished(job->state.load()); }));
  }
  if (app.queue_list == nullptr) return;

  // Rebuilt only when the jobs or their states differ from what the rows show.
  bool same = app.queue_rows.size() == app.render_queue.size() && !app.render_queue.empty();
  for (std::size_t i = 0; same && i < app.render_queue.size(); ++i) {
    same = app.queue_rows[i].id == app.render_queue[i]->id &&
           app.queue_rows[i].state == app.render_queue[i]->state.load();
  }
  if (!same) {
    rebuild_queue_rows(app);
    // Whichever window the panel is in, which may be a torn-out one.
    for (Shell* shell : app.shells()) {
      if (shell->host != nullptr) shell->host->request_layout();
    }
  } else {
    for (std::size_t i = 0; i < app.queue_rows.size(); ++i) {
      const App::RenderJob& job = *app.render_queue[i];
      if (job.state.load() != RenderState::Rendering) continue;
      app.queue_rows[i].progress->set_fraction(job.fraction.load());
      app.queue_rows[i].status->set_text(status_of(job));
    }
  }
  mark_dirty(app);
}

void poll_export(App& app) {
  App::RenderJob* job = app.rendering;
  if (job == nullptr) return;
  const RenderState state = job->state.load();
  if (state == RenderState::Rendering) {
    refresh_queue(app);
    refresh_export_dialog(app);
    return;
  }

  if (app.render_worker.joinable()) app.render_worker.join();
  app.rendering = nullptr;
  advance_queue(app);
  // A long job ending is worth hearing about from across the room: a failure
  // at once, and success when the whole queue is through. One that was
  // cancelled was ended by the person, who knows.
  if (state == RenderState::Failed) {
    play_sound(app, cutline::ui::Sound::Failed);
  } else if (state == RenderState::Done && app.rendering == nullptr) {
    play_sound(app, cutline::ui::Sound::Done);
  }
  refresh_queue(app);
  refresh_export_dialog(app);
}

void settle_export(App& app) {
  for (const auto& job : app.render_queue) job->cancel = true;
  app.queue_running = false;
  if (app.render_worker.joinable()) app.render_worker.join();
  app.rendering = nullptr;
  // And the preview render, which stops at the end of the piece it is on.
  app.preview_render_cancel = true;
  if (app.preview_render_worker.joinable()) app.preview_render_worker.join();
  // And mask tracking, which stops at the end of the frame it is on.
  app.mask_track_cancel = true;
  if (app.mask_track_worker.joinable()) app.mask_track_worker.join();
}

bool confirm_abandon_queue(App& app) {
  const std::size_t left = unfinished_jobs(app);
  if (left == 0) return true;
  const std::string message =
      left == 1 ? "An export is still in the render queue. Quit and abandon it?"
                : std::format("{} exports are still in the render queue. Quit and abandon them?",
                              left);
  return MessageBoxA(app.main.window, message.c_str(), "Cutline",
                     MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

[[nodiscard]] std::unique_ptr<Widget> make_queue_panel(App* app) {
  auto panel = std::make_unique<Panel>();

  auto& bar = panel->emplace<Box>(Axis::Horizontal);
  auto& start = bar.emplace<Button>("Start Queue", [app] {
    if (app == nullptr) return;
    if (app->queue_running) {
      pause_queue(*app);
    } else {
      start_queue(*app);
    }
  });
  start.set_tooltip("Render the waiting jobs one after another, or stop after this one");
  auto& clear = bar.emplace<Button>("Clear Finished", [app] {
    if (app != nullptr) clear_finished_jobs(*app);
  });
  bar.emplace<Spacer>();

  auto& scroll = panel->emplace<ScrollView>(Axis::Vertical);
  auto& list = static_cast<Box&>(scroll.set_content(std::make_unique<Box>(Axis::Vertical)));
  list.set_spacing(8.0);

  if (app != nullptr) {
    app->queue_start = &start;
    app->queue_clear = &clear;
    app->queue_list = &list;
    app->queue_rows.clear();
    refresh_queue(*app);
  } else {
    list.emplace<Label>("Nothing queued.").set_small(true);
  }
  return panel;
}

}  // namespace cutline::tool
