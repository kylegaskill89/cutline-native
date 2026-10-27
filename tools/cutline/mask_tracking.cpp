/// Premiere's mask tracking, as the Effect Controls panel offers it: four
/// buttons beside a mask, one frame or all the way either side, and its
/// Tracking Method.
///
/// The tracking itself is `engine::track_mask`, run on a thread of its own as
/// Render In to Out is, with a Tracking window saying how far it has got and
/// Cancel. What it found is written when it finishes, as one edit: undo takes
/// the whole run back, and the playhead is left on the last frame tracked, as
/// Premiere leaves it.

#include "app.hpp"

#include "cutline/editor/mask_tracking.hpp"
#include "cutline/engine/mask_tracker.hpp"

namespace cutline::tool {
namespace {

/// The Tracking window's bar, while that window is still the one showing.
[[nodiscard]] cutline::ui::ProgressBar* tracking_bar_of(App& app) {
  if (app.mask_track_dialog == nullptr || app.main.host == nullptr ||
      app.main.host->popup() != app.mask_track_dialog) {
    return nullptr;
  }
  return app.mask_track_bar;
}

void show_tracking_progress(App& app) {
  cutline::ui::ProgressBar* bar = tracking_bar_of(app);
  if (bar == nullptr) return;
  const int done = app.mask_track_done.load();
  const int total = std::max(app.mask_track_total.load(), 1);
  bar->set_fraction(static_cast<double>(done) / total);
  bar->set_text(std::format("Frame {} of {}", std::min(done + 1, total), total));
  mark_dirty(app);
}

}  // namespace

bool tracking_mask(const App& app) { return app.mask_track_running.load(); }

void track_mask(App& app, const std::string& clip_id, std::size_t effect, bool backward,
                bool one_frame) {
  if (tracking_mask(app)) return;
  if (app.mask_track_worker.joinable()) app.mask_track_worker.join();

  app.mask_track_running = true;
  app.mask_track_cancel = false;
  app.mask_track_done = 0;
  app.mask_track_total = 0;
  app.mask_track_clip = clip_id;
  app.mask_track_effect = effect;
  app.mask_track_result.reset();
  refresh_busy(app);

  // A single frame is over before a window could be read.
  if (!one_frame && app.main.host != nullptr) {
    auto dialog = std::make_unique<cutline::ui::Dialog>("Tracking");
    auto& bar = dialog->body().emplace<cutline::ui::ProgressBar>(0.0);
    dialog->set_cancel("Cancel", [&app] { app.mask_track_cancel = true; });
    app.mask_track_dialog = dialog.get();
    app.mask_track_bar = &bar;
    Rect anchor{};
    if (app.monitor != nullptr) anchor = app.monitor->bounds();
    app.main.host->open_popup(std::move(dialog), anchor);
    show_tracking_progress(app);
  }

  const cutline::engine::MaskTrackRequest request{.clip_id = clip_id,
                                                  .effect = effect,
                                                  .from = local_playhead_of(app, clip_id),
                                                  .backward = backward,
                                                  .one_frame = one_frame};
  const cutline::core::Project project = app.session.project();
  const bool software = app.settings.software_renderer;
  HWND window = app.main.window;
  app.mask_track_worker = std::thread([&app, project, request, software, window] {
    auto device = cutline::gpu::Device::create({.force_software = software});
    std::expected<cutline::engine::MaskTrackResult, std::string> result =
        device.has_value()
            ? cutline::engine::track_mask(
                  *device, project, request,
                  [&app, window, last = std::chrono::steady_clock::now()](int done,
                                                                         int total) mutable {
                    app.mask_track_done = done;
                    app.mask_track_total = total;
                    // A few times a second, which is as often as a bar is read.
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last > std::chrono::milliseconds(200)) {
                      last = now;
                      PostMessageW(window, kMaskTracked, 1, 0);
                    }
                    return !app.mask_track_cancel.load();
                  })
            : std::unexpected(device.error());
    {
      const std::scoped_lock lock(app.mask_track_lock);
      app.mask_track_result = std::move(result);
    }
    app.mask_track_running = false;
    PostMessageW(window, kMaskTracked, 0, 0);
  });
}

void collect_tracked_mask(App& app, bool progress_only) {
  show_tracking_progress(app);
  if (progress_only && tracking_mask(app)) return;
  if (tracking_mask(app)) return;
  if (app.mask_track_worker.joinable()) app.mask_track_worker.join();
  if (tracking_bar_of(app) != nullptr) app.main.host->close_popup();
  app.mask_track_dialog = nullptr;
  app.mask_track_bar = nullptr;
  refresh_busy(app);

  std::optional<std::expected<cutline::engine::MaskTrackResult, std::string>> result;
  {
    const std::scoped_lock lock(app.mask_track_lock);
    result = std::move(app.mask_track_result);
    app.mask_track_result.reset();
  }
  if (!result.has_value()) return;
  if (!result->has_value()) {
    complain(app.main.window, "Could not track the mask.\n\n" + result->error());
    return;
  }
  const cutline::engine::MaskTrackResult& run = **result;
  if (run.frames.size() > 1) {
    app.session.apply(cutline::editor::apply_tracked_masks(
        app.session.project(), app.mask_track_clip, app.mask_track_effect, run.frames));
    if (const cutline::core::Clip* clip =
            cutline::core::find_clip(app.session.project(), app.mask_track_clip);
        clip != nullptr) {
      app.session.set_playhead(clip->start + run.frames.back().t);
      show_playhead(app);
    }
    invalidate_preview(app);
    refresh_handles(app);
    refresh_timeline(app);
    app.inspector_stale = true;
  }
  // Cancelled is what was asked for; anything else that stopped it short is
  // worth saying, since the mask is no longer following the picture past here.
  if (!run.stopped.empty() && !app.mask_track_cancel.load()) {
    complain(app.main.window, "Tracking stopped because " + run.stopped + ".");
  }
}

}  // namespace cutline::tool
