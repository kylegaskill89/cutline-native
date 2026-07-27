#pragma once

/// The Direct3D 12 presentation path.
///
/// Video is decoded to YUV, converted to *linear* RGB in a shader, and drawn
/// into a 16-bit float render target. That target is then copied to the
/// swapchain, whose _SRGB view makes the hardware encode back to display space.
///
/// The float target is the point. Compositing, blending, and effects all happen
/// in linear light at high precision, which is both more correct than the old
/// 8-bit sRGB canvas and the thing that makes HDR an addition later rather than
/// a restructure.

#include "cutline/gpu/frame_view.hpp"

#include <expected>
#include <memory>
#include <string>

namespace cutline::gpu {

class Renderer {
 public:
  /// Creates a device and a swapchain for a window. `hwnd` is an HWND, passed
  /// untyped so this header does not drag in windows.h.
  [[nodiscard]] static std::expected<std::unique_ptr<Renderer>, std::string> create(
      void* hwnd, int width, int height);

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  ~Renderer();

  /// Uploads a frame and presents it. A null frame presents black, which is
  /// what an empty timeline should look like rather than stale pixels.
  [[nodiscard]] std::expected<void, std::string> render(const FrameView* frame);

  /// Recreates the swapchain buffers. Zero in either dimension is ignored, so
  /// a minimised window does not destroy the surface.
  [[nodiscard]] std::expected<void, std::string> resize(int width, int height);

  /// Blocks until the GPU has finished everything submitted so far.
  void wait_for_idle();

  /// The adapter the device was created on, for logging.
  [[nodiscard]] const std::string& adapter_name() const noexcept;

 private:
  struct Impl;
  explicit Renderer(std::unique_ptr<Impl> impl);

  /// Records the copies that put a frame's planes into GPU textures, creating
  /// or resizing them when the frame's shape changes.
  [[nodiscard]] std::expected<void, std::string> upload_frame(const FrameView& frame);

  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::gpu
