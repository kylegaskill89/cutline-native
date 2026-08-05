#include "cutline/app/thumbnails.hpp"

#include "cutline/media/thumbnail.hpp"

#include <algorithm>
#include <utility>

namespace cutline::app {

ThumbnailCache::~ThumbnailCache() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    queue_.clear();
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

int ThumbnailCache::frames_for(double duration) noexcept {
  if (!(duration > 0.0)) return kMinThumbnails;
  const auto wanted = static_cast<int>(duration / kThumbnailSeconds);
  return std::clamp(wanted, kMinThumbnails, kMaxThumbnails);
}

std::shared_ptr<const ui::Filmstrip> ThumbnailCache::find(std::string_view media_id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto found = strips_.find(std::string(media_id));
  if (found == strips_.end()) return nullptr;
  // Asking for it is what keeps it: the budget evicts what has gone longest
  // without being wanted, and what is on screen is asked for every rebuild.
  found->second.used = ++tick_;
  return found->second.strip;
}

std::size_t ThumbnailCache::size() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return strips_.size();
}

std::size_t ThumbnailCache::pending() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return outstanding_;
}

std::size_t ThumbnailCache::bytes() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return bytes_;
}

void ThumbnailCache::request(std::string media_id, std::string path, double duration) {
  // Generated media have no file behind them, and a still has one frame that
  // never changes — neither is a filmstrip.
  if (path.empty()) return;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    if (strips_.contains(media_id)) return;
    if (requested_.contains(media_id)) return;
    requested_.emplace(media_id, true);
    queue_.push_back(Job{std::move(media_id), std::move(path), duration});
    ++outstanding_;
  }
  ensure_worker();
  wake_.notify_one();
}

void ThumbnailCache::set_on_arrival(std::function<void()> on_arrival) {
  const std::lock_guard<std::mutex> lock(mutex_);
  on_arrival_ = std::move(on_arrival);
}

void ThumbnailCache::clear() {
  const std::lock_guard<std::mutex> lock(mutex_);
  strips_.clear();
  requested_.clear();
  queue_.clear();
  bytes_ = 0;
  outstanding_ = 0;
}

void ThumbnailCache::ensure_worker() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || worker_.joinable()) return;
  worker_ = std::thread([this] { run(); });
}

void ThumbnailCache::evict() {
  while (bytes_ > budget_ && strips_.size() > 1) {
    // The least recently asked for. Never the last one standing: a cache that
    // evicted its only entry under a budget smaller than one filmstrip would
    // extract it again on the next frame, for ever.
    auto oldest = strips_.begin();
    for (auto it = strips_.begin(); it != strips_.end(); ++it) {
      if (it->second.used < oldest->second.used) oldest = it;
    }
    bytes_ -= oldest->second.bytes;
    // Forgotten as a request too, so a source scrolled back into view is
    // extracted again rather than being remembered as done and never drawn.
    requested_.erase(oldest->first);
    strips_.erase(oldest);
  }
}

void ThumbnailCache::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }

    // Whatever becomes of this job — extracted, refused, or yielding no usable
    // frames — it stops being outstanding. Done here rather than at each of the
    // three ways out of the loop, which is how one of them gets forgotten and
    // the interface says it is still working long after it stopped.
    struct Done {
      ThumbnailCache* cache;
      ~Done() {
        const std::lock_guard<std::mutex> lock(cache->mutex_);
        if (cache->outstanding_ > 0) --cache->outstanding_;
      }
    } done{this};

    // Outside the lock: this is seconds of seeking and decoding, and holding
    // the mutex across it would make every lookup on the paint thread wait for
    // exactly the work this class exists to keep off it.
    media::ThumbnailOptions options;
    options.height = kThumbnailHeight;
    auto frames = media::extract_thumbnails(job.path, frames_for(job.duration), options);
    if (!frames.has_value()) continue;

    auto strip = std::make_shared<ui::Filmstrip>();
    std::size_t weight = 0;
    strip->frames.reserve(frames->size());
    for (media::Thumbnail& thumb : *frames) {
      if (thumb.width <= 0 || thumb.height <= 0 || thumb.rgba.empty()) continue;
      weight += thumb.rgba.size();
      strip->frames.push_back(ui::FilmFrame{.t = thumb.timestamp,
                                            .width = thumb.width,
                                            .height = thumb.height,
                                            .rgba = std::move(thumb.rgba)});
    }
    if (strip->frames.empty()) continue;
    // In time order, because the drawing asks for the frame nearest a source
    // time and a caller may reasonably assume the strip reads left to right.
    std::ranges::sort(strip->frames, {}, &ui::FilmFrame::t);

    std::function<void()> tell;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;
      bytes_ += weight;
      strips_.insert_or_assign(job.media_id,
                               Entry{.strip = std::move(strip), .bytes = weight, .used = ++tick_});
      evict();
      tell = on_arrival_;
    }
    arrived_.store(true);
    if (tell) tell();
  }
}

}  // namespace cutline::app
