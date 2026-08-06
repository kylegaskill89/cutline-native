#include "cutline/app/proxies.hpp"

#include <chrono>
#include <utility>

namespace cutline::app {

ProxyBuilder::~ProxyBuilder() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    queue_.clear();
  }
  // Set outside the lock because the worker reads it without one, and set at all
  // because a transcode in progress is minutes long: a builder torn down at
  // shutdown would otherwise hold the application open until the current file
  // finished.
  cancelling_.store(true);
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void ProxyBuilder::request(std::string media_id, std::string name, std::string source,
                           std::string destination, int height) {
  // Generated media has no file behind it, and neither does a source whose
  // proxy has nowhere to go.
  if (source.empty() || destination.empty()) return;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    const auto already = [&](const Job& job) { return job.media_id == media_id; };
    if (current_.has_value() && already(*current_)) return;
    for (const Job& job : queue_) {
      if (already(job)) return;
    }
    queue_.push_back(Job{std::move(media_id), std::move(name), std::move(source),
                         std::move(destination), height});
  }
  ensure_worker();
  wake_.notify_one();
}

std::vector<FinishedProxy> ProxyBuilder::take_finished() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(finished_, {});
}

std::vector<FailedProxy> ProxyBuilder::take_failures() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(failures_, {});
}

std::optional<ProxyProgress> ProxyBuilder::progress() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (!current_.has_value()) return std::nullopt;
  return ProxyProgress{.media_id = current_->media_id,
                       .name = current_->name,
                       .done = current_done_,
                       .queued = queue_.size()};
}

std::size_t ProxyBuilder::pending() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size() + (current_.has_value() ? 1u : 0u);
}

void ProxyBuilder::cancel_all() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
  }
  cancelling_.store(true);
}

void ProxyBuilder::set_on_change(std::function<void()> on_change) {
  const std::lock_guard<std::mutex> lock(mutex_);
  on_change_ = std::move(on_change);
}

void ProxyBuilder::announce() {
  std::function<void()> tell;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    tell = on_change_;
  }
  if (tell) tell();
}

void ProxyBuilder::ensure_worker() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || worker_.joinable()) return;
  // Started on the first request: a session that never asks for a proxy never
  // pays for a thread.
  worker_ = std::thread([this] { run(); });
}

void ProxyBuilder::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
      current_ = job;
      current_done_ = 0.0;
      // Cleared as each job starts rather than when the cancel arrives: a cancel
      // has to outlive the transcode it interrupts, or the flag would be gone
      // before the transcode next looked at it.
      cancelling_.store(false);
    }
    // Said before the work as well as after, so the indicator names the file
    // being transcoded from the moment it starts rather than at the first
    // progress report a minute later.
    announce();

    // Progress is reported per decoded frame, which on a long source is
    // thousands of times a second — far more often than anything showing it
    // could use, and each one costs a lock and a message posted to a window.
    // Four times a second is faster than a percentage changes and slow enough
    // to be free.
    auto told_at = std::chrono::steady_clock::now();
    constexpr auto kTellEvery = std::chrono::milliseconds(250);

    media::ProxyOptions options;
    options.height = job.height;
    options.on_progress = [&, this](double done) {
      // `stopping_` is read under the lock and `cancelling_` is not, because
      // only one of them is written by a thread that could be inside this
      // function at the time.
      bool stopping = false;
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        current_done_ = done;
        stopping = stopping_;
      }
      if (const auto now = std::chrono::steady_clock::now(); now - told_at >= kTellEvery) {
        told_at = now;
        announce();
      }
      return !stopping && !cancelling_.load();
    };

    // Outside the lock, and minutes long. Everything the interface asks about
    // while this runs — progress, what is queued, what has finished — is read
    // from state written under the lock either side of it.
    const auto result = media::write_proxy(job.source, job.destination, options);

    {
      const std::lock_guard<std::mutex> lock(mutex_);
      current_.reset();
      current_done_ = 0.0;
      if (stopping_) return;
      if (!result.has_value()) {
        failures_.push_back(
            FailedProxy{.media_id = job.media_id, .name = job.name, .message = result.error()});
      } else if (*result == media::ProxyResult::Written) {
        finished_.push_back(FinishedProxy{.media_id = job.media_id, .path = job.destination});
      }
      // A cancelled proxy is neither: nothing was written, and nobody needs
      // telling about a thing they asked to stop.
    }
    announce();
  }
}

}  // namespace cutline::app
