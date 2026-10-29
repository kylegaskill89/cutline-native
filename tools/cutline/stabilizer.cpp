/// Warp Stabilizer's analysis, done in the background as Premiere does it.
///
/// Applying the effect is enough to start it: whatever Warp Stabilizer in the
/// open sequence has no analysis yet is analysed, one at a time, on a thread
/// of its own, while the Program Monitor says so across the picture. Step one
/// is following corners through the clip; step two is working out the
/// stabilisation from them, which is done here too so the first frame shown
/// afterwards does not have to wait for it. What an analysis is named after
/// is what was analysed, so nothing is written into the project and undo has
/// nothing to undo; see `render/warp_stabilizer.hpp`.

#include "app.hpp"

#include "cutline/engine/stabilizer_analysis.hpp"
#include "cutline/render/warp_stabilizer.hpp"

namespace cutline::tool {
namespace {

/// A Warp Stabilizer in the open sequence, and the analysis it wants.
struct Wanted {
  std::string clip_id;
  std::size_t effect = 0;
  std::string key;
};

[[nodiscard]] std::vector<Wanted> stabilizers(const cutline::core::Project& project) {
  std::vector<Wanted> out;
  for (const cutline::core::Track& track : project.sequence().tracks) {
    if (track.kind != cutline::core::TrackKind::Video) continue;
    for (const cutline::core::Clip& clip : track.clips) {
      for (std::size_t i = 0; i < clip.effects.size(); ++i) {
        const cutline::core::ClipEffect& effect = clip.effects[i];
        if (!effect.enabled || effect.type != "warpstabilizer") continue;
        const std::string key = cutline::render::analysis_key(project, clip, effect);
        if (!key.empty()) out.push_back({clip.id, i, key});
      }
    }
  }
  return out;
}

void start(App& app, const Wanted& wanted) {
  if (app.stabilize_worker.joinable()) app.stabilize_worker.join();
  app.stabilize_running = true;
  app.stabilize_cancel = false;
  app.stabilize_done = 0;
  app.stabilize_total = 0;
  app.stabilize_step = 1;
  app.stabilize_key = wanted.key;
  app.stabilize_error.clear();
  refresh_busy(app);

  const cutline::core::Project project = app.session.project();
  const bool software = app.settings.software_renderer;
  const cutline::track::StabilizeSettings settings = cutline::render::stabilize_settings(
      cutline::core::find_clip(project, wanted.clip_id)->effects[wanted.effect]);
  HWND window = app.main.window;
  app.stabilize_worker = std::thread([&app, project, wanted, settings, software, window] {
    auto device = cutline::gpu::Device::create({.force_software = software});
    auto analysis =
        device.has_value()
            ? cutline::engine::analyse_for_stabilizer(
                  *device, project, wanted.clip_id, wanted.effect,
                  [&app, window, last = std::chrono::steady_clock::now()](int done,
                                                                         int total) mutable {
                    app.stabilize_done = done;
                    app.stabilize_total = total;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last > std::chrono::milliseconds(250)) {
                      last = now;
                      PostMessageW(window, kStabilizerAnalysed, 1, 0);
                    }
                    return !app.stabilize_cancel.load();
                  })
            : std::unexpected(device.error());
    std::string error;
    if (analysis.has_value()) {
      // Written beside its final name and moved there whole, so a render
      // never finds half of one.
      std::error_code failed;
      const std::filesystem::path path = cutline::render::analysis_path(wanted.key);
      std::filesystem::create_directories(path.parent_path(), failed);
      std::filesystem::path partial = path;
      partial += ".part";
      if (cutline::track::save_analysis(*analysis, partial)) {
        std::filesystem::rename(partial, path, failed);
        if (failed) error = "the analysis could not be saved";
      } else {
        error = "the analysis could not be saved";
      }
      if (error.empty()) {
        app.stabilize_step = 2;
        PostMessageW(window, kStabilizerAnalysed, 1, 0);
        (void)cutline::render::stabilization(wanted.key, settings);
      }
    } else {
      error = analysis.error();
    }
    {
      const std::scoped_lock lock(app.stabilize_lock);
      app.stabilize_error = std::move(error);
    }
    app.stabilize_running = false;
    PostMessageW(window, kStabilizerAnalysed, 0, 0);
  });
}

}  // namespace

bool analysing_stabilizer(const App& app) { return app.stabilize_running.load(); }

void refresh_stabilizer_analysis(App& app) {
  if (analysing_stabilizer(app)) return;
  for (const Wanted& wanted : stabilizers(app.session.project())) {
    if (cutline::render::has_analysis(wanted.key) || app.stabilize_held.contains(wanted.key)) {
      continue;
    }
    start(app, wanted);
    return;
  }
}

void collect_stabilizer_analysis(App& app, bool progress_only) {
  show_stabilizer_banner(app);
  app.inspector_stale = true;
  mark_dirty(app);
  if (progress_only || analysing_stabilizer(app)) return;
  if (app.stabilize_worker.joinable()) app.stabilize_worker.join();
  std::string error;
  {
    const std::scoped_lock lock(app.stabilize_lock);
    error = std::move(app.stabilize_error);
  }
  // One that failed, or was cancelled, is not started again by itself; the
  // Analyze button asks for it.
  if (!error.empty()) app.stabilize_held.insert(app.stabilize_key);
  if (!error.empty() && !app.stabilize_cancel.load()) {
    complain(app.main.window, "Warp Stabilizer could not analyse the clip: " + error + ".");
  }
  app.stabilize_key.clear();
  refresh_busy(app);
  invalidate_preview(app);
  refresh_stabilizer_analysis(app);
  show_stabilizer_banner(app);
}

void analyse_stabilizer_again(App& app, const std::string& clip_id, std::size_t effect) {
  const cutline::core::Project& project = app.session.project();
  const cutline::core::Clip* clip = cutline::core::find_clip(project, clip_id);
  if (clip == nullptr || effect >= clip->effects.size()) return;
  const std::string key = cutline::render::analysis_key(project, *clip, clip->effects[effect]);
  if (analysing_stabilizer(app)) {
    if (app.stabilize_key == key) return;
    app.stabilize_cancel = true;
  }
  std::error_code error;
  std::filesystem::remove(cutline::render::analysis_path(key), error);
  app.stabilize_held.erase(key);
  invalidate_preview(app);
  if (!analysing_stabilizer(app)) refresh_stabilizer_analysis(app);
  app.inspector_stale = true;
}

void cancel_stabilizer_analysis(App& app) {
  if (analysing_stabilizer(app)) app.stabilize_cancel = true;
}

std::string stabilizer_status(App& app, const std::string& clip_id, std::size_t effect) {
  const cutline::core::Project& project = app.session.project();
  const cutline::core::Clip* clip = cutline::core::find_clip(project, clip_id);
  if (clip == nullptr || effect >= clip->effects.size()) return {};
  const cutline::core::ClipEffect& fx = clip->effects[effect];
  const std::string key = cutline::render::analysis_key(project, *clip, fx);
  if (analysing_stabilizer(app) && app.stabilize_key == key) {
    if (app.stabilize_step.load() == 2) return "Stabilizing (step 2 of 2)";
    const int total = std::max(app.stabilize_total.load(), 1);
    return std::format("Analyzing in background (step 1 of 2) {}%",
                       app.stabilize_done.load() * 100 / total);
  }
  if (!cutline::render::has_analysis(key)) {
    return app.stabilize_held.contains(key) ? "Not analysed" : "Waiting to analyse";
  }
  const auto result =
      cutline::render::stabilization(key, cutline::render::stabilize_settings(fx), false);
  if (result == nullptr) return "Stabilizing (step 2 of 2)";
  if (cutline::render::stabilize_settings(fx).framing ==
      cutline::track::Framing::StabilizeCropAutoScale) {
    return std::format("Auto-scale ({:.1f}%)", result->auto_scale * 100.0);
  }
  return "Analysed";
}

void show_stabilizer_banner(App& app) {
  if (app.monitor == nullptr) return;
  // The clip under the playhead with a Warp Stabilizer still being worked on,
  // as Premiere bands the frame it is working on.
  std::string banner;
  const cutline::core::Project& project = app.session.project();
  const double t = app.session.playhead();
  for (const cutline::core::Track& track : project.sequence().tracks) {
    if (track.kind != cutline::core::TrackKind::Video) continue;
    for (const cutline::core::Clip& clip : track.clips) {
      if (t < clip.start || t >= cutline::core::clip_end(clip)) continue;
      for (std::size_t i = 0; i < clip.effects.size() && banner.empty(); ++i) {
        const cutline::core::ClipEffect& fx = clip.effects[i];
        if (!fx.enabled || fx.type != "warpstabilizer") continue;
        const auto hide = fx.params.find("hide_banner");
        if (hide != fx.params.end() && hide->second >= 0.5) continue;
        const std::string status = stabilizer_status(app, clip.id, i);
        if (status.starts_with("Analyzing") || status.starts_with("Stabilizing") ||
            status.starts_with("Waiting")) {
          banner = status.starts_with("Waiting") ? "Analyzing in background (step 1 of 2)"
                                                 : status;
        }
      }
    }
  }
  // A Morph Cut not yet analysed says the same thing over its transition.
  if (banner.empty()) banner = morph_banner(app);
  app.monitor->set_banner(std::move(banner));
}

}  // namespace cutline::tool
