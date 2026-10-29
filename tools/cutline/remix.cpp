/// Premiere's Remix: a clip of music made another length by rearranging its
/// song, from the Remix tool or Essential Sound's Duration.
///
/// The song is listened to once, on a thread of its own — it is decoded, which
/// for a long one is a second or so — and what was heard is kept, by the part
/// of the file it was, so asking again for another length is immediate. A
/// remix asked for while its song is still being listened to waits for it;
/// one asked for since replaces it, since only the latest length is wanted.

#include "app.hpp"

#include "cutline/core/remix.hpp"
#include "cutline/engine/music_analysis.hpp"

#include <format>

namespace cutline::tool {
namespace {

/// What a song was heard as is kept under: the file, the stream, and the part
/// of it, since a clip trimmed differently before its remix is another song.
[[nodiscard]] std::optional<std::string> heard_key(const cutline::core::Project& project,
                                                   const std::string& clip_id) {
  const std::optional<cutline::core::RemixSource> source =
      cutline::core::remix_source(project, clip_id);
  if (!source.has_value()) return std::nullopt;
  return std::format("{}|{}|{:.4f}|{:.4f}", source->media_id, source->audio_stream,
                     source->source_in, source->source_out);
}

/// Makes the remix from what was heard, as one edit.
void apply(App& app, const PendingRemix& wanted, const cutline::audio::MusicAnalysis& heard) {
  if (!heard.usable()) {
    complain(app.main.window,
             "No beat was found in this clip's sound, so there is nowhere to cut it. Remix is "
             "for music with a steady beat.");
    return;
  }
  const std::optional<cutline::core::Remix> remix = cutline::engine::make_remix(
      app.session.project(), wanted.clip_id, heard, wanted.target, wanted.segments,
      wanted.variations);
  if (!remix.has_value()) return;
  app.session.name_next_edit("Remix");
  app.session.apply(cutline::core::apply_remix(app.session.project(), wanted.clip_id, *remix));
  refresh_timeline(app);
  invalidate_preview(app);
  app.inspector_stale = true;
  mark_dirty(app);
}

void listen(App& app, PendingRemix wanted) {
  if (app.remix_worker.joinable()) app.remix_worker.join();
  app.remix_running = true;
  app.remix_pending = std::move(wanted);
  app.remix_result.reset();
  refresh_busy(app);

  const cutline::core::Project project = app.session.project();
  const std::string clip_id = app.remix_pending->clip_id;
  HWND window = app.main.window;
  app.remix_worker = std::thread([&app, project, clip_id, window] {
    auto heard = cutline::engine::analyse_clip_music(project, clip_id);
    {
      const std::scoped_lock lock(app.remix_lock);
      app.remix_result = std::move(heard);
    }
    app.remix_running = false;
    PostMessageW(window, kRemixHeard, 0, 0);
  });
}

}  // namespace

void remix_clip(App& app, const std::string& clip_id, double target, int segments,
                int variations) {
  const cutline::core::Project& project = app.session.project();
  if (const std::optional<std::string> why = cutline::core::why_not_remix(project, clip_id)) {
    complain(app.main.window, *why);
    return;
  }
  const std::optional<std::string> key = heard_key(project, clip_id);
  if (!key.has_value() || !(target > 0.0)) return;
  PendingRemix wanted{.clip_id = clip_id,
                      .key = *key,
                      .target = target,
                      .segments = segments,
                      .variations = variations};
  if (const auto found = app.remix_heard.find(*key); found != app.remix_heard.end()) {
    apply(app, wanted, *found->second);
    return;
  }
  // Still listening to this song: the newest length waits for it instead.
  if (app.remix_running.load()) {
    if (app.remix_pending.has_value() && app.remix_pending->key == *key) {
      app.remix_pending = std::move(wanted);
      return;
    }
    if (app.remix_worker.joinable()) app.remix_worker.join();
    collect_remix(app, false);
  }
  listen(app, std::move(wanted));
}

void collect_remix(App& app, bool apply_pending) {
  std::optional<std::expected<cutline::audio::MusicAnalysis, std::string>> result;
  {
    const std::scoped_lock lock(app.remix_lock);
    result = std::move(app.remix_result);
    app.remix_result.reset();
  }
  if (app.remix_worker.joinable() && !app.remix_running.load()) app.remix_worker.join();
  refresh_busy(app);
  std::optional<PendingRemix> wanted = std::move(app.remix_pending);
  app.remix_pending.reset();
  if (!result.has_value() || !wanted.has_value()) return;
  if (!result->has_value()) {
    if (apply_pending) complain(app.main.window, "Remix could not listen to this clip: " + result->error());
    return;
  }
  auto heard = std::make_shared<const cutline::audio::MusicAnalysis>(std::move(**result));
  app.remix_heard[wanted->key] = heard;
  // Only if the clip is still the song that was listened to: undone, deleted
  // or trimmed to another part of it in the meantime, and the remix would be
  // of something else.
  if (!apply_pending) return;
  const std::optional<std::string> now = heard_key(app.session.project(), wanted->clip_id);
  if (now.has_value() && *now == wanted->key) apply(app, *wanted, *heard);
}

bool remixing(const App& app) { return app.remix_running.load(); }

void stop_remix(App& app) {
  if (app.remix_worker.joinable()) app.remix_worker.join();
}

}  // namespace cutline::tool
