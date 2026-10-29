/// Premiere's Scene Edit Detection: on clips of footage that was edited before
/// it arrived, find the cuts and do any of three things at each.
///
/// The dialog asks which; Analyze looks at every frame of every selected clip
/// on a thread of its own, behind a window that says how far it has got and
/// can stop it; and what was found is applied as one edit, so one undo takes
/// it all back. What is found is kept as times within each clip rather than on
/// the timeline, so a clip moved while it was looked at is cut where its
/// pictures change and not where they used to be.

#include "app.hpp"

#include "cutline/core/scene_edits.hpp"
#include "cutline/engine/scene_detection.hpp"

namespace cutline::tool {
namespace {

[[nodiscard]] cutline::ui::ProgressBar* scene_bar_of(App& app) {
  if (app.scene_dialog == nullptr || app.main.host == nullptr ||
      app.main.host->popup() != app.scene_dialog) {
    return nullptr;
  }
  return app.scene_bar;
}

/// The clips among `selected` that have a picture to look at: a linked sound
/// is looked at through its picture, and a still, a title or a matte has no
/// cuts in it to find.
[[nodiscard]] std::vector<std::string> pictures_in(const App& app,
                                                   const std::vector<std::string>& selected) {
  const cutline::core::Project& project = app.session.project();
  std::vector<std::string> out;
  for (const std::string& id : selected) {
    const cutline::core::Clip* clip = cutline::core::find_clip(project, id);
    if (clip == nullptr || clip->kind != cutline::core::TrackKind::Video) continue;
    const auto media = std::ranges::find(project.media, clip->media_id, &cutline::core::Media::id);
    if (media == project.media.end() || cutline::core::is_still_like(*media)) continue;
    if (!media->has_video && media->sequence_id.empty()) continue;
    if (std::ranges::find(out, id) == out.end()) out.push_back(id);
  }
  return out;
}

void start(App& app, std::vector<std::string> clips, cutline::core::SceneEditOptions options) {
  if (app.scene_worker.joinable()) app.scene_worker.join();
  app.scene_running = true;
  app.scene_cancel = false;
  app.scene_done = 0;
  app.scene_total = 0;
  app.scene_clip = 0;
  app.scene_clips = static_cast<int>(clips.size());
  app.scene_options = options;
  app.scene_result.reset();
  refresh_busy(app);

  const cutline::core::Project project = app.session.project();
  const bool software = app.settings.software_renderer;
  HWND window = app.main.window;
  app.scene_worker = std::thread([&app, project, clips = std::move(clips), software, window] {
    std::expected<SceneCuts, std::string> found = SceneCuts{};
    auto device = cutline::gpu::Device::create({.force_software = software});
    if (!device.has_value()) found = std::unexpected(device.error());
    for (std::size_t i = 0; found.has_value() && i < clips.size(); ++i) {
      app.scene_clip = static_cast<int>(i);
      auto cuts = cutline::engine::detect_scene_edits(
          *device, project, clips[i],
          [&app, window, last = std::chrono::steady_clock::now()](int done, int total) mutable {
            app.scene_done = done;
            app.scene_total = total;
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::milliseconds(100)) {
              last = now;
              PostMessageW(window, kScenesDetected, 1, 0);
            }
            return !app.scene_cancel.load();
          });
      if (!cuts.has_value()) {
        found = std::unexpected(cuts.error());
        break;
      }
      const double start = cutline::core::find_clip(project, clips[i])->start;
      std::vector<double> within;
      for (const double t : *cuts) within.push_back(t - start);
      found->emplace_back(clips[i], std::move(within));
    }
    {
      const std::scoped_lock lock(app.scene_lock);
      app.scene_result = std::move(found);
    }
    app.scene_running = false;
    PostMessageW(window, kScenesDetected, 0, 0);
  });
  // The progress window goes up once the dialog has closed, which it does on
  // the way out of its Analyze.
  PostMessageW(app.main.window, kScenesDetected, 2, 0);
}

}  // namespace

bool can_detect_scenes(const App& app) {
  return !pictures_in(app, app.session.selected_group()).empty();
}

void open_scene_edit_dialog(App& app) {
  if (app.main.host == nullptr) return;
  std::vector<std::string> clips = pictures_in(app, app.session.selected_group());
  if (clips.empty()) {
    complain(app.main.window, "Select a clip of video in the timeline first.");
    return;
  }

  auto dialog = std::make_unique<cutline::ui::Dialog>("Scene Edit Detection");
  Box& body = dialog->body();
  auto& cut = body.emplace<Checkbox>("Apply a cut at each detected cut point", true);
  auto& subclips =
      body.emplace<Checkbox>("Create subclips from each detected cut point in a bin", false);
  auto& markers = body.emplace<Checkbox>("Create clip markers at each detected cut point", false);
  dialog->set_accept("Analyze", [&app, clips = std::move(clips), cut = &cut,
                                 subclips = &subclips,
                                 markers = &markers]() -> std::optional<std::string> {
    const cutline::core::SceneEditOptions options{
        .cut = cut->checked(), .subclips = subclips->checked(), .markers = markers->checked()};
    if (!options.cut && !options.subclips && !options.markers) {
      return "Choose at least one thing to do at each cut.";
    }
    if (app.scene_running.load()) return "Scene edits are already being detected.";
    start(app, clips, options);
    return std::nullopt;
  });
  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

bool detecting_scenes(const App& app) { return app.scene_running.load(); }

void show_scene_detection(App& app) {
  if (!app.scene_running.load() || app.main.host == nullptr) return;
  auto progress = std::make_unique<cutline::ui::Dialog>("Scene Edit Detection");
  auto& bar = progress->body().emplace<cutline::ui::ProgressBar>(0.0);
  progress->set_accept("", {});
  progress->set_cancel("Cancel", [&app] { app.scene_cancel = true; });
  app.scene_dialog = progress.get();
  app.scene_bar = &bar;
  app.main.host->open_popup(std::move(progress), settings_anchor());
  collect_scene_detection(app, true);
}

void collect_scene_detection(App& app, bool progress_only) {
  if (cutline::ui::ProgressBar* bar = scene_bar_of(app); bar != nullptr) {
    const int done = app.scene_done.load();
    const int total = std::max(app.scene_total.load(), 1);
    const int clips = std::max(app.scene_clips.load(), 1);
    // The clips are equal parts of the bar, whatever their lengths: the
    // length of the next is not known until it is opened.
    bar->set_fraction((app.scene_clip.load() + static_cast<double>(done) / total) / clips);
    bar->set_text(clips > 1 ? std::format("Analyzing clip {} of {}", app.scene_clip.load() + 1,
                                          clips)
                            : std::string("Analyzing"));
    mark_dirty(app);
  }
  if (progress_only || app.scene_running.load()) return;
  if (app.scene_worker.joinable()) app.scene_worker.join();
  if (scene_bar_of(app) != nullptr) app.main.host->close_popup();
  app.scene_dialog = nullptr;
  app.scene_bar = nullptr;
  refresh_busy(app);

  std::optional<std::expected<SceneCuts, std::string>> result;
  {
    const std::scoped_lock lock(app.scene_lock);
    result = std::move(app.scene_result);
    app.scene_result.reset();
  }
  if (!result.has_value() || app.scene_cancel.load()) return;
  if (!result->has_value()) {
    complain(app.main.window, "Could not detect the scene edits.\n\n" + result->error());
    return;
  }
  cutline::core::Project project = app.session.project();
  std::size_t found = 0;
  for (const auto& [clip_id, within] : **result) {
    const cutline::core::Clip* clip = cutline::core::find_clip(project, clip_id);
    if (clip == nullptr) continue;
    std::vector<double> times;
    for (const double t : within) times.push_back(clip->start + t);
    found += times.size();
    project = cutline::core::apply_scene_edits(std::move(project), clip_id, times,
                                               app.scene_options);
  }
  if (found == 0) {
    complain(app.main.window, "No scene edits were found.");
    return;
  }
  app.session.name_next_edit("Scene Edit Detection");
  app.session.apply(std::move(project));
  refresh_all(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  mark_dirty(app);
}

void cancel_scene_detection(App& app) {
  app.scene_cancel = true;
  if (app.scene_worker.joinable()) app.scene_worker.join();
}

}  // namespace cutline::tool
