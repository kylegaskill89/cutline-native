#include "cutline/gpu/compositor.hpp"

#include "compositor_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>
#include <ranges>
#include <tuple>

namespace cutline::gpu {
namespace {

/// The linear-light scene everything composites into.
constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
/// The display encoding: sRGB-encoded bytes in a plain, fully-typed target.
///
/// Deliberately *not* `_SRGB`, though it was until the interface started
/// sampling this texture instead of copying it. The reasoning, because it is
/// not obvious and the wrong choice is not visible in a picture:
///
///   - `_SRGB` lets the hardware encode on write for nothing, which is why it
///     was chosen. But sampling an `_SRGB` texture decodes back to linear, so
///     the preview would be drawn far too dark.
///   - `TYPELESS` with an `_SRGB` view to write and a plain view to read is the
///     textbook answer, and Direct3D will not have it: a shader resource view
///     on a typeless resource is an invalid call that *removes the device*,
///     which surfaces as the window dying rather than as an error.
///
/// So the encoding moved into `PSPresent` and the target holds exactly the
/// bytes everyone wants: what `read_back` has always returned, and what a
/// sampler reads without transforming.
constexpr DXGI_FORMAT kDisplayFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

/// Render target slots.
constexpr UINT kSceneRtv = 0;
constexpr UINT kDisplayRtv = 1;
constexpr UINT kScratchRtv = 2;  // and 3, one per blur ping-pong buffer
constexpr UINT kLumaRtv = 4;
constexpr UINT kChromaRtv = 5;
constexpr UINT kHdrDisplayRtv = 6;
constexpr UINT kRtvCount = 7;

/// scRGB for an HDR display: linear and able to go past 1.0 and below 0.
constexpr DXGI_FORMAT kHdrDisplayFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

/// The encoder's planes, a byte a sample or sixteen bits.
[[nodiscard]] DXGI_FORMAT luma_format(YuvLayout layout) noexcept {
  return layout == YuvLayout::P010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
}
[[nodiscard]] DXGI_FORMAT chroma_format(YuvLayout layout) noexcept {
  return layout == YuvLayout::P010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
}

/// Taps each side of centre in one blur pass, matching BLUR_TAPS in the shader.
constexpr int kBlurTaps = 24;

/// LAYOUT_CODED: a scratch target holding coded R'G'B' with straight alpha,
/// which is what a layer becomes once its passes have run.
constexpr int kCodedLayout = 2;

/// How a layout decomposes into GPU textures. Chroma is half resolution in both
/// axes for every layout handled here.
[[nodiscard]] std::vector<PlaneDescription> describe_planes(const FrameView& frame) {
  const auto width = static_cast<UINT>(frame.width);
  const auto height = static_cast<UINT>(frame.height);
  const UINT half_width = std::max(1u, (width + 1) / 2);
  const UINT half_height = std::max(1u, (height + 1) / 2);

  // Drawn rather than decoded: one plane, four bytes, no chroma subsampling to
  // undo.
  if (frame.layout == PixelLayout::Rgba8) {
    return {{width, height, 4, DXGI_FORMAT_R8G8B8A8_UNORM}};
  }
  if (frame.layout == PixelLayout::Nv12) {
    return {
        {width, height, 1, DXGI_FORMAT_R8_UNORM},
        {half_width, half_height, 2, DXGI_FORMAT_R8G8_UNORM},
    };
  }
  if (frame.layout == PixelLayout::P010) {
    return {
        {width, height, 2, DXGI_FORMAT_R16_UNORM},
        {half_width, half_height, 4, DXGI_FORMAT_R16G16_UNORM},
    };
  }
  if (frame.layout == PixelLayout::Yuv420p16) {
    return {
        {width, height, 2, DXGI_FORMAT_R16_UNORM},
        {half_width, half_height, 2, DXGI_FORMAT_R16_UNORM},
        {half_width, half_height, 2, DXGI_FORMAT_R16_UNORM},
    };
  }
  return {
      {width, height, 1, DXGI_FORMAT_R8_UNORM},
      {half_width, half_height, 1, DXGI_FORMAT_R8_UNORM},
      {half_width, half_height, 1, DXGI_FORMAT_R8_UNORM},
  };
}

/// True when the mode needs to read what is already on the canvas. Normal and
/// Add are exactly the two the blend unit can do on its own.
[[nodiscard]] bool needs_backdrop(BlendMode mode) noexcept {
  return mode != BlendMode::Normal && mode != BlendMode::Add;
}

[[nodiscard]] int shader_layout(const Layer& layer) noexcept {
  if (layer.frame == nullptr) return -1;  // LAYOUT_SOLID
  switch (layer.frame->layout) {
    case PixelLayout::Nv12:
    case PixelLayout::P010:
      return 0;
    case PixelLayout::Yuv420p:
    case PixelLayout::Yuv420p16:
      return 1;
    case PixelLayout::Rgba8:
      // 2 is `kCodedLayout`, a scratch target, which means something else.
      return 3;
    case PixelLayout::Scene:
      // And a nested sequence is exactly that: coded R'G'B' with straight
      // alpha, which is what a scratch target holds and what the shader's
      // coded path already samples without touching.
      return kCodedLayout;
  }
  return 1;
}

[[nodiscard]] int shader_space(ColorSpace space) noexcept {
  switch (space) {
    case ColorSpace::Bt601:
      return 1;
    case ColorSpace::Bt2020:
      return 2;
    default:
      return 0;
  }
}

[[nodiscard]] int shader_transfer(TransferFunction transfer) noexcept {
  switch (transfer) {
    case TransferFunction::Smpte2084:
      return 1;
    case TransferFunction::AribStdB67:
      return 2;
    default:
      return 0;
  }
}

}  // namespace

Color Color::from_srgb(float r, float g, float b, float a) noexcept {
  const auto linearize = [](float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
  };
  return {linearize(r), linearize(g), linearize(b), a};
}

// -------------------------------------------------------------------- impl --

// ------------------------------------------------------------------ set-up --

std::expected<void, std::string> Compositor::Impl::ensure_capacity(UINT layers) {
  // One extra run of descriptors serves the present pass.
  const UINT wanted = std::max(1u, layers);
  if (srv_heap && wanted <= layer_capacity) return {};

  // Grow generously: reallocating the heap means recreating every descriptor,
  // so doing it once per size change rather than once per added layer matters.
  const UINT capacity = std::max(8u, wanted * 2);
  // No wait: every submission this compositor makes is finished before it
  // returns, so nothing in flight can be reading these descriptors. See
  // `Recorder`.

  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  // Past every layer's run: one for the present pass, two for blur scratch.
  desc.NumDescriptors = (capacity + 3) * kSlotsPerLayer;
  desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

  ComPtr<ID3D12DescriptorHeap> heap;
  if (HRESULT hr = gpu().device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)); FAILED(hr)) {
    return std::unexpected(std::format("cannot create an SRV heap: {}", hresult_string(hr)));
  }

  srv_heap = std::move(heap);
  layer_capacity = capacity;
  planes.resize(capacity);
  // Every descriptor is stale now; the plane sets must be rebuilt so their
  // views are written into the new heap.
  for (PlaneSet& set : planes) set.layout.clear();

  for (UINT i = 0; i < desc.NumDescriptors; ++i) write_null_srv(srv_cpu(i));

  // The backdrop and the scene views live at fixed offsets and can be written
  // once here rather than per compose.
  if (backdrop) {
    for (UINT i = 0; i < layer_capacity; ++i) {
      gpu().device->CreateShaderResourceView(backdrop.Get(), nullptr,
                                             srv_cpu(i * kSlotsPerLayer + 3));
    }
  }
  if (scene) {
    gpu().device->CreateShaderResourceView(scene.Get(), nullptr, srv_cpu(present_slot()));
  }
  for (int i = 0; i < 2; ++i) {
    if (scratch[i]) {
      gpu().device->CreateShaderResourceView(scratch[i].Get(), nullptr,
                                             srv_cpu(scratch_slot(i)));
    }
    // A blurred layer still composites back with its blend mode, which may need
    // the backdrop, so these runs carry it in the same slot the layer runs do.
    if (backdrop) {
      gpu().device->CreateShaderResourceView(backdrop.Get(), nullptr,
                                             srv_cpu(scratch_slot(i) + 3));
    }
  }
  return {};
}

/// Handed out to every compositor in the process, never restarted.
///
/// A count kept per compositor is unique only while there is one of them, and
/// there is not: the preview throws its renderer away and builds another
/// whenever the canvas changes size — which is what changing the preview
/// quality does. A fresh compositor counting from zero hands back generations
/// its predecessor already used, so a stale handle compares equal to a live one
/// and the guard reports "unchanged" about memory that has been freed since.
///
/// Atomic, because compositors are built on more than one thread now: the
/// preview's renderer and the source monitor's each live on a render thread of
/// their own, and two of them taking the same generation is the stale handle
/// this exists to catch.
unsigned next_generation() {
  static std::atomic<unsigned> counter = 0;
  return ++counter;
}


std::expected<void, std::string> Compositor::Impl::create_targets() {
  // The whole queue, unlike everywhere else in here. The display target is
  // sampled by the window's own drawing, which is somebody else's submission,
  // and freeing it under them is the crash the bug list describes.
  gpu().wait_for_idle();
  // Before anything is freed, so a caller that kept the old handle sees a
  // different generation even if the replacement lands at the same address.
  generation = next_generation();
  scene.Reset();
  backdrop.Reset();
  display.Reset();
  hdr_display.Reset();
  // Canvas-sized, so a resize invalidates them. They are rebuilt lazily, only
  // if a layer asks for a blur again.
  scratch[0].Reset();
  scratch[1].Reset();
  // And the encoder's planes, for the same reason and the same way.
  yuv_luma.Reset();
  yuv_chroma.Reset();

  const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_DEFAULT);

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = static_cast<UINT64>(width);
  desc.Height = static_cast<UINT>(height);
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;

  // The scene: linear light at 16-bit float, where everything is composited.
  desc.Format = kSceneFormat;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = kSceneFormat;
  if (HRESULT hr = gpu().device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
          IID_PPV_ARGS(&scene));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create the scene target: {}", hresult_string(hr)));
  }

  // The backdrop: a copy of the scene taken before a layer that needs to read
  // what is underneath it. Only the non-separable blend modes touch it.
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  if (HRESULT hr = gpu().device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&backdrop));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create the backdrop: {}", hresult_string(hr)));
  }

  // The display encoding, used for readback and as the shape the on-screen
  // presenter matches.
  desc.Format = kDisplayFormat;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  clear.Format = kDisplayFormat;
  if (HRESULT hr = gpu().device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
          IID_PPV_ARGS(&display));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create the display target: {}",
                                       hresult_string(hr)));
  }

  gpu().device->CreateRenderTargetView(scene.Get(), nullptr, rtv(kSceneRtv));
  gpu().device->CreateRenderTargetView(display.Get(), nullptr, rtv(kDisplayRtv));

  if (srv_heap) {
    for (UINT i = 0; i < layer_capacity; ++i) {
      gpu().device->CreateShaderResourceView(backdrop.Get(), nullptr,
                                             srv_cpu(i * kSlotsPerLayer + 3));
    }
    gpu().device->CreateShaderResourceView(scene.Get(), nullptr, srv_cpu(present_slot()));
    for (int i = 0; i < 2; ++i) {
      gpu().device->CreateShaderResourceView(backdrop.Get(), nullptr,
                                             srv_cpu(scratch_slot(i) + 3));
    }
  }
  return {};
}

std::expected<void, std::string> Compositor::Impl::ensure_scratch() {
  if (scratch[0] && scratch[1]) return {};

  const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_DEFAULT);

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = static_cast<UINT64>(width);
  desc.Height = static_cast<UINT>(height);
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = kSceneFormat;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_CLEAR_VALUE clear{};
  clear.Format = kSceneFormat;

  for (int i = 0; i < 2; ++i) {
    if (scratch[i]) continue;
    if (HRESULT hr = gpu().device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear, IID_PPV_ARGS(&scratch[i]));
        FAILED(hr)) {
      return std::unexpected(
          std::format("cannot create a blur target: {}", hresult_string(hr)));
    }
    gpu().device->CreateRenderTargetView(scratch[i].Get(), nullptr,
                                         rtv(kScratchRtv + static_cast<UINT>(i)));
    gpu().device->CreateShaderResourceView(scratch[i].Get(), nullptr, srv_cpu(scratch_slot(i)));
    if (backdrop) {
      gpu().device->CreateShaderResourceView(backdrop.Get(), nullptr,
                                             srv_cpu(scratch_slot(i) + 3));
    }
  }
  return {};
}

std::expected<void, std::string> Compositor::Impl::ensure_mask_points(std::size_t points) {
  const std::size_t wanted = std::max<std::size_t>(points, 1);
  if (mask_points && wanted <= mask_points_capacity) return {};


  // Grown generously and never shrunk, like the descriptor heap: a frame with
  // one more corner than the last should not reallocate.
  const std::size_t capacity = std::max<std::size_t>(64, wanted * 2);
  const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_UPLOAD);

  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = static_cast<UINT64>(capacity * sizeof(float) * 2);
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  ComPtr<ID3D12Resource> buffer;
  if (HRESULT hr = gpu().device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&buffer));
      FAILED(hr)) {
    return std::unexpected(
        std::format("cannot create the mask point buffer: {}", hresult_string(hr)));
  }

  mask_points = std::move(buffer);
  mask_points_capacity = capacity;

  // An upload heap read straight by the shader: the points are a handful of
  // floats rewritten every frame, and a copy to a default-heap buffer would
  // cost a barrier and a transfer to save a read that is already cached.
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_UNKNOWN;
  view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Buffer.NumElements = static_cast<UINT>(capacity);
  view.Buffer.StructureByteStride = sizeof(float) * 2;

  // Into every run of the table, because a path may be masking any pass of any
  // layer and the run is chosen by which layer is being drawn.
  for (UINT i = 0; i < layer_capacity + 3; ++i) {
    gpu().device->CreateShaderResourceView(mask_points.Get(), &view,
                                           srv_cpu(i * kSlotsPerLayer + 4));
  }
  return {};
}

// ------------------------------------------------------------ plane upload --

std::expected<void, std::string> Compositor::Impl::view_decoded(const FrameView& frame,
                                                                UINT layer) {
  auto* resource = static_cast<ID3D12Resource*>(frame.texture.resource);
  if (resource == nullptr) return std::unexpected("a decoded frame with no texture in it");

  // A decoder's frame pool is often one texture array with a slice per frame,
  // and sometimes a plain texture. Which it is decides how the view addresses
  // the picture, and the resource is the only thing that knows.
  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  const bool array = desc.DepthOrArraySize > 1;
  const auto slice = static_cast<UINT>(std::max(0, frame.texture.subresource));

  // A composited scene is one plane of colour, not a decoder's two. It comes
  // from a compositor on this same device, already in the state a sampler
  // wants, so there is nothing to convert and nothing to wait for beyond what
  // producing it already waited for.
  if (frame.layout == PixelLayout::Scene) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    gpu().device->CreateShaderResourceView(resource, &view, srv_cpu(layer * kSlotsPerLayer));
    // The other two keep null descriptors rather than a stale view of some
    // previous frame's plane.
    write_null_srv(srv_cpu(layer * kSlotsPerLayer + 1));
    write_null_srv(srv_cpu(layer * kSlotsPerLayer + 2));
    return {};
  }

  // NV12 is luma in plane 0 and interleaved chroma at half resolution in plane
  // 1 — the same two textures the uploading path creates, so everything after
  // this is identical whichever way the pixels arrived. P010 is the same at
  // sixteen bits.
  const bool deep = desc.Format == DXGI_FORMAT_P010;
  const DXGI_FORMAT plane_formats[2] = {deep ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
                                        deep ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM};
  for (UINT plane = 0; plane < 2; ++plane) {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = plane_formats[plane];
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (array) {
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      view.Texture2DArray.MipLevels = 1;
      view.Texture2DArray.FirstArraySlice = slice;
      view.Texture2DArray.ArraySize = 1;
      view.Texture2DArray.PlaneSlice = plane;
    } else {
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      view.Texture2D.MipLevels = 1;
      view.Texture2D.PlaneSlice = plane;
    }
    gpu().device->CreateShaderResourceView(resource, &view,
                                           srv_cpu(layer * kSlotsPerLayer + plane));
  }
  // NV12 uses two of the three slots. The third keeps a null descriptor rather
  // than a stale view of some previous frame's plane.
  write_null_srv(srv_cpu(layer * kSlotsPerLayer + 2));

  if (frame.texture.fence != nullptr) {
    decoded_waits.push_back({static_cast<ID3D12Fence*>(frame.texture.fence),
                             frame.texture.fence_value});
  }
  return {};
}

std::expected<void, std::string> Compositor::Impl::upload_planes(std::span<const Layer> layers) {
  // Work out every layer's plane textures first, then size one upload buffer
  // for the whole compose rather than one per layer.
  struct Copy {
    std::size_t layer = 0;
    std::size_t plane = 0;
    UINT64 offset = 0;
    UINT pitch = 0;
  };
  std::vector<Copy> copies;
  UINT64 total = 0;

  decoded_waits.clear();

  for (std::size_t i = 0; i < layers.size(); ++i) {
    if (layers[i].frame == nullptr) continue;
    const FrameView& frame = *layers[i].frame;
    if (frame.width <= 0 || frame.height <= 0) continue;

    // Already on this device. Views over the resource's own plane slices, no
    // copy, and a note to wait for the decoder before any of it is sampled.
    if (!frame.texture.empty()) {
      if (auto ok = view_decoded(frame, static_cast<UINT>(i)); !ok) return ok;
      // The textures this layer used to own are not wanted while a decoder is
      // supplying them, and holding them would keep a frame's worth of memory
      // per layer for nothing.
      PlaneSet& set = planes[i];
      if (!set.textures.empty()) {
        set.textures.clear();
        set.layout.clear();
      }
      continue;
    }

    const std::vector<PlaneDescription> layout = describe_planes(frame);
    PlaneSet& set = planes[i];

    const bool shape_changed =
        layout.size() != set.layout.size() ||
        !std::equal(layout.begin(), layout.end(), set.layout.begin(),
                    [](const PlaneDescription& a, const PlaneDescription& b) {
                      return a.same_shape_as(b);
                    });

    if (shape_changed) {
      set.textures.assign(layout.size(), {});
      set.layout = layout;

      const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_DEFAULT);
      for (std::size_t p = 0; p < layout.size(); ++p) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = layout[p].width;
        desc.Height = layout[p].height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = layout[p].format;
        desc.SampleDesc.Count = 1;

        if (HRESULT hr = gpu().device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&set.textures[p]));
            FAILED(hr)) {
          return std::unexpected(
              std::format("cannot create a plane texture: {}", hresult_string(hr)));
        }
        gpu().device->CreateShaderResourceView(
            set.textures[p].Get(), nullptr,
            srv_cpu(static_cast<UINT>(i) * kSlotsPerLayer + static_cast<UINT>(p)));
      }
      // Slots this layout leaves unused keep a null descriptor rather than a
      // stale view of some previous frame's plane.
      for (std::size_t p = layout.size(); p < 3; ++p) {
        write_null_srv(srv_cpu(static_cast<UINT>(i) * kSlotsPerLayer + static_cast<UINT>(p)));
      }
    }

    for (std::size_t p = 0; p < layout.size(); ++p) {
      const UINT pitch = aligned_pitch(layout[p].width * layout[p].bytes_per_pixel);
      copies.push_back({i, p, total, pitch});
      total += static_cast<UINT64>(pitch) * layout[p].height;
    }
  }

  if (total == 0) return {};

  if (upload_capacity < total) {
    upload.Reset();

    const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = total;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (HRESULT hr = gpu().device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                                           nullptr, IID_PPV_ARGS(&upload));
        FAILED(hr)) {
      return std::unexpected(
          std::format("cannot create an upload buffer: {}", hresult_string(hr)));
    }
    upload_capacity = static_cast<std::size_t>(total);
  }

  std::byte* mapped = nullptr;
  D3D12_RANGE nothing_read{0, 0};
  if (HRESULT hr = upload->Map(0, &nothing_read, reinterpret_cast<void**>(&mapped)); FAILED(hr)) {
    return std::unexpected(std::format("cannot map the upload buffer: {}", hresult_string(hr)));
  }

  for (const Copy& copy : copies) {
    const PlaneDescription& plane = planes[copy.layer].layout[copy.plane];
    const PlaneView& source = layers[copy.layer].frame->planes[copy.plane];
    const UINT row_bytes = plane.width * plane.bytes_per_pixel;

    for (UINT y = 0; y < plane.height; ++y) {
      std::byte* destination = mapped + copy.offset + static_cast<UINT64>(y) * copy.pitch;
      if (source.data != nullptr) {
        std::memcpy(destination, source.data + static_cast<std::ptrdiff_t>(y) * source.stride,
                    row_bytes);
      } else {
        std::memset(destination, 0, row_bytes);
      }
    }
  }
  upload->Unmap(0, nullptr);

  for (const Copy& copy : copies) {
    ID3D12Resource* texture = planes[copy.layer].textures[copy.plane].Get();
    const PlaneDescription& plane = planes[copy.layer].layout[copy.plane];

    auto to_copy = transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_COPY_DEST);
    recorder.list->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = copy.offset;
    source.PlacedFootprint.Footprint.Format = plane.format;
    source.PlacedFootprint.Footprint.Width = plane.width;
    source.PlacedFootprint.Footprint.Height = plane.height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = copy.pitch;

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;

    recorder.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    auto to_read = transition(texture, D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    recorder.list->ResourceBarrier(1, &to_read);
  }

  return {};
}

// ------------------------------------------------------------------ create --

Compositor::Compositor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Compositor::~Compositor() {
  if (impl_ && impl_->owner) impl_->owner->wait_for_idle();
}

int Compositor::width() const noexcept { return impl_->width; }
int Compositor::height() const noexcept { return impl_->height; }

std::expected<std::unique_ptr<Compositor>, std::string> Compositor::create(
    std::shared_ptr<Device> device, int canvas_width, int canvas_height,
    std::shared_ptr<Queue> queue) {
  if (!device) return std::unexpected("a compositor needs a device");

  auto impl = std::make_unique<Impl>();
  impl->owner = std::move(device);
  impl->width = std::max(1, canvas_width);
  impl->height = std::max(1, canvas_height);

  ID3D12Device* gpu = impl->gpu().device.Get();

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = kRtvCount;
  if (HRESULT hr = gpu->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&impl->rtv_heap));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create an RTV heap: {}", hresult_string(hr)));
  }
  impl->rtv_size = gpu->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  impl->srv_size = gpu->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  auto signature = create_root_signature(gpu);
  if (!signature) return std::unexpected(signature.error());
  impl->root_signature = std::move(*signature);

  if (auto ok = impl->recorder.create(impl->gpu(), std::move(queue)); !ok) {
    return std::unexpected(ok.error());
  }

  // ------------------------------------------------------------ pipelines --

  const std::filesystem::path shaders = module_directory();
  const auto layer_vs = read_file(shaders / "composite_layer_vs.cso");
  if (!layer_vs) return std::unexpected(layer_vs.error());
  const auto fullscreen_vs = read_file(shaders / "composite_fullscreen_vs.cso");
  if (!fullscreen_vs) return std::unexpected(fullscreen_vs.error());
  const auto layer_ps = read_file(shaders / "composite_layer_ps.cso");
  if (!layer_ps) return std::unexpected(layer_ps.error());
  const auto blend_ps = read_file(shaders / "composite_blend_ps.cso");
  if (!blend_ps) return std::unexpected(blend_ps.error());
  const auto adjustment_ps = read_file(shaders / "composite_adjustment_ps.cso");
  if (!adjustment_ps) return std::unexpected(adjustment_ps.error());
  const auto source_ps = read_file(shaders / "composite_source_ps.cso");
  if (!source_ps) return std::unexpected(source_ps.error());
  const auto adjust_source_ps = read_file(shaders / "composite_adjust_source_ps.cso");
  if (!adjust_source_ps) return std::unexpected(adjust_source_ps.error());
  const auto effect_ps = read_file(shaders / "composite_effect_ps.cso");
  if (!effect_ps) return std::unexpected(effect_ps.error());
  const auto present_ps = read_file(shaders / "composite_present_ps.cso");
  if (!present_ps) return std::unexpected(present_ps.error());
  const auto luma_ps = read_file(shaders / "composite_luma_ps.cso");
  if (!luma_ps) return std::unexpected(luma_ps.error());
  const auto chroma_ps = read_file(shaders / "composite_chroma_ps.cso");
  if (!chroma_ps) return std::unexpected(chroma_ps.error());

  const auto make_pipeline =
      [&](const std::vector<std::byte>& vs, const std::vector<std::byte>& ps, DXGI_FORMAT target,
          D3D12_BLEND source_factor, D3D12_BLEND destination_factor, bool blending,
          ComPtr<ID3D12PipelineState>& out) -> std::expected<void, std::string> {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = impl->root_signature.Get();
    desc.VS = {vs.data(), vs.size()};
    desc.PS = {ps.data(), ps.size()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    auto& target_blend = desc.BlendState.RenderTarget[0];
    target_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    target_blend.BlendEnable = blending ? TRUE : FALSE;
    target_blend.SrcBlend = source_factor;
    target_blend.DestBlend = destination_factor;
    target_blend.BlendOp = D3D12_BLEND_OP_ADD;
    // Alpha accumulates so the scene reports coverage, which matters once the
    // composite is drawn over something else.
    target_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    target_blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    target_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;

    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = target;
    desc.SampleDesc.Count = 1;

    if (HRESULT hr = gpu->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out)); FAILED(hr)) {
      return std::unexpected(std::format("cannot create a pipeline state: {}",
                                         hresult_string(hr)));
    }
    return {};
  };

  if (auto ok = make_pipeline(*layer_vs, *layer_ps, kSceneFormat, D3D12_BLEND_SRC_ALPHA,
                              D3D12_BLEND_INV_SRC_ALPHA, true, impl->pipeline_normal);
      !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = make_pipeline(*layer_vs, *layer_ps, kSceneFormat, D3D12_BLEND_SRC_ALPHA,
                              D3D12_BLEND_ONE, true, impl->pipeline_add);
      !ok) {
    return std::unexpected(ok.error());
  }
  // The shader produces the final value for these modes, so the blend unit
  // must stay out of the way.
  if (auto ok = make_pipeline(*layer_vs, *blend_ps, kSceneFormat, D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO, false, impl->pipeline_blend);
      !ok) {
    return std::unexpected(ok.error());
  }
  // Everything that writes a scratch target does so with blending off: the
  // scratch holds the layer alone, and blending against a cleared target would
  // premultiply an alpha the composite then applies again.
  if (auto ok = make_pipeline(*fullscreen_vs, *source_ps, kSceneFormat, D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO, false, impl->pipeline_source);
      !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = make_pipeline(*fullscreen_vs, *adjust_source_ps, kSceneFormat, D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO, false, impl->pipeline_adjust_source);
      !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = make_pipeline(*fullscreen_vs, *effect_ps, kSceneFormat, D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO, false, impl->pipeline_effect);
      !ok) {
    return std::unexpected(ok.error());
  }

  // Like the shader-side blend modes, an adjustment produces the final value
  // itself, so the blend unit stays out of the way.
  if (auto ok = make_pipeline(*layer_vs, *adjustment_ps, kSceneFormat, D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO, false, impl->pipeline_adjustment);
      !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = make_pipeline(*fullscreen_vs, *present_ps, kDisplayFormat, D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO, false, impl->pipeline_present);
      !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = make_pipeline(*fullscreen_vs, *present_ps, kHdrDisplayFormat, D3D12_BLEND_ONE,
                              D3D12_BLEND_ZERO, false, impl->pipeline_present_hdr);
      !ok) {
    return std::unexpected(ok.error());
  }
  using EncodePipeline =
      std::tuple<const std::vector<std::byte>*, DXGI_FORMAT, ComPtr<ID3D12PipelineState>*>;
  for (const auto& [ps, format, out] : {
           EncodePipeline{&*luma_ps, luma_format(YuvLayout::Nv12),
                          std::addressof(impl->pipeline_luma8)},
           EncodePipeline{&*chroma_ps, chroma_format(YuvLayout::Nv12),
                          std::addressof(impl->pipeline_chroma8)},
           EncodePipeline{&*luma_ps, luma_format(YuvLayout::P010),
                          std::addressof(impl->pipeline_luma16)},
           EncodePipeline{&*chroma_ps, chroma_format(YuvLayout::P010),
                          std::addressof(impl->pipeline_chroma16)},
       }) {
    if (auto ok = make_pipeline(*fullscreen_vs, *ps, format, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
                                false, *out);
        !ok) {
      return std::unexpected(ok.error());
    }
  }

  std::unique_ptr<Compositor> compositor(new Compositor(std::move(impl)));
  if (auto ok = compositor->impl_->create_targets(); !ok) return std::unexpected(ok.error());
  if (auto ok = compositor->impl_->ensure_capacity(1); !ok) return std::unexpected(ok.error());
  return compositor;
}

std::expected<void, std::string> Compositor::resize(int width, int height) {
  if (width <= 0 || height <= 0) return {};
  if (width == impl_->width && height == impl_->height) return {};

  impl_->width = width;
  impl_->height = height;
  impl_->readback.Reset();
  impl_->readback_capacity = 0;
  return impl_->create_targets();
}

// ----------------------------------------------------------------- compose --

std::expected<void, std::string> Compositor::compose(std::span<const Layer> layers) {
  Impl& d = *impl_;

  if (auto ok = d.ensure_capacity(static_cast<UINT>(layers.size())); !ok) {
    return std::unexpected(ok.error());
  }

  // Resource creation waits for the GPU, so it cannot happen once the command
  // list is open.
  // Any layer with an effect on it goes through the scratch, so the targets
  // have to exist before the command list opens - creating a resource waits for
  // the GPU, which cannot happen in the middle of recording.
  const bool any_passes = std::ranges::any_of(
      layers, [](const Layer& layer) { return !layer.passes.empty(); });
  if (any_passes) {
    if (auto ok = d.ensure_scratch(); !ok) return std::unexpected(ok.error());
  }

  // Every free-drawn mask's corners, gathered into one buffer before anything is
  // recorded. One buffer for the frame rather than one per pass, because the
  // command list is recorded once and submitted once — a buffer rewritten
  // between draws would be read by all of them with whatever it ended up
  // holding. Each pass is told where its own run starts instead.
  std::vector<std::array<float, 2>> corners;
  std::map<const EffectPass*, std::pair<UINT, UINT>> path_runs;
  for (const Layer& layer : layers) {
    for (const EffectPass& effect : layer.passes) {
      if (effect.mask.points.empty()) continue;
      const auto first = static_cast<UINT>(corners.size());
      corners.insert(corners.end(), effect.mask.points.begin(), effect.mask.points.end());
      path_runs.emplace(&effect,
                        std::pair{first, static_cast<UINT>(effect.mask.points.size())});
    }
  }
  if (!corners.empty()) {
    if (auto ok = d.ensure_mask_points(corners.size()); !ok) {
      return std::unexpected(ok.error());
    }
    void* mapped = nullptr;
    const D3D12_RANGE nothing{0, 0};
    if (HRESULT hr = d.mask_points->Map(0, &nothing, &mapped); FAILED(hr)) {
      return std::unexpected(
          std::format("cannot map the mask point buffer: {}", hresult_string(hr)));
    }
    std::memcpy(mapped, corners.data(), corners.size() * sizeof(corners[0]));
    d.mask_points->Unmap(0, nullptr);
  }
  const auto opened = d.recorder.begin();
  if (!opened) return std::unexpected(opened.error());

  // Uploads go in before any render target is bound, keeping the copies out of
  // the middle of a pass.
  if (auto ok = d.upload_planes(layers); !ok) {
    d.recorder.abandon();
    return std::unexpected(ok.error());
  }

  ID3D12GraphicsCommandList* commands = *opened;
  ID3D12DescriptorHeap* heaps[] = {d.srv_heap.Get()};
  commands->SetDescriptorHeaps(1, heaps);
  commands->SetGraphicsRootSignature(d.root_signature.Get());

  const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(d.width),
                                static_cast<float>(d.height), 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
  commands->RSSetViewports(1, &viewport);
  commands->RSSetScissorRects(1, &scissor);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  const D3D12_CPU_DESCRIPTOR_HANDLE scene_rtv = d.rtv(kSceneRtv);

  auto to_target = transition(d.scene.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
  commands->ResourceBarrier(1, &to_target);
  commands->OMSetRenderTargets(1, &scene_rtv, FALSE, nullptr);

  constexpr float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  commands->ClearRenderTargetView(scene_rtv, transparent, 0, nullptr);

  for (std::size_t i = 0; i < layers.size(); ++i) {
    const Layer& layer = layers[i];
    if (layer.opacity <= 0.0f) continue;
    if (layer.quad.width == 0.0f || layer.quad.height == 0.0f) continue;

    // Effects are passes, and any of them means the layer goes through a
    // scratch target first. An adjustment layer does too - its source is the
    // backdrop rather than a picture of its own, but everything after that is
    // the same chain.
    const bool through_passes = !layer.passes.empty();
    const bool backdrop_needed = layer.adjustment || needs_backdrop(layer.blend);
    if (backdrop_needed) {
      // Snapshot what is underneath. The copy is why these modes cost more
      // than Normal, and why only the ones that genuinely need it pay.
      const D3D12_RESOURCE_BARRIER before[] = {
          transition(d.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_COPY_SOURCE),
          transition(d.backdrop.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                     D3D12_RESOURCE_STATE_COPY_DEST),
      };
      commands->ResourceBarrier(2, before);
      commands->CopyResource(d.backdrop.Get(), d.scene.Get());

      const D3D12_RESOURCE_BARRIER after[] = {
          transition(d.scene.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                     D3D12_RESOURCE_STATE_RENDER_TARGET),
          transition(d.backdrop.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
      };
      commands->ResourceBarrier(2, after);
      commands->OMSetRenderTargets(1, &scene_rtv, FALSE, nullptr);
    }

    const float radians =
        static_cast<float>(layer.quad.rotation_deg * std::numbers::pi / 180.0);

    ShaderParams params{};
    d.colour_params(params, d.colour.working);
    params.canvas[0] = static_cast<float>(d.width);
    params.canvas[1] = static_cast<float>(d.height);
    params.center[0] = layer.quad.center_x;
    params.center[1] = layer.quad.center_y;
    params.size[0] = layer.quad.width;
    params.size[1] = layer.quad.height;
    params.rotation[0] = std::cos(radians);
    params.rotation[1] = std::sin(radians);
    params.opacity = std::clamp(layer.opacity, 0.0f, 1.0f);
    params.anti_flicker = std::clamp(layer.anti_flicker, 0.0f, 1.0f);
    params.layout = shader_layout(layer);
    params.blend = static_cast<int>(layer.blend);
    params.solid[0] = layer.color.r;
    params.solid[1] = layer.color.g;
    params.solid[2] = layer.color.b;
    params.solid[3] = layer.color.a;

    if (layer.gradient) {
      const float gradient_radians =
          static_cast<float>(layer.gradient_angle_deg * std::numbers::pi / 180.0);
      params.gradient[0] = layer.gradient_color.r;
      params.gradient[1] = layer.gradient_color.g;
      params.gradient[2] = layer.gradient_color.b;
      params.gradient[3] = 1.0f;
      params.gradient_dir[0] = std::cos(gradient_radians);
      params.gradient_dir[1] = std::sin(gradient_radians);
    }

    if (layer.frame != nullptr) {
      params.color_space = shader_space(layer.frame->space);
      params.transfer = shader_transfer(layer.frame->transfer);
      params.source_primaries = layer.frame->primaries == ColourPrimaries::Bt2020 ? 1 : 0;
      params.full_range = layer.frame->full_range ? 1 : 0;
      params.bits = static_cast<float>(std::clamp(layer.frame->bits, 8, 16));
      switch (layer.frame->layout) {
        case PixelLayout::P010:
          // Ten bits at the top of sixteen: the code is the value over 64.
          params.code_scale = 65535.0f / 64.0f;
          break;
        case PixelLayout::Yuv420p16:
          params.code_scale = 65535.0f;
          break;
        default:
          params.code_scale = 255.0f;
          break;
      }
    }

    // Which scratch target holds the layer as it stands. The chain ping-pongs
    // between the two, and this is the one to read next.
    int held = 0;

    // How much empty border the scratch needs, per side, as a fraction of it.
    //
    // A blur has to be able to spread past the layer the way Premiere's does,
    // and it can only spread into pixels that exist — so a stack that spreads
    // draws the layer smaller, into the middle of the scratch, and the
    // composite grows the quad back by the same factor.
    //
    // Sized to the reach of the widest spreading pass rather than to some fixed
    // amount, so a layer that does not spread pays nothing and one that does
    // loses exactly the resolution it needed to. Measured against the shorter
    // side, so the margin is at least that many pixels on both axes.
    //
    // The layer then occupies `span` of the scratch, which makes the blur wider
    // relative to it — so the reach it needs is `3σ·span`, and the fraction
    // solves to r/(1+2r).
    float margin = 0.0f;
    if (through_passes) {
      const auto shorter = static_cast<float>(std::min(d.width, d.height));
      float reach = 0.0f;
      for (const EffectPass& effect : layer.passes) {
        if (effect.kind != PassKind::Blur && effect.kind != PassKind::DirectionalBlur) continue;
        reach = std::max(reach, 3.0f * std::max(0.0f, effect.values[0]));
      }
      if (reach > 0.0f && shorter > 0.0f) {
        const float r = reach / shorter;
        margin = std::min(kMaxScratchMargin, r / (1.0f + 2.0f * r));
      }
    }
    const float span = 1.0f - 2.0f * margin;
    params.margin = margin;

    if (through_passes) {
      const D3D12_CPU_DESCRIPTOR_HANDLE scratch_rtv[2] = {d.rtv(kScratchRtv),
                                                          d.rtv(kScratchRtv + 1)};

      auto scratch_to_target = transition(d.scratch[0].Get(),
                                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET);
      commands->ResourceBarrier(1, &scratch_to_target);
      commands->OMSetRenderTargets(1, &scratch_rtv[0], FALSE, nullptr);
      commands->ClearRenderTargetView(scratch_rtv[0], transparent, 0, nullptr);

      // The layer into the scratch, in its own space: unrotated, unscaled,
      // filling it edge to edge. That is what gives every pass a uv running
      // 0..1 across the picture, so a crop cuts where it says it does whatever
      // the layer is doing on the canvas, and it leaves the rotation and the
      // scaling to the very end where they cost one filtering step instead of
      // one per pass.
      commands->SetPipelineState(layer.adjustment ? d.pipeline_adjust_source.Get()
                                                  : d.pipeline_source.Get());
      commands->SetGraphicsRootDescriptorTable(
          0, d.srv_gpu(static_cast<UINT>(i) * kSlotsPerLayer));
      commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &params, 0);
      commands->DrawInstanced(3, 1, 0, 0);

      // One draw per pass, and two for a blur - a separable Gaussian is two
      // passes at right angles, which is what makes a large radius affordable.
      for (const EffectPass& effect : layer.passes) {
        // A Gaussian is two draws at right angles; everything else is one,
        // including a directional blur, which is one of those axes aimed
        // somewhere the compositor works out below.
        const int draws = effect.kind == PassKind::Blur ? 2 : 1;
        for (int draw = 0; draw < draws; ++draw) {
          const int source = held;
          const int destination = 1 - held;

          ShaderParams pass = params;
          pass.pass_kind = static_cast<int>(effect.kind);
          std::copy_n(effect.values.begin(), 4, pass.pass_a);
          std::copy_n(effect.values.begin() + 4, 4, pass.pass_b);

          pass.mask_shape = effect.mask.shape;
          pass.mask_center[0] = effect.mask.x;
          pass.mask_center[1] = effect.mask.y;
          pass.mask_size[0] = effect.mask.width;
          pass.mask_size[1] = effect.mask.height;
          pass.mask_rotation[0] = effect.mask.cos_rotation;
          pass.mask_rotation[1] = effect.mask.sin_rotation;
          pass.mask_feather = effect.mask.feather;
          pass.mask_opacity = effect.mask.opacity;
          pass.mask_inverted = effect.mask.inverted;

          if (const auto run = path_runs.find(&effect); run != path_runs.end()) {
            pass.path_first = static_cast<float>(run->second.first);
            pass.path_count = static_cast<float>(run->second.second);
          }

          // How wide a tap is depends on the target's size, which is the one
          // thing the plan cannot know — so every pass that samples its
          // neighbours has its step filled in here.
          const float texel_x = 1.0f / static_cast<float>(d.width);
          const float texel_y = 1.0f / static_cast<float>(d.height);

          if (effect.kind == PassKind::Blur || effect.kind == PassKind::DirectionalBlur) {
            // Taps are spread over three sigma, where a Gaussian has
            // effectively fallen to nothing, and the stride widens to cover
            // that with a bounded number of them.
            // Scaled by the span: sigma is in the layer's own pixels, and the
            // margin made those smaller in the scratch. Without this the blur
            // would widen every time the margin grew, which is a loop.
            const float sigma = effect.values[0] * span;
            pass.pass_a[0] = sigma;
            const float radius = std::max(1.0f, 3.0f * sigma);
            const float stride = std::max(1.0f, radius / static_cast<float>(kBlurTaps));
            pass.pass_a[1] = stride;

            if (effect.kind == PassKind::Blur) {
              pass.pass_a[2] = draw == 0 ? stride * texel_x : 0.0f;
              pass.pass_a[3] = draw == 0 ? 0.0f : stride * texel_y;
            } else {
              const float angle = effect.values[4];
              pass.pass_a[2] = std::cos(angle) * stride * texel_x;
              pass.pass_a[3] = std::sin(angle) * stride * texel_y;
            }
          } else if (effect.kind == PassKind::Sharpen) {
            const float radius = std::max(0.0f, effect.values[1]) * span;
            pass.pass_a[2] = radius * texel_x;
            pass.pass_a[3] = radius * texel_y;
          }

          const D3D12_RESOURCE_BARRIER swap[] = {
              transition(d.scratch[source].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
              transition(d.scratch[destination].Get(),
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_RENDER_TARGET),
          };
          commands->ResourceBarrier(2, swap);
          commands->OMSetRenderTargets(1, &scratch_rtv[destination], FALSE, nullptr);
          commands->ClearRenderTargetView(scratch_rtv[destination], transparent, 0, nullptr);

          commands->SetPipelineState(d.pipeline_effect.Get());
          commands->SetGraphicsRootDescriptorTable(0, d.srv_gpu(d.scratch_slot(source)));
          commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &pass, 0);
          commands->DrawInstanced(3, 1, 0, 0);

          held = destination;
        }
      }

      auto result_to_resource = transition(d.scratch[held].Get(),
                                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      commands->ResourceBarrier(1, &result_to_resource);
      commands->OMSetRenderTargets(1, &scene_rtv, FALSE, nullptr);

      // The scratch holds coded values with straight alpha, and the composite
      // draw is what applies the transfer function and positions the quad.
      // Already in the sequence's own space, so nothing converts it again.
      params.layout = kCodedLayout;
      params.transfer = static_cast<int>(d.colour.working);
      params.source_primaries = d.colour.working == WorkingColour::Rec709 ? 0 : 1;
    }

    ID3D12PipelineState* pipeline =
        layer.adjustment                 ? d.pipeline_adjustment.Get()
        : backdrop_needed                ? d.pipeline_blend.Get()
        : layer.blend == BlendMode::Add  ? d.pipeline_add.Get()
                                         : d.pipeline_normal.Get();
    commands->SetPipelineState(pipeline);
    commands->SetGraphicsRootDescriptorTable(
        0, through_passes ? d.srv_gpu(d.scratch_slot(held))
                          : d.srv_gpu(static_cast<UINT>(i) * kSlotsPerLayer));
    commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &params, 0);
    commands->DrawInstanced(6, 1, 0, 0);
  }

  auto to_resource = transition(d.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  commands->ResourceBarrier(1, &to_resource);

  // Before the queue is given anything that samples them. A wait on the queue
  // costs the CPU nothing — it is the GPU that stalls, and only until the
  // decoder has finished the picture it is about to read.
  for (const Impl::DecodedWait& wait : d.decoded_waits) {
    if (wait.fence != nullptr) d.recorder.target->queue->Wait(wait.fence, wait.value);
  }

  if (auto ok = d.recorder.submit(); !ok) return std::unexpected(ok.error());
  // Waited on before returning, and not for tidiness. A hardware frame reaches
  // this through a texture its decoder shares and will overwrite with the next
  // frame, and the only thing ordering that copy after this read is that the
  // read is over by the time anybody asks for another frame. Its own fence,
  // though, not the whole queue: the interface's drawing is none of its
  // business.
  d.recorder.finish();
  return {};
}

// ----------------------------------------------------------------- display --

namespace {

/// Runs the present pass: the linear scene encoded into the display target.
///
/// Both ways out of the compositor need this and only this in common — one
/// then copies the result to the CPU, the other hands the texture over — so it
/// lives in one place rather than being written twice and drifting.
///
/// Leaves `display` in RENDER_TARGET state; the caller transitions it onward
/// to whatever it wants next.
void record_display_pass(Compositor::Impl& d, ID3D12GraphicsCommandList* commands) {
  ID3D12DescriptorHeap* heaps[] = {d.srv_heap.Get()};
  commands->SetDescriptorHeaps(1, heaps);
  commands->SetGraphicsRootSignature(d.root_signature.Get());

  const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(d.width),
                                static_cast<float>(d.height), 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
  commands->RSSetViewports(1, &viewport);
  commands->RSSetScissorRects(1, &scissor);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  auto display_to_target = transition(d.display.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
  commands->ResourceBarrier(1, &display_to_target);

  const D3D12_CPU_DESCRIPTOR_HANDLE display_rtv = d.rtv(kDisplayRtv);
  commands->OMSetRenderTargets(1, &display_rtv, FALSE, nullptr);

  ShaderParams params{};
  params.canvas[0] = static_cast<float>(d.width);
  params.canvas[1] = static_cast<float>(d.height);
  // The display is SDR for now: an HDR sequence is tone mapped onto it.
  d.colour_params(params, WorkingColour::Rec709);
  commands->SetPipelineState(d.pipeline_present.Get());
  commands->SetGraphicsRootDescriptorTable(0, d.srv_gpu(d.present_slot()));
  commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &params, 0);
  commands->DrawInstanced(3, 1, 0, 0);
}

}  // namespace

std::expected<SceneTexture, std::string> Compositor::display_texture() {
  Impl& d = *impl_;
  if (d.display == nullptr) return std::unexpected("nothing has been composited yet");

  const auto opened = d.recorder.begin();
  if (!opened) return std::unexpected(opened.error());
  ID3D12GraphicsCommandList* commands = *opened;

  record_display_pass(d, commands);

  // Straight back to a shader resource: the state it is created in, the state
  // it is left in between frames, and the state Skia will be told it is in.
  auto to_resource = transition(d.display.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  commands->ResourceBarrier(1, &to_resource);

  // And the HDR display's copy, when there is an HDR display to show it on.
  const bool hdr = d.hdr_sdr_white.has_value();
  if (hdr) {
    if (d.hdr_display == nullptr) {
      const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_DEFAULT);
      D3D12_RESOURCE_DESC desc = d.display->GetDesc();
      desc.Format = kHdrDisplayFormat;
      D3D12_CLEAR_VALUE clear{};
      clear.Format = kHdrDisplayFormat;
      if (HRESULT hr = d.gpu().device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
              &clear, IID_PPV_ARGS(&d.hdr_display));
          FAILED(hr)) {
        return std::unexpected(std::format("cannot create the HDR display target: {}",
                                           hresult_string(hr)));
      }
      d.gpu().device->CreateRenderTargetView(d.hdr_display.Get(), nullptr, d.rtv(kHdrDisplayRtv));
    }

    auto hdr_to_target = transition(d.hdr_display.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands->ResourceBarrier(1, &hdr_to_target);
    const D3D12_CPU_DESCRIPTOR_HANDLE hdr_rtv = d.rtv(kHdrDisplayRtv);
    commands->OMSetRenderTargets(1, &hdr_rtv, FALSE, nullptr);

    ShaderParams params{};
    params.canvas[0] = static_cast<float>(d.width);
    params.canvas[1] = static_cast<float>(d.height);
    d.colour_params(params, WorkingColour::Rec709);
    params.output = 3;  // OUTPUT_SCRGB
    // An SDR sequence at the desktop's SDR white, so it matches the interface
    // around it; an HDR one at its real brightness, graphics white in nits.
    const float white = d.colour.working == WorkingColour::Rec709
                            ? *d.hdr_sdr_white
                            : std::max(d.colour.graphics_white_nits, 1.0f);
    params.pass_a[0] = white / 80.0f;
    commands->SetPipelineState(d.pipeline_present_hdr.Get());
    commands->SetGraphicsRootDescriptorTable(0, d.srv_gpu(d.present_slot()));
    commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &params, 0);
    commands->DrawInstanced(3, 1, 0, 0);

    auto hdr_to_resource = transition(d.hdr_display.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->ResourceBarrier(1, &hdr_to_resource);
  }

  if (auto ok = d.recorder.submit(); !ok) return std::unexpected(ok.error());
  d.recorder.finish();

  return SceneTexture{.resource = d.display.Get(),
                      .width = d.width,
                      .height = d.height,
                      .format = static_cast<unsigned>(kDisplayFormat),
                      .state = static_cast<unsigned>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                      .generation = d.generation,
                      .hdr_resource = hdr ? d.hdr_display.Get() : nullptr,
                      .hdr_format = hdr ? static_cast<unsigned>(kHdrDisplayFormat) : 0u};
}

void Compositor::set_hdr_display(std::optional<float> sdr_white_nits) noexcept {
  impl_->hdr_sdr_white = sdr_white_nits;
}

std::expected<std::vector<float>, std::string> Compositor::read_back_hdr() {
  Impl& d = *impl_;
  if (!d.hdr_sdr_white.has_value()) return std::unexpected("no HDR display was asked for");
  if (auto shown = display_texture(); !shown) return std::unexpected(shown.error());

  const UINT row_bytes = static_cast<UINT>(d.width) * 8;
  const UINT pitch = aligned_pitch(row_bytes);
  const std::size_t needed = static_cast<std::size_t>(pitch) * d.height;

  ComPtr<ID3D12Resource> buffer;
  const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_READBACK);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = needed;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (HRESULT hr = d.gpu().device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&buffer));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a readback buffer: {}", hresult_string(hr)));
  }

  const auto opened = d.recorder.begin();
  if (!opened) return std::unexpected(opened.error());
  ID3D12GraphicsCommandList* commands = *opened;

  auto to_source = transition(d.hdr_display.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands->ResourceBarrier(1, &to_source);
  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = d.hdr_display.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = buffer.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint.Footprint.Format = kHdrDisplayFormat;
  destination.PlacedFootprint.Footprint.Width = static_cast<UINT>(d.width);
  destination.PlacedFootprint.Footprint.Height = static_cast<UINT>(d.height);
  destination.PlacedFootprint.Footprint.Depth = 1;
  destination.PlacedFootprint.Footprint.RowPitch = pitch;
  commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  auto to_resource = transition(d.hdr_display.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  commands->ResourceBarrier(1, &to_resource);

  if (auto ok = d.recorder.submit(); !ok) return std::unexpected(ok.error());
  d.recorder.finish();

  std::byte* mapped = nullptr;
  D3D12_RANGE everything{0, needed};
  if (HRESULT hr = buffer->Map(0, &everything, reinterpret_cast<void**>(&mapped)); FAILED(hr)) {
    return std::unexpected(std::format("cannot map the readback buffer: {}", hresult_string(hr)));
  }
  // Half floats to floats, by hand: one place, and not worth a library.
  const auto widen = [](std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exponent = (h >> 10) & 0x1Fu;
    const std::uint32_t mantissa = h & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
      const float tiny = static_cast<float>(mantissa) / 1024.0f / 16384.0f;
      return sign != 0 ? -tiny : tiny;
    }
    bits = exponent == 31 ? (sign | 0x7F800000u | (mantissa << 13))
                          : (sign | ((exponent + 112) << 23) | (mantissa << 13));
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof out);
    return out;
  };
  std::vector<float> pixels(static_cast<std::size_t>(d.width) * d.height * 4);
  for (int y = 0; y < d.height; ++y) {
    const auto* row = reinterpret_cast<const std::uint16_t*>(mapped + static_cast<std::size_t>(y) * pitch);
    for (int x = 0; x < d.width * 4; ++x) {
      pixels[static_cast<std::size_t>(y) * d.width * 4 + x] = widen(row[x]);
    }
  }
  D3D12_RANGE nothing{0, 0};
  buffer->Unmap(0, &nothing);
  return pixels;
}

// ---------------------------------------------------------------- readback --

std::expected<Image, std::string> Compositor::read_back() {
  Impl& d = *impl_;

  const UINT row_bytes = static_cast<UINT>(d.width) * 4;
  const UINT pitch = aligned_pitch(row_bytes);
  const std::size_t needed = static_cast<std::size_t>(pitch) * d.height;

  if (d.readback_capacity < needed) {
    d.readback.Reset();

    const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = needed;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (HRESULT hr = d.gpu().device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&d.readback));
        FAILED(hr)) {
      return std::unexpected(
          std::format("cannot create a readback buffer: {}", hresult_string(hr)));
    }
    d.readback_capacity = needed;
  }

  const auto opened = d.recorder.begin();
  if (!opened) return std::unexpected(opened.error());
  ID3D12GraphicsCommandList* commands = *opened;

  record_display_pass(d, commands);

  auto display_to_source = transition(d.display.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                      D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands->ResourceBarrier(1, &display_to_source);

  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = d.display.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  source.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = d.readback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint.Offset = 0;
  destination.PlacedFootprint.Footprint.Format = kDisplayFormat;
  destination.PlacedFootprint.Footprint.Width = static_cast<UINT>(d.width);
  destination.PlacedFootprint.Footprint.Height = static_cast<UINT>(d.height);
  destination.PlacedFootprint.Footprint.Depth = 1;
  destination.PlacedFootprint.Footprint.RowPitch = pitch;

  commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

  auto display_to_resource = transition(d.display.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  commands->ResourceBarrier(1, &display_to_resource);

  if (auto ok = d.recorder.submit(); !ok) return std::unexpected(ok.error());
  d.recorder.finish();

  std::byte* mapped = nullptr;
  D3D12_RANGE everything{0, needed};
  if (HRESULT hr = d.readback->Map(0, &everything, reinterpret_cast<void**>(&mapped));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot map the readback buffer: {}", hresult_string(hr)));
  }

  Image image;
  image.width = d.width;
  image.height = d.height;
  image.pixels.resize(static_cast<std::size_t>(row_bytes) * d.height);
  for (int y = 0; y < d.height; ++y) {
    std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * row_bytes,
                mapped + static_cast<std::size_t>(y) * pitch, row_bytes);
  }

  D3D12_RANGE nothing_written{0, 0};
  d.readback->Unmap(0, &nothing_written);
  return image;
}

// ------------------------------------------------------------- for encoding --

std::expected<void, std::string> Compositor::Impl::ensure_yuv_targets(YuvLayout layout) {
  if (yuv_luma && yuv_chroma && yuv_layout == layout) return {};
  yuv_luma.Reset();
  yuv_chroma.Reset();

  const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  desc.Width = static_cast<UINT64>(width);
  desc.Height = static_cast<UINT>(height);
  desc.Format = luma_format(layout);
  if (HRESULT hr = gpu().device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
          IID_PPV_ARGS(&yuv_luma));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create the luma plane: {}", hresult_string(hr)));
  }

  desc.Width = static_cast<UINT64>(std::max(1, width / 2));
  desc.Height = static_cast<UINT>(std::max(1, height / 2));
  desc.Format = chroma_format(layout);
  if (HRESULT hr = gpu().device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
          IID_PPV_ARGS(&yuv_chroma));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create the chroma plane: {}", hresult_string(hr)));
  }

  gpu().device->CreateRenderTargetView(yuv_luma.Get(), nullptr, rtv(kLumaRtv));
  gpu().device->CreateRenderTargetView(yuv_chroma.Get(), nullptr, rtv(kChromaRtv));
  yuv_layout = layout;
  return {};
}

void Compositor::set_colour(const ColourSettings& colour) noexcept { impl_->colour = colour; }

const ColourSettings& Compositor::colour() const noexcept { return impl_->colour; }

std::expected<std::size_t, std::string> Compositor::begin_yuv_read_back(YuvLayout layout,
                                                                          WorkingColour output) {
  Impl& d = *impl_;
  // Both planes subsample by two, so an odd edge has no chroma to go with it.
  if (d.width % 2 != 0 || d.height % 2 != 0) {
    return std::unexpected("a frame for an encoder must be an even size each way");
  }
  if (auto ok = d.ensure_yuv_targets(layout); !ok) return std::unexpected(ok.error());

  std::size_t index = 0;
  {
    std::unique_lock<std::mutex> held(d.yuv_lock);
    d.yuv_returned.wait(held, [&] {
      return std::ranges::any_of(d.yuv_slots, [](const Impl::YuvSlot& s) { return !s.lent; });
    });
    while (d.yuv_slots[index].lent) ++index;
    d.yuv_slots[index].lent = true;
  }
  Impl::YuvSlot& slot = d.yuv_slots[index];

  const UINT sample = layout == YuvLayout::P010 ? 2 : 1;
  const UINT half_width = static_cast<UINT>(d.width / 2);
  const UINT half_height = static_cast<UINT>(d.height / 2);
  const UINT luma_pitch = aligned_pitch(static_cast<UINT>(d.width) * sample);
  const UINT chroma_pitch = aligned_pitch(half_width * 2 * sample);
  const std::size_t luma_bytes = static_cast<std::size_t>(luma_pitch) * d.height;
  const std::size_t chroma_offset =
      (luma_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
      ~static_cast<std::size_t>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  const std::size_t needed = chroma_offset + static_cast<std::size_t>(chroma_pitch) * half_height;

  const auto give_back = [&] {
    const std::lock_guard<std::mutex> held(d.yuv_lock);
    slot.lent = false;
    d.yuv_returned.notify_all();
  };

  if (slot.capacity < needed) {
    slot.buffer.Reset();
    const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = needed;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (HRESULT hr = d.gpu().device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&slot.buffer));
        FAILED(hr)) {
      give_back();
      return std::unexpected(
          std::format("cannot create a readback buffer: {}", hresult_string(hr)));
    }
    slot.capacity = needed;
  }

  const auto opened = d.recorder.begin();
  if (!opened) {
    give_back();
    return std::unexpected(opened.error());
  }
  ID3D12GraphicsCommandList* commands = *opened;

  ID3D12DescriptorHeap* heaps[] = {d.srv_heap.Get()};
  commands->SetDescriptorHeaps(1, heaps);
  commands->SetGraphicsRootSignature(d.root_signature.Get());
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  commands->SetGraphicsRootDescriptorTable(0, d.srv_gpu(d.present_slot()));

  ShaderParams params{};
  params.canvas[0] = static_cast<float>(d.width);
  params.canvas[1] = static_cast<float>(d.height);
  params.layout = layout == YuvLayout::P010 ? 10 : 8;
  d.colour_params(params, output);
  commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &params, 0);

  const bool deep = layout == YuvLayout::P010;
  const auto draw = [&](ID3D12PipelineState* pipeline, UINT target, UINT w, UINT h) {
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h),
                                  0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = d.rtv(target);
    commands->OMSetRenderTargets(1, &handle, FALSE, nullptr);
    commands->SetPipelineState(pipeline);
    commands->DrawInstanced(3, 1, 0, 0);
  };
  draw(deep ? d.pipeline_luma16.Get() : d.pipeline_luma8.Get(), kLumaRtv,
       static_cast<UINT>(d.width), static_cast<UINT>(d.height));
  draw(deep ? d.pipeline_chroma16.Get() : d.pipeline_chroma8.Get(), kChromaRtv, half_width,
       half_height);

  D3D12_RESOURCE_BARRIER to_copy[] = {
      transition(d.yuv_luma.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(d.yuv_chroma.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_COPY_SOURCE)};
  commands->ResourceBarrier(2, to_copy);

  const auto copy = [&](ID3D12Resource* plane, DXGI_FORMAT format, UINT w, UINT h, UINT pitch,
                        std::size_t offset) {
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = plane;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = slot.buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Offset = offset;
    destination.PlacedFootprint.Footprint = {format, w, h, 1, pitch};
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
  };
  copy(d.yuv_luma.Get(), luma_format(layout), static_cast<UINT>(d.width),
       static_cast<UINT>(d.height), luma_pitch, 0);
  copy(d.yuv_chroma.Get(), chroma_format(layout), half_width, half_height, chroma_pitch,
       chroma_offset);

  D3D12_RESOURCE_BARRIER to_target[] = {
      transition(d.yuv_luma.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_RENDER_TARGET),
      transition(d.yuv_chroma.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                 D3D12_RESOURCE_STATE_RENDER_TARGET)};
  commands->ResourceBarrier(2, to_target);

  if (auto ok = d.recorder.submit(); !ok) {
    give_back();
    return std::unexpected(ok.error());
  }
  // Deliberately not `finish`: the copy runs while the next frame decodes, and
  // whoever collects it waits for exactly this fence and nothing later.
  slot.fence = d.recorder.submitted;
  slot.chroma_offset = chroma_offset;
  slot.shape = YuvPlanes{.width = d.width,
                         .height = d.height,
                         .layout = layout,
                         .luma_stride = luma_pitch,
                         .chroma_stride = chroma_pitch};
  return index;
}

std::expected<void, std::string> Compositor::finish_yuv_read_back(
    std::size_t index, const std::function<void(const YuvPlanes&)>& use) {
  Impl& d = *impl_;
  if (index >= d.yuv_slots.size() || !d.yuv_slots[index].lent) {
    return std::unexpected("that frame is not on its way back");
  }
  Impl::YuvSlot& slot = d.yuv_slots[index];
  d.recorder.target->wait_for(slot.fence);

  std::uint8_t* mapped = nullptr;
  const D3D12_RANGE everything{0, slot.capacity};
  std::expected<void, std::string> result;
  if (HRESULT hr = slot.buffer->Map(0, &everything, reinterpret_cast<void**>(&mapped));
      FAILED(hr)) {
    result = std::unexpected(
        std::format("cannot map the readback buffer: {}", hresult_string(hr)));
  } else {
    YuvPlanes planes = slot.shape;
    planes.luma = mapped;
    planes.chroma = mapped + slot.chroma_offset;
    use(planes);
    const D3D12_RANGE nothing_written{0, 0};
    slot.buffer->Unmap(0, &nothing_written);
  }

  const std::lock_guard<std::mutex> held(d.yuv_lock);
  slot.lent = false;
  d.yuv_returned.notify_all();
  return result;
}

}  // namespace cutline::gpu
