/// The Export window: what to render to, and how it is going.
///
/// A dialog window of its own, because an export runs for minutes and the
/// window stays to show its progress; the render itself runs on a worker
/// thread the window polls.

#include "app.hpp"

namespace cutline::tool {

// ------------------------------------------------------------------ export --

#if CUTLINE_HAVE_PREVIEW

/// The formats offered, and what each means to the encoder.
constexpr std::array kExportCodecs{"H.264", "HEVC (H.265)"};

/// Named qualities rather than a bare number. The encoder wants a CRF, which
/// is a scale most people have no reason to know and which runs the wrong way
/// round — lower is better. Naming them is what the reference did too.
struct QualityChoice {
  const char* name;
  int quality;
};
constexpr std::array kExportQualities{
    QualityChoice{"Best (large file)", 16},
    QualityChoice{"High", 18},
    QualityChoice{"Medium", 21},
    QualityChoice{"Low (small file)", 25},
};

/// Zero means the project's own rate, which is the honest default: re-timing a
/// sequence on the way out is a decision, not a preference.
struct RateChoice {
  const char* name;
  double fps;
};
constexpr std::array kExportRates{
    RateChoice{"Same as sequence", 0.0}, RateChoice{"24 fps", 24.0},
    RateChoice{"25 fps", 25.0},          RateChoice{"30 fps", 30.0},
    RateChoice{"50 fps", 50.0},          RateChoice{"60 fps", 60.0},
};

/// Output sizes, as a fraction of the sequence's own.
///
/// A fraction rather than a fixed pair of numbers, because the aspect ratio is
/// the sequence's business: offering "1280 x 720" to a vertical sequence would
/// have to either letterbox it or stretch it, and neither is what choosing a
/// smaller size means. What each fraction works out to in pixels is put in the
/// label, so nothing has to be taken on trust.
struct ResolutionChoice {
  const char* name;
  double scale;
};
constexpr std::array kExportResolutions{
    ResolutionChoice{"Same as sequence", 1.0}, ResolutionChoice{"3/4", 0.75},
    ResolutionChoice{"2/3", 2.0 / 3.0},        ResolutionChoice{"1/2", 0.5},
    ResolutionChoice{"1/3", 1.0 / 3.0},        ResolutionChoice{"1/4", 0.25},
};

/// How the mix is folded down. Mono sums the sources rather than dropping a
/// side; the resampler does it as each one is decoded.
struct MixdownChoice {
  const char* name;
  int channels;
  /// One stream per timeline track rather than one summed stream.
  bool separate = false;
};
constexpr std::array kExportMixdowns{
    MixdownChoice{"Stereo", 2},
    MixdownChoice{"Mono", 1},
    // Last, because it is the one that produces a file most players will only
    // play the first stream of. It is for handing the cut to somebody who is
    // going to mix it, and a mixdown is what everything else wants.
    MixdownChoice{"Separate tracks", 2, true},
};

/// The output size a resolution choice works out to for this project.
///
/// Even in both directions: H.264 and HEVC encode in even-sized blocks, and an
/// odd size would be rounded down by the encoder anyway — better to round here,
/// where it can be shown, than to say one size and write another.
[[nodiscard]] std::pair<int, int> export_size(const App& app, std::size_t choice) {
  const cutline::core::Project& project = app.session.project();
  const double scale = choice < kExportResolutions.size() ? kExportResolutions[choice].scale : 1.0;
  const auto even = [](double value) {
    return std::max(2, static_cast<int>(std::llround(value)) & ~1);
  };
  return {even(project.sequence().canvas_w * scale), even(project.sequence().canvas_h * scale)};
}

/// Where the export writes to by default: beside the project, named after it.
[[nodiscard]] std::filesystem::path default_export_path(const App& app) {
  const std::filesystem::path& project = app.session.path();
  if (project.empty()) return {};
  return std::filesystem::path(project).replace_extension(".mp4");
}

/// What the button under "Output Name" says. The full path is too long for the
/// dialog and the interesting part is the end of it.
[[nodiscard]] std::string output_label(const App& app) {
  if (app.export_setup.path.empty()) return "Choose a file...";
  return app.export_setup.path.filename().string();
}

void refresh_export_dialog(App& app) {
  if (app.export_window == nullptr) return;

  const bool busy = app.exporting();
  if (app.export_output != nullptr) app.export_output->set_text(output_label(app));
  if (app.export_mixdown != nullptr) app.export_mixdown->set_enabled(app.export_setup.audio);
  if (app.export_start != nullptr) {
    app.export_start->set_text(busy ? "Cancel" : "Export");
    // Nothing to export to is not an error worth a dialog; it is a button that
    // should not be pressable yet.
    app.export_start->set_enabled(busy || !app.export_setup.path.empty());
  }
  if (app.export_progress != nullptr) {
    app.export_progress->set_fraction(app.export_job.fraction.load());
  }
  if (app.export_status != nullptr && !busy && !app.export_job.outcome.empty()) {
    app.export_status->set_text(app.export_job.outcome);
  }
  mark_dirty(app);
}

/// Runs the export on its own thread.
///
/// Its own `gpu::Device` as well, deliberately. The compositor keeps one
/// command list and one allocator, so two threads compositing on the same
/// device would be a race on both. The window keeps drawing on the shared
/// device throughout; the export gets its own and they never meet.
void start_export(App& app) {
  if (app.exporting() || app.export_setup.path.empty()) return;

  const cutline::core::Project project = app.session.project();
  cutline::engine::ExportSettings settings;
  settings.path = app.export_setup.path.string();
  settings.codec = app.export_setup.codec == 1 ? cutline::media::VideoCodec::Hevc
                                               : cutline::media::VideoCodec::H264;
  if (app.export_setup.colour > 0) {
    settings.colour =
        static_cast<cutline::core::WorkingColour>(std::min<std::size_t>(app.export_setup.colour - 1, 2));
  }
  // HDR is HEVC only, as in Premiere. Chosen here rather than refused, and said
  // so below, because an HDR sequence matched by default would otherwise fail
  // for anybody who left the format at H.264.
  const bool hdr = settings.colour.value_or(project.sequence().colour) !=
                   cutline::core::WorkingColour::Rec709;
  const bool switched = hdr && settings.codec != cutline::media::VideoCodec::Hevc;
  if (switched) settings.codec = cutline::media::VideoCodec::Hevc;
  settings.quality = kExportQualities[app.export_setup.quality].quality;
  settings.fps = kExportRates[app.export_setup.rate].fps;
  const auto [width, height] = export_size(app, app.export_setup.resolution);
  settings.width = width;
  settings.height = height;
  settings.audio = app.export_setup.audio;
  settings.audio_channels = kExportMixdowns[app.export_setup.mixdown].channels;
  settings.separate_audio = kExportMixdowns[app.export_setup.mixdown].separate;
  if (app.export_setup.marked_only && cutline::core::has_marks(project)) {
    const cutline::core::MarkedSpan span = cutline::core::marked_span(project);
    settings.start = span.start;
    settings.duration = span.duration;
  }

  App::ExportJob& job = app.export_job;
  job.cancel = false;
  job.fraction = 0.0;
  job.finished = false;
  job.failed = false;
  job.outcome.clear();
  job.running = true;

  if (app.export_status != nullptr) {
    app.export_status->set_text(switched ? "Exporting as HEVC, for HDR..." : "Exporting...");
  }

  // Captured by value: the worker outlives this call, and reaching into the
  // application's settings from another thread to ask again would be reading a
  // field the interface may be writing.
  const bool software = app.settings.software_renderer;

  job.worker = std::thread([&app, &job, project, settings, software] {
    // The export renders on a device of its own, and it has to make the same
    // choice the preview did. Somebody who picked the software renderer because
    // their driver produces wrong pixels would otherwise get a preview they
    // trust and a file they cannot.
    auto device = cutline::gpu::Device::create({.force_software = software});
    if (!device.has_value()) {
      job.outcome = "No usable graphics device: " + device.error();
      job.failed = true;
      job.finished = true;
      job.running = false;
      return;
    }

    const auto result = cutline::engine::export_project(
        *device, project, settings, [&job](const cutline::engine::ExportProgress& progress) {
          if (progress.total > 0) {
            job.fraction = static_cast<double>(progress.frame) /
                           static_cast<double>(progress.total);
          }
          // Returning false is how the exporter is told to stop, which leaves a
          // truncated file rather than a complete one.
          return !job.cancel.load();
        });

    if (!result.has_value()) {
      job.outcome = result.error();
      job.failed = true;
    } else if (job.cancel.load()) {
      job.outcome = "Cancelled.";
      job.failed = true;
    } else {
      job.outcome = std::format("Done — {} frames in {:.1f}s ({:.1f} fps) via {}",
                                result->frames, result->elapsed_seconds,
                                result->frames_per_second(), result->encoder);
    }
    job.finished = true;
    job.running = false;
  });

  refresh_export_dialog(app);
}

/// Asks the export to stop. It notices at the next frame it finishes.
void cancel_export(App& app) {
  if (!app.exporting()) return;
  app.export_job.cancel = true;
  if (app.export_status != nullptr) app.export_status->set_text("Cancelling...");
  mark_dirty(app);
}

/// Joins a finished worker and shows what it did. Called once a frame while a
/// window is open, which is also what keeps the progress bar moving.
void poll_export(App& app) {
  App::ExportJob& job = app.export_job;
  if (job.running.load()) {
    refresh_export_dialog(app);
    return;
  }
  if (!job.finished.load()) return;

  job.finished = false;
  if (job.worker.joinable()) job.worker.join();
  job.fraction = job.failed ? 0.0 : 1.0;
  // A long job ending is worth hearing about from across the room; one that
  // was cancelled was ended by the person, who knows.
  if (!job.cancel.load()) {
    play_sound(app, job.failed ? cutline::ui::Sound::Failed : cutline::ui::Sound::Done);
  }
  refresh_export_dialog(app);
}

/// Waits for an export to finish, because the process is going away and the
/// thread reads a project this is about to destroy.
void settle_export(App& app) {
  if (app.export_job.running.load()) app.export_job.cancel = true;
  if (app.export_job.worker.joinable()) app.export_job.worker.join();
}

[[nodiscard]] std::unique_ptr<Widget> build_export_interface(App* app, Shell* shell) {
  auto root = std::make_unique<Box>(Axis::Vertical);
  root->set_spacing(0.0);

  auto& caption = root->emplace<TitleBar>("Export Settings");
  shell->title_bar = &caption;
  caption.emplace<CaptionButton>(CaptionButton::Kind::Close, [app] {
    if (app != nullptr) close_export_dialog(*app);
  });

  auto& dialog = root->emplace<cutline::ui::Dialog>("");
  Box& body = dialog.body();

  // One row per setting, label on the left, control on the right — which is
  // how every settings dialog on the platform reads, Premiere's included —
  // and the labels lined up, which the dialog does.
  const auto row = [&dialog](std::string label) -> Box& { return dialog.row(std::move(label)); };

  {
    auto& line = row("Format");
    auto& choice = line.emplace<Dropdown>(
        std::vector<std::string>{kExportCodecs.begin(), kExportCodecs.end()},
        app != nullptr ? app->export_setup.codec : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.codec = index;
    });
  }
  {
    auto& line = row("Colour");
    auto& choice = line.emplace<Dropdown>(
        std::vector<std::string>{"Match sequence", "Rec. 709", "Rec. 2100 PQ", "Rec. 2100 HLG"},
        app != nullptr ? app->export_setup.colour : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.colour = index;
    });
  }
  {
    auto& line = row("Quality");
    std::vector<std::string> names;
    for (const QualityChoice& quality : kExportQualities) names.emplace_back(quality.name);
    auto& choice =
        line.emplace<Dropdown>(std::move(names), app != nullptr ? app->export_setup.quality : 1);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.quality = index;
    });
  }
  {
    auto& line = row("Resolution");
    std::vector<std::string> names;
    for (std::size_t i = 0; i < kExportResolutions.size(); ++i) {
      if (app == nullptr) {
        names.emplace_back(kExportResolutions[i].name);
        continue;
      }
      const auto [width, height] = export_size(*app, i);
      names.push_back(std::format("{} ({} x {})", kExportResolutions[i].name, width, height));
    }
    auto& choice = line.emplace<Dropdown>(std::move(names),
                                          app != nullptr ? app->export_setup.resolution : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.resolution = index;
    });
  }
  {
    auto& line = row("Frame rate");
    std::vector<std::string> names;
    for (const RateChoice& rate : kExportRates) names.emplace_back(rate.name);
    auto& choice =
        line.emplace<Dropdown>(std::move(names), app != nullptr ? app->export_setup.rate : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.rate = index;
    });
  }
  {
    auto& line = row("Output name");
    auto& output = line.emplace<Button>(app != nullptr ? output_label(*app) : "Choose a file...",
                                        [app] {
                                          if (app == nullptr) return;
                                          const auto chosen = choose_movie_file(
                                              app->export_window != nullptr
                                                  ? app->export_window->window
                                                  : app->main.window);
                                          if (!chosen.has_value()) return;
                                          app->export_setup.path = *chosen;
                                          refresh_export_dialog(*app);
                                        });
    if (app != nullptr) app->export_output = &output;
  }

  {
    auto& line = row("Audio");
    std::vector<std::string> names;
    for (const MixdownChoice& mix : kExportMixdowns) names.emplace_back(mix.name);
    auto& choice =
        line.emplace<Dropdown>(std::move(names), app != nullptr ? app->export_setup.mixdown : 0);
    choice.set_on_change([app](std::size_t index) {
      if (app != nullptr) app->export_setup.mixdown = index;
    });
    if (app != nullptr) app->export_mixdown = &choice;
  }

  auto& sound = body.emplace<Checkbox>("Export audio", app == nullptr || app->export_setup.audio);
  sound.set_on_change([app](bool on) {
    if (app == nullptr) return;
    app->export_setup.audio = on;
    // A mixdown for a file with no audio in it is a choice with nothing behind
    // it, so it greys out rather than sitting there looking answerable.
    refresh_export_dialog(*app);
  });

  // Labelled with the span it means. "In to out" says nothing about how long
  // the file will be, and this is the last screen before a render that may take
  // minutes.
  const bool marked = app != nullptr && cutline::core::has_marks(app->session.project());
  std::string range = "Only the marked range";
  if (marked) {
    const cutline::core::MarkedSpan span = cutline::core::marked_span(app->session.project());
    const double fps = app->session.project().sequence().fps;
    const bool drop = app->session.project().sequence().drop_frame;
    range += std::format(" ({} to {})",
                         cutline::core::seconds_to_timecode(span.start, fps, drop),
                         cutline::core::seconds_to_timecode(span.start + span.duration, fps,
                                                            drop));
  }
  auto& only = body.emplace<Checkbox>(std::move(range),
                                      app != nullptr && app->export_setup.marked_only && marked);
  only.set_enabled(marked);
  only.set_on_change([app](bool on) {
    if (app != nullptr) app->export_setup.marked_only = on;
  });

  body.emplace<Spacer>();

  auto& progress = body.emplace<ProgressBar>();
  if (app != nullptr) app->export_progress = &progress;

  auto& status = body.emplace<Label>("");
  status.set_small(true);
  if (app != nullptr) app->export_status = &status;

  // Export first and the default, then Close: the platform's order, which this
  // had the wrong way round. Exporting keeps the window, to show the progress.
  dialog.set_accept("Export", [app] {
    if (app != nullptr) {
      if (app->exporting()) {
        cancel_export(*app);
      } else {
        start_export(*app);
      }
    }
    return std::optional<std::string>{};
  });
  dialog.set_closes_on_accept(false);
  dialog.set_cancel("Close");
  dialog.set_on_close([app] {
    if (app != nullptr) close_export_dialog(*app);
  });
  if (app != nullptr) app->export_start = &dialog.accept_button();

  return root;
}

void open_export_dialog(App& app) {
  if (app.export_window != nullptr) {
    SetForegroundWindow(app.export_window->window);
    return;
  }
  if (app.export_setup.path.empty()) app.export_setup.path = default_export_path(app);

  auto shell = std::make_unique<Shell>();
  shell->app = &app;
  shell->is_dialog = true;
  shell->host = std::make_unique<WidgetHost>(build_export_interface(&app, shell.get()));
  give_sounds(app, *shell->host);

  // Centred on the main window, which is where a dialog belongs and where the
  // eye already is.
  RECT owner{};
  GetWindowRect(app.main.window, &owner);
  const auto [units_wide, units_tall] =
      dialog_size(app, "Export Settings", kExportWindowWidth, kExportWindowHeight);
  shell->scale = app.main.scale;
  const int kWidth = to_pixels(units_wide, shell->scale);
  const int kHeight = to_pixels(units_tall, shell->scale);
  const int x = owner.left + ((owner.right - owner.left) - kWidth) / 2;
  const int y = owner.top + ((owner.bottom - owner.top) - kHeight) / 2;

  const HWND window =
      CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Export Settings",
                      WS_POPUP | WS_THICKFRAME, x, y, kWidth, kHeight, app.main.window, nullptr,
                      GetModuleHandleW(nullptr), nullptr);
  if (window == nullptr) return;

  shell->window = window;
  shell->size_key = "Export Settings";
  SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(shell.get()));

  const MARGINS shadow{0, 0, 1, 0};
  DwmExtendFrameIntoClientArea(window, &shadow);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

  app.export_window = std::move(shell);
  ShowWindow(window, SW_SHOW);
  SetForegroundWindow(window);
  refresh_export_dialog(app);
}

void close_export_dialog(App& app) {
  if (app.export_window == nullptr) return;
  // An export in flight keeps going. It writes to a file, not to the dialog,
  // and stopping it because a window was closed would throw away minutes of
  // work for no reason anybody asked for.
  app.export_output = nullptr;
  app.export_progress = nullptr;
  app.export_status = nullptr;
  app.export_start = nullptr;
  app.export_mixdown = nullptr;

  const HWND window = app.export_window->window;
  remember_dialog_size(app, *app.export_window);
  app.export_window.reset();
  if (window != nullptr) DestroyWindow(window);
  mark_dirty(app);
}

#else
void open_export_dialog(App&) {}
void poll_export(App&) {}
void settle_export(App&) {}
#endif

}  // namespace cutline::tool
