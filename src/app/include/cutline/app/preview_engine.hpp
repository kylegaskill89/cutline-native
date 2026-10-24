#pragma once

/// The picture in a monitor, rendered on a thread of its own.
///
/// Everything a monitor shows used to be decoded and composited on the
/// interface thread, in the paint that wanted it. That is cheap most of the
/// time and catastrophic some of the time: crossing a cut means a seek and a
/// whole group of pictures, measured at 96 to 287 ms on the reference capture,
/// and for every one of those milliseconds the window took no input at all. A
/// scrub that lands somewhere expensive froze the very drag that caused it.
/// Premiere renders on worker threads and the interface only picks finished
/// pictures up; this is that.
///
/// The interface says what it *wants* — this project, this moment, playing or
/// not — and returns at once. A thread renders whatever the latest want is,
/// and while playing renders the frames after it too, so they are ready before
/// the playhead arrives. The interface asks for the picture for *now* each time
/// it turns, and gets the best one that is ready.
///
/// Two of these exist: one for the program monitor and one for the source
/// monitor, each with its own renderer and its own decoders.

#include "cutline/core/model.hpp"
#include "cutline/engine/frame_renderer.hpp"
#include "cutline/gpu/compositor.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/ui/painter.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutline::app {

class PreviewEngine {
 public:
  struct Options {
    /// Pictures that can be finished and waiting at once. While playing, all
    /// but one of them are frames ahead of the playhead.
    std::size_t slots = 6;
    /// Called on the render thread whenever a picture is ready. It must not
    /// touch the engine or the interface; posting a window message is what it
    /// is for, since the interface's loop sleeps when nothing is playing.
    std::function<void()> on_ready;
  };

  [[nodiscard]] static std::expected<std::unique_ptr<PreviewEngine>, std::string> create(
      std::shared_ptr<gpu::Device> device, Options options);

  PreviewEngine(const PreviewEngine&) = delete;
  PreviewEngine& operator=(const PreviewEngine&) = delete;
  /// Stops the thread and waits for it. Whatever it was rendering is finished
  /// first, since a render cannot be abandoned halfway.
  ~PreviewEngine();

  // ----------------------------------------------------- the interface --

  /// What the monitor should show. Returns at once, whatever the thread is
  /// doing; a want that arrives while another is being rendered replaces it.
  ///
  /// The project is held, not copied, and is never written to. Pass the same
  /// pointer for the same project: a different pointer is a different project,
  /// and every picture rendered from the old one is thrown away.
  void want(std::shared_ptr<const core::Project> project, double t, bool playing);

  /// The picture to show for `t`, if there is a better one ready than the one
  /// already up. While playing that is the latest frame not after `t`; when
  /// still, it is the frame at `t`, or failing that the most recent picture
  /// for this project — a scrub showing where it was a moment ago is better
  /// than a scrub showing nothing.
  ///
  /// The texture is the same one every time until the canvas changes size.
  [[nodiscard]] std::expected<std::optional<ui::TextureView>, std::string> take(double t);

  /// Renders one frame and reads it back, waiting for it. On the render thread,
  /// since that is the only thread the renderer may be used from; the wait is
  /// for whatever it is doing to finish first.
  ///
  /// For the things that need pixels in hand: a scope, a snapshot, a preview on
  /// a machine whose windows cannot share the card.
  [[nodiscard]] std::expected<gpu::Image, std::string> capture(
      std::shared_ptr<const core::Project> project, double t);

  /// Closes every decoder, on the render thread, and waits until it has.
  void release_sources();

  /// Sources that could not be opened in the last frame rendered.
  [[nodiscard]] std::vector<std::string> missing_media() const;
  [[nodiscard]] engine::FrameRenderer::DecodeStats decode_stats() const;

  /// Why rendering stopped, or empty. Sticky until the next `want` with a
  /// different project: a frame that failed will fail again if asked again.
  [[nodiscard]] std::string error() const;

  /// For tests: frames rendered so far, and the pixels last put up.
  [[nodiscard]] std::uint64_t frames_rendered() const;
  [[nodiscard]] std::expected<gpu::Image, std::string> read_shown();

 private:
  struct Impl;
  explicit PreviewEngine(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::app
