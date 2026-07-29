#pragma once

/// Borrowing a Direct3D texture as something Skia can draw.
///
/// Its own translation unit for one reason: `d3d12.h` drags in `windows.h`,
/// which redefines `small`, `near`, `interface` and a dozen other ordinary
/// words, and `skia_painter.cpp` is a long file full of ordinary words. The
/// painter asks for an image and never learns what a resource is.
///
/// Internal to `src/ui`; not installed.

// Skia's headers are noisy at /W4 and are not ours to keep warning-free.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "include/core/SkImage.h"
#include "include/core/SkRefCnt.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <map>

class SkCanvas;

namespace cutline::ui {

struct TextureView;

/// The Skia-side wrappers for textures somebody else owns, kept between frames.
///
/// This exists because of a specific and expensive lesson. Wrapping the
/// compositor's display target afresh on every paint — the obvious thing, since
/// the pixels change every frame and nothing appears to be worth keeping —
/// **removes the Direct3D device** after two or three frames. Each wrapper
/// tracks the resource's state independently, and tearing one down while
/// another is live emits a barrier from a state the resource is no longer in.
/// The symptom is not a wrong picture: it is `DXGI_ERROR_DEVICE_REMOVED`,
/// followed by every later allocation failing, followed by the window dying.
///
/// So a resource is wrapped once and the image is reused. The pixels behind it
/// still change every frame — that is the entire point, and it is fine, because
/// the image refers to the texture rather than to a copy of it.
///
/// Owned by whoever owns the Skia context, and destroyed before it: an image
/// outliving its context is a crash at teardown.
class TextureCache {
 public:
  TextureCache();
  ~TextureCache();

  TextureCache(const TextureCache&) = delete;
  TextureCache& operator=(const TextureCache&) = delete;

  /// The image for `frame` on whatever context `canvas` draws through, made on
  /// first sight and kept thereafter.
  ///
  /// Null when the canvas is not on the GPU — a raster surface has nothing to
  /// sample a texture with, which is the headless check and the pixel tests —
  /// and null when Skia will not take the texture.
  [[nodiscard]] sk_sp<SkImage> image_for(SkCanvas* canvas, const TextureView& frame);

  /// Forgets everything. For when the context is going away.
  void clear();

 private:
  /// Keyed by the resource address. The shape is kept alongside because a
  /// resize frees the old target and Direct3D reuses addresses freely — so the
  /// same pointer coming back with a different size means a different texture,
  /// and reusing the old image would sample freed memory.
  struct Entry {
    sk_sp<SkImage> image;
    int width = 0;
    int height = 0;
    unsigned format = 0;
    unsigned generation = 0;
  };
  std::map<void*, Entry> entries_;
};

}  // namespace cutline::ui
