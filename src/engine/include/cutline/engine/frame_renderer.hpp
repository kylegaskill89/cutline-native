#pragma once

/// Rendering a project at a moment in time, end to end.
///
/// This is where the layers finally meet: the core model says what exists,
/// `render::plan_frame` decides what draws, the media layer supplies the
/// pixels, and the compositor combines them. Nothing above this needs to know
/// how any of that works.
///
/// One renderer serves both preview and export, which is the whole argument of
/// the rewrite. The only difference between them is what happens to the result
/// — a swapchain or an encoder — and that is the caller's business.
///
/// **Decoding is sequential when it can be.** Asking for frames in increasing
/// time order costs about 1.6 ms/frame on 4K60; asking for them out of order
/// costs about 28 ms, because every seek re-decodes from the nearest keyframe.
/// The renderer keeps each source's decoder open between calls and only seeks
/// when a request moves backwards or jumps far enough forwards that decoding
/// through would be slower. Export walks forwards and therefore never seeks;
/// scrubbing does, and that is a deliberate trade rather than an oversight.

#include "cutline/core/model.hpp"
#include "cutline/gpu/compositor.hpp"
#include "cutline/gpu/device.hpp"

#include <expected>
#include <memory>
#include <string>

namespace cutline::engine {

class FrameRenderer {
 public:
  /// Creates a renderer for a canvas of the given size. The compositor is
  /// owned here rather than passed in, because its size and the project's have
  /// to agree and there is no useful state to share with anything else.
  [[nodiscard]] static std::expected<std::unique_ptr<FrameRenderer>, std::string> create(
      std::shared_ptr<gpu::Device> device, int canvas_width, int canvas_height);

  FrameRenderer(const FrameRenderer&) = delete;
  FrameRenderer& operator=(const FrameRenderer&) = delete;
  ~FrameRenderer();

  /// Composites the project at timeline time `t`. The result stays on the GPU;
  /// `read_back` or a presenter takes it from there.
  ///
  /// A source that cannot be opened is skipped rather than failing the frame,
  /// so one missing file does not black out an entire render. Which sources
  /// were missing is reported by `missing_media`.
  [[nodiscard]] std::expected<void, std::string> render(const core::Project& project, double t);

  /// The composited frame as 8-bit sRGB RGBA. Stalls on the GPU, so this is for
  /// stills and tests rather than every frame of an export.
  [[nodiscard]] std::expected<gpu::Image, std::string> read_back();

  /// Media ids that could not be opened during the last `render`, in the order
  /// they were met. Empty when everything resolved.
  [[nodiscard]] const std::vector<std::string>& missing_media() const noexcept;

  /// Drops every open decoder. Worth doing when the timeline changes enough
  /// that the cached sources are no longer the ones being asked for.
  void release_sources();

  [[nodiscard]] gpu::Compositor& compositor() noexcept;

  /// Changes the canvas size, discarding the current contents.
  [[nodiscard]] std::expected<void, std::string> resize(int width, int height);

 private:
  struct Impl;
  explicit FrameRenderer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::engine
