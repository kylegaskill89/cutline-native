/// The Export window: a preset, what it means for this sequence, and where the
/// file goes.
///
/// Premiere's arrangement: a Preset menu at the top that sets everything under
/// it, the settings themselves, which turn the preset to "Custom" as soon as
/// one is changed, and Export and Queue at the bottom. Both put the render in
/// the queue; Export sets the queue going and stays to show the progress, and
/// Queue leaves it waiting, as sending to Media Encoder does.

#include "app.hpp"

namespace cutline::tool {

// ------------------------------------------------------------------ export --

#if CUTLINE_HAVE_PREVIEW

namespace {

using cutline::editor::ExportFormat;
using cutline::editor::ExportPreset;

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

/// Zero means the sequence's own rate, which is the honest default: re-timing
/// a sequence on the way out is a decision, not a preference.
constexpr std::array kExportRates{0.0, 23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0};

/// Output sizes by the picture's short side, as Premiere's presets name them.
///
/// The short side rather than a pair of numbers, because the aspect ratio is
/// the sequence's business: offering "1280 x 720" to a vertical sequence would
/// have to either letterbox it or stretch it, and neither is what choosing a
/// smaller size means. What each works out to in pixels is put in the label,
/// so nothing has to be taken on trust.
constexpr std::array kExportShortSides{0, 2160, 1440, 1080, 720, 540, 480, 360};

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

constexpr std::array<const char*, 4> kExportColours{"Match sequence", "Rec. 709", "Rec. 2100 PQ",
                                                    "Rec. 2100 HLG"};

/// The index of a value among choices, or of the nearest one: a preset saved
/// with a value the menu does not offer still shows something close rather
/// than something arbitrary.
template <typename Values, typename Value>
[[nodiscard]] std::size_t nearest(const Values& values, Value value) {
  std::size_t best = 0;
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (std::abs(static_cast<double>(values[i]) - static_cast<double>(value)) <
        std::abs(static_cast<double>(values[best]) - static_cast<double>(value))) {
      best = i;
    }
  }
  return best;
}

[[nodiscard]] std::size_t quality_index(int quality) {
  std::array<int, kExportQualities.size()> values{};
  for (std::size_t i = 0; i < values.size(); ++i) values[i] = kExportQualities[i].quality;
  return nearest(values, quality);
}

[[nodiscard]] std::size_t mixdown_index(const ExportPreset& preset) {
  if (preset.separate_audio) return 2;
  return preset.channels == 1 ? 1 : 0;
}

[[nodiscard]] std::size_t colour_index(const ExportPreset& preset) {
  return preset.colour.has_value() ? static_cast<std::size_t>(*preset.colour) + 1 : 0;
}

[[nodiscard]] std::string rate_name(double fps) {
  return fps <= 0.0 ? std::string("Same as sequence") : fps_label(fps) + " fps";
}

/// What a short side works out to for this sequence, in the label.
[[nodiscard]] std::string resolution_name(const App& app, int short_side) {
  ExportPreset probe;
  probe.short_side = short_side;
  const auto plan = cutline::editor::plan_for(probe, app.session.project());
  const std::string size =
      plan.has_value() ? std::format("{} x {}", plan->width, plan->height) : std::string("?");
  if (short_side == 0) return "Same as sequence (" + size + ")";
  return std::format("{}p ({})", short_side, size);
}

[[nodiscard]] std::vector<std::string> preset_names(const App& app, bool custom) {
  std::vector<std::string> names;
  for (const ExportPreset& preset : cutline::editor::all_export_presets(app.export_presets)) {
    names.push_back(preset.name);
  }
  if (custom) names.emplace_back("Custom");
  return names;
}

/// Changes the dropdown's choices only when they differ, so a refresh does
/// not close a list somebody has open.
void offer(Dropdown* dropdown, std::vector<std::string> options, std::size_t selected) {
  if (dropdown == nullptr) return;
  if (dropdown->options() != options) dropdown->set_options(std::move(options));
  if (dropdown->selected() != selected) dropdown->set_selected(selected);
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

void save_presets(App& app) {
  const auto written = cutline::editor::write_export_presets(
      cutline::editor::default_export_presets_path(), app.export_presets);
  if (!written.has_value()) {
    complain(app.export_window != nullptr ? app.export_window->window : app.main.window,
             "Could not save the export presets.\n\n" + written.error());
  }
}

/// Premiere's Save Preset: a name, and the settings as they stand.
void open_save_preset(App& app) {
  if (app.export_window == nullptr || app.export_window->host == nullptr) return;
  auto dialog = std::make_unique<cutline::ui::Dialog>("Save Preset");
  auto& name_row = dialog->row("Name");
  const std::string matched =
      cutline::editor::matching_preset_name(app.export_presets, app.export_setup.preset);
  // A saved preset's own name to start from, so saving again over it is one
  // press; a built-in's would only be refused.
  const bool own = matched != "Custom" && !std::ranges::any_of(
                                              cutline::editor::built_in_export_presets(),
                                              [&](const ExportPreset& p) { return p.name == matched; });
  auto& name = name_row.emplace<TextField>(own ? matched : std::string{});
  name.set_columns(24);
  dialog->set_accept("OK", [&app, field = &name]() -> std::optional<std::string> {
    ExportPreset preset = app.export_setup.preset;
    preset.name = field->text();
    const auto saved = cutline::editor::save_export_preset(app.export_presets, preset);
    if (!saved.has_value()) return saved.error();
    app.export_setup.preset = std::move(preset);
    save_presets(app);
    refresh_export_dialog(app);
    return std::nullopt;
  });
  dialog->set_cancel("Cancel");
  Rect anchor = settings_anchor();
  if (app.export_controls.preset != nullptr) {
    const Rect at = app.export_controls.preset->bounds();
    anchor = Rect{at.x, at.bottom(), 0.0, 0.0};
  }
  app.export_window->host->open_popup(std::move(dialog), anchor);
}

void delete_current_preset(App& app) {
  const std::string name = app.export_setup.preset.name;
  if (!cutline::editor::delete_export_preset(app.export_presets, name)) return;
  save_presets(app);
  // What was shown stays shown; it is simply nobody's preset now.
  refresh_export_dialog(app);
}

}  // namespace

void refresh_export_dialog(App& app) {
  // The controls rather than the window: `--check` builds them with no window.
  App::ExportControls& controls = app.export_controls;
  if (controls.preset == nullptr) return;
  const ExportPreset& preset = app.export_setup.preset;

  const std::string matched = cutline::editor::matching_preset_name(app.export_presets, preset);
  const bool custom = matched == "Custom";
  {
    std::vector<std::string> names = preset_names(app, custom);
    const auto at = std::ranges::find(names, matched);
    const auto index = static_cast<std::size_t>(at - names.begin());
    offer(controls.preset, std::move(names), index);
  }
  if (controls.delete_preset != nullptr) {
    controls.delete_preset->set_enabled(std::ranges::any_of(
        app.export_presets.presets, [&](const ExportPreset& p) { return p.name == matched; }));
  }

  {
    std::vector<std::string> names;
    for (const ExportFormat format : cutline::editor::export_formats()) {
      names.emplace_back(cutline::editor::format_label(format));
    }
    const auto formats = cutline::editor::export_formats();
    offer(controls.format, std::move(names),
          static_cast<std::size_t>(std::ranges::find(formats, preset.format) - formats.begin()));
  }
  offer(controls.colour, {kExportColours.begin(), kExportColours.end()}, colour_index(preset));
  {
    std::vector<std::string> names;
    for (const QualityChoice& quality : kExportQualities) names.emplace_back(quality.name);
    offer(controls.quality, std::move(names), quality_index(preset.quality));
    // An intermediate's quality is its profile; there is nothing to choose.
    if (controls.quality != nullptr) {
      controls.quality->set_enabled(!cutline::editor::is_intermediate(preset.format));
    }
  }
  {
    std::vector<std::string> names;
    for (const int side : kExportShortSides) names.push_back(resolution_name(app, side));
    offer(controls.resolution, std::move(names), nearest(kExportShortSides, preset.short_side));
  }
  {
    std::vector<std::string> names;
    for (const double fps : kExportRates) names.push_back(rate_name(fps));
    offer(controls.rate, std::move(names), nearest(kExportRates, preset.fps));
  }
  {
    std::vector<std::string> names;
    for (const MixdownChoice& mix : kExportMixdowns) names.emplace_back(mix.name);
    offer(controls.mixdown, std::move(names), mixdown_index(preset));
    // A mixdown for a file with no audio in it is a choice with nothing behind
    // it, so it greys out rather than sitting there looking answerable.
    if (controls.mixdown != nullptr) controls.mixdown->set_enabled(preset.audio);
  }
  if (controls.audio != nullptr) controls.audio->set_checked(preset.audio);

  // What the settings mean here, which may be that they cannot be written.
  const auto plan = cutline::editor::plan_for(preset, app.session.project());
  // The container follows the format, and the name says so before Export is
  // pressed rather than after: a ProRes file is a .mov.
  if (plan.has_value() && !app.export_setup.path.empty() &&
      app.export_setup.path.extension() != plan->extension) {
    app.export_setup.path.replace_extension(plan->extension);
  }
  if (controls.output != nullptr) controls.output->set_text(output_label(app));
  App::RenderJob* job = find_job(app, app.export_setup.watching);
  const bool rendering = job != nullptr && job->state.load() == RenderState::Rendering;
  if (controls.start != nullptr) {
    controls.start->set_text(rendering ? "Cancel" : "Export");
    // Nothing to export to is not an error worth a dialog; it is a button that
    // should not be pressable yet.
    controls.start->set_enabled(rendering ||
                                (!app.export_setup.path.empty() && plan.has_value()));
  }
  if (controls.queue != nullptr) {
    controls.queue->set_enabled(!app.export_setup.path.empty() && plan.has_value());
  }

  double fraction = 0.0;
  std::string status;
  if (!plan.has_value()) {
    status = plan.error();
  } else if (job != nullptr) {
    switch (job->state.load()) {
      case RenderState::Waiting: {
        const auto ahead = std::ranges::count_if(app.render_queue, [&](const auto& other) {
          const RenderState state = other->state.load();
          return other->id < job->id &&
                 (state == RenderState::Waiting || state == RenderState::Rendering);
        });
        status = app.queue_running
                     ? std::format("In the render queue, {} ahead of it.", ahead)
                     : std::string("In the render queue, waiting for it to be started.");
        break;
      }
      case RenderState::Rendering:
        fraction = job->fraction.load();
        status = "Exporting...";
        break;
      case RenderState::Done:
        fraction = 1.0;
        status = job->outcome;
        break;
      case RenderState::Failed:
      case RenderState::Cancelled: status = job->outcome; break;
    }
  } else {
    status = std::format("{} x {}, {}", plan->width, plan->height,
                         cutline::editor::format_label(plan->format));
  }
  if (controls.progress != nullptr) controls.progress->set_fraction(fraction);
  if (controls.status != nullptr) controls.status->set_text(std::move(status));
  mark_dirty(app);
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
  App::ExportControls scratch;
  App::ExportControls& controls = app != nullptr ? app->export_controls : scratch;

  // One row per setting, label on the left, control on the right — which is
  // how every settings dialog on the platform reads, Premiere's included —
  // and the labels lined up, which the dialog does. The choices themselves
  // are filled in by the refresh, which is what a preset changing them uses.
  const auto row = [&dialog](std::string label) -> Box& { return dialog.row(std::move(label)); };
  // A setting changed by hand. The preset keeps its name, and the refresh
  // shows it as "Custom" for as long as the settings differ from it.
  const auto edit = [app](auto change) {
    return [app, change](auto value) {
      if (app == nullptr) return;
      change(app->export_setup.preset, value);
      refresh_export_dialog(*app);
    };
  };

  {
    auto& line = row("Preset");
    auto& choice = line.emplace<Dropdown>(std::vector<std::string>{"Match Source - High Quality"});
    choice.set_on_change([app](std::size_t index) {
      if (app == nullptr) return;
      const auto all = cutline::editor::all_export_presets(app->export_presets);
      // "Custom" is the entry after the last preset, and choosing it is
      // choosing what is already there.
      if (index < all.size()) app->export_setup.preset = all[index];
      refresh_export_dialog(*app);
    });
    controls.preset = &choice;
  }
  {
    auto& line = row("");
    auto& save = line.emplace<Button>("Save Preset...", [app] {
      if (app != nullptr) open_save_preset(*app);
    });
    save.set_tooltip("Keep these settings under a name of your own");
    auto& remove = line.emplace<Button>("Delete Preset", [app] {
      if (app != nullptr) delete_current_preset(*app);
    });
    remove.set_tooltip("Delete this saved preset. Built-in presets cannot be deleted");
    controls.delete_preset = &remove;
  }
  {
    auto& choice = row("Format").emplace<Dropdown>();
    choice.set_on_change(edit([](ExportPreset& preset, std::size_t index) {
      const auto formats = cutline::editor::export_formats();
      if (index < formats.size()) preset.format = formats[index];
    }));
    controls.format = &choice;
  }
  {
    auto& choice = row("Colour").emplace<Dropdown>();
    choice.set_on_change(edit([](ExportPreset& preset, std::size_t index) {
      preset.colour = index == 0 ? std::nullopt
                                 : std::optional(static_cast<cutline::core::WorkingColour>(
                                       std::min<std::size_t>(index - 1, 2)));
    }));
    controls.colour = &choice;
  }
  {
    auto& choice = row("Quality").emplace<Dropdown>();
    choice.set_on_change(edit([](ExportPreset& preset, std::size_t index) {
      if (index < kExportQualities.size()) preset.quality = kExportQualities[index].quality;
    }));
    controls.quality = &choice;
  }
  {
    auto& choice = row("Resolution").emplace<Dropdown>();
    choice.set_on_change(edit([](ExportPreset& preset, std::size_t index) {
      if (index < kExportShortSides.size()) preset.short_side = kExportShortSides[index];
    }));
    controls.resolution = &choice;
  }
  {
    auto& choice = row("Frame rate").emplace<Dropdown>();
    choice.set_on_change(edit([](ExportPreset& preset, std::size_t index) {
      if (index < kExportRates.size()) preset.fps = kExportRates[index];
    }));
    controls.rate = &choice;
  }
  {
    auto& choice = row("Audio").emplace<Dropdown>();
    choice.set_on_change(edit([](ExportPreset& preset, std::size_t index) {
      if (index >= kExportMixdowns.size()) return;
      preset.channels = kExportMixdowns[index].channels;
      preset.separate_audio = kExportMixdowns[index].separate;
    }));
    controls.mixdown = &choice;
  }
  {
    // Premiere's Captions section, offered only when there are captions to
    // do something with.
    bool any = false;
    if (app != nullptr) {
      for (const cutline::core::Track& track : app->session.project().sequence().tracks) {
        any = any || (track.kind == cutline::core::TrackKind::Caption && !track.clips.empty());
      }
    }
    auto& choice = row("Captions").emplace<Dropdown>(
        std::vector<std::string>{"None", "Burn Captions Into Video", "Create Sidecar File"},
        app != nullptr ? static_cast<std::size_t>(app->export_setup.captions) : 0u);
    choice.set_on_change([app](std::size_t index) {
      if (app == nullptr) return;
      app->export_setup.captions =
          static_cast<cutline::engine::CaptionExport>(std::min<std::size_t>(index, 2));
    });
    choice.set_enabled(any);
  }
  {
    auto& line = row("Output name");
    auto& output = line.emplace<Button>(app != nullptr ? output_label(*app) : "Choose a file...",
                                        [app] {
                                          if (app == nullptr) return;
                                          const auto plan = cutline::editor::plan_for(
                                              app->export_setup.preset, app->session.project());
                                          const auto chosen = choose_movie_file(
                                              app->export_window != nullptr
                                                  ? app->export_window->window
                                                  : app->main.window,
                                              plan.has_value() ? plan->extension : ".mp4");
                                          if (!chosen.has_value()) return;
                                          app->export_setup.path = *chosen;
                                          refresh_export_dialog(*app);
                                        });
    controls.output = &output;
  }

  auto& sound = body.emplace<Checkbox>("Export audio", true);
  sound.set_on_change(edit([](ExportPreset& preset, bool on) { preset.audio = on; }));
  controls.audio = &sound;

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

  controls.progress = &body.emplace<ProgressBar>();
  auto& status = body.emplace<Label>("");
  status.set_small(true);
  controls.status = &status;

  // Export first and the default, then Close: the platform's order. Queue is
  // with the actions that are not an answer to the question, on the left, as
  // Premiere has it beside Export.
  dialog.set_accept("Export", [app] {
    if (app == nullptr) return std::optional<std::string>{};
    if (App::RenderJob* job = find_job(*app, app->export_setup.watching);
        job != nullptr && job->state.load() == RenderState::Rendering) {
      remove_job(*app, job->id);
      return std::optional<std::string>{};
    }
    const auto queued = queue_export(*app, true);
    if (!queued.has_value()) return std::optional<std::string>{queued.error()};
    app->export_setup.watching = *queued;
    refresh_export_dialog(*app);
    return std::optional<std::string>{};
  });
  dialog.set_closes_on_accept(false);
  dialog.set_cancel("Close");
  dialog.set_on_close([app] {
    if (app != nullptr) close_export_dialog(*app);
  });
  controls.start = &dialog.accept_button();
  auto& queue = dialog.add_button("Queue", [app] {
    if (app == nullptr) return;
    const auto queued = queue_export(*app, false);
    if (!queued.has_value()) {
      if (app->export_controls.status != nullptr) {
        app->export_controls.status->set_text(queued.error());
      }
      mark_dirty(*app);
      return;
    }
    app->export_setup.watching = *queued;
    // Shown, so the job is seen to have gone somewhere: opened if it was
    // closed, and brought in front of the tab it shares a strip with.
    if (!panel_is_open(*app, PanelId("queue"))) {
      toggle_panel(*app, PanelId("queue"));
    } else if (cutline::ui::activate_panel(layout_of(*app), PanelId("queue"))) {
      app->dock_stale = true;
    }
    refresh_export_dialog(*app);
  });
  queue.set_tooltip("Add this to the render queue without starting it");
  controls.queue = &queue;

  if (app != nullptr) {
    refresh_export_dialog(*app);
  } else {
    // Filled once here for the gallery, which lays the window out with no
    // application behind it.
    controls.format->set_options({"H.264"});
  }
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
      CreateWindowExW(WS_EX_TOOLWINDOW | kWindowExStyle, kWindowClass, L"Export Settings",
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
  app.export_controls = {};

  const HWND window = app.export_window->window;
  remember_dialog_size(app, *app.export_window);
  app.export_window.reset();
  if (window != nullptr) DestroyWindow(window);
  mark_dirty(app);
}

#else
void open_export_dialog(App&) {}
void refresh_export_dialog(App&) {}
#endif

}  // namespace cutline::tool
