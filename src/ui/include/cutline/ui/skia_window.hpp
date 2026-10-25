#pragma once

/// A window's pixels, drawn on the GPU.
///
/// The interface was drawn into a CPU raster surface and blitted with GDI, and
/// for a while that was the right call: a frame is a few hundred rounded
/// rectangles and it only happens when something changes. Measurement decided
/// otherwise. A frame costs 9ms at 1080p under the flat theme and 23ms at 4K,
/// and the themes that need a blur behind their chrome are far worse than that
/// — Aero was well over a second a frame, because every glass surface wants its
/// own layer over the backdrop and the CPU has to fill each one by hand. Those
/// are the operations a GPU does for nothing.
///
/// Nothing above this changes. `SkiaPainter` draws into whatever canvas it is
/// given and does not know or care where the pixels live, which is the whole
/// reason this could be swapped underneath a finished interface.
///
/// Windows only, and Direct3D 12 specifically, because that is what the
/// compositor already uses. Everything is passed untyped so that d3d12.h — and
/// through it windows.h, which redefines `small`, `near`, `interface` and a
/// dozen other perfectly ordinary words — stays inside this library.

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace cutline::ui {

class TextureCache;

/// An existing Direct3D device to draw on rather than making another.
///
/// The compositor already has one. Two devices on one machine means a video
/// frame has to be copied or shared before the interface can show it, so
/// handing this one over is what eventually lets a decoded frame reach the
/// screen without going anywhere near the CPU. All three or none: a device
/// without its queue is no use.
struct AdoptedDevice {
  void* adapter = nullptr;  ///< IDXGIAdapter1*
  void* device = nullptr;   ///< ID3D12Device*
  void* queue = nullptr;    ///< ID3D12CommandQueue*

  [[nodiscard]] bool complete() const noexcept {
    return adapter != nullptr && device != nullptr && queue != nullptr;
  }
};

class SkiaWindow {
 public:
  /// `hwnd` is an `HWND`. Fails rather than falling back to the CPU: whoever
  /// asked for this can decide what to do without a window that silently
  /// performs a tenth as well as it looks like it should.
  [[nodiscard]] static std::expected<std::unique_ptr<SkiaWindow>, std::string> create(
      void* hwnd, int width, int height, AdoptedDevice adopted = {});

  SkiaWindow(const SkiaWindow&) = delete;
  SkiaWindow& operator=(const SkiaWindow&) = delete;
  ~SkiaWindow();

  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;

  /// Resizes the swapchain. Waits for the GPU first, because the buffers being
  /// replaced may still be being read.
  [[nodiscard]] std::expected<void, std::string> resize(int width, int height);

  /// The `SkCanvas*` to draw this frame into, or null when the swapchain is
  /// not usable. Waits until the GPU has finished with the buffer being handed
  /// back, so drawing into it cannot race a frame still on screen.
  [[nodiscard]] void* begin_frame();

  /// Hands the frame to the compositor. Must follow a `begin_frame` that
  /// returned a canvas.
  void present();

  /// Where wrappers for other people's GPU textures are kept, to be handed to
  /// `SkiaPainter::create`.
  ///
  /// It lives here because it has to be destroyed before the Skia context, and
  /// this is what owns that. Never null.
  [[nodiscard]] TextureCache* textures() noexcept;

  /// Issues the drawing recorded so far and waits for the GPU to finish it,
  /// without presenting anything.
  ///
  /// For measuring, and only for measuring. A frame's real cost cannot be
  /// timed through `present`, which waits for the display and so reports the
  /// refresh rate whatever the drawing cost. Nothing in the running
  /// application calls this: waiting for the GPU is the one thing a frame loop
  /// should never do.
  void flush_and_wait();

  /// Whether this ended up on Microsoft's software rasteriser, which is what a
  /// machine with no usable adapter gets. Worth being able to say out loud: it
  /// works and it is slow, and those look the same from the outside.
  [[nodiscard]] bool is_software() const noexcept;

  /// The adapter it is running on, for saying so.
  [[nodiscard]] const std::string& adapter_name() const noexcept;

  /// Shows HDR, with SDR white at this many nits, or not.
  ///
  /// On an HDR display the swapchain becomes sixteen-bit float scRGB. The
  /// interface is still drawn exactly as before, into an ordinary surface, and
  /// put on screen at the desktop's SDR white so it looks the same as every
  /// other window; a monitor's picture goes underneath it at its own
  /// brightness, through the hole the painter leaves. Nothing changes on an
  /// SDR display, where this is never asked for.
  [[nodiscard]] std::expected<void, std::string> set_hdr(std::optional<float> sdr_white_nits);
  [[nodiscard]] std::optional<float> hdr() const noexcept;

  /// Whether what a frame leaves clear reaches the screen clear, so whatever
  /// is behind the window -- the system's glass, when it is asked for -- shows
  /// there. True when the swapchain went through the compositor; false on a
  /// system where only the window's own, opaque, swapchain could be made.
  ///
  /// The window has to have been made without a redirection bitmap for this to
  /// mean anything: with one, Windows keeps an opaque copy behind it.
  [[nodiscard]] bool translucent() const noexcept;

  /// Whether the display `hwnd` is mostly on is showing HDR, and if so where
  /// its SDR white is in nits -- the brightness Windows' own slider sets for
  /// SDR content. Nothing for an SDR display, or when it cannot be told.
  [[nodiscard]] static std::optional<float> hdr_white_for(void* hwnd);

 private:
  struct Impl;
  explicit SkiaWindow(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::ui
