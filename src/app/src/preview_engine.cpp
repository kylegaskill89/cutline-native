#include "cutline/app/preview_engine.hpp"

#include "cutline/core/query.hpp"
#include "cutline/gpu/frame_ring.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace cutline::app {
namespace {

[[nodiscard]] double rate_of(const core::Project& project) noexcept {
  return project.sequence().fps > 0.0 ? project.sequence().fps : 30.0;
}

/// Which frame of the sequence a moment falls in. The playhead is already on
/// the frame grid, so the small allowance only stops 2.0 arriving as 1.9999999
/// and naming the frame before.
[[nodiscard]] long long frame_of(const core::Project& project, double t) noexcept {
  return static_cast<long long>(std::floor(t * rate_of(project) + 1e-6));
}

[[nodiscard]] double time_of(const core::Project& project, long long frame) noexcept {
  return static_cast<double>(frame) / rate_of(project);
}

}  // namespace

struct PreviewEngine::Impl {
  std::shared_ptr<gpu::Device> device;
  /// The render thread's own queue. On the device's queue, a heavy frame being
  /// composited would hold the window's next paint behind it on the card, and
  /// the interface would wait after all -- measured at twelve paints a second
  /// under sixteen blurs, against the display's thirty.
  std::shared_ptr<gpu::Queue> queue;
  std::unique_ptr<gpu::FrameRing> ring;
  std::function<void()> on_ready;

  /// Only ever touched on the render thread.
  std::unique_ptr<engine::FrameRenderer> renderer;

  // Everything below is shared, under `lock`.
  mutable std::mutex lock;
  std::condition_variable wake;
  bool stopping = false;

  /// The latest want. `serial` names the project: it moves when the pointer
  /// does, and everything rendered under an older one is somebody else's
  /// picture.
  std::shared_ptr<const core::Project> project;
  double at = 0.0;
  /// Frames of the sequence the playhead crosses per frame shown, signed:
  /// below zero going backwards, zero still. Rendering ahead steps by it.
  long long step = 0;
  std::uint64_t serial = 0;
  /// The HDR display's SDR white, when the window is on one.
  std::optional<float> hdr_display;

  /// A finished picture waiting to be taken, in the order it was rendered.
  struct Ready {
    std::uint64_t serial = 0;
    long long frame = 0;
    std::size_t slot = 0;
  };
  std::deque<Ready> ready;

  /// The frame being rendered right now, so the next want does not ask for it
  /// a second time while it is still on its way.
  std::optional<std::pair<std::uint64_t, long long>> rendering;

  /// The last frame the interface put up, for this serial. While moving,
  /// nothing at it or behind it is worth rendering again.
  std::optional<long long> taken;

  /// Work that has to run on the render thread because it uses the renderer.
  std::deque<std::function<void()>> jobs;

  std::string failure;
  std::vector<std::string> missing;
  engine::FrameRenderer::DecodeStats stats;
  std::uint64_t rendered = 0;

  std::thread thread;

  /// Throws away every waiting picture that `keep` does not want. Under `lock`.
  template <typename Keep>
  void prune(Keep keep) {
    for (auto it = ready.begin(); it != ready.end();) {
      if (keep(*it)) {
        ++it;
      } else {
        ring->discard(it->slot);
        it = ready.erase(it);
      }
    }
  }

  [[nodiscard]] bool have(long long frame) const {
    if (rendering && rendering->first == serial && rendering->second == frame) return true;
    return std::ranges::any_of(
        ready, [&](const Ready& r) { return r.serial == serial && r.frame == frame; });
  }

  /// The next frame worth rendering, or nothing. Under `lock`.
  [[nodiscard]] std::optional<long long> next_target() const {
    if (!project || !failure.empty()) return std::nullopt;
    const long long base = frame_of(*project, at);

    if (step == 0) {
      if (taken == base || have(base)) return std::nullopt;
      return base;
    }

    // Ahead of the playhead by as many frames as there are slots to hold
    // them, less the one the interface may be copying out of -- ahead in the
    // direction it is going. A shuttle backwards used to be rendered a frame
    // at a time as each was asked for, so a frame that cost more than its
    // share of the turn was a frame missing from the screen.
    const long long last = frame_of(*project, core::timeline_duration(*project));
    const auto ahead = std::max(1ll, static_cast<long long>(ring->capacity()) - 1);
    for (long long k = 0; k < ahead; ++k) {
      const long long frame = base + k * step;
      if (k > 0 && (frame > last || frame < 0)) break;
      if (taken && (step > 0 ? frame <= *taken : frame >= *taken)) continue;
      if (!have(frame)) return frame;
    }
    return std::nullopt;
  }

  void run();
  /// Renders one frame on this thread. Returns the error, or empty.
  [[nodiscard]] std::string render(const core::Project& project, double t);
};

std::string PreviewEngine::Impl::render(const core::Project& p, double t) {
  if (renderer == nullptr) {
    auto made = engine::FrameRenderer::create(device, p.sequence().canvas_w,
                                              p.sequence().canvas_h, queue);
    if (!made) return made.error();
    renderer = std::move(*made);
  }
  // A different canvas rebuilds the compositor's targets and keeps the
  // decoders, which are sized to their sources rather than to it.
  if (auto ok = renderer->resize(p.sequence().canvas_w, p.sequence().canvas_h); !ok) {
    return ok.error();
  }
  // Every render, from the project: switching proxies on takes effect on the
  // next frame rather than whenever somebody remembers to say so.
  renderer->set_use_proxies(p.use_proxies);
  {
    const std::lock_guard<std::mutex> held(lock);
    renderer->compositor().set_hdr_display(hdr_display);
  }
  if (auto ok = renderer->render(p, t); !ok) return ok.error();

  const std::lock_guard<std::mutex> held(lock);
  missing = renderer->missing_media();
  stats = renderer->decode_stats();
  ++rendered;
  return {};
}

void PreviewEngine::Impl::run() {
  while (true) {
    std::shared_ptr<const core::Project> project_now;
    std::uint64_t serial_now = 0;
    long long frame = 0;
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> held(lock);
      wake.wait(held, [&] {
        return stopping || !jobs.empty() || (ring->has_room() && next_target().has_value());
      });
      if (stopping) return;
      if (!jobs.empty()) {
        job = std::move(jobs.front());
        jobs.pop_front();
      } else {
        frame = *next_target();
        project_now = project;
        serial_now = serial;
        rendering = std::pair{serial_now, frame};
      }
    }

    if (job) {
      job();
      continue;
    }

    std::string failed = render(*project_now, time_of(*project_now, frame));
    std::optional<std::size_t> slot;
    if (failed.empty()) {
      if (auto shown = renderer->compositor().display_texture(); !shown) {
        failed = shown.error();
      } else if (auto stored = ring->store(renderer->compositor()); !stored) {
        failed = stored.error();
      } else {
        slot = *stored;
      }
    }

    {
      const std::lock_guard<std::mutex> held(lock);
      rendering.reset();
      if (!failed.empty()) {
        // Sticky until the project changes. Asking the same frame of the same
        // project again would fail the same way, as fast as the loop can turn.
        if (serial_now == serial) failure = std::move(failed);
      } else if (serial_now != serial) {
        // The project changed while this was being rendered. Nobody wants a
        // picture of the old one.
        ring->discard(*slot);
        slot.reset();
      } else {
        ready.push_back(Ready{.serial = serial_now, .frame = frame, .slot = *slot});
      }
    }
    if (on_ready) on_ready();
  }
}

PreviewEngine::PreviewEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PreviewEngine::~PreviewEngine() {
  if (!impl_) return;
  {
    const std::lock_guard<std::mutex> held(impl_->lock);
    impl_->stopping = true;
  }
  impl_->wake.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
  // Unfinished jobs still have somebody waiting on them; dropping them breaks
  // those promises rather than leaving the waits to hang.
  impl_->jobs.clear();
  impl_->renderer.reset();
  if (impl_->device) impl_->device->wait_for_idle();
}

std::expected<std::unique_ptr<PreviewEngine>, std::string> PreviewEngine::create(
    std::shared_ptr<gpu::Device> device, Options options) {
  if (!device) return std::unexpected("a preview needs a device");

  auto impl = std::make_unique<Impl>();
  impl->device = std::move(device);
  impl->on_ready = std::move(options.on_ready);
  auto queue = impl->device->make_queue();
  if (!queue) return std::unexpected(queue.error());
  impl->queue = std::move(*queue);
  auto ring =
      gpu::FrameRing::create(impl->device, std::max<std::size_t>(2, options.slots), impl->queue);
  if (!ring) return std::unexpected(ring.error());
  impl->ring = std::move(*ring);

  Impl* raw = impl.get();
  impl->thread = std::thread([raw] { raw->run(); });
  return std::unique_ptr<PreviewEngine>(new PreviewEngine(std::move(impl)));
}

void PreviewEngine::set_hdr_display(std::optional<float> sdr_white_nits) {
  Impl& d = *impl_;
  {
    const std::lock_guard<std::mutex> held(d.lock);
    if (d.hdr_display == sdr_white_nits) return;
    d.hdr_display = sdr_white_nits;
    // As a new project would: every frame made the old way is the wrong shape.
    ++d.serial;
    d.taken.reset();
    d.prune([](const Impl::Ready&) { return false; });
  }
  d.wake.notify_all();
}

void PreviewEngine::want(std::shared_ptr<const core::Project> project, double t, double rate) {
  Impl& d = *impl_;
  {
    const std::lock_guard<std::mutex> held(d.lock);
    if (project != d.project) {
      d.project = std::move(project);
      ++d.serial;
      d.taken.reset();
      d.failure.clear();
      d.prune([](const Impl::Ready&) { return false; });
    }
    d.at = t;
    // At least a frame at a time whichever way it is going: slower than one
    // frame a frame still crosses them one at a time.
    const auto frames = std::max(1ll, std::llround(std::abs(rate)));
    const long long step = rate > 0.0 ? frames : rate < 0.0 ? -frames : 0;
    // Turning round, the frames rendered for the old direction are all behind.
    if (step * d.step < 0) d.taken.reset();
    d.step = step;
    if (d.project && step != 0) {
      // Frames behind the playhead will never be shown. Freeing their slots now
      // is what lets the thread get on with the ones ahead of it.
      const long long now = frame_of(*d.project, t);
      d.prune([&](const Impl::Ready& r) {
        return step > 0 ? r.frame >= now - 1 : r.frame <= now + 1;
      });
    }
  }
  d.wake.notify_all();
}

std::expected<std::optional<ui::TextureView>, std::string> PreviewEngine::take(double t) {
  Impl& d = *impl_;
  std::optional<Impl::Ready> chosen;
  {
    const std::lock_guard<std::mutex> held(d.lock);
    if (!d.failure.empty()) return std::unexpected(d.failure);
    if (!d.project) return std::optional<ui::TextureView>{};

    const long long now = frame_of(*d.project, t);
    const long long step = d.step;
    for (const Impl::Ready& r : d.ready) {
      if (r.serial != d.serial) continue;
      if (step > 0) {
        if (r.frame <= now && (!chosen || r.frame > chosen->frame)) chosen = r;
      } else if (step < 0) {
        if (r.frame >= now && (!chosen || r.frame < chosen->frame)) chosen = r;
      } else if (r.frame == now) {
        chosen = r;
      }
    }
    // Still, and the exact frame is not in yet: the newest picture of this
    // project, which is where a scrub was a moment ago.
    if (step == 0 && !chosen) {
      for (auto it = d.ready.rbegin(); it != d.ready.rend(); ++it) {
        if (it->serial == d.serial) {
          chosen = *it;
          break;
        }
      }
    }
    if (!chosen) return std::optional<ui::TextureView>{};

    // Taken out of the list before the copy, so the thread cannot hand the
    // slot to another frame while it is being copied out of.
    std::erase_if(d.ready, [&](const Impl::Ready& r) { return r.slot == chosen->slot; });
    d.taken = chosen->frame;
    // And everything this makes redundant: while moving, any frame behind the
    // one going up; when still, everything — the next want says what is next.
    d.prune([&](const Impl::Ready& r) {
      if (r.serial != d.serial || step == 0) return false;
      return step > 0 ? r.frame > chosen->frame : r.frame < chosen->frame;
    });
  }

  auto shown = d.ring->show(chosen->slot);
  d.wake.notify_all();  // a slot is free again
  if (!shown) return std::unexpected(shown.error());
  return std::optional<ui::TextureView>(ui::TextureView{.texture = shown->resource,
                                                        .width = shown->width,
                                                        .height = shown->height,
                                                        .format = shown->format,
                                                        .state = shown->state,
                                                        .generation = shown->generation,
                                                        .hdr_texture = shown->hdr_resource,
                                                        .hdr_format = shown->hdr_format});
}

std::expected<gpu::Image, std::string> PreviewEngine::capture(
    std::shared_ptr<const core::Project> project, double t, gpu::WorkingColour output) {
  Impl& d = *impl_;
  if (!project) return std::unexpected("nothing to capture");
  auto promise = std::make_shared<std::promise<std::expected<gpu::Image, std::string>>>();
  auto answer = promise->get_future();
  {
    const std::lock_guard<std::mutex> held(d.lock);
    d.jobs.push_back([&d, promise, project, t, output] {
      if (std::string failed = d.render(*project, t); !failed.empty()) {
        promise->set_value(std::unexpected(std::move(failed)));
        return;
      }
      promise->set_value(d.renderer->read_back(output));
    });
  }
  d.wake.notify_all();
  try {
    return answer.get();
  } catch (const std::future_error&) {
    return std::unexpected("the preview stopped before the frame was captured");
  }
}

void PreviewEngine::release_sources() {
  Impl& d = *impl_;
  auto promise = std::make_shared<std::promise<void>>();
  auto done = promise->get_future();
  {
    const std::lock_guard<std::mutex> held(d.lock);
    d.jobs.push_back([&d, promise] {
      if (d.renderer) d.renderer->release_sources();
      promise->set_value();
    });
  }
  d.wake.notify_all();
  try {
    done.get();
  } catch (const std::future_error&) {
  }
}

std::vector<std::string> PreviewEngine::missing_media() const {
  const std::lock_guard<std::mutex> held(impl_->lock);
  return impl_->missing;
}

engine::FrameRenderer::DecodeStats PreviewEngine::decode_stats() const {
  const std::lock_guard<std::mutex> held(impl_->lock);
  return impl_->stats;
}

std::string PreviewEngine::error() const {
  const std::lock_guard<std::mutex> held(impl_->lock);
  return impl_->failure;
}

std::uint64_t PreviewEngine::frames_rendered() const {
  const std::lock_guard<std::mutex> held(impl_->lock);
  return impl_->rendered;
}

std::expected<gpu::Image, std::string> PreviewEngine::read_shown() {
  return impl_->ring->read_shown();
}

}  // namespace cutline::app
