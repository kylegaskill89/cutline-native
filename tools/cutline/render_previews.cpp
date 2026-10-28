/// Premiere's preview files: Render In to Out, the render bar above the
/// timeline, and playing rendered stretches from their files.
///
/// The pieces are worked out by `editor::render_pieces` from the document,
/// whenever it changes, and handed to the timeline as its bar and to the
/// program monitor's engine as the files that stand in for those stretches.
/// Rendering is an export of each piece on a thread of its own, as the render
/// queue's jobs are, written beside its final name and moved there whole.

#include "app.hpp"

#include "cutline/editor/render_previews.hpp"
#include "cutline/engine/exporter.hpp"

namespace cutline::tool {
namespace {

/// Where this project's previews go, by its Video Previews scratch disk.
[[nodiscard]] std::filesystem::path previews_of(const App& app) {
  return cutline::editor::previews_folder(app.session.project(), app.session.path());
}

using cutline::editor::RenderPiece;
using cutline::editor::RenderStatus;

[[nodiscard]] cutline::ui::RenderBarSpan::Colour colour_of(RenderStatus status) {
  switch (status) {
    case RenderStatus::Rendered: return cutline::ui::RenderBarSpan::Colour::Green;
    case RenderStatus::Heavy: return cutline::ui::RenderBarSpan::Colour::Red;
    default: return cutline::ui::RenderBarSpan::Colour::Yellow;
  }
}

/// Brings the pieces up to date with the document, when it has changed or a
/// render has finished one.
void update_pieces(App& app) {
  const std::uint64_t revision = app.session.document_revision();
  if (revision == app.render_pieces_revision && !app.render_pieces_stale) return;
  app.render_pieces_revision = revision;
  app.render_pieces_stale = false;
  app.render_pieces = cutline::editor::render_pieces(app.session.project(), previews_of(app));
}

}  // namespace

void refresh_render_bar(App& app) {
  update_pieces(app);
  if (app.timeline == nullptr) return;
  std::vector<cutline::ui::RenderBarSpan> bar;
  for (const RenderPiece& piece : app.render_pieces) {
    if (piece.status == RenderStatus::None) continue;
    bar.push_back(cutline::ui::RenderBarSpan{
        .start = piece.start, .end = piece.end, .colour = colour_of(piece.status)});
  }
  app.timeline->set_render_bar(std::move(bar));
}

std::vector<cutline::app::PreviewSpan> rendered_spans(App& app) {
  update_pieces(app);
  std::vector<cutline::app::PreviewSpan> spans;
  const std::filesystem::path folder = previews_of(app);
  for (const RenderPiece& piece : app.render_pieces) {
    if (piece.status != RenderStatus::Rendered) continue;
    spans.push_back(cutline::app::PreviewSpan{
        .start = piece.start,
        .end = piece.end,
        .path = cutline::editor::preview_file(folder, piece.key).string()});
  }
  return spans;
}

bool rendering_previews(const App& app) { return app.preview_render_running.load(); }

namespace {

/// The Rendering window's bar, while that window is still the one showing.
[[nodiscard]] cutline::ui::ProgressBar* render_bar_of(App& app) {
  if (app.preview_render_dialog == nullptr || app.main.host == nullptr ||
      app.main.host->popup() != app.preview_render_dialog) {
    return nullptr;
  }
  return app.preview_render_bar;
}

void show_render_progress(App& app) {
  cutline::ui::ProgressBar* bar = render_bar_of(app);
  if (bar == nullptr) return;
  const int done = app.preview_render_done.load();
  const int total = std::max(app.preview_render_total, 1);
  bar->set_fraction((done + std::min(app.preview_render_piece.load(), 1.0)) / total);
  bar->set_text(std::format("Piece {} of {}", std::min(done + 1, total), total));
  mark_dirty(app);
}

/// Premiere's Rendering window: how far it has got, and Cancel.
void open_render_window(App& app) {
  if (app.main.host == nullptr) return;
  auto dialog = std::make_unique<cutline::ui::Dialog>("Rendering");
  auto& bar = dialog->body().emplace<cutline::ui::ProgressBar>(0.0);
  dialog->set_accept("", {});
  dialog->set_cancel("Cancel", [&app] { cancel_preview_render(app); });
  app.preview_render_dialog = dialog.get();
  app.preview_render_bar = &bar;
  Rect anchor{};
  if (app.monitor != nullptr) anchor = app.monitor->bounds();
  app.main.host->open_popup(std::move(dialog), anchor);
  show_render_progress(app);
}

}  // namespace

void render_in_to_out(App& app) {
  if (rendering_previews(app)) return;
  if (app.preview_render_worker.joinable()) app.preview_render_worker.join();
  update_pieces(app);

  // The marked span, or the whole sequence when nothing is marked, as
  // Premiere's Render In to Out does.
  const cutline::core::Sequence& sequence = app.session.project().sequence();
  const double from = sequence.in_point.value_or(0.0);
  const double to = sequence.out_point.value_or(std::numeric_limits<double>::infinity());
  std::vector<RenderPiece> pieces = cutline::editor::pieces_to_render(app.render_pieces, from, to);
  if (pieces.empty()) return;

  app.preview_render_running = true;
  app.preview_render_cancel = false;
  app.preview_render_done = 0;
  app.preview_render_piece = 0.0;
  app.preview_render_total = static_cast<int>(pieces.size());
  refresh_busy(app);
  open_render_window(app);

  const bool software = app.settings.software_renderer;
  const cutline::core::Project project = app.session.project();
  const std::filesystem::path folder = previews_of(app);
  HWND window = app.main.window;
  app.preview_render_worker = std::thread([&app, project, pieces, folder, software, window] {
    auto device = cutline::gpu::Device::create({.force_software = software});
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    // What a render that was stopped by the application closing left behind.
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
      if (entry.path().string().ends_with(".rendering.mov")) {
        std::filesystem::remove(entry.path(), error);
      }
    }
    auto last_told = std::chrono::steady_clock::now();
    for (const RenderPiece& piece : pieces) {
      if (!device.has_value() || app.preview_render_cancel.load()) break;
      const std::filesystem::path final_path = cutline::editor::preview_file(folder, piece.key);
      std::filesystem::path partial = final_path;
      partial += ".rendering.mov";
      cutline::engine::ExportSettings settings;
      settings.path = partial.string();
      // HEVC, at a quality nothing is lost to by eye, ten bits for an HDR
      // sequence. ProRes was tried first and wrote over a gigabyte for eight
      // seconds of 4K, which is more disk than a preview is worth; the card
      // encodes and decodes this as fast as the picture needs.
      settings.codec = cutline::media::VideoCodec::Hevc;
      settings.quality = 16;
      settings.start = piece.start;
      settings.duration = piece.end - piece.start;
      settings.audio = false;
      const auto result = cutline::engine::export_project(
          *device, project, settings,
          [&app, &last_told, window](const cutline::engine::ExportProgress& progress) {
            if (progress.total > 0) {
              app.preview_render_piece =
                  static_cast<double>(progress.frame) / static_cast<double>(progress.total);
            }
            // Told a few times a second, which is as often as a bar is read.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_told > std::chrono::milliseconds(200)) {
              last_told = now;
              PostMessageW(window, kPreviewRendered, 1, 0);
            }
            return !app.preview_render_cancel.load();
          });
      if (!result.has_value() || app.preview_render_cancel.load()) {
        std::filesystem::remove(partial, error);
        if (!result.has_value()) app.preview_render_error = result.error();
        break;
      }
      std::filesystem::rename(partial, final_path, error);
      app.preview_render_piece = 0.0;
      ++app.preview_render_done;
      PostMessageW(window, kPreviewRendered, 0, 0);
    }
    app.preview_render_running = false;
    PostMessageW(window, kPreviewRendered, 0, 0);
  });
}

void cancel_preview_render(App& app) { app.preview_render_cancel = true; }

void collect_rendered_previews(App& app, bool progress_only) {
  show_render_progress(app);
  if (progress_only && rendering_previews(app)) return;
  app.render_pieces_stale = true;
  refresh_render_bar(app);
  // The monitor's snapshot is the same project, so it is given its files now
  // rather than at the next edit.
  invalidate_preview(app);
  refresh_busy(app);
  if (!rendering_previews(app)) {
    if (app.preview_render_worker.joinable()) app.preview_render_worker.join();
    if (render_bar_of(app) != nullptr) app.main.host->close_popup();
    app.preview_render_dialog = nullptr;
    app.preview_render_bar = nullptr;
    if (!app.preview_render_error.empty()) {
      complain(app.main.window, "Could not render previews.\n\n" + app.preview_render_error);
      app.preview_render_error.clear();
    }
  }
}

void delete_render_files(App& app) {
  if (rendering_previews(app)) return;
  // The monitor holds the files it is playing open, and Windows will not
  // delete an open file.
#if CUTLINE_HAVE_PREVIEW
  if (app.preview != nullptr) {
    app.preview->set_previews(nullptr, {});
    app.preview->release_sources();
  }
#endif
  cutline::editor::delete_preview_files(previews_of(app));
  app.render_pieces_stale = true;
  refresh_render_bar(app);
  invalidate_preview(app);
}

}  // namespace cutline::tool
