#include "cutline/app/waveforms.hpp"

#include "cutline/app/media_cache.hpp"
#include "cutline/media/audio.hpp"

#include <utility>
#include <vector>

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

void WaveformCache::set_cache_dir(std::filesystem::path dir) {
  const std::lock_guard<std::mutex> lock(mutex_);
  cache_dir_ = std::move(dir);
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
    std::vector<Job> batch;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;

      // Every queued stream of the *same file* goes together, because getting
      // one stream's samples means demuxing the whole container: one job at a
      // time reads the file once per stream. The material this is built for is
      // a 1.5 GB capture with four audio streams, so that is six gigabytes read
      // to draw four lines, and it measured in minutes.
      batch.push_back(std::move(queue_.front()));
      queue_.pop_front();
      for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->path == batch.front().path) {
          batch.push_back(std::move(*it));
          it = queue_.erase(it);
        } else {
          ++it;
        }
      }
    }

    std::filesystem::path cache_dir;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      cache_dir = cache_dir_;
    }

    // What was worked out in an earlier session, before anything is decoded.
    // The key carries the file's size and modification time, so footage that
    // has been replaced misses rather than handing back an envelope belonging
    // to something else.
    const std::string key =
        cache_dir.empty() ? std::string{} : media_cache_key(batch.front().path);

    std::vector<ui::Waveform> found(batch.size());
    std::vector<Job> missing;
    for (std::size_t i = 0; i < batch.size(); ++i) {
      if (!key.empty()) {
        if (auto stored = read_cached_waveform(cache_dir, key, batch[i].stream)) {
          found[i] = std::move(*stored);
          continue;
        }
      }
      missing.push_back(batch[i]);
    }

    std::vector<int> streams;
    streams.reserve(missing.size());
    for (const Job& job : missing) streams.push_back(job.stream);

    // Outside the lock. This is seconds of decoding, and holding the mutex
    // across it would make every lookup on the paint thread wait for it —
    // turning a cache built to keep the interface answerable into the thing
    // that stops it.
    auto peaks = media::extract_waveforms(batch.front().path, streams, kWaveformBucketsPerSecond);
    if (!peaks.has_value() || peaks->size() != missing.size()) {
      // A source that cannot be decoded is left with no envelope and not asked
      // for again. `requested_` keeps the entry, which is what stops a broken
      // path being retried on every rebuild for the life of the session — and
      // is why `outstanding_` has to come down here too, or one unreadable file
      // would leave the interface saying it was still working for ever.
      const std::lock_guard<std::mutex> lock(mutex_);
      if (outstanding_ >= batch.size()) outstanding_ -= batch.size();
      else outstanding_ = 0;
      continue;
    }

    // What was decoded takes its place beside what was already stored, in the
    // order the batch asked for.
    for (std::size_t i = 0, next = 0; i < batch.size() && next < peaks->size(); ++i) {
      if (!found[i].empty()) continue;
      found[i].buckets_per_second = (*peaks)[next].buckets_per_second;
      found[i].minimum = std::move((*peaks)[next].minimum);
      found[i].maximum = std::move((*peaks)[next].maximum);
      // Written before the interface is told, so the session that paid for the
      // decode is the one that banks it — a crash on the next frame still
      // leaves the answer behind for next time.
      if (!key.empty()) write_cached_waveform(cache_dir, key, batch[i].stream, found[i]);
      ++next;
    }

    std::function<void()> tell;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;
      for (std::size_t i = 0; i < batch.size(); ++i) {
        waveforms_.insert_or_assign(Key{batch[i].media_id, batch[i].stream},
                                    std::make_shared<ui::Waveform>(std::move(found[i])));
        if (outstanding_ > 0) --outstanding_;
      }
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
