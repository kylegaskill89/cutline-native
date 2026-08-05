#pragma once

/// Filmstrips for the timeline, extracted off the thread that paints.
///
/// The same shape as `WaveformCache` and for the same reason: getting one means
/// decoding, decoding takes seconds, and the interface cannot wait. What
/// differs is that these are pixels rather than numbers, so the cache is bounded
/// — an envelope is half a megabyte and a filmstrip is several, and a session
/// that imported forty sources would otherwise hold every one of them for as
/// long as it ran.
///
/// Extraction seeks per frame, which is the one place that access pattern is
/// right: a handful of frames spread across a long file would otherwise mean
/// decoding everything between them.

#include "cutline/ui/timeline.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cutline::app {

/// How tall a thumbnail is extracted. Width follows the source's aspect.
///
/// Taller than any track is drawn, so a filmstrip stays sharp on the roomiest
/// theme and when a track is enlarged, without being a frame worth keeping.
inline constexpr int kThumbnailHeight = 72;

/// How much of a source's length one frame is expected to stand for.
///
/// The strip says what the footage *is* rather than what each instant of it
/// looks like — sampling densely enough for the latter would mean holding a
/// decoded film. Two seconds is fine enough that a cut is usually visible and
/// coarse enough that a ten-minute source stays inside the frame cap.
inline constexpr double kThumbnailSeconds = 2.0;

/// The most frames extracted for one source, and the fewest.
///
/// The cap is what stops a long source costing minutes of seeking and tens of
/// megabytes; the floor is what stops a two-second clip getting one frame and
/// a strip that says nothing.
inline constexpr int kMaxThumbnails = 48;
inline constexpr int kMinThumbnails = 6;

/// How many decoding threads the extractor may use.
///
/// Left to itself libav takes every core, which is right for a tool doing one
/// job and wrong for a worker running behind an interface: a ten-minute 4K
/// source cost about a hundred seconds of processor time in ten of wall clock,
/// and a machine in that state feels like one about to fall over even though
/// the message loop never missed a beat. Two threads keep the extraction
/// comfortably ahead of anyone scrolling and leave the rest of the machine to
/// whoever is using it.
inline constexpr int kThumbnailThreads = 2;

/// The most frames to hold for one source.
///
/// Extraction follows the view now, so a source scrolled from end to end would
/// otherwise accumulate a frame every two seconds for its whole length — three
/// hundred of them on a ten-minute capture, where the old whole-file strip was
/// capped at 48. The cap is kept and the frames furthest from what was last
/// asked for are the ones dropped.
inline constexpr std::size_t kFramesPerSource = 96;

/// Roughly how many bytes of filmstrip to keep before dropping the least
/// recently asked for. Forty-eight frames of 128x72 RGBA is about 1.8 MB, so
/// this holds a dozen or so sources.
inline constexpr std::size_t kThumbnailBudget = 24u * 1024u * 1024u;

/// Filmstrips, and the worker that extracts them.
class ThumbnailCache {
 public:
  /// The budget is a parameter so eviction can be tested against a few
  /// filmstrips rather than the two dozen megabytes it would otherwise take —
  /// dropping the wrong entry is exactly the sort of thing that is wrong until
  /// somebody checks.
  explicit ThumbnailCache(std::size_t budget = kThumbnailBudget) : budget_(budget) {}
  ~ThumbnailCache();

  ThumbnailCache(const ThumbnailCache&) = delete;
  ThumbnailCache& operator=(const ThumbnailCache&) = delete;

  /// The filmstrip for a source, or null when there is not one yet.
  ///
  /// Never extracts, and never blocks on the worker. Also marks the source as
  /// recently wanted, which is what the budget evicts against — so a strip on
  /// screen every frame is the last thing dropped.
  [[nodiscard]] std::shared_ptr<const ui::Filmstrip> find(std::string_view media_id) const;

  /// Asks for the stretch of a source between `from` and `to`, in source
  /// seconds, if it is not already known or already queued.
  ///
  /// A stretch rather than the whole file, because the whole file is nearly
  /// never what is on screen. A ten-minute source at an ordinary zoom shows
  /// about thirteen seconds of itself; extracting all of it meant 48 seeks
  /// across a 106 Mbps 4K capture to draw seven thumbnails, and that is the
  /// hundred seconds of processor time that made dropping a clip feel like a
  /// crash. Asking only for what is visible costs a handful of seeks, and
  /// scrolling or zooming tops the strip up as it goes.
  ///
  /// Overlapping requests are cheap: what a source already has is subtracted
  /// before anything is queued, so a view that moves by a second asks for a
  /// second's worth.
  void request(std::string media_id, std::string path, double from, double to);

  /// Whether a filmstrip has arrived since this was last called, and clears the
  /// flag.
  [[nodiscard]] bool take_arrival() noexcept { return arrived_.exchange(false); }

  /// Called on the worker when one lands. Runs on that thread, so it must be
  /// something safe from one — posting a message is, touching a widget is not.
  void set_on_arrival(std::function<void()> on_arrival);

  void clear();

  [[nodiscard]] std::size_t size() const;
  /// How many bytes of pixels are held.
  [[nodiscard]] std::size_t bytes() const;

  /// How many filmstrips are still to come: queued, plus the one in hand.
  /// Counted rather than derived, for the same reason as the envelopes — a
  /// source that will not decode keeps its `requested_` entry for good.
  [[nodiscard]] std::size_t pending() const;

  /// How many frames a stretch of this length is worth taking.
  [[nodiscard]] static int frames_for(double duration) noexcept;

  /// A stretch of a source, in source seconds.
  struct Span {
    double from = 0.0;
    double to = 0.0;

    [[nodiscard]] bool empty() const noexcept { return !(to > from); }
    friend bool operator==(const Span&, const Span&) = default;
  };

  /// What of `want` is not already in `have`, as a single stretch.
  ///
  /// A single stretch and not a set of them: the parts of a view that are
  /// missing are nearly always one contiguous piece at one end — the direction
  /// being scrolled — and the smallest range covering the gaps costs at worst a
  /// few frames that were already held. Exposed for tests, which is the only
  /// way to be sure a scroll asks for the new second rather than for all ten
  /// minutes again.
  [[nodiscard]] static Span missing(const std::vector<Span>& have, Span want) noexcept;

 private:
  struct Job {
    std::string media_id;
    std::string path;
    Span span;
  };

  struct Entry {
    std::shared_ptr<const ui::Filmstrip> strip;
    std::size_t bytes = 0;
    /// Bumped whenever it is asked for, so the least recently wanted is the one
    /// evicted. A counter rather than a clock: two lookups in the same
    /// millisecond still order, and there is no clock to disagree about.
    std::uint64_t used = 0;
    /// Stretches already extracted, merged and in order. This is what makes a
    /// second request for the same view cost nothing.
    std::vector<Span> covered;
  };

  void ensure_worker();
  void run();
  /// Drops the least recently used entries until the budget is met. Called with
  /// the lock held.
  void evict();

  mutable std::mutex mutex_;
  mutable std::map<std::string, Entry> strips_;
  mutable std::uint64_t tick_ = 0;
  /// Stretches asked for and not yet finished with, per source. What is in
  /// flight has to be subtracted as well as what has arrived, or a view that
  /// rebuilds every frame queues the same stretch sixty times.
  std::map<std::string, std::vector<Span>> requested_;
  std::deque<Job> queue_;
  std::size_t bytes_ = 0;
  std::size_t budget_ = kThumbnailBudget;

  std::condition_variable wake_;
  std::thread worker_;
  bool stopping_ = false;
  std::atomic<bool> arrived_{false};
  std::function<void()> on_arrival_;
  std::size_t outstanding_ = 0;
};

}  // namespace cutline::app
