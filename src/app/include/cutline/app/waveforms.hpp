#pragma once

/// Audio envelopes for the timeline, computed off the thread that paints.
///
/// The timeline wants a waveform per source. Getting one means decoding a whole
/// audio stream, which for the reference captures — ten minutes, four streams —
/// costs seconds and hundreds of megabytes. Doing that where a clip is dragged
/// would stop the interface dead, so it happens on a worker and the drawing
/// simply goes without until the answer arrives.
///
/// The cache is keyed by source and stream rather than by clip, because that is
/// what an envelope describes: trimming a clip does not change the shape of the
/// file behind it, and a source cut into twenty pieces is decoded once.

#include "cutline/ui/timeline.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace cutline::app {

/// Envelopes, and the worker that fills them in.
///
/// Thread-safe by one mutex over the map. The worker only ever inserts, the
/// interface only ever reads, and both go through `mutex_` — the shared pointer
/// handed out is what makes reading safe afterwards, since the reader keeps the
/// envelope alive whatever the cache does next.
class WaveformCache {
 public:
  WaveformCache() = default;
  ~WaveformCache();

  WaveformCache(const WaveformCache&) = delete;
  WaveformCache& operator=(const WaveformCache&) = delete;

  /// The envelope for a source's stream, or null when there is not one yet.
  ///
  /// Never decodes. This is called from the paint path, once per clip per
  /// rebuild, and a lookup that could block would be the whole problem this
  /// class exists to avoid.
  [[nodiscard]] std::shared_ptr<const ui::Waveform> find(std::string_view media_id,
                                                         int stream) const;

  /// Asks for an envelope, if it is not already known or already being worked
  /// on. Returns at once either way.
  void request(std::string media_id, std::string path, int stream);

  /// Whether an envelope has arrived since this was last called, and clears the
  /// flag.
  ///
  /// Polled by the frame loop rather than pushed from the worker, so nothing
  /// touches a widget from another thread — the one rule that makes the rest of
  /// this safe to reason about.
  [[nodiscard]] bool take_arrival() noexcept { return arrived_.exchange(false); }

  /// Called on the worker when an envelope lands.
  ///
  /// A wake-up, not a delivery: an interface that waits on its message queue
  /// would otherwise sit there until the next mouse move before showing a
  /// waveform that has been ready for a second. Whatever is given here runs on
  /// the *worker's* thread, so it must be something safe to do from one —
  /// posting a message is; touching a widget is not.
  void set_on_arrival(std::function<void()> on_arrival);

  /// Drops everything, including what is queued. For when a project is closed
  /// and the sources it named are no longer anybody's business.
  void clear();

  /// Where envelopes are kept between sessions, or empty for not at all.
  ///
  /// Settable rather than fixed because it is a preference, and one that can
  /// change while the application runs. Read on the worker, so it is guarded
  /// like everything else here.
  void set_cache_dir(std::filesystem::path dir);

  /// How many envelopes are held. For tests and for saying something honest in
  /// a status line.
  [[nodiscard]] std::size_t size() const;

  /// How many are still to come: queued, plus the one being worked on.
  ///
  /// The interface never blocks on this work — measured, and the message loop
  /// does not stall for a millisecond — but a ten-minute capture with four
  /// audio streams costs about a hundred seconds of processor time spread
  /// across every core it can reach, and for those seconds the machine is
  /// visibly busy with nothing on screen to say why. This is what lets it say.
  [[nodiscard]] std::size_t pending() const;

 private:
  struct Job {
    std::string media_id;
    std::string path;
    int stream = 0;
  };

  using Key = std::pair<std::string, int>;

  void ensure_worker();
  void run();

  mutable std::mutex mutex_;
  std::filesystem::path cache_dir_;
  std::map<Key, std::shared_ptr<const ui::Waveform>> waveforms_;
  /// What has been asked for, so a source on screen every frame is not queued
  /// every frame. An entry stays after it is done, which is what `waveforms_`
  /// having it already covers — this is for the window in between.
  std::map<Key, bool> requested_;
  std::deque<Job> queue_;

  std::condition_variable wake_;
  std::thread worker_;
  bool stopping_ = false;
  /// Asked for and not yet finished with, counted rather than derived.
  ///
  /// `requested_.size() - waveforms_.size()` looks like the same number and is
  /// not: a source that cannot be decoded keeps its `requested_` entry for ever
  /// — deliberately, so a broken path is not retried on every rebuild — and
  /// would leave the count stuck at one and the indicator spinning for the life
  /// of the session.
  std::size_t outstanding_ = 0;
  std::atomic<bool> arrived_{false};
  /// Guarded by `mutex_`, and copied out before being called so the worker
  /// never holds the lock across something it does not control.
  std::function<void()> on_arrival_;
};

/// How finely the envelope is sampled. A hundred buckets a second is about a
/// bucket every three pixels at the zoom a clip is usually edited at, and half a
/// megabyte for a ten-minute source.
inline constexpr int kWaveformBucketsPerSecond = 100;

}  // namespace cutline::app
