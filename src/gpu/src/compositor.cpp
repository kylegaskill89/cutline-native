#include "cutline/gpu/compositor.hpp"

#include "compositor_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace cutline::gpu {
namespace {

/// The linear-light scene everything composites into.
constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
/// The display encoding. An _SRGB view makes the hardware encode on write.
constexpr DXGI_FORMAT kDisplayFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

/// Render target slots.
constexpr UINT kSceneRtv = 0;
constexpr UINT kDisplayRtv = 1;
constexpr UINT kRtvCount = 2;

/// How a layout decomposes into GPU textures. Chroma is half resolution in both
/// axes for every layout handled here.
[[nodiscard]] std::vector<PlaneDescription> describe_planes(const FrameView& frame) {
  const auto width = static_cast<UINT>(frame.width);
  const auto height = static_cast<UINT>(frame.height);
  const UINT half_width = std::max(1u, (width + 1) / 2);
  const UINT half_height = std::max(1u, (height + 1) / 2);

  if (frame.layout == PixelLayout::Nv12) {
    return {
        {width, height, 1, DXGI_FORMAT_R8_UNORM},
        {half_width, half_height, 2, DXGI_FORMAT_R8G8_UNORM},
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
  return layer.frame->layout == PixelLayout::Nv12 ? 0 : 1;
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
  gpu().wait_for_idle();  // descriptors may still be referenced in flight

  D3D12_DESCRIPTOR_HEAP_DESC desc{};
  desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  desc.NumDescriptors = (capacity + 1) * kSlotsPerLayer;
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
  return {};
}

std::expected<void, std::string> Compositor::Impl::create_targets() {
  gpu().wait_for_idle();
  scene.Reset();
  backdrop.Reset();
  display.Reset();

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
  }
  return {};
}

// ------------------------------------------------------------ plane upload --

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

  for (std::size_t i = 0; i < layers.size(); ++i) {
    if (layers[i].frame == nullptr) continue;
    const FrameView& frame = *layers[i].frame;
    if (frame.width <= 0 || frame.height <= 0) continue;

    const std::vector<PlaneDescription> layout = describe_planes(frame);
    PlaneSet& set = planes[i];

    const bool shape_changed =
        layout.size() != set.layout.size() ||
        !std::equal(layout.begin(), layout.end(), set.layout.begin(),
                    [](const PlaneDescription& a, const PlaneDescription& b) {
                      return a.same_shape_as(b);
                    });

    if (shape_changed) {
      gpu().wait_for_idle();  // the old textures may still be in flight
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
    gpu().wait_for_idle();
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
    gpu().commands->ResourceBarrier(1, &to_copy);

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

    gpu().commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    auto to_read = transition(texture, D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gpu().commands->ResourceBarrier(1, &to_read);
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
    std::shared_ptr<Device> device, int canvas_width, int canvas_height) {
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
  const auto present_ps = read_file(shaders / "composite_present_ps.cso");
  if (!present_ps) return std::unexpected(present_ps.error());

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
  if (auto ok = d.gpu().begin(); !ok) return std::unexpected(ok.error());

  // Uploads go in before any render target is bound, keeping the copies out of
  // the middle of a pass.
  if (auto ok = d.upload_planes(layers); !ok) {
    d.gpu().commands->Close();
    return std::unexpected(ok.error());
  }

  ID3D12GraphicsCommandList* commands = d.gpu().commands.Get();
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
    params.canvas[0] = static_cast<float>(d.width);
    params.canvas[1] = static_cast<float>(d.height);
    params.center[0] = layer.quad.center_x;
    params.center[1] = layer.quad.center_y;
    params.size[0] = layer.quad.width;
    params.size[1] = layer.quad.height;
    params.rotation[0] = std::cos(radians);
    params.rotation[1] = std::sin(radians);
    params.opacity = std::clamp(layer.opacity, 0.0f, 1.0f);
    params.layout = shader_layout(layer);
    params.flip[0] = layer.flip_x ? -1.0f : 1.0f;
    params.flip[1] = layer.flip_y ? -1.0f : 1.0f;
    params.blend = static_cast<int>(layer.blend);
    params.solid[0] = layer.color.r;
    params.solid[1] = layer.color.g;
    params.solid[2] = layer.color.b;
    params.solid[3] = layer.color.a;

    const LayerEffects& effects = layer.effects;
    params.brightness = effects.brightness;
    params.contrast = effects.contrast;
    params.saturation = effects.saturation;
    params.hue_radians = static_cast<float>(effects.hue_degrees * std::numbers::pi / 180.0);
    params.invert = effects.invert ? 1.0f : 0.0f;
    params.vignette = effects.vignette;
    params.crop[0] = effects.crop_left;
    params.crop[1] = effects.crop_top;
    params.crop[2] = effects.crop_right;
    params.crop[3] = effects.crop_bottom;
    params.chroma_similarity = effects.chroma_similarity;
    params.chroma_blend = effects.chroma_blend;
    params.chroma_color[0] = effects.chroma_color.r;
    params.chroma_color[1] = effects.chroma_color.g;
    params.chroma_color[2] = effects.chroma_color.b;
    // The shader keys off this rather than a separate flag, which keeps the
    // whole decision inside one float4.
    params.chroma_color[3] = effects.chroma_key ? 1.0f : 0.0f;

    if (layer.frame != nullptr) {
      params.color_space = shader_space(layer.frame->space);
      params.transfer = shader_transfer(layer.frame->transfer);
      params.full_range = layer.frame->full_range ? 1 : 0;
    }

    ID3D12PipelineState* pipeline =
        layer.adjustment                 ? d.pipeline_adjustment.Get()
        : backdrop_needed                ? d.pipeline_blend.Get()
        : layer.blend == BlendMode::Add  ? d.pipeline_add.Get()
                                         : d.pipeline_normal.Get();
    commands->SetPipelineState(pipeline);
    commands->SetGraphicsRootDescriptorTable(0,
                                             d.srv_gpu(static_cast<UINT>(i) * kSlotsPerLayer));
    commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &params, 0);
    commands->DrawInstanced(6, 1, 0, 0);
  }

  auto to_resource = transition(d.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  commands->ResourceBarrier(1, &to_resource);

  if (auto ok = d.gpu().submit(); !ok) return std::unexpected(ok.error());
  d.gpu().wait_for_idle();
  return {};
}

// ---------------------------------------------------------------- readback --

std::expected<Image, std::string> Compositor::read_back() {
  Impl& d = *impl_;

  const UINT row_bytes = static_cast<UINT>(d.width) * 4;
  const UINT pitch = aligned_pitch(row_bytes);
  const std::size_t needed = static_cast<std::size_t>(pitch) * d.height;

  if (d.readback_capacity < needed) {
    d.gpu().wait_for_idle();
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

  if (auto ok = d.gpu().begin(); !ok) return std::unexpected(ok.error());
  ID3D12GraphicsCommandList* commands = d.gpu().commands.Get();

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
  commands->SetPipelineState(d.pipeline_present.Get());
  commands->SetGraphicsRootDescriptorTable(0, d.srv_gpu(d.present_slot()));
  commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &params, 0);
  commands->DrawInstanced(3, 1, 0, 0);

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

  if (auto ok = d.gpu().submit(); !ok) return std::unexpected(ok.error());
  d.gpu().wait_for_idle();

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

}  // namespace cutline::gpu
