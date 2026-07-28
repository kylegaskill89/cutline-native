#include "cutline/gpu/presenter.hpp"

#include "compositor_internal.hpp"

#include <algorithm>
#include <array>

namespace cutline::gpu {
namespace {

constexpr UINT kBackBufferCount = 2;

/// The swapchain buffer is UNORM and its render target view is _SRGB, so the
/// hardware encodes from linear on write.
constexpr DXGI_FORMAT kBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kViewFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

/// The largest rectangle of the given aspect that fits in the window, centred.
/// Letterboxing rather than stretching is not cosmetic: a program monitor that
/// distorts the frame is lying about what will be exported.
struct Fitted {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

[[nodiscard]] Fitted fit(int window_w, int window_h, int canvas_w, int canvas_h) {
  if (canvas_w <= 0 || canvas_h <= 0 || window_w <= 0 || window_h <= 0) {
    return {0.0f, 0.0f, static_cast<float>(std::max(0, window_w)),
            static_cast<float>(std::max(0, window_h))};
  }

  const double scale = std::min(static_cast<double>(window_w) / canvas_w,
                                static_cast<double>(window_h) / canvas_h);
  const auto width = static_cast<float>(canvas_w * scale);
  const auto height = static_cast<float>(canvas_h * scale);
  return {(static_cast<float>(window_w) - width) * 0.5f,
          (static_cast<float>(window_h) - height) * 0.5f, width, height};
}

}  // namespace

struct Presenter::Impl {
  std::shared_ptr<Device> owner;

  ComPtr<IDXGISwapChain3> swapchain;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> pipeline;

  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> srv_heap;
  UINT rtv_size = 0;

  std::array<ComPtr<ID3D12Resource>, kBackBufferCount> back_buffers;

  int width = 0;
  int height = 0;

  [[nodiscard]] Device::Impl& gpu() noexcept { return owner->internals(); }
};

Presenter::Presenter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Presenter::~Presenter() {
  if (impl_ && impl_->owner) impl_->owner->wait_for_idle();
}

std::expected<std::unique_ptr<Presenter>, std::string> Presenter::create(
    std::shared_ptr<Device> device, void* hwnd, int width, int height) {
  if (!device) return std::unexpected("a presenter needs a device");

  auto impl = std::make_unique<Impl>();
  impl->owner = std::move(device);
  impl->width = std::max(1, width);
  impl->height = std::max(1, height);

  Device::Impl& gpu = impl->gpu();

  DXGI_SWAP_CHAIN_DESC1 swap_desc{};
  swap_desc.BufferCount = kBackBufferCount;
  swap_desc.Width = static_cast<UINT>(impl->width);
  swap_desc.Height = static_cast<UINT>(impl->height);
  swap_desc.Format = kBufferFormat;
  swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swap_desc.SampleDesc.Count = 1;

  ComPtr<IDXGISwapChain1> swapchain1;
  if (HRESULT hr = gpu.factory->CreateSwapChainForHwnd(gpu.queue.Get(), static_cast<HWND>(hwnd),
                                                       &swap_desc, nullptr, nullptr, &swapchain1);
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a swapchain: {}", hresult_string(hr)));
  }
  gpu.factory->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);
  if (HRESULT hr = swapchain1.As(&impl->swapchain); FAILED(hr)) {
    return std::unexpected(std::format("swapchain is too old: {}", hresult_string(hr)));
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = kBackBufferCount;
  if (HRESULT hr = gpu.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&impl->rtv_heap));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create an RTV heap: {}", hresult_string(hr)));
  }
  impl->rtv_size = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  // One table's worth: only the first slot is read, but a descriptor table is
  // a contiguous range and the shared root signature declares four.
  D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
  srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv_desc.NumDescriptors = kSlotsPerLayer;
  srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (HRESULT hr = gpu.device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&impl->srv_heap));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create an SRV heap: {}", hresult_string(hr)));
  }

  auto signature = create_root_signature(gpu.device.Get());
  if (!signature) return std::unexpected(signature.error());
  impl->root_signature = std::move(*signature);

  const std::filesystem::path shaders = module_directory();
  const auto vertex = read_file(shaders / "composite_fullscreen_vs.cso");
  if (!vertex) return std::unexpected(vertex.error());
  const auto pixel = read_file(shaders / "composite_present_ps.cso");
  if (!pixel) return std::unexpected(pixel.error());

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = impl->root_signature.Get();
  desc.VS = {vertex->data(), vertex->size()};
  desc.PS = {pixel->data(), pixel->size()};
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.DepthStencilState.DepthEnable = FALSE;
  desc.DepthStencilState.StencilEnable = FALSE;
  desc.SampleMask = UINT_MAX;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = kViewFormat;
  desc.SampleDesc.Count = 1;

  if (HRESULT hr = gpu.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&impl->pipeline));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a pipeline state: {}", hresult_string(hr)));
  }

  std::unique_ptr<Presenter> presenter(new Presenter(std::move(impl)));
  if (auto ok = presenter->resize(width, height); !ok) return std::unexpected(ok.error());
  return presenter;
}

std::expected<void, std::string> Presenter::resize(int width, int height) {
  Impl& d = *impl_;
  if (width <= 0 || height <= 0) return {};  // minimised; keep what we have

  d.owner->wait_for_idle();
  for (auto& buffer : d.back_buffers) buffer.Reset();

  if (HRESULT hr = d.swapchain->ResizeBuffers(kBackBufferCount, static_cast<UINT>(width),
                                              static_cast<UINT>(height), kBufferFormat, 0);
      FAILED(hr)) {
    return std::unexpected(std::format("cannot resize the swapchain: {}", hresult_string(hr)));
  }
  d.width = width;
  d.height = height;

  D3D12_RENDER_TARGET_VIEW_DESC view{};
  view.Format = kViewFormat;
  view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  for (UINT i = 0; i < kBackBufferCount; ++i) {
    if (HRESULT hr = d.swapchain->GetBuffer(i, IID_PPV_ARGS(&d.back_buffers[i])); FAILED(hr)) {
      return std::unexpected(std::format("cannot read a back buffer: {}", hresult_string(hr)));
    }
    d.gpu().device->CreateRenderTargetView(d.back_buffers[i].Get(), &view, rtv);
    rtv.ptr += d.rtv_size;
  }
  return {};
}

std::expected<void, std::string> Presenter::present(Compositor& scene) {
  Impl& d = *impl_;

  // Written every frame rather than cached: the compositor may have resized
  // and replaced its scene texture, and a stale view would sample freed memory.
  d.gpu().device->CreateShaderResourceView(scene.internals().scene.Get(), nullptr,
                                           d.srv_heap->GetCPUDescriptorHandleForHeapStart());

  if (auto ok = d.gpu().begin(); !ok) return std::unexpected(ok.error());
  ID3D12GraphicsCommandList* commands = d.gpu().commands.Get();

  const UINT index = d.swapchain->GetCurrentBackBufferIndex();
  auto to_target = transition(d.back_buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
  commands->ResourceBarrier(1, &to_target);

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += static_cast<SIZE_T>(index) * d.rtv_size;
  commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

  // Clearing first is what makes the letterbox bars black rather than whatever
  // the discarded buffer held.
  constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  commands->ClearRenderTargetView(rtv, black, 0, nullptr);

  const Fitted area = fit(d.width, d.height, scene.width(), scene.height());
  const D3D12_VIEWPORT viewport{area.x, area.y, area.width, area.height, 0.0f, 1.0f};
  const D3D12_RECT scissor{static_cast<LONG>(area.x), static_cast<LONG>(area.y),
                           static_cast<LONG>(area.x + area.width),
                           static_cast<LONG>(area.y + area.height)};
  commands->RSSetViewports(1, &viewport);
  commands->RSSetScissorRects(1, &scissor);
  commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  ID3D12DescriptorHeap* heaps[] = {d.srv_heap.Get()};
  commands->SetDescriptorHeaps(1, heaps);
  commands->SetGraphicsRootSignature(d.root_signature.Get());

  ShaderParams params{};
  params.canvas[0] = static_cast<float>(d.width);
  params.canvas[1] = static_cast<float>(d.height);

  commands->SetPipelineState(d.pipeline.Get());
  commands->SetGraphicsRootDescriptorTable(0, d.srv_heap->GetGPUDescriptorHandleForHeapStart());
  commands->SetGraphicsRoot32BitConstants(1, kShaderParamCount, &params, 0);
  commands->DrawInstanced(3, 1, 0, 0);

  auto to_present = transition(d.back_buffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_PRESENT);
  commands->ResourceBarrier(1, &to_present);

  if (auto ok = d.gpu().submit(); !ok) return std::unexpected(ok.error());

  if (HRESULT hr = d.swapchain->Present(1, 0); FAILED(hr)) {
    return std::unexpected(std::format("present failed: {}", hresult_string(hr)));
  }

  // One frame in flight, fully synchronised. Overlapping frames is a
  // throughput optimisation to make once there is something to measure.
  d.owner->wait_for_idle();
  return {};
}

}  // namespace cutline::gpu
