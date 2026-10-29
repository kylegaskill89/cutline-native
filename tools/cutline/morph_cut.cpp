/// Premiere's Morph Cut, analysed in the background as Premiere does it.
///
/// Putting one on a cut is enough to start the looking: each Morph Cut in the
/// open sequence without an analysis is analysed, one at a time, on a thread
/// of its own, while the Program Monitor says so across the transition, the
/// way Warp Stabilizer's are. Nothing is written into the project; see
/// `render/morph_cut.hpp`. Until it is done the transition draws as a
/// dissolve.

#include "app.hpp"

#include "cutline/engine/morph_analysis.hpp"
#include "cutline/render/morph_cut.hpp"

#include <format>

namespace cutline::tool {
namespace {

/// A Morph Cut in the open sequence, and the analysis it wants.
struct Wanted {
  std::string clip_id;
  std::string key;
};

[[nodiscard]] std::vector<Wanted> morph_cuts(const cutline::core::Project& project) {
  std::vector<Wanted> out;
  for (const cutline::render::MorphJoin& join : cutline::render::morph_cuts(project)) {
    if (!join.key.empty()) out.push_back({join.out.clip->id, join.key});
  }
  return out;
}

void start(App& app, const Wanted& wanted) {
  if (app.morph_worker.joinable()) app.morph_worker.join();
  app.morph_running = true;
  app.morph_cancel = false;
  app.morph_done = 0;
  app.morph_total = 0;
  app.morph_key = wanted.key;
  app.morph_error.clear();
  refresh_busy(app);

  const cutline::core::Project project = app.session.project();
  const bool software = app.settings.software_renderer;
  HWND window = app.main.window;
  app.morph_worker = std::thread([&app, project, wanted, software, window] {
    auto device = cutline::gpu::Device::create({.force_software = software});
    auto analysis =
        device.has_value()
            ? cutline::engine::analyse_morph_cut(
                  *device, project, wanted.clip_id,
                  [&app, window, last = std::chrono::steady_clock::now()](int done,
                                                                         int total) mutable {
                    app.morph_done = done;
                    app.morph_total = total;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last > std::chrono::milliseconds(250)) {
                      last = now;
                      PostMessageW(window, kMorphAnalysed, 1, 0);
                    }
                    return !app.morph_cancel.load();
                  })
            : std::unexpected(device.error());
    std::string error;
    if (analysis.has_value()) {
      // Beside its final name and moved there whole, so a render never finds
      // half of one.
      std::error_code failed;
      const std::filesystem::path path = cutline::render::morph_analysis_path(wanted.key);
      std::filesystem::create_directories(path.parent_path(), failed);
      std::filesystem::path partial = path;
      partial += ".part";
      if (cutline::track::save_morph(*analysis, partial)) {
        std::filesystem::rename(partial, path, failed);
        if (failed) error = "the analysis could not be saved";
      } else {
        error = "the analysis could not be saved";
      }
    } else {
      error = analysis.error();
    }
    {
      const std::scoped_lock lock(app.morph_lock);
      app.morph_error = std::move(error);
    }
    app.morph_running = false;
    PostMessageW(window, kMorphAnalysed, 0, 0);
  });
}

}  // namespace

bool analysing_morph(const App& app) { return app.morph_running.load(); }

void refresh_morph_analysis(App& app) {
  if (analysing_morph(app)) return;
  for (const Wanted& wanted : morph_cuts(app.session.project())) {
    if (cutline::render::has_morph_analysis(wanted.key) || app.morph_held.contains(wanted.key)) {
      continue;
    }
    start(app, wanted);
    return;
  }
}

void collect_morph_analysis(App& app, bool progress_only) {
  show_stabilizer_banner(app);
  app.inspector_stale = true;
  mark_dirty(app);
  if (progress_only || analysing_morph(app)) return;
  if (app.morph_worker.joinable()) app.morph_worker.join();
  std::string error;
  {
    const std::scoped_lock lock(app.morph_lock);
    error = std::move(app.morph_error);
  }
  // One that failed is not started again by itself: the same cut would only
  // fail the same way. Changing the transition names a new analysis, which is
  // tried afresh.
  if (!error.empty()) app.morph_held.insert(app.morph_key);
  if (!error.empty() && !app.morph_cancel.load()) {
    complain(app.main.window, "Morph Cut could not analyse the cut: " + error + ".");
  }
  app.morph_key.clear();
  refresh_busy(app);
  invalidate_preview(app);
  refresh_morph_analysis(app);
  show_stabilizer_banner(app);
}

std::string morph_status(App& app, const std::string& clip_id) {
  const std::optional<cutline::render::MorphJoin> join =
      cutline::render::morph_cut_of(app.session.project(), clip_id);
  if (!join.has_value() || join->key.empty()) return {};
  if (analysing_morph(app) && app.morph_key == join->key) {
    const int total = std::max(app.morph_total.load(), 1);
    return std::format("Analyzing in background {}%", app.morph_done.load() * 100 / total);
  }
  if (cutline::render::has_morph_analysis(join->key)) return "Analysed";
  return app.morph_held.contains(join->key) ? "Not analysed" : "Analyzing in background";
}

std::string morph_banner(App& app) {
  // The transition under the playhead, while it is still being worked on, as
  // Premiere bands the frames it has yet to finish.
  const double t = app.session.playhead();
  for (const cutline::render::MorphJoin& join :
       cutline::render::morph_cuts(app.session.project())) {
    if (t < join.start || t >= join.end || join.key.empty()) continue;
    if (cutline::render::has_morph_analysis(join.key) || app.morph_held.contains(join.key)) {
      continue;
    }
    return "Analyzing in background";
  }
  return {};
}

void stop_morph_analysis(App& app) {
  app.morph_cancel = true;
  if (app.morph_worker.joinable()) app.morph_worker.join();
}

}  // namespace cutline::tool
