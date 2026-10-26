/// Premiere's trim mode: an edit point selected on the timeline and trimmed a
/// frame or five at a time, with the Program Monitor showing the outgoing frame
/// beside the incoming one.
///
/// Shift+T goes in on the cut nearest the playhead and comes out again, Ctrl+T
/// changes the kind of trim, and Ctrl and Ctrl+Shift with the arrows trim. The
/// edits are the core's ripple, roll and edge trims; this keeps the edit point,
/// the brackets on the timeline and the two pictures in step with them.

#include "app.hpp"

#include "cutline/render/plan.hpp"

namespace cutline::tool {
namespace {

using cutline::editor::TrimType;

[[nodiscard]] double frame_seconds(const App& app) {
  const double fps = app.session.project().sequence().fps;
  return 1.0 / (fps > 0.0 ? fps : 30.0);
}

[[nodiscard]] const char* name_of(TrimType type) {
  switch (type) {
    case TrimType::Roll: return "Roll";
    case TrimType::RippleOut: return "Ripple Out";
    case TrimType::RippleIn: return "Ripple In";
    case TrimType::TrimOut: return "Trim Out";
    case TrimType::TrimIn: return "Trim In";
  }
  return "Trim";
}

/// The last frame before the cut, for the left of the monitor. The incoming
/// frame is the picture itself, with the playhead on the cut.
void capture_outgoing(App& app) {
#if CUTLINE_HAVE_PREVIEW
  app.trim_outgoing.clear();
  if (app.preview == nullptr || !app.trim_point.has_value()) return;
  const double at = std::max(0.0, app.trim_point->at - frame_seconds(app));
  cutline::core::Project project = app.session.project();
  if (app.preview_scale != 1.0) project = cutline::render::scaled_canvas(project, app.preview_scale);
  auto frame = app.preview->capture(
      std::make_shared<const cutline::core::Project>(std::move(project)), at);
  if (!frame.has_value() || frame->empty()) return;
  app.trim_outgoing = std::move(frame->pixels);
  app.trim_outgoing_width = frame->width;
  app.trim_outgoing_height = frame->height;
#else
  (void)app;
#endif
}

void show_row(App& app, bool shown) {
  if (app.trim_row != nullptr) app.trim_row->set_visible(shown);
  for (Shell* shell : app.shells()) {
    if (shell->host != nullptr) shell->host->request_layout();
  }
  mark_dirty(app);
}

}  // namespace

void show_trim(App& app) {
  if (!app.trim_point.has_value()) return;
  const std::optional<cutline::editor::EditPoint> now =
      cutline::editor::refresh_edit_point(app.session.project(), *app.trim_point);
  if (!now.has_value()) {
    exit_trim_mode(app);
    return;
  }
  app.trim_point = now;

  const auto& tracks = app.session.project().sequence().tracks;
  const auto track = std::ranges::find(tracks, now->track_id, &cutline::core::Track::id);
  if (app.timeline != nullptr && track != tracks.end()) {
    const TrimType type = now->type;
    app.timeline->set_edit_mark(cutline::ui::EditMark{
        .track = static_cast<std::size_t>(track - tracks.begin()),
        .at = now->at,
        .out_side = type == TrimType::Roll || type == TrimType::RippleOut ||
                    type == TrimType::TrimOut,
        .in_side =
            type == TrimType::Roll || type == TrimType::RippleIn || type == TrimType::TrimIn,
        .ripple = type == TrimType::RippleOut || type == TrimType::RippleIn});
  }

  capture_outgoing(app);
  if (app.monitor != nullptr) {
    app.monitor->set_reference(
        app.trim_outgoing.empty()
            ? cutline::ui::ImageView{}
            : cutline::ui::ImageView{app.trim_outgoing.data(), app.trim_outgoing_width,
                                     app.trim_outgoing_height, app.trim_outgoing_width * 4});
  }
  // The playhead stays on the cut, so the picture is the incoming frame.
  if (std::abs(app.session.playhead() - now->at) > 1e-9) {
    app.session.set_playhead(now->at);
    show_playhead(app);
    invalidate_preview(app);
  }
  if (app.trim_label != nullptr) {
    // How far the cut has moved, which a ripple of the incoming side never
    // does: that one changes what follows, so it shows only its name.
    app.trim_label->set_text(
        cutline::editor::trim_moves_cut(now->type)
            ? std::format("{}  {:+.0f}", name_of(now->type),
                          std::round((now->at - app.trim_started) / frame_seconds(app)))
            : std::string(name_of(now->type)));
  }
  show_row(app, true);
}

void exit_trim_mode(App& app) {
  app.trim_point.reset();
  app.trim_outgoing.clear();
  if (app.timeline != nullptr) app.timeline->set_edit_mark(std::nullopt);
  // Back to whatever the monitor was showing beside the picture before.
  show_comparison(app);
  show_row(app, false);
}

void toggle_trim_mode(App& app) {
  if (app.trim_point.has_value()) {
    exit_trim_mode(app);
    return;
  }
  const std::optional<cutline::editor::EditPoint> point =
      cutline::editor::nearest_edit_point(app.session.project(), app.session.playhead());
  if (!point.has_value()) return;
  if (app.playing()) stop_playback(app);
  if (app.shuttling()) set_shuttle(app, 0.0);
  app.trim_point = point;
  app.trim_started = point->at;
  // On the cut, so the picture is the incoming frame.
  app.session.set_playhead(point->at);
  show_playhead(app);
  refresh_timeline(app);
  invalidate_preview(app);
  show_trim(app);
}

bool trim_by(App& app, double frames) {
  if (!app.trim_point.has_value()) return false;
  const cutline::core::Project before = app.session.project();
  cutline::editor::Trimmed done = cutline::editor::trim_edit_point(
      before, *app.trim_point, frames * frame_seconds(app));
  if (done.project == before) return true;
  app.session.name_next_edit("Trim");
  app.session.apply(std::move(done.project));
  app.session.name_next_edit({});
  app.trim_point = done.point;
  app.session.set_playhead(done.point.at);
  refresh_all(app);
  return true;
}

void toggle_trim_type(App& app) {
  if (!app.trim_point.has_value()) return;
  app.trim_point = cutline::editor::next_trim_type(*app.trim_point);
  show_trim(app);
}

}  // namespace cutline::tool
