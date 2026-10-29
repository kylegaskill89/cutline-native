/// Premiere's Make Subclip and Edit Subclip.
///
/// Make Subclip takes its span from wherever the person is: the marks in the
/// Source Monitor, the marks on an entry selected in the Project panel, or the
/// part of the footage a selected timeline clip uses. The subclip is filed
/// beside its source, as Premiere files it.

#include "app.hpp"

#include "cutline/core/interpret.hpp"
#include "cutline/core/subclips.hpp"
#include "cutline/core/time.hpp"

namespace cutline::tool {
namespace {

[[nodiscard]] const cutline::core::Media* media_named(const App& app, std::string_view id) {
  const auto& media = app.session.project().media;
  const auto it = std::ranges::find(media, id, &cutline::core::Media::id);
  return it == media.end() ? nullptr : &*it;
}

/// The rate a source's times are shown at, as the Source Monitor shows them.
[[nodiscard]] double rate_of(const cutline::core::Media& media) {
  const std::optional<double> rate = cutline::core::playback_fps(media);
  return rate.value_or(0.0) > 0.0 ? *rate : 30.0;
}

/// What Make Subclip would be made from, as things stand: the source, and the
/// span of it.
struct Wanted {
  std::string media_id;
  double start = 0.0;
  double end = 0.0;
};

[[nodiscard]] std::optional<Wanted> marked_span(const cutline::core::Media& media) {
  if (!cutline::core::can_make_subclip(media)) return std::nullopt;
  const cutline::core::SourceBounds bounds = cutline::core::source_bounds(media);
  return Wanted{.media_id = media.id,
                .start = media.in_point.value_or(bounds.lo),
                .end = media.out_point.value_or(bounds.hi)};
}

[[nodiscard]] std::optional<Wanted> wanted_subclip(const App& app) {
  if (source_has_focus(app)) {
    if (const cutline::core::Media* media = source_media_of(app); media != nullptr) {
      return marked_span(*media);
    }
  }
  if (app.browser != nullptr && app.browser->focused()) {
    if (const cutline::ui::MediaItem* item = app.browser->selected(); item != nullptr) {
      if (const cutline::core::Media* media = media_named(app, item->id); media != nullptr) {
        return marked_span(*media);
      }
    }
  }
  // A clip in the timeline: the part of the footage it plays.
  for (const std::string& id : app.session.selected_group()) {
    const cutline::core::Clip* clip = cutline::core::find_clip(app.session.project(), id);
    if (clip == nullptr) continue;
    const cutline::core::Media* media = media_named(app, clip->media_id);
    if (media == nullptr || !cutline::core::can_make_subclip(*media)) continue;
    return Wanted{.media_id = media->id,
                  .start = std::min(clip->source_in, clip->source_out),
                  .end = std::max(clip->source_in, clip->source_out)};
  }
  // Failing all of those, whatever the Source Monitor holds.
  if (const cutline::core::Media* media = source_media_of(app); media != nullptr) {
    return marked_span(*media);
  }
  return std::nullopt;
}

}  // namespace

bool can_make_subclip_now(const App& app) { return wanted_subclip(app).has_value(); }

void open_make_subclip_dialog(App& app) {
  if (app.main.host == nullptr) return;
  const std::optional<Wanted> wanted = wanted_subclip(app);
  if (!wanted.has_value()) {
    complain(app.main.window,
             "Open footage in the Source Monitor, or select a clip of some, to make a "
             "subclip of it.");
    return;
  }
  const cutline::core::Media& source = *media_named(app, wanted->media_id);

  auto dialog = std::make_unique<cutline::ui::Dialog>("Make Subclip");
  auto& name_row = dialog->row("Name");
  auto& name = name_row.emplace<TextField>(cutline::core::subclip_name(source));
  name.set_columns(28);
  // Premiere's box starts ticked: a subclip is usually made to be exactly
  // that part.
  auto& restrict_trims =
      dialog->body().emplace<Checkbox>("Restrict Trims To Subclip Boundaries", true);
  dialog->set_accept("OK", [&app, wanted = *wanted, name = &name,
                            restrict_trims = &restrict_trims]() -> std::optional<std::string> {
    if (name->text().empty()) return "The subclip needs a name.";
    const cutline::core::Media* media = media_named(app, wanted.media_id);
    if (media == nullptr) return "The footage is no longer in the project.";
    app.session.name_next_edit("Make Subclip");
    app.session.apply(cutline::core::make_subclip(app.session.project(), wanted.media_id,
                                                  name->text(), wanted.start, wanted.end,
                                                  restrict_trims->checked(), media->bin));
    refresh_browser(app);
    mark_dirty(app);
    return std::nullopt;
  });
  app.main.host->open_popup(std::move(dialog), settings_anchor());
  app.main.host->set_focus(&name);
}

void open_edit_subclip_dialog(App& app, const std::string& media_id) {
  if (app.main.host == nullptr) return;
  const cutline::core::Media* media = media_named(app, media_id);
  if (media == nullptr || !media->subclip.has_value()) return;
  const double fps = rate_of(*media);
  const bool drop = false;
  const auto timecode = [fps, drop](double seconds) {
    return cutline::core::seconds_to_timecode(seconds, fps, drop);
  };

  auto dialog = std::make_unique<cutline::ui::Dialog>("Edit Subclip");
  dialog->body().emplace<Label>("Master Clip").set_small(true);
  dialog->row("Start").emplace<Label>(timecode(0.0));
  dialog->row("End").emplace<Label>(timecode(media->duration));
  dialog->body().emplace<Label>("Subclip").set_small(true);
  auto& start = dialog->row("Start ").emplace<TextField>(timecode(media->subclip->start));
  start.set_columns(11);
  auto& end = dialog->row("End ").emplace<TextField>(timecode(media->subclip->end));
  end.set_columns(11);
  auto& hard = dialog->body().emplace<Checkbox>("Hard Boundaries", media->subclip->restrict_trims);
  auto& master = dialog->body().emplace<Checkbox>("Convert to Master Clip", false);

  dialog->set_accept("OK", [&app, media_id, fps, drop, start = &start, end = &end, hard = &hard,
                            master = &master]() -> std::optional<std::string> {
    if (master->checked()) {
      app.session.name_next_edit("Convert to Master Clip");
      app.session.apply(cutline::core::convert_to_master_clip(app.session.project(), media_id));
    } else {
      const std::optional<double> from = cutline::core::timecode_to_seconds(start->text(), fps, drop);
      const std::optional<double> to = cutline::core::timecode_to_seconds(end->text(), fps, drop);
      if (!from.has_value() || !to.has_value()) return "Start and End have to be timecodes.";
      if (*to <= *from) return "End has to come after Start.";
      app.session.name_next_edit("Edit Subclip");
      app.session.apply(cutline::core::edit_subclip(app.session.project(), media_id, *from, *to,
                                                    hard->checked()));
    }
    refresh_all(app);
    mark_dirty(app);
    return std::nullopt;
  });
  app.main.host->open_popup(std::move(dialog), settings_anchor());
}

}  // namespace cutline::tool
