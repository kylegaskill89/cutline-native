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

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace cutline::engine {

/// Whether this build can rasterise a title.
///
/// The text layer is Skia, and the presets that carry no Skia build the whole
/// renderer without it: a title clip is then planned, laid out and composited
/// as nothing at all. That is deliberate — the `media` preset exports
/// everything else and does not pay a Skia build for it — but it means "a title
/// drew no pixels" is the correct answer in one build and a bug in another, and
/// nothing could tell the two apart from outside.
///
/// It is asked at run time rather than by a macro so that whoever needs the
/// answer does not have to be compiled with the same definitions the engine
/// was. The tests are what needed it: five of them asserted that a title draws
/// its glyphs, and under the `media` preset they had been failing CI for weeks
/// for the one reason that is not a defect. A sixth — that a title is
/// transparent around its glyphs — was passing, which is worse: it is true of a
/// title that drew nothing at all.
[[nodiscard]] bool can_draw_text() noexcept;

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

  /// How much decoding has happened since the renderer was made.
  ///
  /// A seek costs roughly seventeen times what decoding the next frame does, so
  /// the ratio between these two is the difference between a preview that plays
  /// and one that crawls. It is not visible from outside otherwise — a slow
  /// preview looks the same whether it is compositing slowly or seeking on
  /// every frame, and those want opposite fixes.
  struct DecodeStats {
    std::int64_t frames_decoded = 0;
    std::int64_t seeks = 0;
    /// Split by reason, because they mean opposite things. Backwards seeks
    /// during forward playback mean the playhead is going backwards and
    /// something upstream is wrong; forward ones mean the request jumped
    /// further than decoding through would cover, which is the threshold's
    /// judgement and may simply be miscalibrated.
    std::int64_t backward_seeks = 0;
    std::int64_t forward_seeks = 0;
    /// Frames served from the run of recently decoded ones rather than decoded
    /// again. Playing backwards is almost entirely these — that is what makes
    /// it possible — so a reverse pass where this stays near zero means the run
    /// is not being kept or not being found.
    std::int64_t frames_remembered = 0;
    /// Frames decoded ahead of need, into the run before the one being played,
    /// and how many times such a run was ready when the playhead reached it.
    ///
    /// A reverse pass where `runs_taken_ahead` stays at zero while
    /// `backward_seeks` climbs is the prefetch never finishing in time, which
    /// is the stall it exists to remove and looks identical to not having it.
    std::int64_t frames_decoded_ahead = 0;
    std::int64_t runs_taken_ahead = 0;
  };
  [[nodiscard]] DecodeStats decode_stats() const noexcept;

  /// Drops every open decoder. Worth doing when the timeline changes enough
  /// that the cached sources are no longer the ones being asked for.
  void release_sources();

  /// Whether to read from proxies where a source has one.
  ///
  /// **Off by default, and that is the whole design.** Export must never write
  /// the small copy, and the way to be sure of that is for the exporter to do
  /// nothing at all rather than to remember to turn something off. The preview
  /// opts in, from the project's own setting; everything else gets originals
  /// because it never asked.
  void set_use_proxies(bool use_proxies);
  [[nodiscard]] bool use_proxies() const noexcept;

  [[nodiscard]] gpu::Compositor& compositor() noexcept;

  /// Changes the canvas size, discarding the current contents.
  [[nodiscard]] std::expected<void, std::string> resize(int width, int height);

 private:
  struct Impl;
  explicit FrameRenderer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::engine
