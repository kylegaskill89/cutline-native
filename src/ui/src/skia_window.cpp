#include "cutline/ui/skia_window.hpp"

#include "skia_texture.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/core/SkSurfaceProps.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendContext.h"
#include "include/gpu/ganesh/d3d/GrD3DBackendSurface.h"
#include "include/gpu/ganesh/d3d/GrD3DDirectContext.h"
#include "include/gpu/ganesh/d3d/GrD3DTypes.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include <utility>

namespace cutline::ui {
namespace {

using Microsoft::WRL::ComPtr;

/// Two, not three. Three buffers buy throughput when frames are produced
/// faster than they are shown, and this produces one only when something
/// changed — so the third would be memory that is never looked at.
constexpr unsigned kBufferCount = 2;

/// Matches the raster path this replaces, and is what a swapchain wants.
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

/// What the swapchain is on an HDR display: scRGB, linear, 1.0 at 80 nits.
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

/// How many monitors' pictures one frame can put through holes. Two monitors
/// is what the interface has; room for a few more costs nothing.
constexpr UINT kMaxHoles = 7;

/// The pass that puts an HDR frame together: each monitor's picture into the
/// back buffer, then the interface over it. Compiled once, when a window first
/// shows HDR, so the build needs no shader step for the sake of two tiny
/// shaders.
constexpr const char* kResolveShaders = R"(
cbuffer Quad : register(b0) {
    float4 rect;   // left, top, right, bottom in clip space
    float4 uv;     // the texture's corners
    float scale;   // SDR white over 80 nits, for the interface
    float3 pad;
};
Texture2D picture : register(t0);
SamplerState linearSampler : register(s0);

struct Out { float4 position : SV_Position; float2 uv : TEXCOORD0; };

Out VSQuad(uint id : SV_VertexID) {
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(0, 1),
                                float2(0, 1), float2(1, 0), float2(1, 1) };
    const float2 c = corners[id];
    Out o;
    o.position = float4(lerp(rect.x, rect.z, c.x), lerp(rect.y, rect.w, c.y), 0.0, 1.0);
    o.uv = float2(lerp(uv.x, uv.z, c.x), lerp(uv.y, uv.w, c.y));
    return o;
}

// A monitor's picture, already scRGB at its own brightness.
float4 PSPicture(Out i) : SV_Target {
    return float4(picture.Sample(linearSampler, i.uv).rgb, 1.0);
}

// The interface, as it was drawn: sRGB, premultiplied. Out to linear light at
// the desktop's SDR white, still premultiplied, for the blend to lay over the
// pictures where it is transparent.
float4 PSInterface(Out i) : SV_Target {
    const float4 c = picture.Load(int3(i.position.xy, 0));
    if (c.a <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    const float3 coded = saturate(c.rgb / c.a);
    // Per component, as the old compiler this goes through spells it.
    const float3 light = coded <= 0.04045 ? coded / 12.92
                                          : pow(abs(coded + 0.055) / 1.055, 2.4);
    return float4(light * scale * c.a, c.a);
}
)";

struct QuadConstants {
  float rect[4];
  float uv[4];
  float scale;
  float pad[3];
};

[[nodiscard]] std::string described(HRESULT result) {
  return std::format("Direct3D returned 0x{:08x}", static_cast<unsigned>(result));
}

/// Hands a Direct3D object to Skia, keeping a reference here as well.
///
/// `gr_cp` adopts a bare pointer rather than retaining it, and these are still
/// owned by this object, so the reference has to be added by hand. Getting this
/// wrong releases the device out from under Skia at teardown.
template <typename T>
[[nodiscard]] gr_cp<T> shared(const ComPtr<T>& pointer) {
  if (pointer) pointer->AddRef();
  return gr_cp<T>(pointer.Get());
}

[[nodiscard]] std::string narrowed(const WCHAR* text) {
  if (text == nullptr) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};

  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

}  // namespace

struct SkiaWindow::Impl {
  HWND window = nullptr;

  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGISwapChain3> swapchain;
  sk_sp<GrDirectContext> context;

  /// What puts the swapchain on the window when it is one whose transparency
  /// reaches the screen. A swapchain made for a window directly is always
  /// opaque -- Windows ignores its alpha -- so a frame that leaves somewhere
  /// clear for the glass behind it has to go through the compositor instead.
  ComPtr<IDCompositionDevice> composition;
  ComPtr<IDCompositionTarget> composition_target;
  ComPtr<IDCompositionVisual> composition_visual;

  /// One per swapchain buffer. `retired` is the fence value that will be
  /// signalled once the GPU has finished the frame drawn into it, which is
  /// what has to be waited on before drawing into it again.
  struct Buffer {
    ComPtr<ID3D12Resource> resource;
    sk_sp<SkSurface> surface;
    UINT64 retired = 0;
    /// On an HDR display: what Skia draws into instead of the swapchain's
    /// buffer, and the commands that put the frame together from it.
    ComPtr<ID3D12Resource> chrome;
    ComPtr<ID3D12CommandAllocator> allocator;
  };
  std::array<Buffer, kBufferCount> buffers;

  ComPtr<ID3D12Fence> fence;
  UINT64 next_fence = 1;
  HANDLE fence_event = nullptr;
  unsigned current = 0;

  /// The desktop's SDR white, when the window is showing HDR.
  std::optional<float> hdr_white;
  ComPtr<ID3D12RootSignature> resolve_signature;
  ComPtr<ID3D12PipelineState> resolve_picture;
  ComPtr<ID3D12PipelineState> resolve_interface;
  ComPtr<ID3D12GraphicsCommandList> resolve_list;
  /// A slot per buffer per picture, and one for the interface, so nothing is
  /// rewritten while a frame still on the card reads it.
  ComPtr<ID3D12DescriptorHeap> resolve_heap;
  UINT descriptor_size = 0;

  int width = 0;
  int height = 0;
  bool software = false;
  bool drawing = false;
  std::string adapter_name;

  /// Declared after `context` on purpose. Members are destroyed in reverse, so
  /// this goes first — and an image outliving the context it belongs to is a
  /// crash at teardown rather than a leak.
  TextureCache textures;

  ~Impl() {
    // The GPU may still be reading buffers that are about to be released.
    // Skia's own teardown does not know about the swapchain, so the wait has to
    // happen here and before anything is dropped.
    wait_for_idle();
    release_buffers();
    if (context != nullptr) context->abandonContext();
    if (fence_event != nullptr) CloseHandle(fence_event);
  }

  void wait_for_idle() {
    if (queue == nullptr || fence == nullptr || fence_event == nullptr) return;

    const UINT64 marker = next_fence++;
    if (FAILED(queue->Signal(fence.Get(), marker))) return;
    if (fence->GetCompletedValue() >= marker) return;
    if (SUCCEEDED(fence->SetEventOnCompletion(marker, fence_event))) {
      WaitForSingleObject(fence_event, INFINITE);
    }
  }

  void release_buffers() {
    for (Buffer& buffer : buffers) {
      buffer.surface.reset();
      buffer.resource.Reset();
      buffer.chrome.Reset();
      buffer.retired = 0;
    }
  }

  /// The pass that puts an HDR frame together, made the first time it is
  /// wanted.
  [[nodiscard]] std::expected<void, std::string> make_resolve() {
    if (resolve_interface != nullptr) return {};

    const auto compile = [](const char* entry, const char* target) -> std::expected<ComPtr<ID3DBlob>, std::string> {
      ComPtr<ID3DBlob> code;
      ComPtr<ID3DBlob> errors;
      if (FAILED(D3DCompile(kResolveShaders, std::strlen(kResolveShaders), "hdr", nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        return std::unexpected(std::string("the HDR pass would not compile: ") +
                               (errors ? static_cast<const char*>(errors->GetBufferPointer()) : ""));
      }
      return code;
    };
    auto vs = compile("VSQuad", "vs_5_0");
    auto picture_ps = compile("PSPicture", "ps_5_0");
    auto interface_ps = compile("PSInterface", "ps_5_0");
    if (!vs) return std::unexpected(vs.error());
    if (!picture_ps) return std::unexpected(picture_ps.error());
    if (!interface_ps) return std::unexpected(interface_ps.error());

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = sizeof(QuadConstants) / 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 2;
    signature.pParameters = parameters;
    signature.NumStaticSamplers = 1;
    signature.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> serialised;
    if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialised,
                                           nullptr)) ||
        FAILED(device->CreateRootSignature(0, serialised->GetBufferPointer(),
                                           serialised->GetBufferSize(),
                                           IID_PPV_ARGS(&resolve_signature)))) {
      return std::unexpected("could not make the HDR pass's root signature");
    }

    const auto pipeline = [&](ID3DBlob* ps, bool over, ComPtr<ID3D12PipelineState>& out) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = resolve_signature.Get();
      desc.VS = {(*vs)->GetBufferPointer(), (*vs)->GetBufferSize()};
      desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
      D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
      blend.BlendEnable = over ? TRUE : FALSE;
      // Premultiplied: the interface over whatever is under it.
      blend.SrcBlend = D3D12_BLEND_ONE;
      blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
      blend.BlendOp = D3D12_BLEND_OP_ADD;
      blend.SrcBlendAlpha = D3D12_BLEND_ONE;
      blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
      blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
      blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      desc.SampleMask = UINT_MAX;
      desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      desc.RasterizerState.DepthClipEnable = TRUE;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1;
      desc.RTVFormats[0] = kHdrFormat;
      desc.SampleDesc.Count = 1;
      return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out)));
    };
    if (!pipeline(picture_ps->Get(), false, resolve_picture) ||
        !pipeline(interface_ps->Get(), true, resolve_interface)) {
      return std::unexpected("could not make the HDR pass's pipelines");
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kBufferCount * (kMaxHoles + 1);
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&resolve_heap)))) {
      return std::unexpected("could not make the HDR pass's descriptors");
    }
    descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (Buffer& buffer : buffers) {
      if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&buffer.allocator)))) {
        return std::unexpected("could not make the HDR pass's allocators");
      }
    }
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         buffers[0].allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&resolve_list)))) {
      return std::unexpected("could not make the HDR pass's command list");
    }
    resolve_list->Close();
    return {};
  }

  /// Puts the HDR frame together in `buffer`: the pictures through their
  /// holes, then the interface over them.
  void resolve(unsigned which, const std::vector<TextureCache::HdrHole>& holes) {
    Buffer& buffer = buffers[which];
    buffer.allocator->Reset();
    resolve_list->Reset(buffer.allocator.Get(), nullptr);
    ID3D12GraphicsCommandList* list = resolve_list.Get();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &barrier);

    // A render target view on the stack's worth of heap: one, rewritten per
    // frame, in a heap of its own that the card never reads from directly.
    if (rtv_heap == nullptr) {
      D3D12_DESCRIPTOR_HEAP_DESC heap{};
      heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      heap.NumDescriptors = kBufferCount;
      device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(which) *
               device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    device->CreateRenderTargetView(buffer.resource.Get(), nullptr, rtv);
    // Clear rather than black: where the interface leaves the frame clear, the
    // glass behind the window is what shows.
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = {resolve_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(resolve_signature.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    list->RSSetViewports(1, &viewport);

    const auto slot = [&](UINT index) {
      D3D12_CPU_DESCRIPTOR_HANDLE cpu = resolve_heap->GetCPUDescriptorHandleForHeapStart();
      D3D12_GPU_DESCRIPTOR_HANDLE gpu = resolve_heap->GetGPUDescriptorHandleForHeapStart();
      const UINT offset = (which * (kMaxHoles + 1) + index) * descriptor_size;
      cpu.ptr += offset;
      gpu.ptr += offset;
      return std::pair{cpu, gpu};
    };
    const auto to_clip_x = [&](float x) { return x / static_cast<float>(width) * 2.0f - 1.0f; };
    const auto to_clip_y = [&](float y) { return 1.0f - y / static_cast<float>(height) * 2.0f; };

    list->SetPipelineState(resolve_picture.Get());
    UINT used = 0;
    for (const TextureCache::HdrHole& hole : holes) {
      if (used >= kMaxHoles || hole.texture == nullptr) break;
      const auto [cpu, gpu] = slot(used++);
      D3D12_SHADER_RESOURCE_VIEW_DESC view{};
      view.Format = static_cast<DXGI_FORMAT>(hole.format);
      view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      view.Texture2D.MipLevels = 1;
      device->CreateShaderResourceView(static_cast<ID3D12Resource*>(hole.texture), &view, cpu);
      const D3D12_RECT clip{std::max(0, hole.clip_left), std::max(0, hole.clip_top),
                            std::min(width, hole.clip_right), std::min(height, hole.clip_bottom)};
      if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
      list->RSSetScissorRects(1, &clip);
      const QuadConstants quad{{to_clip_x(hole.left), to_clip_y(hole.top), to_clip_x(hole.right),
                                to_clip_y(hole.bottom)},
                               {0.0f, 0.0f, 1.0f, 1.0f},
                               1.0f,
                               {}};
      list->SetGraphicsRootDescriptorTable(0, gpu);
      list->SetGraphicsRoot32BitConstants(1, sizeof quad / 4, &quad, 0);
      list->DrawInstanced(6, 1, 0, 0);
    }

    // The interface over all of it. Skia left it in the common state, from
    // which a shader may read it without a barrier of its own.
    const auto [cpu, gpu] = slot(kMaxHoles);
    device->CreateShaderResourceView(buffer.chrome.Get(), nullptr, cpu);
    const D3D12_RECT whole{0, 0, width, height};
    list->RSSetScissorRects(1, &whole);
    list->SetPipelineState(resolve_interface.Get());
    const QuadConstants quad{{-1.0f, 1.0f, 1.0f, -1.0f}, {0.0f, 0.0f, 1.0f, 1.0f},
                             hdr_white.value_or(80.0f) / 80.0f, {}};
    list->SetGraphicsRootDescriptorTable(0, gpu);
    list->SetGraphicsRoot32BitConstants(1, sizeof quad / 4, &quad, 0);
    list->DrawInstanced(6, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
  }

  ComPtr<ID3D12DescriptorHeap> rtv_heap;

  /// Wraps each swapchain buffer in a surface Skia can draw into.
  [[nodiscard]] std::expected<void, std::string> adopt_buffers() {
    // Sub-pixel text needs to know the geometry it is being drawn on. The
    // raster path took the default; saying so explicitly keeps the two paths
    // from rendering text differently.
    const SkSurfaceProps props{0, kRGB_H_SkPixelGeometry};

    for (unsigned i = 0; i < kBufferCount; ++i) {
      Buffer& buffer = buffers[i];
      if (const HRESULT result = swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer.resource));
          FAILED(result)) {
        return std::unexpected("could not take the swapchain's buffers: " + described(result));
      }

      // On an HDR display Skia draws into a texture of its own, exactly as it
      // would into the swapchain, and the frame is put together from it.
      ID3D12Resource* drawn_into = buffer.resource.Get();
      if (hdr_white.has_value()) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&buffer.chrome)))) {
          return std::unexpected("could not make a surface to draw the interface on");
        }
        drawn_into = buffer.chrome.Get();
      }

      // Through `shared`, like the adapter, the device and the queue above.
      // `gr_cp` **adopts** a bare pointer — the constructor takes ownership
      // without an `AddRef` — so handing it `Get()` leaves two owners with one
      // reference between them. Both release it, the count goes below zero, and
      // the debug layer says so: "DeviceChild reference counter underflow".
      //
      // This one call site was the only one that missed the rule, and it cost
      // the application every resize of every window — a resize is the only
      // place buffers are released while it keeps running. The release happens
      // inside a window procedure, where an exception is fatal by definition,
      // so what it looked like was the editor vanishing.
      GrD3DTextureResourceInfo info;
      info.fResource = shared(ComPtr<ID3D12Resource>(drawn_into));
      // `PRESENT` is the state a freshly acquired back buffer is in, and Skia
      // needs to be told so it can put it back afterwards rather than guessing.
      info.fResourceState = D3D12_RESOURCE_STATE_PRESENT;
      info.fFormat = kFormat;
      info.fSampleCount = 1;
      info.fLevelCount = 1;
      const GrBackendRenderTarget target = GrBackendRenderTargets::MakeD3D(width, height, info);

      buffer.surface = SkSurfaces::WrapBackendRenderTarget(
          context.get(), target, kTopLeft_GrSurfaceOrigin, kBGRA_8888_SkColorType, nullptr,
          &props);
      if (buffer.surface == nullptr) {
        return std::unexpected("Skia would not draw on the swapchain's buffers");
      }
      buffer.retired = 0;
    }
    return {};
  }
};

SkiaWindow::SkiaWindow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SkiaWindow::~SkiaWindow() = default;

std::expected<std::unique_ptr<SkiaWindow>, std::string> SkiaWindow::create(
    void* hwnd, int width, int height, AdoptedDevice adopted) {
  if (hwnd == nullptr) return std::unexpected("a window is needed to draw on");
  if (width <= 0 || height <= 0) return std::unexpected("a window with no size cannot be drawn");

  auto impl = std::make_unique<Impl>();
  impl->window = static_cast<HWND>(hwnd);
  impl->width = width;
  impl->height = height;

  if (const HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&impl->factory)); FAILED(result)) {
    return std::unexpected("no DXGI at all: " + described(result));
  }

  if (adopted.complete()) {
    // Handed a device rather than making one. Reference counts are taken
    // because this outlives nothing in particular and must not depend on
    // whoever lent them staying alive in a particular order.
    impl->adapter = static_cast<IDXGIAdapter1*>(adopted.adapter);
    impl->device = static_cast<ID3D12Device*>(adopted.device);
    impl->queue = static_cast<ID3D12CommandQueue*>(adopted.queue);
  } else {
    for (UINT i = 0; impl->factory->EnumAdapters1(i, &impl->adapter) != DXGI_ERROR_NOT_FOUND;
         ++i) {
      DXGI_ADAPTER_DESC1 description{};
      impl->adapter->GetDesc1(&description);

      const bool software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      if (SUCCEEDED(D3D12CreateDevice(impl->adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                      IID_PPV_ARGS(&impl->device)))) {
        impl->software = software;
        impl->adapter_name = narrowed(description.Description);
        break;
      }
      impl->adapter.Reset();
    }
    if (impl->device == nullptr) {
      return std::unexpected("no Direct3D 12 device could be created");
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (const HRESULT result =
            impl->device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&impl->queue));
        FAILED(result)) {
      return std::unexpected("could not make a command queue: " + described(result));
    }
  }

  if (impl->adapter_name.empty() && impl->adapter != nullptr) {
    DXGI_ADAPTER_DESC1 description{};
    impl->adapter->GetDesc1(&description);
    impl->adapter_name = narrowed(description.Description);
    impl->software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
  }

  GrD3DBackendContext backend;
  backend.fAdapter = shared(impl->adapter);
  backend.fDevice = shared(impl->device);
  backend.fQueue = shared(impl->queue);
  impl->context = GrDirectContexts::MakeD3D(backend);
  if (impl->context == nullptr) {
    return std::unexpected("Skia would not start on this Direct3D device");
  }

  // Flip-discard, which is the only model that does not copy the frame on the
  // way to the screen. Tearing is left off: the interface is not a game and a
  // torn menu looks like a fault.
  DXGI_SWAP_CHAIN_DESC1 description{};
  description.Width = static_cast<UINT>(width);
  description.Height = static_cast<UINT>(height);
  description.Format = kFormat;
  description.SampleDesc.Count = 1;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.BufferCount = kBufferCount;
  description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  // Through the compositor, premultiplied, so a frame can leave somewhere
  // clear and the glass behind the window shows there. Every theme but Aero
  // draws every pixel opaque, and looks exactly as it did; on a system where
  // the compositor will not have it, the window's own swapchain, opaque.
  ComPtr<IDXGISwapChain1> chain;
  description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  description.Scaling = DXGI_SCALING_STRETCH;
  if (SUCCEEDED(impl->factory->CreateSwapChainForComposition(impl->queue.Get(), &description,
                                                             nullptr, &chain)) &&
      SUCCEEDED(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&impl->composition))) &&
      SUCCEEDED(impl->composition->CreateTargetForHwnd(impl->window, TRUE,
                                                       &impl->composition_target)) &&
      SUCCEEDED(impl->composition->CreateVisual(&impl->composition_visual)) &&
      SUCCEEDED(impl->composition_visual->SetContent(chain.Get())) &&
      SUCCEEDED(impl->composition_target->SetRoot(impl->composition_visual.Get())) &&
      SUCCEEDED(impl->composition->Commit())) {
    // Bound to the window through the compositor.
  } else {
    chain.Reset();
    impl->composition_visual.Reset();
    impl->composition_target.Reset();
    impl->composition.Reset();
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Scaling = DXGI_SCALING_STRETCH;
    if (const HRESULT result = impl->factory->CreateSwapChainForHwnd(
            impl->queue.Get(), impl->window, &description, nullptr, nullptr, &chain);
        FAILED(result)) {
      return std::unexpected("could not make a swapchain: " + described(result));
    }
  }
  if (FAILED(chain.As(&impl->swapchain))) {
    return std::unexpected("this DXGI is too old for a flip-model swapchain");
  }

  // The window's own caption is drawn, and Alt+Enter would put a system one
  // back on top of it.
  impl->factory->MakeWindowAssociation(impl->window, DXGI_MWA_NO_ALT_ENTER);

  if (const HRESULT result =
          impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fence));
      FAILED(result)) {
    return std::unexpected("could not make a fence: " + described(result));
  }
  impl->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (impl->fence_event == nullptr) {
    return std::unexpected("could not make an event to wait on");
  }

  if (auto adopted_buffers = impl->adopt_buffers(); !adopted_buffers.has_value()) {
    return std::unexpected(adopted_buffers.error());
  }

  return std::unique_ptr<SkiaWindow>(new SkiaWindow(std::move(impl)));
}

std::expected<void, std::string> SkiaWindow::set_hdr(std::optional<float> sdr_white_nits) {
  Impl& d = *impl_;
  const bool was = d.hdr_white.has_value();
  const bool now = sdr_white_nits.has_value();
  d.hdr_white = sdr_white_nits;
  if (was == now) return {};  // only the white level moved, which the next frame reads

  if (now) {
    if (auto made = d.make_resolve(); !made) {
      d.hdr_white.reset();
      return std::unexpected(made.error());
    }
  }

  // The same order as a resize, for the same reasons: Skia's work out, the
  // card idle, the buffers and Skia's copies of them gone.
  if (d.context != nullptr) d.context->flushAndSubmit(GrSyncCpu::kYes);
  d.wait_for_idle();
  d.release_buffers();
  if (d.context != nullptr) {
    d.context->freeGpuResources();
    d.context->flushAndSubmit(GrSyncCpu::kYes);
  }
  d.drawing = false;

  if (const HRESULT result = d.swapchain->ResizeBuffers(kBufferCount, static_cast<UINT>(d.width),
                                                        static_cast<UINT>(d.height),
                                                        now ? kHdrFormat : kFormat, 0);
      FAILED(result)) {
    return std::unexpected("could not change the swapchain's format: " + described(result));
  }
  ComPtr<IDXGISwapChain3> chain = d.swapchain;
  chain->SetColorSpace1(now ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
  return d.adopt_buffers();
}

std::optional<float> SkiaWindow::hdr() const noexcept { return impl_->hdr_white; }

bool SkiaWindow::translucent() const noexcept { return impl_->composition != nullptr; }

std::optional<float> SkiaWindow::hdr_white_for(void* hwnd) {
  HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST);
  if (monitor == nullptr) return std::nullopt;

  // A fresh factory each time: an old one keeps describing the displays as
  // they were when it was made, and this is asked precisely because they may
  // have changed.
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::nullopt;
  bool hdr = false;
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; !hdr && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      ComPtr<IDXGIOutput6> described_output;
      DXGI_OUTPUT_DESC1 description{};
      if (SUCCEEDED(output.As(&described_output)) &&
          SUCCEEDED(described_output->GetDesc1(&description)) && description.Monitor == monitor) {
        hdr = description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        break;
      }
    }
  }
  if (!hdr) return std::nullopt;

  // Where Windows puts SDR white on this display: the "SDR content
  // brightness" slider, in units of a thousandth of 80 nits.
  MONITORINFOEXW info{};
  info.cbSize = sizeof info;
  if (!GetMonitorInfoW(monitor, &info)) return 80.0f;
  UINT32 paths = 0;
  UINT32 modes = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS) {
    return 80.0f;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
  std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(),
                         nullptr) != ERROR_SUCCESS) {
    return 80.0f;
  }
  for (UINT32 i = 0; i < paths; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof source;
    source.header.adapterId = path[i].sourceInfo.adapterId;
    source.header.id = path[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
    if (std::wstring_view(source.viewGdiDeviceName) != std::wstring_view(info.szDevice)) continue;
    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof white;
    white.header.adapterId = path[i].targetInfo.adapterId;
    white.header.id = path[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) return 80.0f;
    return static_cast<float>(white.SDRWhiteLevel) * 80.0f / 1000.0f;
  }
  return 80.0f;
}

int SkiaWindow::width() const noexcept { return impl_->width; }
int SkiaWindow::height() const noexcept { return impl_->height; }
bool SkiaWindow::is_software() const noexcept { return impl_->software; }
const std::string& SkiaWindow::adapter_name() const noexcept { return impl_->adapter_name; }
TextureCache* SkiaWindow::textures() noexcept { return &impl_->textures; }

std::expected<void, std::string> SkiaWindow::resize(int width, int height) {
  if (width <= 0 || height <= 0) return std::unexpected("a window with no size cannot be drawn");
  if (width == impl_->width && height == impl_->height) return {};

  // In this order, and every step of it matters.
  //
  // Skia first: it has recorded work against the buffers it wrapped, and that
  // work has to be handed to the GPU before anything is destroyed. Flushing
  // *after* dropping the surfaces — which is what this did — submits commands
  // that refer to objects already gone, and leaves the context holding render
  // targets for buffers the swapchain is about to replace. The window survived
  // that until the next `present`, which is exactly where it died.
  if (impl_->context != nullptr) impl_->context->flushAndSubmit(GrSyncCpu::kYes);
  // Then the GPU: it may still be reading the buffers for a frame on screen.
  impl_->wait_for_idle();
  // Then our references, and then Skia's own cached copies of them. Without
  // this the context can hand back a render target for a resource the
  // swapchain has freed, and whether that faults depends on what the driver
  // put in its place — which is why this crashed three times in five rather
  // than every time.
  impl_->release_buffers();
  if (impl_->context != nullptr) {
    impl_->context->freeGpuResources();
    impl_->context->flushAndSubmit(GrSyncCpu::kYes);
  }
  // A frame that was in flight is not in flight any more: its surface has just
  // been destroyed, so `present` must not go looking for it.
  impl_->drawing = false;

  if (const HRESULT result = impl_->swapchain->ResizeBuffers(
          kBufferCount, static_cast<UINT>(width), static_cast<UINT>(height),
          impl_->hdr_white.has_value() ? kHdrFormat : kFormat, 0);
      FAILED(result)) {
    return std::unexpected("could not resize the swapchain: " + described(result));
  }

  impl_->width = width;
  impl_->height = height;
  return impl_->adopt_buffers();
}

void* SkiaWindow::begin_frame() {
  if (impl_->swapchain == nullptr || impl_->drawing) return nullptr;

  impl_->current = impl_->swapchain->GetCurrentBackBufferIndex();
  Impl::Buffer& buffer = impl_->buffers[impl_->current];
  if (buffer.surface == nullptr) return nullptr;

  // The buffer coming back round may still be on screen. Drawing into it before
  // the GPU has finished with it is the classic way to get a frame that tears
  // or flickers between two pictures.
  if (buffer.retired != 0 && impl_->fence->GetCompletedValue() < buffer.retired) {
    if (SUCCEEDED(impl_->fence->SetEventOnCompletion(buffer.retired, impl_->fence_event))) {
      WaitForSingleObject(impl_->fence_event, INFINITE);
    }
  }

  impl_->drawing = true;
  impl_->textures.set_hdr(impl_->hdr_white.has_value());
  // Whatever the last frame left, which a frame never begun would.
  (void)impl_->textures.take_holes();
  return buffer.surface->getCanvas();
}

void SkiaWindow::flush_and_wait() {
  if (!impl_->drawing || impl_->context == nullptr) return;
  impl_->context->flush(GrFlushInfo{});
  // Synchronous on purpose. Submitting without waiting would time how long it
  // takes to *ask* for the drawing, which on a GPU is nearly free and tells
  // you nothing.
  impl_->context->submit(GrSyncCpu::kYes);
}

void SkiaWindow::present() {
  if (!impl_->drawing) return;
  impl_->drawing = false;

  Impl::Buffer& buffer = impl_->buffers[impl_->current];

  // `kPresent` is what tells Skia to leave the resource in the state the
  // swapchain needs. Flushing without it leaves the buffer as a render target
  // and the present is invalid.
  impl_->context->flush(buffer.surface.get(), SkSurfaces::BackendSurfaceAccess::kPresent,
                        GrFlushInfo{});
  impl_->context->submit();

  // On an HDR display what Skia drew is the interface, and the frame is put
  // together from it and the pictures on the same queue, after it.
  if (impl_->hdr_white.has_value()) impl_->resolve(impl_->current, impl_->textures.take_holes());

  // One vertical blank. The frame loop only draws when something changed, so
  // this is a wait for the screen rather than a throttle on a spinning loop.
  impl_->swapchain->Present(1, 0);

  buffer.retired = impl_->next_fence++;
  impl_->queue->Signal(impl_->fence.Get(), buffer.retired);
}

}  // namespace cutline::ui
