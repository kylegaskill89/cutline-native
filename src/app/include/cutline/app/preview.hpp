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

/// Renders a project frame and keeps it on the CPU for the interface to draw.
///
/// Read-back stalls the GPU, so this is a *scrubbing* preview: one frame per
/// thing the user did, not sixty a second. Playing back at rate wants the
/// frame never to leave the GPU, which is a different architecture — Skia on
/// the same D3D12 device as the compositor — and a deliberate later step
/// rather than something this is pretending to be.
class ProjectPreview {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<ProjectPreview>, std::string> create(
      int canvas_width, int canvas_height);

  ProjectPreview(const ProjectPreview&) = delete;
  ProjectPreview& operator=(const ProjectPreview&) = delete;
  ~ProjectPreview();

  /// Composites the project at `t` and returns a view of the result.
  ///
  /// The pixels belong to this object and stay valid until the next call, so
  /// the monitor can borrow them for a paint without a copy.
  [[nodiscard]] std::expected<ui::ImageView, std::string> frame_at(const core::Project& project,
                                                                   double t);

  /// Matches the renderer to a sequence of a different size. Cheap and does
  /// nothing when the size already agrees.
  [[nodiscard]] std::expected<void, std::string> resize(int width, int height);

  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;

  /// Media that could not be opened while rendering the last frame. A missing
  /// file leaves a hole in the picture rather than failing the render, so this
  /// is the only way to find out.
  [[nodiscard]] const std::vector<std::string>& missing_media() const noexcept;

 private:
  struct Impl;
  explicit ProjectPreview(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::app
