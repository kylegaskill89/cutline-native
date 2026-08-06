#pragma once

/// Making proxies in the background, one at a time.
///
/// The same shape as the waveform and filmstrip workers, and a different
/// arrangement inside for one reason: this job is minutes long rather than
/// seconds, and it is the only one whose result outlives the session. So the
/// queue is served strictly in order by a single thread — a machine already
/// struggling enough to want proxies is not helped by transcoding four files at
/// once — and finished work is handed back to be attached rather than cached
/// here, because where a proxy ended up belongs in the project.
///
/// Nothing here touches a project. The builder is told a media id and two paths
/// and reports the same id back; whoever asked decides what that means. That is
/// what lets a proxy finish after the project has moved on without the builder
/// having to know what changed.

#include "cutline/media/transcode.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cutline::app {

/// A proxy that was written, waiting to be attached to its source.
struct FinishedProxy {
  std::string media_id;
  std::string path;
};

/// A proxy that was not written, and why.
struct FailedProxy {
  std::string media_id;
  std::string name;
  std::string message;
};

/// What the builder is doing at this moment, for saying so on screen.
struct ProxyProgress {
  std::string media_id;
  std::string name;
  /// How much of the current source is written, from 0 to 1.
  double done = 0.0;
  /// How many are waiting behind it, not counting this one.
  std::size_t queued = 0;
};

/// Transcodes sources to proxies on a worker thread.
class ProxyBuilder {
 public:
  ProxyBuilder() = default;
  ~ProxyBuilder();

  ProxyBuilder(const ProxyBuilder&) = delete;
  ProxyBuilder& operator=(const ProxyBuilder&) = delete;

  /// Queues a proxy for a source. `name` is only ever shown to somebody.
  ///
  /// Asking twice for the same source while the first is outstanding does
  /// nothing, so a button pressed twice does not transcode twice. Asking again
  /// after one finished *does* run again — footage can be replaced, and refusing
  /// would leave no way to rebuild a proxy that no longer matches.
  void request(std::string media_id, std::string name, std::string source,
               std::string destination);

  /// Proxies finished since this was last called, and clears them.
  ///
  /// Taken rather than pushed, for the same reason as everywhere else here: the
  /// answer arrives on the worker and has to be applied where the project lives.
  [[nodiscard]] std::vector<FinishedProxy> take_finished();

  /// Failures since this was last called, and clears them. Worth surfacing: a
  /// proxy that silently never appears looks exactly like one still being made.
  [[nodiscard]] std::vector<FailedProxy> take_failures();

  /// What is being worked on, or nothing when the builder is idle.
  [[nodiscard]] std::optional<ProxyProgress> progress() const;

  /// How many are outstanding: queued, plus the one in hand.
  [[nodiscard]] std::size_t pending() const;

  /// Abandons the queue and stops the one in progress.
  ///
  /// The file being written is removed, so nothing is left half-made. Returns
  /// once the worker has noticed, which is within a frame of the transcode —
  /// cancellation is checked where progress is reported.
  void cancel_all();

  /// Called on the worker when something finishes or fails. A wake-up, not a
  /// delivery: it runs on the worker's thread, so posting a message is safe and
  /// touching a widget is not.
  void set_on_change(std::function<void()> on_change);

 private:
  struct Job {
    std::string media_id;
    std::string name;
    std::string source;
    std::string destination;
  };

  void ensure_worker();
  void run();
  /// Copies the callback out under the lock and calls it without one, so the
  /// worker never holds the mutex across something it does not control.
  void announce();

  mutable std::mutex mutex_;
  std::deque<Job> queue_;
  std::vector<FinishedProxy> finished_;
  std::vector<FailedProxy> failures_;

  /// The job in hand, and how far into it the worker is. Held apart from the
  /// queue because a cancel has to be able to say "not this one either".
  std::optional<Job> current_;
  double current_done_ = 0.0;

  std::condition_variable wake_;
  std::thread worker_;
  bool stopping_ = false;
  /// Read by the worker inside the transcode, where the mutex is not held.
  std::atomic<bool> cancelling_{false};
  std::function<void()> on_change_;
};

}  // namespace cutline::app
