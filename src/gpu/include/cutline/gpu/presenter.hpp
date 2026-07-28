#pragma once

/// Putting a composited scene on screen.
///
/// This is the only part of rendering that needs a window, and it does nothing
/// but encode the scene for display and letterbox it into whatever size the
/// window happens to be. All the actual drawing already happened in the
/// compositor, which is what keeps the preview and the export identical: the
/// preview is the export plus a blit.

#include "cutline/gpu/compositor.hpp"
#include "cutline/gpu/device.hpp"

#include <expected>
#include <memory>
#include <string>

namespace cutline::gpu {

class Presenter {
 public:
  /// Creates a swapchain for a window. `hwnd` is an HWND, passed untyped so
  /// this header does not drag in windows.h.
  [[nodiscard]] static std::expected<std::unique_ptr<Presenter>, std::string> create(
      std::shared_ptr<Device> device, void* hwnd, int width, int height);

  Presenter(const Presenter&) = delete;
  Presenter& operator=(const Presenter&) = delete;
  ~Presenter();

  /// Draws the compositor's scene into the window, preserving the canvas
  /// aspect ratio and filling the remainder with black.
  [[nodiscard]] std::expected<void, std::string> present(Compositor& scene);

  /// Recreates the swapchain buffers. Zero in either dimension is ignored, so
  /// a minimised window does not destroy the surface.
  [[nodiscard]] std::expected<void, std::string> resize(int width, int height);

 private:
  struct Impl;
  explicit Presenter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::gpu
