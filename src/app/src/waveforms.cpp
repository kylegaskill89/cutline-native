#include "cutline/app/waveforms.hpp"

#include "cutline/media/audio.hpp"

#include <utility>

namespace cutline::app {

WaveformCache::~WaveformCache() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    queue_.clear();
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::shared_ptr<const ui::Waveform> WaveformCache::find(std::string_view media_id,
                                                        int stream) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto found = waveforms_.find(Key{std::string(media_id), stream});
  return found == waveforms_.end() ? nullptr : found->second;
}

std::size_t WaveformCache::size() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return waveforms_.size();
}

std::size_t WaveformCache::pending() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return outstanding_;
}

void WaveformCache::request(std::string media_id, std::string path, int stream) {
  // Generated media — titles, colour mattes, adjustment layers — have no file
  // behind them, and asking the decoder for one would be an error reported once
  // a frame for as long as the clip is on screen.
  if (path.empty()) return;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    const Key key{media_id, stream};
    if (waveforms_.contains(key)) return;
    // Already asked for. Without this the timeline queues every audio clip
    // again on every rebuild, which is after every gesture.
    if (requested_.contains(key)) return;
    requested_.emplace(key, true);
    queue_.push_back(Job{std::move(media_id), std::move(path), stream});
    ++outstanding_;
  }
  ensure_worker();
  wake_.notify_one();
}

void WaveformCache::set_on_arrival(std::function<void()> on_arrival) {
  const std::lock_guard<std::mutex> lock(mutex_);
  on_arrival_ = std::move(on_arrival);
}

void WaveformCache::clear() {
  const std::lock_guard<std::mutex> lock(mutex_);
  waveforms_.clear();
  requested_.clear();
  queue_.clear();
  // Whatever the worker is in the middle of is no longer anybody's business,
  // so it is not counted either.
  outstanding_ = 0;
}

void WaveformCache::ensure_worker() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || worker_.joinable()) return;
  // Started on the first request rather than in the constructor: a project with
  // no audio in it never pays for a thread.
  worker_ = std::thread([this] { run(); });
}

void WaveformCache::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }

    // Outside the lock. This is seconds of decoding, and holding the mutex
    // across it would make every lookup on the paint thread wait for it —
    // turning a cache built to keep the interface answerable into the thing
    // that stops it.
    auto peaks = media::extract_waveform(job.path, job.stream, kWaveformBucketsPerSecond);
    if (!peaks.has_value()) {
      // A source that cannot be decoded is left with no envelope and not asked
      // for again. `requested_` keeps the entry, which is what stops a broken
      // path being retried on every rebuild for the life of the session — and
      // is why `outstanding_` has to come down here too, or one unreadable file
      // would leave the interface saying it was still working for ever.
      const std::lock_guard<std::mutex> lock(mutex_);
      if (outstanding_ > 0) --outstanding_;
      continue;
    }

    auto wave = std::make_shared<ui::Waveform>();
    wave->buckets_per_second = peaks->buckets_per_second;
    wave->minimum = std::move(peaks->minimum);
    wave->maximum = std::move(peaks->maximum);

    std::function<void()> tell;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;
      waveforms_.insert_or_assign(Key{job.media_id, job.stream}, std::move(wave));
      if (outstanding_ > 0) --outstanding_;
      tell = on_arrival_;
    }
    // Set after the map is written, so a frame that sees the flag finds the
    // envelope rather than missing it and waiting for the next change to
    // anything.
    arrived_.store(true);
    // And outside the lock, because what this does is somebody else's — and
    // what it usually does is wake a thread that will immediately want the
    // mutex this would still be holding.
    if (tell) tell();
  }
}

}  // namespace cutline::app
