#include "skia_texture.hpp"

#include "cutline/ui/painter.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrRecordingContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendSurface.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <d3d12.h>

namespace cutline::ui {
namespace {

/// Wraps `frame` as an image on `context`. One per resource, ever — see the
/// note on `TextureCache` for what happens otherwise.
[[nodiscard]] sk_sp<SkImage> borrow(GrDirectContext* context, const TextureView& frame) {
  auto* resource = static_cast<ID3D12Resource*>(frame.texture);

  GrD3DTextureResourceInfo info;
  // `gr_cp` adopts rather than retains, so the reference has to be added by
  // hand — the same trap as the backend context in `skia_window.cpp`. Getting
  // it wrong releases the compositor's display target when the image dies.
  resource->AddRef();
  info.fResource = gr_cp<ID3D12Resource>(resource);
  info.fResourceState = static_cast<D3D12_RESOURCE_STATES>(frame.state);
  info.fFormat = static_cast<DXGI_FORMAT>(frame.format);
  info.fSampleCount = 1;
  info.fLevelCount = 1;

  const GrBackendTexture backend =
      GrBackendTextures::MakeD3D(frame.width, frame.height, info, "cutline preview");
  if (!backend.isValid()) return nullptr;

  // Opaque, not premultiplied: the composite has already been flattened over
  // black by the time it reaches the display target, so claiming it has alpha
  // would ask Skia to divide by an alpha of one for nothing.
  return SkImages::BorrowTextureFrom(context, backend, kTopLeft_GrSurfaceOrigin,
                                     kRGBA_8888_SkColorType, kOpaque_SkAlphaType, nullptr);
}

}  // namespace

TextureCache::TextureCache() = default;
TextureCache::~TextureCache() = default;

void TextureCache::clear() { entries_.clear(); }

sk_sp<SkImage> TextureCache::image_for(SkCanvas* canvas, const TextureView& frame) {
  if (canvas == nullptr || frame.empty()) return nullptr;

  // A raster surface has no context, which is not a failure: it is the headless
  // check and the pixel tests, where there is no GPU to sample with.
  GrRecordingContext* recording = canvas->recordingContext();
  if (recording == nullptr) return nullptr;
  GrDirectContext* context = GrAsDirectContext(recording);
  if (context == nullptr) return nullptr;

  Entry& entry = entries_[frame.texture];
  const bool same = entry.image != nullptr && entry.width == frame.width &&
                    entry.height == frame.height && entry.format == frame.format &&
                    entry.generation == frame.generation;
  if (!same) {
    // Dropped before the new one is made, so the old image is not alive
    // alongside a second wrapper of the same resource — which is the thing
    // that removes the device.
    entry.image.reset();
    entry.image = borrow(context, frame);
    entry.width = frame.width;
    entry.height = frame.height;
    entry.format = frame.format;
    entry.generation = frame.generation;
  }
  return entry.image;
}

}  // namespace cutline::ui
