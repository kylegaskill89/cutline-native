#pragma once

/// The application services that need the whole stack.
///
/// Everything below this is deliberately partial: `cutline::ui` draws without
/// knowing what a project is, `cutline::editor` edits without knowing what a
/// decoder is, and both build and test with neither FFmpeg nor a GPU present.
/// This is where those halves finally meet the media layer, and it is the only
/// thing in the tree that needs all of it at once.

#include "cutline/core/model.hpp"
#include "cutline/editor/import.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/ui/painter.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::app {

/// Reads a file's structure and describes it the way the editor wants.
///
/// The one conversion between the media layer's view of a file and the
/// editor's, kept in a single function so a field added to one is a compile
/// error rather than a silently missing value.
[[nodiscard]] std::expected<editor::MediaSource, std::string> probe_source(
    std::string_view path);

/// Renders a project frame for the interface to draw.
///
/// There are two ways out, and they are not equivalent. `texture_at` leaves the
/// frame where it was rendered and hands over the texture, which costs no copy
/// at all. `frame_at` brings it down to system memory, which moves a canvas
/// across the bus and gives the interface something to upload again; it is for
/// whoever has no GPU painter to draw with — the pixel tests, and anything that
/// wants the actual bytes.
///
/// Both render one frame per call and wait for it. This is a *scrubbing*
/// preview: one frame per thing the user did, not sixty a second. Playing at
/// rate needs the frame loop to stop waiting as well, which is a change to how
/// work is queued rather than to where the pixels live.
class ProjectPreview {
 public:
  /// `device` may be null, in which case one is made. Passing one in is how the
  /// preview and the window end up on the same device — and sharing a device
  /// is the whole of what makes `texture_at` a pointer instead of a copy.
  [[nodiscard]] static std::expected<std::unique_ptr<ProjectPreview>, std::string> create(
      int canvas_width, int canvas_height, std::shared_ptr<gpu::Device> device = nullptr);

  ProjectPreview(const ProjectPreview&) = delete;
  ProjectPreview& operator=(const ProjectPreview&) = delete;
  ~ProjectPreview();

  /// Composites the project at `t` and returns a view of the result.
  ///
  /// The pixels belong to this object and stay valid until the next call, so
  /// the monitor can borrow them for a paint without a copy.
  [[nodiscard]] std::expected<ui::ImageView, std::string> frame_at(const core::Project& project,
                                                                   double t);

  /// Composites the project at `t` and hands back the texture it rendered into,
  /// without copying anything.
  ///
  /// Valid until the next call, and only drawable by a painter on this
  /// object's device — which is why `create` takes one.
  [[nodiscard]] std::expected<ui::TextureView, std::string> texture_at(
      const core::Project& project, double t);

  /// The device everything here renders on, for handing to whoever draws the
  /// result.
  [[nodiscard]] const std::shared_ptr<gpu::Device>& device() const noexcept;

  /// Matches the renderer to a sequence of a different size. Cheap and does
  /// nothing when the size already agrees.
  [[nodiscard]] std::expected<void, std::string> resize(int width, int height);

  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;

  /// Media that could not be opened while rendering the last frame. A missing
  /// file leaves a hole in the picture rather than failing the render, so this
  /// is the only way to find out.
  [[nodiscard]] const std::vector<std::string>& missing_media() const noexcept;

  /// Drops every open decoder, so the next frame opens whatever the project
  /// names now.
  ///
  /// Decoders are kept for as long as the timeline keeps asking for them, and
  /// they are keyed by media id rather than by path — which is right until a
  /// media's path changes underneath one. Relinking is the case: without this
  /// the renderer would go on decoding the file that moved, and the picture
  /// would be of something the project no longer refers to.
  void release_sources();

 private:
  struct Impl;
  explicit ProjectPreview(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::app
