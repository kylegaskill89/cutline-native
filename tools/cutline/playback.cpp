/// The monitors and playback: the preview engines behind the Program and
/// Source monitors, what each shows, the scopes and the meter, and the
/// transport -- play, loop, J/K/L shuttle -- that moves the playhead.

#include "app.hpp"

namespace cutline::tool {

/// Makes a monitor's render thread on first use, on the windows' own device so
/// its pictures can be handed over without leaving the card.
///
/// Null when it cannot be made, and then `failed` is set so it is not tried on
/// every frame. A machine with no device will not acquire one.
[[nodiscard]] cutline::app::PreviewEngine* engine_for(
    App& app, std::unique_ptr<cutline::app::PreviewEngine>& engine, bool& failed,
    const char* what) {
#if CUTLINE_HAVE_PREVIEW
  if (engine != nullptr) return engine.get();
  if (failed) return nullptr;

  std::shared_ptr<cutline::gpu::Device> device = app.device;
  if (device == nullptr) {
    auto made = cutline::gpu::Device::create({.allow_software = true});
    if (!made.has_value()) {
      failed = true;
      complain(app.main.window, std::string(what) + " is unavailable.\n\n" + made.error());
      return nullptr;
    }
    device = std::move(*made);
  }

  HWND window = app.main.window;
  auto made = cutline::app::PreviewEngine::create(
      std::move(device), {.on_ready = [window] { PostMessageW(window, kPreviewReady, 0, 0); }});
  if (!made.has_value()) {
    failed = true;
    complain(app.main.window, std::string(what) + " is unavailable.\n\n" + made.error());
    return nullptr;
  }
  engine = std::move(*made);
  engine->set_hdr_display(app.hdr_white);
  show_display_options(app);
  return engine.get();
#else
  (void)app;
  (void)engine;
  (void)failed;
  (void)what;
  return nullptr;
#endif
}

/// The project the program monitor shows, as one held pointer.
///
/// The same document at the same preview scale is the same pointer, which is
/// what lets the engine keep the frames it rendered ahead of the playhead. A
/// gesture in flight is a new project on every move, because it is one.
[[nodiscard]] std::shared_ptr<const cutline::core::Project> preview_project(App& app) {
  const bool gesture = app.live_project.has_value();
  if (!gesture && app.preview_snapshot != nullptr &&
      app.preview_snapshot_revision == app.session.document_revision() &&
      app.preview_snapshot_scale == app.preview_scale) {
    return app.preview_snapshot;
  }
  const cutline::core::Project& document = app.showing();
  app.preview_snapshot = std::make_shared<const cutline::core::Project>(
      app.preview_scale == 1.0 ? document
                               : cutline::render::scaled_canvas(document, app.preview_scale));
  // A gesture's project is never reused, so it must not be mistaken for the
  // document's at this revision once the gesture ends.
  app.preview_snapshot_revision = gesture ? ~std::uint64_t{0} : app.session.document_revision();
  app.preview_snapshot_scale = app.preview_scale;
  // The rendered stretches stand in for this snapshot's picture, and only for
  // it: a gesture's project is not what was rendered.
#if CUTLINE_HAVE_PREVIEW
  if (app.preview != nullptr) {
    app.preview->set_previews(app.preview_snapshot,
                              gesture ? std::vector<cutline::app::PreviewSpan>{}
                                      : rendered_spans(app));
  }
#endif
  return app.preview_snapshot;
}

/// Asks for the frame under the playhead.
///
/// Asks, and returns: the picture is rendered on the program monitor's own
/// thread and arrives through `poll_preview`. It used to be rendered here, in
/// the paint, and every seek and every cut held the window still for as long
/// as the decode took.
void refresh_preview(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.monitor == nullptr || app.preview_failed) return;

  // Nothing to show until something has been imported. Rendering an empty
  // project would replace the colour bars with a black rectangle and look
  // like a fault.
  bool has_media = false;
  for (const cutline::core::Track& track : app.session.project().sequence().tracks) {
    if (!track.clips.empty()) has_media = true;
  }
  if (!has_media) return;

  cutline::app::PreviewEngine* engine =
      engine_for(app, app.preview, app.preview_failed, "Preview");
  if (engine == nullptr) return;

  const std::shared_ptr<const cutline::core::Project> project = preview_project(app);
  app.monitor->set_canvas_size(project->sequence().canvas_w, project->sequence().canvas_h);

  // Two ways to the same picture. Sharing a device means the frame is already
  // in memory the window can draw from, so it is handed over as a texture;
  // without one the two have nothing in common and it has to come back through
  // memory, which waits — that machine was always going to be the slow one.
  if (app.shares_device() && std::getenv("CUTLINE_CPU_PREVIEW") == nullptr) {
    // A shuttle is the playhead moving too, at its own rate and either way.
    engine->want(project, app.session.playhead(), app.playing() ? 1.0 : app.shuttle);
    poll_preview(app);
    refresh_angles(app);
    return;
  }

  auto frame = engine->capture(project, app.session.playhead());
  if (!frame.has_value()) {
    app.preview_failed = true;
    complain(app.main.window, "Could not render the preview.\n\n" + frame.error());
    return;
  }
  app.preview_pixels = std::move(*frame);
  app.monitor->set_frame(cutline::ui::ImageView{.pixels = app.preview_pixels.pixels.data(),
                                                .width = app.preview_pixels.width,
                                                .height = app.preview_pixels.height});
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Puts up whatever the program monitor's thread has finished that is better
/// than what is showing. Cheap when there is nothing: one lock and a look.
void poll_preview([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.preview == nullptr || app.monitor == nullptr || app.preview_failed) return;

  auto taken = app.preview->take(app.session.playhead());
  if (!taken.has_value()) {
    app.preview_failed = true;
    complain(app.main.window, "Could not render the preview.\n\n" + taken.error());
    return;
  }
  if (taken->has_value()) {
    app.monitor->set_texture(**taken);
    app.shown_preview = **taken;
    mark_dirty(app);
  }
  poll_angles(app);

  // A source that will not open is only known once the renderer has tried,
  // and that happens on the other thread now. The pool's offline marks follow
  // it here rather than waiting for the next edit to redraw them.
  std::vector<std::string> missing = app.preview->missing_media();
  if (missing != app.preview_missing) {
    app.preview_missing = std::move(missing);
    refresh_browser(app);
  }
#endif
}

/// The part of a source the Source Monitor reaches: all of it, or a subclip's
/// span when its trims are restricted, as Premiere shows such a subclip.
[[nodiscard]] std::pair<double, double> source_reach(const cutline::core::Media& media) {
  if (cutline::core::is_still_like(media)) return {0.0, std::max(0.0, media.duration)};
  const cutline::core::SourceBounds bounds = cutline::core::source_bounds(media);
  return {bounds.lo, std::max(bounds.lo, bounds.hi)};
}

/// The source monitor's sequence: one video track holding one clip, the whole
/// of the media, at the media's own shape and rate.
///
/// A source monitor *is* a sequence of one clip, so it can be shown by handing
/// the ordinary renderer a project built on the spot. Nothing new decodes,
/// nothing new composites, and what the source monitor shows is by construction
/// what the sequence would show — which is the property that makes it worth
/// trusting when deciding what to place.
///
/// A sequence is shown the same way, as Premiere opens one in the Source
/// Monitor: it needs the sequence and everything in it, so its one-clip
/// sequence is added to a copy of the document rather than to nothing.
[[nodiscard]] cutline::core::Project source_project(const cutline::core::Project& document,
                                                   const cutline::core::Media& media) {
  cutline::core::Project project;
  if (cutline::core::is_nested_sequence(media)) {
    project = document;
    project.sequences.push_back(cutline::core::Sequence{.id = "source-monitor"});
    project.open = project.sequences.size() - 1;
  }
  project.sequence().canvas_w = std::max(1, media.width.value_or(1280));
  project.sequence().canvas_h = std::max(1, media.height.value_or(720));
  // The rate it is *played* at, so a conformed source steps and scrubs on the
  // grid it will actually be cut on rather than on the file's.
  const std::optional<double> rate = cutline::core::playback_fps(media);
  project.sequence().fps = rate.value_or(0.0) > 0.0 ? *rate : 30.0;
  if (!cutline::core::is_nested_sequence(media)) project.media = {media};

  cutline::core::Track track;
  track.id = "v1";
  track.kind = cutline::core::TrackKind::Video;

  cutline::core::Clip clip;
  clip.id = "c1";
  clip.media_id = media.id;
  clip.kind = cutline::core::TrackKind::Video;
  // The whole of it, marks and all. The marks say what will be *placed*; the
  // monitor still shows the entire source, because a mark you cannot see past
  // is one you cannot move. A restricted subclip is the exception, being the
  // part nothing outside can be reached from; it starts where it starts in
  // the source, so the sequence's time is still the source's.
  const auto [from, to] = source_reach(media);
  clip.source_in = from;
  clip.source_out = std::max(to, from + 1.0 / project.sequence().fps);
  clip.start = from;
  track.clips.push_back(std::move(clip));

  project.sequence().tracks.push_back(std::move(track));
  return project;
}

/// The media the source monitor is showing, or null when there is none.
[[nodiscard]] const cutline::core::Media* source_media_of(const App& app) {
  const std::string& id = app.session.source_media();
  if (id.empty()) return nullptr;
  const cutline::core::Project& project = app.session.project();
  const auto found = std::ranges::find(project.media, id, &cutline::core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// Whether the keyboard is somewhere inside the source monitor.
///
/// Premiere routes the transport and the marking keys to whichever monitor is
/// focused, and this is that question asked of the widget tree rather than of a
/// flag: a flag has to be cleared by everything that could take the focus away,
/// and the one that forgets is the one that leaves `I` marking the wrong thing.
[[nodiscard]] bool source_has_focus(const App& app) {
  if (app.source_panel == nullptr || app.main.host == nullptr) return false;
  for (const Widget* at = app.main.host->focused(); at != nullptr; at = at->parent()) {
    if (at == app.source_panel) return true;
  }
  return false;
}

/// Moves the source monitor's playhead, in frames of the source itself.
void step_source(App& app, double frames) {
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) return;

  const std::optional<double> rate = cutline::core::playback_fps(*media);
  const double fps = rate.value_or(0.0) > 0.0 ? *rate : 30.0;
  const double at = app.source_playhead + frames / fps;
  const auto [from, to] = source_reach(*media);
  app.source_playhead = std::clamp(cutline::core::snap_to_frame(at, fps), from, to);
  refresh_source(app);
}

/// Marks the source at its own playhead, and takes the mark away when it is
/// already there — the same rule the sequence's own `I` and `O` follow.
void mark_source(App& app, bool out_point) {
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) return;

  const std::optional<double>& already = out_point ? media->out_point : media->in_point;
  const std::optional<double> rate = cutline::core::playback_fps(*media);
  const double fps = rate.value_or(0.0) > 0.0 ? *rate : 30.0;
  const bool here =
      already.has_value() && std::abs(*already - app.source_playhead) < 0.5 / fps;

  const std::optional<double> to = here ? std::nullopt
                                        : std::optional<double>(app.source_playhead);
  app.session.apply(out_point
                        ? cutline::core::set_source_out_point(app.session.project(),
                                                              media->id, to)
                        : cutline::core::set_source_in_point(app.session.project(),
                                                             media->id, to));
  refresh_all(app);
}

/// Premiere's M with the Source Monitor in hand: a clip marker at the
/// source's playhead, or the one there taken away, as the sequence's M does.
void mark_source_marker(App& app) {
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) return;
  const double fps = cutline::core::playback_fps(*media).value_or(30.0);
  if (const cutline::core::Marker* here =
          cutline::core::source_marker_near(*media, app.source_playhead, 0.5 / fps);
      here != nullptr) {
    app.session.apply(
        cutline::core::remove_source_marker(app.session.project(), media->id, here->id));
  } else {
    app.session.apply(
        cutline::core::add_source_marker(app.session.project(), media->id, app.source_playhead));
  }
  refresh_all(app);
}

/// The source's next or previous clip marker, for Shift+M and Ctrl+Shift+M.
void jump_source_marker(App& app, bool forward) {
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) return;
  const cutline::core::Marker* to =
      forward ? cutline::core::next_source_marker(*media, app.source_playhead)
              : cutline::core::previous_source_marker(*media, app.source_playhead);
  if (to == nullptr) return;
  app.source_playhead = to->time;
  refresh_source(app);
}

/// Stops the source playing, and says so on the button.
void stop_source_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_player != nullptr) app.source_player->pause();
  if (app.source_play_button != nullptr) app.source_play_button->set_text("Play");
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Plays the source, or stops it.
void toggle_source_playback([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_playing()) {
    stop_source_playback(app);
    return;
  }

  const cutline::core::Media* media = source_media_of(app);
  const bool sequence = media != nullptr && cutline::core::is_nested_sequence(*media);
  if (media == nullptr || (media->path.empty() && !sequence)) return;
  if (app.player_failed) return;

  // One sound card, so one thing playing. Stopping the sequence rather than
  // refusing: pressing play on a source is a clear statement about which of the
  // two is being watched.
  if (app.playing()) stop_playback(app);
  if (app.shuttling()) set_shuttle(app, 0.0);
  app.source_shuttle = 0.0;

  // A sequence is heard as it stands now, so an edit inside it since the
  // player was made makes a new one.
  const std::string playing_for =
      sequence ? std::format("{}@{}", media->id, app.session.document_revision()) : media->id;
  if (app.source_player != nullptr && app.source_player_for != playing_for) {
    app.source_player.reset();
  }
  if (app.source_player == nullptr) {
    auto made = cutline::engine::Player::create(source_project(app.session.project(), *media),
                                               {.device_id = app.settings.audio_device});
    if (!made.has_value()) {
      app.player_failed = true;
      complain(app.main.window, "Playback is unavailable.\n\n" + made.error());
      return;
    }
    app.source_player = std::move(*made);
    app.source_player_for = playing_for;
  }

  app.source_player->seek(app.source_playhead);
  app.source_player->play();
  // The same reason the sequence does it: without this `Sleep(1)` in the loop
  // really sleeps about 15 ms, which is most of a frame at 60 Hz.
  timeBeginPeriod(1);
  if (app.source_play_button != nullptr) app.source_play_button->set_text("Stop");
  mark_dirty(app);
#endif
}

/// One turn of source playback. The sound card says where time is, exactly as
/// it does for the sequence, and the picture follows.
void advance_source_playback([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (!app.source_playing()) return;

  if (!app.source_player->error().empty()) {
    const std::string message = app.source_player->error();
    stop_source_playback(app);
    complain(app.main.window, "Playback stopped.\n\n" + message);
    return;
  }

  const double at = app.source_player->position();
  if (app.source_player->finished()) {
    app.source_playhead = at;
    stop_source_playback(app);
    refresh_source(app);
    return;
  }

  app.source_playhead = at;
  refresh_source(app);
#endif
}

/// Shows a source in the source monitor, remembering it as recently looked at.
///
/// The one way in, so the pool's selection, the dropdown and anything later all
/// leave the same state behind: the list in the order last looked at, and the
/// playhead back at the start when the source itself changed.
void show_source(App& app, const std::string& media_id) {
  if (media_id != app.session.source_media()) {
    const auto& media = app.session.project().media;
    const auto it = std::ranges::find(media, media_id, &cutline::core::Media::id);
    app.source_playhead = it == media.end() ? 0.0 : source_reach(*it).first;
  }
  app.session.set_source_media(media_id);

  // Newest first, and only ever *added* to the front — something already in the
  // list stays where it is.
  //
  // Reordering on every look was the first attempt and it fought the dropdown
  // it feeds: choosing the second entry moved that entry to the top, so the
  // index the control had just acted on named something else, and the list
  // settled showing the name of the source you had switched away from. A list
  // whose items move while you are choosing from them is the wrong shape for a
  // control you choose from.
  if (!media_id.empty() && std::ranges::find(app.source_recent, media_id) ==
                               app.source_recent.end()) {
    app.source_recent.insert(app.source_recent.begin(), media_id);
    // Long enough to go back and forth between a handful of takes, short enough
    // that the list is still something to glance at rather than read.
    constexpr std::size_t kRecentSources = 10;
    if (app.source_recent.size() > kRecentSources) app.source_recent.resize(kRecentSources);
  }
  refresh_source(app);
  mark_dirty(app);
}

/// Fills the dropdown from what has been looked at, dropping anything the
/// project no longer has — media can be removed, and a list naming something
/// gone would offer a source that cannot be shown.
void refresh_source_list(App& app) {
  if (app.source_choice == nullptr) return;

  const cutline::core::Project& project = app.session.project();
  std::erase_if(app.source_recent, [&](const std::string& id) {
    return std::ranges::find(project.media, id, &cutline::core::Media::id) == project.media.end();
  });

  std::vector<std::string> names;
  names.reserve(app.source_recent.size());
  for (const std::string& id : app.source_recent) {
    const auto found = std::ranges::find(project.media, id, &cutline::core::Media::id);
    names.push_back(found == project.media.end() ? id : found->name);
  }
  if (names.empty()) names.emplace_back("No source");

  if (names != app.source_choice->options()) app.source_choice->set_options(std::move(names));

  const auto at = std::ranges::find(app.source_recent, app.session.source_media());
  app.source_choice->set_selected(at == app.source_recent.end()
                                      ? 0
                                      : static_cast<std::size_t>(at - app.source_recent.begin()));
}

/// Renders the source monitor, and keeps its scrub bar in step.
void refresh_source([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_monitor == nullptr) return;

  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr || cutline::core::is_generated_media(*media) ||
      (media->path.empty() && !cutline::core::is_nested_sequence(*media))) {
    // Nothing chosen, or something with no file behind it. The panel keeps its
    // placeholder rather than showing the last source, which would be a picture
    // of something nobody is pointing at.
    app.source_monitor->clear_frame();
    app.shown_source = {};
    if (app.source_waveform != nullptr) app.source_waveform->set_visible(false);
    if (app.source_scrub != nullptr) {
      app.source_scrub->set_duration(0.0);
      app.source_scrub->set_marks(std::nullopt, std::nullopt);
      app.source_scrub->set_markers({});
    }
    refresh_source_list(app);
    app.source_snapshot.reset();
    return;
  }

  refresh_source_list(app);
  if (app.source_scrub != nullptr) {
    const auto [from, to] = source_reach(*media);
    app.source_playhead = std::clamp(app.source_playhead, from, to);
    app.source_scrub->set_span(from, to);
    app.source_scrub->set_marks(media->in_point, media->out_point);
    std::vector<cutline::ui::ScrubBar::Marker> ticks;
    for (const cutline::core::Marker& marker : media->markers) {
      ticks.push_back({.time = marker.time, .color = marker.color});
    }
    app.source_scrub->set_markers(std::move(ticks));
    app.source_scrub->set_playhead(app.source_playhead);
  }

  // A source with no picture is shown as its shape instead. Which of the two
  // widgets is in the layout at all is decided here, so the panel never holds
  // an empty rectangle where the other one would be.
  const bool has_picture = media->has_video;
  if (app.source_waveform != nullptr) {
    app.source_waveform->set_visible(!has_picture);
    if (!has_picture) {
      app.waveforms.request(media->id, media->path, 0);
      app.source_waveform->set_waveform(app.waveforms.find(media->id, 0));
      app.source_waveform->set_duration(media->duration);
      app.source_waveform->set_playhead(app.source_playhead);
    }
  }
  app.source_monitor->set_visible(has_picture);
  if (!has_picture) {
    app.source_monitor->clear_frame();
    app.shown_source = {};
    mark_dirty(app);
    return;
  }

  cutline::app::PreviewEngine* engine =
      engine_for(app, app.source_preview, app.source_failed, "The source monitor");
  if (engine == nullptr) return;

  // The same one-clip sequence for as long as the source is the same, so the
  // engine can render ahead of a playing source rather than starting over at
  // every frame. Compared by value, since the media itself can change — a
  // relink, a conform — and that is a different picture.
  cutline::core::Project wanted = source_project(app.session.project(), *media);
  if (app.source_snapshot == nullptr || *app.source_snapshot != wanted) {
    app.source_snapshot = std::make_shared<const cutline::core::Project>(std::move(wanted));
  }
  const cutline::core::Project& project = *app.source_snapshot;
  app.source_monitor->set_canvas_size(project.sequence().canvas_w, project.sequence().canvas_h);

  const auto [from, to] = source_reach(*media);
  const double at = std::clamp(app.source_playhead, from, to);
  if (app.shares_device()) {
    // A shuttle renders ahead in its own direction, as the program monitor's does.
    engine->want(app.source_snapshot, at,
                 app.source_playing() ? 1.0 : app.source_shuttle);
    poll_source(app);
    return;
  }

  auto frame = engine->capture(app.source_snapshot, at);
  if (!frame.has_value()) {
    app.source_failed = true;
    complain(app.main.window, "Could not render the source.\n\n" + frame.error());
    return;
  }
  app.source_pixels = std::move(*frame);
  app.source_monitor->set_frame(cutline::ui::ImageView{.pixels = app.source_pixels.pixels.data(),
                                                       .width = app.source_pixels.width,
                                                       .height = app.source_pixels.height});
  mark_dirty(app);
#endif
}

/// The source monitor's `poll_preview`.
void poll_source([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.source_preview == nullptr || app.source_monitor == nullptr || app.source_failed) return;
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr || !media->has_video) return;

  const auto [from, to] = source_reach(*media);
  const double at = std::clamp(app.source_playhead, from, to);
  auto taken = app.source_preview->take(at);
  if (!taken.has_value()) {
    app.source_failed = true;
    complain(app.main.window, "Could not render the source.\n\n" + taken.error());
    return;
  }
  if (taken->has_value()) {
    app.source_monitor->set_texture(**taken);
    app.shown_source = **taken;
    mark_dirty(app);
  }
#endif
}

/// Writes the frame at the playhead to a PNG.
///
/// Rendered again rather than read back off the monitor: what is on screen is
/// letterboxed into whatever the panel happens to be, and a snapshot is a frame
/// of the *sequence* at its own size. This is the same call export makes per
/// frame, so the file is what the exported movie would contain.
void take_snapshot([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.preview == nullptr) {
    complain(app.main.window, "There is no preview to take a snapshot of yet.");
    return;
  }

  // The sequence's own size, whatever the preview is set to — a snapshot is a
  // frame of the cut, not of the proxy of it that is being scrubbed. The
  // monitor no longer has to let go of anything first: what it shows is a
  // copy the render thread cannot touch, whatever size it renders at next.
  const auto frame = app.preview->capture(
      std::make_shared<const cutline::core::Project>(app.session.project()),
      app.session.playhead());
  invalidate_preview(app);
  if (!frame.has_value()) {
    complain(app.main.window, "Could not render the frame.\n\n" + frame.error());
    return;
  }
  if (frame->empty()) {
    complain(app.main.window, "There is nothing at the playhead to save.");
    return;
  }

  // Asked for after the frame is in hand. A dialog that opens and then reports
  // that there was nothing to save wastes the answer it just collected.
  const auto path = choose_image_file(app.main.window);
  if (!path.has_value()) return;

  // Through Skia, which this already links for the window's surface. The
  // alternative was the uncompressed writer `render_frame` carries for its own
  // use, and a 4K snapshot stored raw is thirty megabytes.
  const SkImageInfo info = SkImageInfo::Make(frame->width, frame->height,
                                             kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  const SkPixmap pixels(info, frame->pixels.data(), static_cast<std::size_t>(frame->width) * 4);

  SkDynamicMemoryWStream out;
  if (!SkPngEncoder::Encode(&out, pixels, SkPngEncoder::Options{})) {
    complain(app.main.window, "Could not encode the snapshot.");
    return;
  }

  const sk_sp<SkData> encoded = out.detachAsData();
  std::ofstream file(*path, std::ios::binary);
  if (!file) {
    complain(app.main.window, "Could not write " + path->string());
    return;
  }
  file.write(static_cast<const char*>(encoded->data()),
             static_cast<std::streamsize>(encoded->size()));
  if (!file) complain(app.main.window, "Could not write " + path->string());
#else
  complain(app.main.window, "This build has no preview to take a snapshot of.");
#endif
}

/// How wide a frame is measured at.
///
/// A scope is a statistical picture, and a couple of hundred columns tell the
/// same story as four thousand for a fortieth of the work — which matters when
/// it happens every time the playhead moves. It is also about the width the
/// panel is drawn at, so the waveform is not being asked to describe detail
/// finer than a pixel of graph.
constexpr int kScopeWidth = 320;

/// A sequence size in words: the preset's name when it is one of them, and the
/// plain numbers when it is not. A sequence somebody typed a size into should
/// show that size rather than the word "Custom", which says nothing.
[[nodiscard]] std::string canvas_label(const cutline::core::Project& project) {
  for (const CanvasPreset& preset : kCanvasPresets) {
    if (preset.width == project.sequence().canvas_w && preset.height == project.sequence().canvas_h) {
      return std::string(preset.name);
    }
  }
  return std::to_string(project.sequence().canvas_w) + "x" + std::to_string(project.sequence().canvas_h);
}

/// What a monitor zoom level is called. Fit is a word rather than a percentage
/// because it is not one: it is whatever percentage the panel happens to be.
[[nodiscard]] std::string zoom_label(double zoom) {
  if (zoom <= cutline::ui::kMonitorFit) return "Fit";
  return std::to_string(std::lround(zoom * 100.0)) + "%";
}

/// The zoom dropdown that goes under a monitor's picture.
///
/// Both monitors get one and they are independent, so this takes the level and
/// what to do with a new one rather than reaching for either monitor by name.
void add_zoom_control(Box& transport, MonitorView& picture, double chosen,
                      std::function<void(double)> on_choose) {
  std::vector<std::string> names;
  std::size_t at = 0;
  for (std::size_t i = 0; i < cutline::ui::kMonitorZooms.size(); ++i) {
    if (cutline::ui::kMonitorZooms[i] == chosen) at = i;
    names.push_back(zoom_label(cutline::ui::kMonitorZooms[i]));
  }

  auto& zoom = transport.emplace<Dropdown>(std::move(names), at);
  zoom.set_tooltip("How far into the picture to look. Drag with the middle button, or scroll, to pan");
  zoom.set_on_change([on_choose = std::move(on_choose)](std::size_t index) {
    if (index < cutline::ui::kMonitorZooms.size()) on_choose(cutline::ui::kMonitorZooms[index]);
  });

  // Pushed rather than left to the widget's default, because the panel is
  // rebuilt by a rearrangement and the level is remembered by the application.
  picture.set_zoom(chosen);
}

/// Resizes the sequence and brings everything that depends on its size along.
///
/// The preview's renderer is sized from the project on every frame, so it
/// follows without being told; what does not is the monitor's letterbox while
/// there is no picture yet, and the export dialog's "same as sequence", which
/// is showing a number that has just changed.
void apply_canvas(App& app, int width, int height) {
  if (!app.session.apply(cutline::core::set_canvas(app.session.project(), width, height))) {
    return;
  }
  if (app.monitor != nullptr) {
    const cutline::core::Project& project = app.session.project();
    app.monitor->set_canvas_size(project.sequence().canvas_w, project.sequence().canvas_h);
  }
  refresh_title(app);
  invalidate_preview(app);
  app.inspector_stale = true;
}

/// Sets the sequence's frame rate and tells what depends on it.
void apply_fps(App& app, double fps) {
  if (!app.session.apply(cutline::core::set_fps(app.session.project(), fps))) return;
  refresh_timeline(app);
  refresh_title(app);
  invalidate_preview(app);
  // The page that changed it shows what the rate makes possible: drop-frame
  // appears at 29.97 and vanishes at 30, and nothing else would rebuild it.
  if (app.settings_window != nullptr && app.settings_kind == SettingsKind::Project) {
    app.settings_page_stale = true;
  }
}

/// A frame rate written the way a rate is written: 23.976 keeps its decimals
/// and 30 does not get three zeroes it never had.
[[nodiscard]] std::string fps_label(double fps) {
  const std::string text = std::format("{:.3f}", fps);
  const std::size_t last = text.find_last_not_of('0');
  const std::size_t cut = text[last] == '.' ? last : last + 1;
  return text.substr(0, cut);
}

void choose_preview_scale(App& app, double scale) {
  if (app.preview_scale == scale) return;
  app.preview_scale = scale;
  save_settings(app);
  // Kept in step for the callers that are not the control itself — the
  // dropdown has already moved when it is the one asking, and setting it back
  // to where it is costs nothing.
  if (app.preview_scale_choice != nullptr) {
    using Entry = std::pair<double, std::string_view>;
    const auto at = std::ranges::find(kPreviewScales, scale, &Entry::first);
    if (at != kPreviewScales.end()) {
      app.preview_scale_choice->set_selected(
          static_cast<std::size_t>(at - kPreviewScales.begin()));
    }
  }
  invalidate_preview(app);
}

/// The reading under the master fader.
///
/// Silence rather than "-36 dB" at the bottom stop: that is what the scale's
/// floor means everywhere else in the application, and a number there would
/// suggest the mix was merely quiet.
void show_master_gain(App& app, double gain) {
  if (app.master_reading == nullptr) return;
  const double db = cutline::ui::gain_to_fader_db(gain);
  app.master_reading->set_text(db <= cutline::ui::kGainFloorDb
                                   ? std::string("Silent")
                                   : std::format("{:+.1f} dB", db));
}

/// Copies the player's levels into the meter, once a frame.
///
/// Polled rather than pushed: levels are produced every few milliseconds on the
/// render thread and the meter is drawn at frame rate, so a message per block
/// would be several wake-ups for each thing drawn. A meter is also the one
/// display where a missed update is invisible — the next one is milliseconds
/// away and the ballistics carry across it.
void refresh_meter([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  // A player that has not been made yet reads as silence, which is true: there
  // is nothing playing.
  if (app.meter != nullptr) {
    app.meter->set_levels(app.player == nullptr ? cutline::audio::MeterReading{}
                                                : app.player->levels());
  }

  // And every strip's own meter. Polled here rather than pushed from the mixer
  // for the same reason the master's is: this runs at frame rate, a level is a
  // few dozen bytes, and a display where a missed update is invisible does not
  // need a notification.
  for (const App::TrackMeter& strip : app.track_meters) {
    if (strip.view == nullptr) continue;
    strip.view->set_levels(app.player == nullptr
                               ? cutline::audio::MeterReading{}
                               : app.player->track_levels(static_cast<int>(strip.index)));
  }
#endif
}

/// Measures the frame at the playhead, if the scopes are on show and stale.
///
/// Through `frame_at` rather than off the monitor's texture: a scope measures
/// the *frame*, and what the monitor holds may be a texture on the graphics
/// card that would have to come down anyway. Downscaled by sampling rather than
/// averaging — a scope counts pixels that exist, and an averaged pixel is one
/// the frame does not contain.
void refresh_scopes([[maybe_unused]] App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.scopes == nullptr || !app.scopes_stale) return;
  app.scopes_stale = false;

  if (app.preview == nullptr) return;

  // At the preview's scale, which is the picture actually on screen — and the
  // size the preview is already built for.
  //
  // It used to ask for the sequence's own size. That was wrong twice over: a
  // scope is a measurement of what is being looked at, and asking for a
  // different size than the monitor just asked for made every frame rebuild
  // the compositor, then rebuild it again for the next picture. The rebuild
  // freed the texture the monitor was showing, which is a crash rather than a
  // slow scope.
  // An HDR sequence is measured as its signal, in its own PQ or HLG code
  // values, rather than as the tone-mapped picture an SDR display is shown.
  const cutline::core::WorkingColour colour = app.session.project().sequence().colour;
  const auto frame = app.preview->capture(preview_project(app), app.session.playhead(),
                                          static_cast<cutline::gpu::WorkingColour>(colour));
  if (!frame.has_value() || frame->empty()) return;

  const int step = std::max(1, frame->width / kScopeWidth);
  const int width = std::max(1, frame->width / step);
  const int height = std::max(1, frame->height / step);

  // Not `small`: <rpcndr.h>, which arrives with windows.h, defines that as
  // `char`, and the error it produces names neither.
  std::vector<std::uint8_t> sampled(static_cast<std::size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    const std::uint8_t* row = frame->pixels.data() +
                              static_cast<std::ptrdiff_t>(y) * step * frame->width * 4;
    for (int x = 0; x < width; ++x) {
      const std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * step * 4;
      const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4;
      sampled[at] = pixel[0];
      sampled[at + 1] = pixel[1];
      sampled[at + 2] = pixel[2];
      sampled[at + 3] = pixel[3];
    }
  }

  const cutline::render::ScopeImage image{
      .pixels = sampled.data(), .width = width, .height = height};

  auto readings = std::make_shared<cutline::ui::ScopeReadings>();
  readings->histogram = cutline::render::compute_histogram(image);
  readings->waveform =
      cutline::render::compute_waveform(image, cutline::render::ScopeChannel::Luma);
  readings->parade = cutline::render::compute_parade(image);
  readings->vectorscope = cutline::render::compute_vectorscope(image);
  readings->measured = true;
  readings->signal = colour == cutline::core::WorkingColour::Rec2100Pq    ? cutline::ui::ScopeSignal::Pq
                     : colour == cutline::core::WorkingColour::Rec2100Hlg ? cutline::ui::ScopeSignal::Hlg
                                                                          : cutline::ui::ScopeSignal::Sdr;

  app.scope_readings = std::move(readings);
  app.scopes->set_readings(app.scope_readings);
  mark_dirty(app);
#endif
}

/// Says the measurements no longer describe what is on screen.
void invalidate_scopes(App& app) {
  app.scopes_stale = true;
  mark_dirty(app);
}

/// Shows one of the four, and lights its tab.
void choose_scope(App& app, cutline::ui::ScopeKind kind) {
  app.scope_kind = kind;
  if (app.scopes != nullptr) app.scopes->set_kind(kind);

  // Relabelled here rather than by rebuilding the row, which would destroy the
  // button whose click is still running — the same trap the theme switcher has.
  if (app.scope_button != nullptr) {
    app.scope_button->set_text(std::string(cutline::ui::to_string(kind)));
  }
  mark_dirty(app);
}

/// Marks the picture as no longer matching the playhead or the project.
void invalidate_preview(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // The gesture's project outlives the gesture only by accident, and an
  // accident here is invisible: the preview goes on rendering a project the
  // document no longer has, and looks merely out of date. Anything asking for
  // a fresh picture while no handle is being dragged is asking for the
  // document's.
  if (!app.dragging_handle && !app.live_gesture && !app.typing_on_picture) {
    app.live_project.reset();
  }

  app.preview_stale = true;
  // The scopes measure the picture, so anything that changes the picture
  // changes them. Here rather than at every call site, because every one of
  // those already says "the picture is out of date" and would otherwise have to
  // remember to say it twice.
  app.scopes_stale = true;
  mark_dirty(app);
#else
  (void)app;
#endif
}

// ---------------------------------------------------------------- playback --

/// Stops playback and puts the transport back the way it looks when idle.
///
/// Separate from pausing because it is also what happens at the end of the
/// timeline and when the document changes underneath a playing project.
void stop_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // Cleared before the early return, so a player that has gone away cannot
  // leave the application believing playback is still wanted.
  app.playback_wanted = false;
  app.playhead_from_playback = -1.0;
  if (app.player == nullptr) return;
  if (app.player->playing()) {
    app.player->pause();
    // Windows' scheduler tick goes back to its lazy default. Asking for a
    // millisecond costs power across the whole machine, so it is held only
    // while something is actually being paced by it.
    timeEndPeriod(1);
  }
  if (app.play_button != nullptr) app.play_button->set_text("Play");
  app.shown_frame = -1;
  // Once more with the sound stopped, or the bars stay where the last thing
  // playing left them and the meter goes on claiming there is audio.
  refresh_meter(app);

  // The pass ends when the sequence does. Applied here rather than as it was
  // recorded, so riding a fader through a passage is one edit and one undo
  // rather than one per displayed frame.
  commit_automation(app);
  end_multicam_pass(app);
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Rebuilds the player for a document that changed under it, and carries on.
///
/// A player holds a plan of the sequence and a decode of the sound it needs, so
/// an edit leaves it playing something the document no longer describes. It
/// used to stop, which was honest and is not what an editor should do: cutting
/// while watching is most of what cutting *is*, and a sequence that halted
/// every time it was touched had to be started again by hand after every cut,
/// every level, every trim.
///
/// So it is rebuilt and resumed from where it had got to. That costs about
/// 25 ms — a gap rather than a stop — and it is flat in the size of the cut,
/// because building a player now decodes the audio at the playhead rather than
/// all of it. Before that it would have been a third of a second at sixteen
/// cuts and worse after, which is why stopping was the right answer then.
///
/// Seamless would be better still, and means replanning the mixer in place
/// while its device keeps running rather than building another one. That is a
/// change to something the audio thread is reading, and it deserves to be done
/// on its own rather than folded in here.
///
/// Nothing is resumed if it was not playing: an edit made while stopped has
/// nothing to carry on.
void rebuild_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.player == nullptr) return;
  if (!app.playing()) {
    invalidate_playback(app);
    return;
  }

  // Where the *sound* has got to, which is where time is during playback. The
  // session's playhead follows it a frame behind.
  const double at = app.player->position();

  // Dropped before the replacement is built, so two players never hold the
  // output device at once.
  app.player.reset();

  auto made = cutline::engine::Player::create(
      app.session.project(),
      {.start_at = at, .device_id = app.settings.audio_device});
  if (!made.has_value()) {
    app.player_failed = true;
    stop_playback(app);
    complain(app.main.window, "Playback stopped.\n\n" + made.error());
    return;
  }

  app.player = std::move(*made);
  app.player_project = app.session.project();
  app.player_revision = app.session.revision();
  app.player->seek(at);
  app.player->play();
  app.playhead_from_playback = app.session.playhead();
  // The picture is drawn from whichever frame the playhead names, and the one
  // on screen is from the document as it was. Forgetting it is what makes the
  // next turn draw the edit rather than leave the old frame up until the
  // playhead happens to cross into another.
  app.shown_frame = -1;
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Forgets the player, because what it decoded no longer matches the document.
void invalidate_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (app.player == nullptr) return;
  stop_playback(app);
  app.player.reset();
#else
  (void)app;
#endif
}

void toggle_looping(App& app) {
  app.looping = !app.looping;
  save_settings(app);
  if (app.loop_button != nullptr) app.loop_button->set_selected(app.looping);
  mark_dirty(app);
}

/// The speeds J and L step through, in each direction.
///
/// Premiere's: press again and it goes up. Four is as fast as a picture is
/// still worth looking at — past that the frames a decoder can produce are so
/// far apart that scrubbing the ruler is quicker.
constexpr std::array kShuttleRates{1.0, 2.0, 4.0};

/// The next speed in `direction` from whatever is running now.
///
/// Changing direction starts again at 1 rather than counting down through the
/// speeds: pressing J while running forwards means "go back", not "go forwards
/// a bit less".
[[nodiscard]] double next_shuttle(double current, double direction) noexcept {
  if (current == 0.0 || (current > 0.0) != (direction > 0.0)) return direction;
  const double speed = std::abs(current);
  for (const double step : kShuttleRates) {
    if (step > speed + 1e-9) return direction * step;
  }
  return current;
}

void set_shuttle(App& app, double rate) {
#if CUTLINE_HAVE_PREVIEW
  // Ordinary playback is a shuttle of exactly one: it is the only rate the
  // sound card can play, so it is the only one that gets sound. Everything
  // else moves the picture and stays silent — which is what J and L are for,
  // since what they are used for is finding a moment by eye.
  if (rate == 1.0) {
    app.shuttle = 0.0;
    if (!app.playing()) toggle_playback(app);
    return;
  }

  if (app.playing()) stop_playback(app);
  if (rate != 0.0 && app.source_shuttling()) set_source_shuttle(app, 0.0);
  app.shuttle = rate;
  app.shuttled_at = std::chrono::steady_clock::now();
  app.shuttle_at = app.session.playhead();
  // So the first turn draws rather than deciding it is on the frame already
  // showing and sleeping instead.
  app.shown_frame = -1;
  if (app.play_button != nullptr) {
    app.play_button->set_text(app.shuttling() ? "Stop" : "Play");
  }
  mark_dirty(app);
#else
  (void)app;
  (void)rate;
#endif
}

/// One turn of a shuttle: moves the playhead by however much time has really
/// passed, times the rate.
///
/// By elapsed time rather than a fixed step per turn, because the loop's turn
/// rate depends on how long a frame took to decode — and a shuttle that ran
/// slower on heavy footage would be a shuttle nobody could aim with.
void set_source_shuttle(App& app, double rate) {
#if CUTLINE_HAVE_PREVIEW
  // As the program monitor's: one is ordinary playback, with its sound, and
  // every other rate is silent.
  if (rate == 1.0) {
    app.source_shuttle = 0.0;
    if (!app.source_playing()) toggle_source_playback(app);
    return;
  }
  if (app.source_playing()) stop_source_playback(app);
  // One transport moving at a time, as one sound card plays one thing.
  if (rate != 0.0 && app.playing()) stop_playback(app);
  if (rate != 0.0 && app.shuttling()) set_shuttle(app, 0.0);
  app.source_shuttle = rate;
  app.source_shuttled_at = std::chrono::steady_clock::now();
  app.source_shuttle_at = app.source_playhead;
  if (app.source_play_button != nullptr) {
    app.source_play_button->set_text(app.source_shuttling() ? "Stop" : "Play");
  }
  mark_dirty(app);
#else
  (void)app;
  (void)rate;
#endif
}

void advance_source_shuttle(App& app) {
  if (!app.source_shuttling()) return;
  const cutline::core::Media* media = source_media_of(app);
  if (media == nullptr) {
    set_source_shuttle(app, 0.0);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  // Capped for the reason the program monitor's is: one slow turn must not
  // make the next leap further.
  constexpr double kMaxShuttleStep = 0.1;
  const double elapsed =
      std::min(std::chrono::duration<double>(now - app.source_shuttled_at).count(),
               kMaxShuttleStep);
  app.source_shuttled_at = now;

  const std::optional<double> rate = cutline::core::playback_fps(*media);
  const double fps = rate.value_or(0.0) > 0.0 ? *rate : 30.0;
  // Stopping on the last frame rather than on the end itself, which is the
  // moment after it and has no picture.
  const auto [first, end] = source_reach(*media);
  const double last = std::max(first, end - 1.0 / fps);
  app.source_shuttle_at += elapsed * app.source_shuttle;
  const bool past_the_end = app.source_shuttle_at <= first || app.source_shuttle_at >= last;
  app.source_shuttle_at = std::clamp(app.source_shuttle_at, first, last);

  // Once per frame of the source, so the loop is not rendering pictures
  // nobody sees between them.
  const double at = std::round(app.source_shuttle_at * fps) / fps;
  if (past_the_end) set_source_shuttle(app, 0.0);
  if (std::abs(at - app.source_playhead) < 0.5 / fps && !past_the_end) {
    Sleep(1);
    return;
  }
  app.source_playhead = at;
  refresh_source(app);
}

void advance_shuttle(App& app) {
  if (!app.shuttling()) return;

  const auto now = std::chrono::steady_clock::now();
  // Capped, and the cap is what stops one slow turn making the next one worse.
  //
  // A shuttle that advances by however long the last turn took will, after a
  // hitch of a quarter of a second, jump fifteen frames of a 60fps source — and
  // fifteen frames is half the run of decoded frames the renderer keeps, so the
  // next stretch has half as many turns to read ahead in, and stalls sooner.
  // Driven, that showed as hitches arriving four seconds apart and then bunching
  // up: 10832, 11396, 11881, 12941, 13493 milliseconds, closing in.
  //
  // Nothing is out of sync as a result. J and L are a *silent* shuttle — the
  // sound card only plays at its own rate — so there is no clock this has to
  // agree with, and playing a moment slow after a hitch is invisible where the
  // hitch itself is not.
  constexpr double kMaxShuttleStep = 0.1;
  const double elapsed =
      std::min(std::chrono::duration<double>(now - app.shuttled_at).count(), kMaxShuttleStep);
  app.shuttled_at = now;

  const double duration = cutline::core::timeline_duration(app.session.project());
  app.shuttle_at += elapsed * app.shuttle;

  // Stopping at the ends rather than wrapping, unless looping is on — the same
  // rule playback follows, and the same answer to "what does the end mean".
  const bool past_the_end = app.shuttle_at <= 0.0 ||
                            (duration > 0.0 && app.shuttle_at >= duration);
  if (past_the_end && app.looping) {
    app.shuttle_at = app.shuttle > 0.0 ? 0.0 : std::max(0.0, duration);
  }
  app.shuttle_at = std::clamp(app.shuttle_at, 0.0, std::max(0.0, duration));
  app.session.set_playhead(app.shuttle_at);
  if (past_the_end && !app.looping) set_shuttle(app, 0.0);

  // Once per frame of the sequence, the same rule playback follows: the
  // playhead moves continuously and the picture does not have to. Without this
  // a shuttle rebuilt the timeline and asked for a render as fast as the loop
  // could turn, which is a busy core for pictures nobody sees.
  const double fps = app.session.project().sequence().fps > 0.0 ? app.session.project().sequence().fps : 30.0;
  // Rounded, not truncated: the playhead is already on the frame grid, and
  // frame k times the rate can come out a hair under k, which truncation
  // turns into the frame before -- one already shown, so this one was never
  // drawn.
  const auto frame = std::llround(app.session.playhead() * fps);
  if (frame == app.shown_frame) {
    Sleep(1);
    return;
  }
  app.shown_frame = frame;

  show_playhead(app);
  follow_playhead(app);
  refresh_timeline(app);
  scroll_to_playhead(app);
  invalidate_preview(app);
}

void toggle_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  // A shuttle is playback of a sort, so the space bar stops it.
  if (app.shuttling()) {
    set_shuttle(app, 0.0);
    return;
  }
  if (app.playing()) {
    stop_playback(app);
    return;
  }
  if (app.player_failed) return;

  // A player that decoded a different project is no use. Dropped here rather
  // than at every edit, so there is one place that decides and it is the place
  // that knows what the player was made from.
  if (app.player != nullptr && app.player_project != app.session.project()) {
    app.player.reset();
  }

  if (app.player == nullptr) {
    // Told where it is about to start, so only the audio that is about to be
    // heard is decoded before this returns. Without it, building a player read
    // every clip in the sequence — which is what made a press of Play cost
    // longer the further into the cut somebody got.
    auto made = cutline::engine::Player::create(
        app.session.project(),
        {.start_at = app.session.playhead(), .device_id = app.settings.audio_device});
    if (!made.has_value()) {
      // Once and no more: a machine with no output device will not acquire one,
      // and a dialog on every press of Play would be worse than the silence.
      app.player_failed = true;
      complain(app.main.window, "Playback is unavailable.\n\n" + made.error());
      return;
    }
    app.player = std::move(*made);
    app.player_project = app.session.project();
  }
  app.player_revision = app.session.revision();

  // From wherever the playhead is, not from wherever playback last stopped —
  // the user may have scrubbed since.
  app.player->seek(app.session.playhead());
  app.player->play();
  app.playback_wanted = true;
  // Armed here, so the first turn compares against where playback was asked to
  // begin rather than against whatever the last run left behind.
  app.playhead_from_playback = app.session.playhead();
  // Pressing play is a statement about what is being watched, so the view goes
  // back to following the playhead even if it was scrolled somewhere else while
  // the last run was going.
  if (app.timeline != nullptr) app.timeline->follow_playhead_again();
  // Without this, `Sleep(1)` in the playback loop really sleeps about 15 ms,
  // which is most of a frame at 60 Hz.
  timeBeginPeriod(1);

  if (app.play_button != nullptr) app.play_button->set_text("Pause");
  app.shown_frame = -1;
  mark_dirty(app);
#else
  (void)app;
#endif
}

/// Everything a new frame of playback costs the interface, short of painting.
///
/// Apart from `advance_playback` so `--benchmark` can time exactly this: the
/// part of a playback turn that grows with the length of the timeline.
void show_playback_frame(App& app, double at) {
  app.session.set_playhead(at);
  // Read back rather than stored from `at`: the setter snaps to the frame grid,
  // and comparing next turn against a value that was never written would make
  // every turn look like somebody had moved the playhead.
  app.playhead_from_playback = app.session.playhead();
  show_playhead(app);
  follow_playhead(app);
  refresh_timeline(app);
  // After the refresh, which is what puts the new time on the view.
  scroll_to_playhead(app);
  invalidate_preview(app);
}

/// One turn of playback: follows the sound card and redraws when the frame it
/// is pointing at changes.
///
/// The playhead is *read*, never advanced here. The sound card decides where
/// time is, because it is the one that cannot be nudged — a repeated or dropped
/// picture frame goes unnoticed, while a gap of the same length in audio is a
/// click. So the picture is what adapts.
void advance_playback(App& app) {
#if CUTLINE_HAVE_PREVIEW
  if (!app.playback_live()) return;

  if (!app.player->error().empty()) {
    const std::string message = app.player->error();
    stop_playback(app);
    complain(app.main.window, "Playback stopped.\n\n" + message);
    return;
  }

  // An edit while the sound is running leaves the player playing audio the
  // document no longer describes. Stopping is the honest answer: it decoded
  // what it needs up front, so there is nothing to update in place.
  //
  // Guarded by the revision so the project comparison happens when something
  // changed rather than thirty times a second. The revision also moves for a
  // change of selection, which is why the projects are then compared rather
  // than trusted — clicking a clip should not stop playback.
  if (app.session.revision() != app.player_revision) {
    app.player_revision = app.session.revision();

    // The master fader is the one edit playback survives, and it has to: it is
    // set by ear against what is playing, and stopping the sound the moment the
    // button came up would mean it could only ever be set against silence. The
    // mixer takes it live, and nothing that was decoded depends on it — so the
    // change is handed over and then discounted from the comparison below.
    const double master = app.session.project().sequence().master_gain;
    if (master != app.player_project.sequence().master_gain) {
      app.player->set_master_gain(master);
      app.player_project.sequence().master_gain = master;
    }

    if (app.player_project != app.session.project()) {
      rebuild_playback(app);
      return;
    }
  }

  // Before the frame check below: a meter is the one thing here that has
  // something new to say between two pictures, and it costs a copy of a dozen
  // doubles to say it.
  refresh_meter(app);
  mark_dirty(app);

  // And a point from every fader that is writing. Also before the frame check,
  // because a pass is a shape drawn by hand and the hand does not wait for a
  // new picture to be decoded.
  record_automation(app);

  // Something else moved the playhead since the last turn: a transport key, a
  // jump to a marker, the timecode field. Take the sound to it rather than
  // dragging the playhead back to wherever the sound happens to be.
  //
  // Noticed here rather than by seeking at each place that can move the
  // playhead. There are several, more get added, and the ones that forget fail
  // silently — the playhead moves for one frame and is pulled back, which is
  // indistinguishable from the key not being bound.
  if (app.playhead_from_playback >= 0.0 &&
      app.session.playhead() != app.playhead_from_playback) {
    app.player->seek(app.session.playhead());
    app.playhead_from_playback = app.session.playhead();
    // The picture is drawn once per frame of the sequence and only when that
    // frame changes; without this the jump waits for the next one.
    app.shown_frame = -1;
  }

  const double at = app.player->position();
  if (app.player->finished()) {
    // Round again from the start of whatever is marked, rather than stopping.
    // The marked range and not the whole sequence, because marking a passage
    // and looping it is one gesture: it is how a cut gets watched twenty times
    // while a level is set against it.
    if (app.looping) {
      const double from = cutline::core::playback_span(app.session.project(), app.settings.preroll,
                                                       app.settings.postroll)
                              .start;
      app.session.set_playhead(from);
      app.player->seek(from);
      app.player->play();
      app.shown_frame = -1;
      refresh_timeline(app);
      invalidate_preview(app);
      return;
    }
    app.session.set_playhead(at);
    refresh_timeline(app);
    invalidate_preview(app);
    stop_playback(app);
    return;
  }

  // The end of the marked range is the loop's end. The player only knows where
  // the *sequence* stops, so this is the one place that can notice.
  //
  // Widened by the preroll and postroll, so a loop shows what leads into the
  // marked passage and what it lands in. With both at zero this is the marked
  // span exactly, which is what it was before they existed.
  if (app.looping) {
    const cutline::core::MarkedSpan span = cutline::core::playback_span(
        app.session.project(), app.settings.preroll, app.settings.postroll);
    const double end = span.start + span.duration;
    if (span.duration > 0.0 && at >= end) {
      app.session.set_playhead(span.start);
      app.player->seek(span.start);
      app.shown_frame = -1;
      refresh_timeline(app);
      invalidate_preview(app);
      return;
    }
  }

  // Once per frame of the sequence, not once per turn of the loop. Rendering
  // faster than the project's frame rate would draw the same picture twice and
  // charge a decode for it.
  const double fps = app.session.project().sequence().fps > 0.0 ? app.session.project().sequence().fps : 30.0;
  const auto frame = static_cast<long long>(at * fps);
  if (frame == app.shown_frame) {
    // Nothing due yet. Sleeping rather than spinning, because the audio thread
    // wants a core more than this loop does.
    Sleep(1);
    return;
  }
  app.shown_frame = frame;
  // The start of the frame the sound is in, not `at` itself. The playhead's
  // setter rounds to the *nearest* frame, and the test above truncates: a turn
  // late in one frame rounded up into the next, the turn after found that next
  // frame "new" and drew it again, and one frame in four was never shown.
  show_playback_frame(app, static_cast<double>(frame) / fps);
#else
  (void)app;
#endif
}

}  // namespace cutline::tool
