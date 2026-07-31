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
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <format>
#include <string>
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

  /// One per swapchain buffer. `retired` is the fence value that will be
  /// signalled once the GPU has finished the frame drawn into it, which is
  /// what has to be waited on before drawing into it again.
  struct Buffer {
    ComPtr<ID3D12Resource> resource;
    sk_sp<SkSurface> surface;
    UINT64 retired = 0;
  };
  std::array<Buffer, kBufferCount> buffers;

  ComPtr<ID3D12Fence> fence;
  UINT64 next_fence = 1;
  HANDLE fence_event = nullptr;
  unsigned current = 0;

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
      buffer.retired = 0;
    }
  }

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

      // `PRESENT` is the state a freshly acquired back buffer is in, and Skia
      // needs to be told so it can put it back afterwards rather than guessing.
      const GrD3DTextureResourceInfo info(buffer.resource.Get(), nullptr,
                                          D3D12_RESOURCE_STATE_PRESENT, kFormat, 1, 1, 0);
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
  description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

  ComPtr<IDXGISwapChain1> chain;
  if (const HRESULT result = impl->factory->CreateSwapChainForHwnd(
          impl->queue.Get(), impl->window, &description, nullptr, nullptr, &chain);
      FAILED(result)) {
    return std::unexpected("could not make a swapchain: " + described(result));
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
          kBufferCount, static_cast<UINT>(width), static_cast<UINT>(height), kFormat, 0);
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

  // One vertical blank. The frame loop only draws when something changed, so
  // this is a wait for the screen rather than a throttle on a spinning loop.
  impl_->swapchain->Present(1, 0);

  buffer.retired = impl_->next_fence++;
  impl_->queue->Signal(impl_->fence.Get(), buffer.retired);
}

}  // namespace cutline::ui
