/// Premiere's Auto Reframe: Sequence > Auto Reframe Sequence, which makes a
/// reframed copy of the open sequence in another shape, and the Auto Reframe
/// effect it puts on each clip, which can also be applied from the Effects
/// panel on its own.
///
/// Applying the effect is enough to start the looking, as Premiere does: each
/// Auto Reframe in the open sequence without an analysis is analysed, one at
/// a time, on a thread of its own, the way Warp Stabilizer's are, and nothing
/// is written into the project; see `render/auto_reframe.hpp`.

#include "app.hpp"

#include "cutline/core/auto_reframe.hpp"
#include "cutline/engine/reframe_analysis.hpp"
#include "cutline/render/auto_reframe.hpp"

#include <format>

namespace cutline::tool {
namespace {

/// An Auto Reframe in the open sequence, and the analysis it wants.
struct Wanted {
  std::string clip_id;
  std::string key;
};

[[nodiscard]] std::vector<Wanted> reframes(const cutline::core::Project& project) {
  std::vector<Wanted> out;
  for (const cutline::core::Track& track : project.sequence().tracks) {
    if (track.kind != cutline::core::TrackKind::Video) continue;
    for (const cutline::core::Clip& clip : track.clips) {
      if (cutline::render::auto_reframe_of(clip) == nullptr) continue;
      const std::string key = cutline::render::reframe_key(project, clip);
      if (!key.empty()) out.push_back({clip.id, key});
    }
  }
  return out;
}

void start(App& app, const Wanted& wanted) {
  if (app.reframe_worker.joinable()) app.reframe_worker.join();
  app.reframe_running = true;
  app.reframe_cancel = false;
  app.reframe_done = 0;
  app.reframe_total = 0;
  app.reframe_key = wanted.key;
  app.reframe_error.clear();
  refresh_busy(app);

  const cutline::core::Project project = app.session.project();
  const bool software = app.settings.software_renderer;
  HWND window = app.main.window;
  app.reframe_worker = std::thread([&app, project, wanted, software, window] {
    auto device = cutline::gpu::Device::create({.force_software = software});
    auto analysis =
        device.has_value()
            ? cutline::engine::analyse_for_reframe(
                  *device, project, wanted.clip_id,
                  [&app, window, last = std::chrono::steady_clock::now()](int done,
                                                                         int total) mutable {
                    app.reframe_done = done;
                    app.reframe_total = total;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last > std::chrono::milliseconds(250)) {
                      last = now;
                      PostMessageW(window, kReframeAnalysed, 1, 0);
                    }
                    return !app.reframe_cancel.load();
                  })
            : std::unexpected(device.error());
    std::string error;
    if (analysis.has_value()) {
      // Beside its final name and moved there whole, so a render never finds
      // half of one.
      std::error_code failed;
      const std::filesystem::path path = cutline::render::reframe_analysis_path(wanted.key);
      std::filesystem::create_directories(path.parent_path(), failed);
      std::filesystem::path partial = path;
      partial += ".part";
      if (cutline::track::save_reframe(*analysis, partial)) {
        std::filesystem::rename(partial, path, failed);
        if (failed) error = "the analysis could not be saved";
      } else {
        error = "the analysis could not be saved";
      }
    } else {
      error = analysis.error();
    }
    {
      const std::scoped_lock lock(app.reframe_lock);
      app.reframe_error = std::move(error);
    }
    app.reframe_running = false;
    PostMessageW(window, kReframeAnalysed, 0, 0);
  });
}

}  // namespace

bool analysing_reframe(const App& app) { return app.reframe_running.load(); }

void refresh_reframe_analysis(App& app) {
  if (analysing_reframe(app)) return;
  for (const Wanted& wanted : reframes(app.session.project())) {
    if (cutline::render::has_reframe_analysis(wanted.key) || app.reframe_held.contains(wanted.key)) {
      continue;
    }
    start(app, wanted);
    return;
  }
}

void collect_reframe_analysis(App& app, bool progress_only) {
  app.inspector_stale = true;
  mark_dirty(app);
  if (progress_only || analysing_reframe(app)) return;
  if (app.reframe_worker.joinable()) app.reframe_worker.join();
  std::string error;
  {
    const std::scoped_lock lock(app.reframe_lock);
    error = std::move(app.reframe_error);
  }
  // One that failed or was stopped is not started again by itself; Analyze
  // asks for it.
  if (!error.empty()) app.reframe_held.insert(app.reframe_key);
  if (!error.empty() && !app.reframe_cancel.load()) {
    complain(app.main.window, "Auto Reframe could not analyse the clip: " + error + ".");
  }
  app.reframe_key.clear();
  refresh_busy(app);
  invalidate_preview(app);
  refresh_reframe_analysis(app);
}

void analyse_reframe_again(App& app, const std::string& clip_id) {
  const cutline::core::Project& project = app.session.project();
  const cutline::core::Clip* clip = cutline::core::find_clip(project, clip_id);
  if (clip == nullptr) return;
  const std::string key = cutline::render::reframe_key(project, *clip);
  if (key.empty()) return;
  if (analysing_reframe(app)) {
    if (app.reframe_key == key) return;
    app.reframe_cancel = true;
  }
  std::error_code error;
  std::filesystem::remove(cutline::render::reframe_analysis_path(key), error);
  app.reframe_held.erase(key);
  invalidate_preview(app);
  if (!analysing_reframe(app)) refresh_reframe_analysis(app);
  app.inspector_stale = true;
}

void cancel_reframe_analysis(App& app) {
  if (analysing_reframe(app)) app.reframe_cancel = true;
}

std::string reframe_status(App& app, const std::string& clip_id) {
  const cutline::core::Project& project = app.session.project();
  const cutline::core::Clip* clip = cutline::core::find_clip(project, clip_id);
  if (clip == nullptr) return {};
  const std::string key = cutline::render::reframe_key(project, *clip);
  if (analysing_reframe(app) && app.reframe_key == key) {
    const int total = std::max(app.reframe_total.load(), 1);
    return std::format("Analyzing {}%", app.reframe_done.load() * 100 / total);
  }
  if (!cutline::render::has_reframe_analysis(key)) {
    return app.reframe_held.contains(key) ? "Not analysed" : "Waiting to analyse";
  }
  return "Analysed";
}

void stop_reframe_analysis(App& app) {
  app.reframe_cancel = true;
  if (app.reframe_worker.joinable()) app.reframe_worker.join();
}

void open_auto_reframe_dialog(App& app) {
  if (app.main.host == nullptr) return;
  const cutline::core::Sequence& open = app.session.project().sequence();

  // Premiere's shapes, in its order, with Custom last.
  struct Shape {
    const char* name;
    double width;
    double height;
  };
  static constexpr std::array kShapes{Shape{"Square 1:1", 1, 1}, Shape{"Vertical 4:5", 4, 5},
                                      Shape{"Vertical 9:16", 9, 16},
                                      Shape{"Horizontal 16:9", 16, 9}, Shape{"Custom", 0, 0}};
  std::vector<std::string> names;
  for (const Shape& shape : kShapes) names.emplace_back(shape.name);

  const auto name_for = [&app](cutline::core::FrameSize size) {
    const cutline::core::Sequence& s = app.session.project().sequence();
    return std::format("{} ({}x{})", s.name.empty() ? std::string("Sequence") : s.name,
                       size.width, size.height);
  };

  auto dialog = std::make_unique<cutline::ui::Dialog>("Auto Reframe Sequence");
  constexpr std::size_t kVertical = 2;
  const cutline::core::FrameSize first = cutline::core::reframed_size(open, 9, 16);
  auto& name = dialog->row("Sequence Name").emplace<cutline::ui::TextField>(name_for(first));
  name.set_columns(24);
  auto& shape = dialog->row("Target Aspect Ratio").emplace<Dropdown>(names, kVertical);
  auto& custom = dialog->row("Custom");
  auto& custom_w = custom.emplace<cutline::ui::NumericField>(
      cutline::ui::ValueRange{.minimum = 1.0, .maximum = 100.0, .step = 1.0}, 9.0);
  custom.emplace<Label>(":").set_small(true);
  auto& custom_h = custom.emplace<cutline::ui::NumericField>(
      cutline::ui::ValueRange{.minimum = 1.0, .maximum = 100.0, .step = 1.0}, 16.0);
  auto& motion = dialog->row("Motion Tracking")
                     .emplace<Dropdown>(std::vector<std::string>{"Slower Motion", "Default",
                                                                 "Faster Motion"},
                                        1u);
  auto& nest = dialog->row("Clip Nesting")
                   .emplace<Dropdown>(std::vector<std::string>{"Don't nest clips", "Nest clips"},
                                      0u);

  // The shape chosen, from the menu or the two numbers.
  const auto size_of = [&app, shape = &shape, w = &custom_w, h = &custom_h] {
    const Shape& chosen = kShapes[std::min(shape->selected(), kShapes.size() - 1)];
    const bool own = chosen.width <= 0.0;
    return cutline::core::reframed_size(app.session.project().sequence(),
                                        own ? w->value() : chosen.width,
                                        own ? h->value() : chosen.height);
  };
  // The name follows the shape until somebody types their own.
  auto named = std::make_shared<std::string>(name.text());
  const auto rename = [field = &name, named, size_of, name_for] {
    if (field->text() != *named) return;
    *named = name_for(size_of());
    field->set_text(*named);
  };
  shape.set_on_change([rename](std::size_t) { rename(); });
  custom_w.set_on_change([rename](double) { rename(); });
  custom_h.set_on_change([rename](double) { rename(); });

  dialog->set_accept("Create", [&app, field = &name, size_of, motion = &motion,
                                nest = &nest]() -> std::optional<std::string> {
    const cutline::core::FrameSize size = size_of();
    app.session.name_next_edit("Auto Reframe Sequence");
    app.session.apply(cutline::core::auto_reframe_sequence(
        app.session.project(),
        {.name = field->text(),
         .size = size,
         .motion = static_cast<int>(motion->selected()),
         .nest = nest->selected() == 1}));
    app.dock_stale = true;
    refresh_all(app);
    invalidate_preview(app);
    mark_dirty(app);
    refresh_reframe_analysis(app);
    return std::nullopt;
  });
  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

}  // namespace cutline::tool
