#include "cutline/app/thumbnails.hpp"

#include "cutline/app/media_cache.hpp"
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

ThumbnailCache::Span ThumbnailCache::missing(const std::vector<Span>& have,
                                             Span want) noexcept {
  if (want.empty()) return {};
  double from = want.from;
  double to = want.to;
  // Eat into the wanted stretch from each end with whatever already covers it.
  // Only the ends, because a hole in the middle is not worth a second job — the
  // range that covers it re-extracts a few frames already held, and one seek
  // saved is worth more than a few frames of memory.
  for (const Span& had : have) {
    if (had.from <= from + 1e-6 && had.to > from) from = std::max(from, had.to);
    if (had.to >= to - 1e-6 && had.from < to) to = std::min(to, had.from);
  }
  if (!(to > from)) return {};
  return Span{from, to};
}

namespace {

/// Merges a stretch into a sorted, non-overlapping list.
void cover(std::vector<ThumbnailCache::Span>& spans, ThumbnailCache::Span add) {
  if (add.empty()) return;
  std::vector<ThumbnailCache::Span> out;
  out.reserve(spans.size() + 1);
  for (const ThumbnailCache::Span& span : spans) {
    // Touching counts as overlapping, so two views scrolled past each other
    // leave one stretch rather than a seam that asks to be filled for ever.
    if (span.to < add.from - 1e-6 || span.from > add.to + 1e-6) {
      out.push_back(span);
      continue;
    }
    add.from = std::min(add.from, span.from);
    add.to = std::max(add.to, span.to);
  }
  out.push_back(add);
  std::ranges::sort(out, {}, &ThumbnailCache::Span::from);
  spans = std::move(out);
}

}  // namespace

void ThumbnailCache::request(std::string media_id, std::string path, double from, double to) {
  // Generated media have no file behind them, and a still has one frame that
  // never changes — neither is a filmstrip.
  if (path.empty()) return;
  if (!(to > from)) return;

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;

    // What is already held and what is already on its way, both subtracted. A
    // timeline rebuilds after every gesture and would otherwise queue the same
    // stretch again each time.
    std::vector<Span> known;
    if (const auto found = strips_.find(media_id); found != strips_.end()) {
      known = found->second.covered;
    }
    if (const auto asked = requested_.find(media_id); asked != requested_.end()) {
      for (const Span& span : asked->second) cover(known, span);
    }

    const Span want = missing(known, Span{from, to});
    if (want.empty()) return;

    cover(requested_[media_id], want);
    queue_.push_back(Job{std::move(media_id), std::move(path), want});
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

void ThumbnailCache::set_cache_dir(std::filesystem::path dir) {
  const std::lock_guard<std::mutex> lock(mutex_);
  cache_dir_ = std::move(dir);
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

    std::filesystem::path cache_dir;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      cache_dir = cache_dir_;
    }
    const std::string key = cache_dir.empty() ? std::string{} : media_cache_key(job.path);

    const double length = job.span.to - job.span.from;
    const int wanted = frames_for(length);

    // What an earlier session already extracted for exactly this stretch.
    //
    // The stretch is the unit because the extractor spreads its frames evenly
    // across whatever range it is given: where they land is a property of the
    // range asked for, so nothing smaller than the range identifies them.
    const CachedStrip what{.from = job.span.from,
                           .to = job.span.to,
                           .count = wanted,
                           .height = kThumbnailHeight};

    std::vector<ui::FilmFrame> fresh;
    bool complete = false;
    if (!key.empty()) {
      if (auto stored = read_cached_strip(cache_dir, key, what)) {
        fresh = std::move(*stored);
        complete = true;
      }
    }

    if (!complete) {
      // Outside the lock: this is seconds of seeking and decoding, and holding
      // the mutex across it would make every lookup on the paint thread wait for
      // exactly the work this class exists to keep off it.
      media::ThumbnailOptions options;
      options.height = kThumbnailHeight;
      options.start = job.span.from;
      options.end = job.span.to;
      // Paced rather than given the machine. This is the difference between an
      // editor that is busy and one that looks like it is failing.
      options.threads = kThumbnailThreads;
      auto frames = media::extract_thumbnails(job.path, wanted, options);
      if (!frames.has_value()) continue;

      fresh.reserve(frames->size());
      for (media::Thumbnail& thumb : *frames) {
        if (thumb.width <= 0 || thumb.height <= 0 || thumb.rgba.empty()) continue;
        fresh.push_back(ui::FilmFrame{.t = thumb.timestamp,
                                      .width = thumb.width,
                                      .height = thumb.height,
                                      .rgba = std::move(thumb.rgba)});
      }

      // Stored before the interface is told, so the session that paid for the
      // seeking is the one that banks it.
      if (!key.empty()) write_cached_strip(cache_dir, key, what, fresh);
    }
    if (fresh.empty()) continue;

    std::function<void()> tell;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;

      Entry& entry = strips_[job.media_id];
      // Built anew rather than added to. Whoever asked for the strip last is
      // holding the old one and may be painting it on another thread this
      // instant; the shared pointer is what makes that safe, and it is only
      // safe while what it points at never changes.
      auto strip = std::make_shared<ui::Filmstrip>();
      if (entry.strip != nullptr) {
        strip->frames.reserve(entry.strip->frames.size() + fresh.size());
        for (const ui::FilmFrame& frame : entry.strip->frames) {
          // A frame the fresh stretch also covers is the fresh one's to
          // provide: the ranges were meant to abut and a little overlap is
          // cheaper than a seam.
          if (frame.t >= job.span.from - 1e-6 && frame.t <= job.span.to + 1e-6) continue;
          strip->frames.push_back(frame);
        }
      }
      for (ui::FilmFrame& frame : fresh) strip->frames.push_back(std::move(frame));

      // In time order, because the drawing asks for the frame nearest a source
      // time and a caller may reasonably assume the strip reads left to right.
      std::ranges::sort(strip->frames, {}, &ui::FilmFrame::t);

      // Following the view means a source scrolled end to end would otherwise
      // grow without limit. What goes is what is furthest from the stretch just
      // asked for, which is the part of the file nobody is looking at.
      const double middle = (job.span.from + job.span.to) * 0.5;
      while (strip->frames.size() > kFramesPerSource) {
        const bool front_is_further =
            std::abs(strip->frames.front().t - middle) > std::abs(strip->frames.back().t - middle);
        if (front_is_further) {
          strip->frames.erase(strip->frames.begin());
        } else {
          strip->frames.pop_back();
        }
      }

      std::size_t kept = 0;
      for (const ui::FilmFrame& frame : strip->frames) kept += frame.rgba.size();
      bytes_ -= std::min(bytes_, entry.bytes);
      bytes_ += kept;

      cover(entry.covered, Span{job.span.from, job.span.to});
      // And what the cap dropped is no longer covered, or it would never be
      // asked for again and the strip would have a hole nothing could fill.
      if (!strip->frames.empty()) {
        const Span kept_span{strip->frames.front().t, strip->frames.back().t};
        std::erase_if(entry.covered, [&](const Span& span) {
          return span.to < kept_span.from - 1e-6 || span.from > kept_span.to + 1e-6;
        });
        for (Span& span : entry.covered) {
          span.from = std::max(span.from, kept_span.from);
          span.to = std::min(span.to, kept_span.to);
        }
        std::erase_if(entry.covered, [](const Span& span) { return span.empty(); });
      }
      // What was in flight is in flight no longer, whatever came of it.
      if (const auto asked = requested_.find(job.media_id); asked != requested_.end()) {
        std::erase_if(asked->second, [&](const Span& span) {
          return span.from >= job.span.from - 1e-6 && span.to <= job.span.to + 1e-6;
        });
      }

      entry.strip = std::move(strip);
      entry.bytes = kept;
      entry.used = ++tick_;
      evict();
      tell = on_arrival_;
    }
    arrived_.store(true);
    if (tell) tell();
  }
}

}  // namespace cutline::app
