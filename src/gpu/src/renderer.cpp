#include "cutline/gpu/renderer.hpp"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <vector>

namespace cutline::gpu {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kBackBufferCount = 2;

/// Slots in the shader-visible descriptor heap. The video pass binds a table
/// starting at slot 0; the present pass one starting at slot 3. A table is a
/// contiguous range, so the scene view needs its own run of three even though
/// only the first is read.
constexpr UINT kVideoPlaneSlot = 0;
constexpr UINT kScenePassSlot = 3;
constexpr UINT kDescriptorCount = 8;

[[nodiscard]] std::string hresult_string(HRESULT hr) {
  return std::format("HRESULT 0x{:08X}", static_cast<unsigned>(hr));
}

/// The directory the running executable lives in. Shaders are built alongside
/// it, so this is how they are found without hard-coding a build layout.
[[nodiscard]] std::filesystem::path module_directory() {
  std::wstring buffer(MAX_PATH, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}

[[nodiscard]] std::expected<std::vector<std::byte>, std::string> read_file(
    const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return std::unexpected(std::format("cannot open {}", path.string()));

  const auto size = static_cast<std::streamsize>(in.tellg());
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  in.seekg(0);
  if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
    return std::unexpected(std::format("cannot read {}", path.string()));
  }
  return bytes;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
                                                D3D12_RESOURCE_STATES from,
                                                D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.StateBefore = from;
  barrier.Transition.StateAfter = to;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return barrier;
}

/// Bytes per row once rounded up to the alignment texture copies require.
[[nodiscard]] UINT aligned_pitch(UINT bytes) {
  constexpr UINT alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  return (bytes + alignment - 1) & ~(alignment - 1);
}

struct PlaneDescription {
  UINT width = 0;
  UINT height = 0;
  UINT bytes_per_pixel = 1;
  DXGI_FORMAT format = DXGI_FORMAT_R8_UNORM;
};

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

/// Matches the constant buffer in present.hlsl.
struct ShaderParams {
  int layout = 0;
  int color_space = 0;
  int transfer = 0;
  int full_range = 0;
};

}  // namespace

struct Renderer::Impl {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGISwapChain3> swapchain;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;

  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> srv_heap;
  UINT rtv_size = 0;
  UINT srv_size = 0;

  std::array<ComPtr<ID3D12Resource>, kBackBufferCount> back_buffers;
  ComPtr<ID3D12Resource> scene;

  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> video_pipeline;
  ComPtr<ID3D12PipelineState> present_pipeline;

  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;

  // Plane textures are rebuilt whenever the frame's shape changes, which is
  // rare, so they are kept rather than reallocated per frame.
  std::vector<ComPtr<ID3D12Resource>> planes;
  std::vector<PlaneDescription> plane_layout;
  ComPtr<ID3D12Resource> upload;
  std::size_t upload_capacity = 0;

  int width = 0;
  int height = 0;
  std::string adapter;

  ~Impl() {
    if (fence_event != nullptr) CloseHandle(fence_event);
  }
};

Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Renderer::~Renderer() {
  if (impl_) wait_for_idle();
}

const std::string& Renderer::adapter_name() const noexcept { return impl_->adapter; }

std::expected<std::unique_ptr<Renderer>, std::string> Renderer::create(void* hwnd, int width,
                                                                      int height) {
  auto impl = std::make_unique<Impl>();
  impl->width = std::max(1, width);
  impl->height = std::max(1, height);

  UINT factory_flags = 0;
#ifndef NDEBUG
  // The debug layer turns silent misuse into a diagnosable message, which is
  // worth the cost while the pipeline is being built.
  {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }
#endif

  ComPtr<IDXGIFactory6> factory;
  if (HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)); FAILED(hr)) {
    return std::unexpected(std::format("cannot create a DXGI factory: {}", hresult_string(hr)));
  }

  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapterByGpuPreference(
                       i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                       IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&impl->device)))) {
      const int length =
          WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
      impl->adapter.resize(static_cast<std::size_t>(std::max(0, length - 1)));
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, impl->adapter.data(), length, nullptr,
                          nullptr);
      break;
    }
  }
  if (!impl->device) return std::unexpected("no Direct3D 12 capable adapter found");

  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (HRESULT hr = impl->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&impl->queue));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a command queue: {}", hresult_string(hr)));
  }

  DXGI_SWAP_CHAIN_DESC1 swap_desc{};
  swap_desc.BufferCount = kBackBufferCount;
  swap_desc.Width = static_cast<UINT>(impl->width);
  swap_desc.Height = static_cast<UINT>(impl->height);
  // The buffer is typeless-friendly UNORM; the render target view is _SRGB, so
  // the hardware performs the encode from linear on write.
  swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swap_desc.SampleDesc.Count = 1;

  ComPtr<IDXGISwapChain1> swapchain1;
  if (HRESULT hr = factory->CreateSwapChainForHwnd(impl->queue.Get(), static_cast<HWND>(hwnd),
                                                   &swap_desc, nullptr, nullptr, &swapchain1);
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a swapchain: {}", hresult_string(hr)));
  }
  factory->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);
  swapchain1.As(&impl->swapchain);

  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = kBackBufferCount + 1;  // back buffers plus the scene target
  if (HRESULT hr = impl->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&impl->rtv_heap));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create an RTV heap: {}", hresult_string(hr)));
  }
  impl->rtv_size =
      impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
  srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv_desc.NumDescriptors = kDescriptorCount;
  srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (HRESULT hr = impl->device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&impl->srv_heap));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create an SRV heap: {}", hresult_string(hr)));
  }
  impl->srv_size =
      impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  if (HRESULT hr = impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&impl->allocator));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a command allocator: {}",
                                       hresult_string(hr)));
  }
  if (HRESULT hr = impl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   impl->allocator.Get(), nullptr,
                                                   IID_PPV_ARGS(&impl->commands));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a command list: {}", hresult_string(hr)));
  }
  impl->commands->Close();

  if (HRESULT hr = impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fence));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a fence: {}", hresult_string(hr)));
  }
  impl->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (impl->fence_event == nullptr) return std::unexpected("cannot create a fence event");

  // ------------------------------------------------------- root signature --

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 3;
  range.BaseShaderRegister = 0;

  std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable.NumDescriptorRanges = 1;
  parameters[0].DescriptorTable.pDescriptorRanges = &range;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Four ints of colour description. Root constants avoid a per-frame upload
  // for what amounts to a handful of bytes.
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants.Num32BitValues = sizeof(ShaderParams) / sizeof(int);
  parameters[1].Constants.ShaderRegister = 0;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC root_desc{};
  root_desc.NumParameters = static_cast<UINT>(parameters.size());
  root_desc.pParameters = parameters.data();
  root_desc.NumStaticSamplers = 1;
  root_desc.pStaticSamplers = &sampler;
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> serialized;
  ComPtr<ID3DBlob> error;
  if (HRESULT hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               &serialized, &error);
      FAILED(hr)) {
    const std::string detail =
        error ? std::string(static_cast<const char*>(error->GetBufferPointer())) : "";
    return std::unexpected(
        std::format("cannot serialise the root signature: {} {}", hresult_string(hr), detail));
  }
  if (HRESULT hr = impl->device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                     serialized->GetBufferSize(),
                                                     IID_PPV_ARGS(&impl->root_signature));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create the root signature: {}",
                                       hresult_string(hr)));
  }

  // ------------------------------------------------------------ pipelines --

  const std::filesystem::path shader_dir = module_directory();
  const auto vertex = read_file(shader_dir / "present_vs.cso");
  if (!vertex) return std::unexpected(vertex.error());
  const auto video_pixel = read_file(shader_dir / "present_video_ps.cso");
  if (!video_pixel) return std::unexpected(video_pixel.error());
  const auto present_pixel = read_file(shader_dir / "present_ps.cso");
  if (!present_pixel) return std::unexpected(present_pixel.error());

  const auto make_pipeline = [&](const std::vector<std::byte>& pixel, DXGI_FORMAT target,
                                 ComPtr<ID3D12PipelineState>& out) -> std::expected<void, std::string> {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = impl->root_signature.Get();
    desc.VS = {vertex->data(), vertex->size()};
    desc.PS = {pixel.data(), pixel.size()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = target;
    desc.SampleDesc.Count = 1;

    if (HRESULT hr = impl->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out));
        FAILED(hr)) {
      return std::unexpected(std::format("cannot create a pipeline state: {}",
                                         hresult_string(hr)));
    }
    return {};
  };

  if (auto ok = make_pipeline(*video_pixel, DXGI_FORMAT_R16G16B16A16_FLOAT, impl->video_pipeline);
      !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = make_pipeline(*present_pixel, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                              impl->present_pipeline);
      !ok) {
    return std::unexpected(ok.error());
  }

  std::unique_ptr<Renderer> renderer(new Renderer(std::move(impl)));
  if (auto ok = renderer->resize(width, height); !ok) return std::unexpected(ok.error());
  return renderer;
}

void Renderer::wait_for_idle() {
  Impl& d = *impl_;
  if (!d.queue || !d.fence) return;

  const UINT64 target = ++d.fence_value;
  if (FAILED(d.queue->Signal(d.fence.Get(), target))) return;
  if (d.fence->GetCompletedValue() < target) {
    d.fence->SetEventOnCompletion(target, d.fence_event);
    WaitForSingleObject(d.fence_event, INFINITE);
  }
}

std::expected<void, std::string> Renderer::resize(int width, int height) {
  Impl& d = *impl_;
  if (width <= 0 || height <= 0) return {};  // minimised; keep what we have

  wait_for_idle();
  for (auto& buffer : d.back_buffers) buffer.Reset();
  d.scene.Reset();

  if (HRESULT hr = d.swapchain->ResizeBuffers(kBackBufferCount, static_cast<UINT>(width),
                                              static_cast<UINT>(height),
                                              DXGI_FORMAT_R8G8B8A8_UNORM, 0);
      FAILED(hr)) {
    return std::unexpected(std::format("cannot resize the swapchain: {}", hresult_string(hr)));
  }
  d.width = width;
  d.height = height;

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC rtv_desc{};
  rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

  for (UINT i = 0; i < kBackBufferCount; ++i) {
    if (HRESULT hr = d.swapchain->GetBuffer(i, IID_PPV_ARGS(&d.back_buffers[i])); FAILED(hr)) {
      return std::unexpected(std::format("cannot read a back buffer: {}", hresult_string(hr)));
    }
    d.device->CreateRenderTargetView(d.back_buffers[i].Get(), &rtv_desc, rtv);
    rtv.ptr += d.rtv_size;
  }

  // The scene target: linear light at 16-bit float, which is where everything
  // is composited before it is encoded for display.
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC scene_desc{};
  scene_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  scene_desc.Width = static_cast<UINT64>(width);
  scene_desc.Height = static_cast<UINT>(height);
  scene_desc.DepthOrArraySize = 1;
  scene_desc.MipLevels = 1;
  scene_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  scene_desc.SampleDesc.Count = 1;
  scene_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

  D3D12_CLEAR_VALUE clear{};
  clear.Format = scene_desc.Format;

  if (HRESULT hr = d.device->CreateCommittedResource(
          &heap, D3D12_HEAP_FLAG_NONE, &scene_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
          &clear, IID_PPV_ARGS(&d.scene));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create the scene target: {}", hresult_string(hr)));
  }

  d.device->CreateRenderTargetView(d.scene.Get(), nullptr, rtv);

  D3D12_CPU_DESCRIPTOR_HANDLE srv = d.srv_heap->GetCPUDescriptorHandleForHeapStart();
  srv.ptr += static_cast<SIZE_T>(kScenePassSlot) * d.srv_size;
  d.device->CreateShaderResourceView(d.scene.Get(), nullptr, srv);

  return {};
}

std::expected<void, std::string> Renderer::upload_frame(const FrameView& frame) {
  Impl& d = *impl_;
  const std::vector<PlaneDescription> layout = describe_planes(frame);

  // Rebuild the plane textures only when the frame's shape actually changes.
  const bool shape_changed =
      layout.size() != d.plane_layout.size() ||
      !std::equal(layout.begin(), layout.end(), d.plane_layout.begin(),
                  [](const PlaneDescription& a, const PlaneDescription& b) {
                    return a.width == b.width && a.height == b.height && a.format == b.format;
                  });

  if (shape_changed) {
    wait_for_idle();  // the old textures may still be in flight
    d.planes.clear();
    d.planes.resize(layout.size());
    d.plane_layout = layout;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CPU_DESCRIPTOR_HANDLE srv = d.srv_heap->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(kVideoPlaneSlot) * d.srv_size;

    for (std::size_t i = 0; i < layout.size(); ++i) {
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = layout[i].width;
      desc.Height = layout[i].height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = layout[i].format;
      desc.SampleDesc.Count = 1;

      if (HRESULT hr = d.device->CreateCommittedResource(
              &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
              nullptr, IID_PPV_ARGS(&d.planes[i]));
          FAILED(hr)) {
        return std::unexpected(std::format("cannot create a plane texture: {}",
                                           hresult_string(hr)));
      }
      d.device->CreateShaderResourceView(d.planes[i].Get(), nullptr, srv);
      srv.ptr += d.srv_size;
    }

    // A three-slot table is always bound, so any slot the layout does not use
    // still needs a valid descriptor behind it.
    for (std::size_t i = layout.size(); i < 3; ++i) {
      D3D12_SHADER_RESOURCE_VIEW_DESC null_desc{};
      null_desc.Format = DXGI_FORMAT_R8_UNORM;
      null_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      null_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      null_desc.Texture2D.MipLevels = 1;
      d.device->CreateShaderResourceView(nullptr, &null_desc, srv);
      srv.ptr += d.srv_size;
    }
  }

  // Copies read from a buffer whose rows are aligned to the API's requirement,
  // which is rarely the stride the decoder produced.
  std::vector<UINT> pitches(layout.size());
  std::vector<UINT64> offsets(layout.size());
  UINT64 total = 0;
  for (std::size_t i = 0; i < layout.size(); ++i) {
    pitches[i] = aligned_pitch(layout[i].width * layout[i].bytes_per_pixel);
    offsets[i] = total;
    total += static_cast<UINT64>(pitches[i]) * layout[i].height;
  }

  if (d.upload_capacity < total) {
    wait_for_idle();
    d.upload.Reset();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = total;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (HRESULT hr = d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&d.upload));
        FAILED(hr)) {
      return std::unexpected(std::format("cannot create an upload buffer: {}",
                                         hresult_string(hr)));
    }
    d.upload_capacity = static_cast<std::size_t>(total);
  }

  std::byte* mapped = nullptr;
  D3D12_RANGE nothing_read{0, 0};
  if (HRESULT hr = d.upload->Map(0, &nothing_read, reinterpret_cast<void**>(&mapped));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot map the upload buffer: {}", hresult_string(hr)));
  }

  for (std::size_t i = 0; i < layout.size(); ++i) {
    const PlaneView& plane = frame.planes[i];
    const UINT row_bytes = layout[i].width * layout[i].bytes_per_pixel;
    for (UINT y = 0; y < layout[i].height; ++y) {
      std::byte* destination = mapped + offsets[i] + static_cast<UINT64>(y) * pitches[i];
      if (plane.data != nullptr) {
        std::memcpy(destination, plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride,
                    row_bytes);
      } else {
        std::memset(destination, 0, row_bytes);
      }
    }
  }
  d.upload->Unmap(0, nullptr);

  for (std::size_t i = 0; i < layout.size(); ++i) {
    auto to_copy = transition(d.planes[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_COPY_DEST);
    d.commands->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = d.upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = offsets[i];
    source.PlacedFootprint.Footprint.Format = layout[i].format;
    source.PlacedFootprint.Footprint.Width = layout[i].width;
    source.PlacedFootprint.Footprint.Height = layout[i].height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = pitches[i];

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = d.planes[i].Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = 0;

    d.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    auto to_read = transition(d.planes[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    d.commands->ResourceBarrier(1, &to_read);
  }

  return {};
}

std::expected<void, std::string> Renderer::render(const FrameView* frame) {
  Impl& d = *impl_;

  d.allocator->Reset();
  d.commands->Reset(d.allocator.Get(), nullptr);

  const bool have_frame = frame != nullptr && frame->width > 0 && frame->height > 0;
  // Uploading before any render target is bound keeps the copies out of the
  // middle of a pass.
  if (have_frame) {
    if (auto ok = upload_frame(*frame); !ok) {
      d.commands->Close();
      return std::unexpected(ok.error());
    }
  }

  ID3D12DescriptorHeap* heaps[] = {d.srv_heap.Get()};
  d.commands->SetDescriptorHeaps(1, heaps);
  d.commands->SetGraphicsRootSignature(d.root_signature.Get());

  D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height),
                          0.0f, 1.0f};
  D3D12_RECT scissor{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
  d.commands->RSSetViewports(1, &viewport);
  d.commands->RSSetScissorRects(1, &scissor);
  d.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  D3D12_CPU_DESCRIPTOR_HANDLE scene_rtv = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  scene_rtv.ptr += static_cast<SIZE_T>(kBackBufferCount) * d.rtv_size;

  // ------------------------------------------------- pass one: into linear --

  auto to_target = transition(d.scene.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
  d.commands->ResourceBarrier(1, &to_target);
  d.commands->OMSetRenderTargets(1, &scene_rtv, FALSE, nullptr);

  constexpr float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  d.commands->ClearRenderTargetView(scene_rtv, black, 0, nullptr);

  if (have_frame) {
    D3D12_GPU_DESCRIPTOR_HANDLE table = d.srv_heap->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(kVideoPlaneSlot) * d.srv_size;

    const ShaderParams params{
        .layout = frame->layout == PixelLayout::Nv12 ? 0 : 1,
        .color_space = frame->space == ColorSpace::Bt601   ? 1
                       : frame->space == ColorSpace::Bt2020 ? 2
                                                            : 0,
        .transfer = frame->transfer == TransferFunction::Smpte2084    ? 1
                    : frame->transfer == TransferFunction::AribStdB67 ? 2
                                                                     : 0,
        .full_range = frame->full_range ? 1 : 0,
    };

    d.commands->SetPipelineState(d.video_pipeline.Get());
    d.commands->SetGraphicsRootDescriptorTable(0, table);
    d.commands->SetGraphicsRoot32BitConstants(1, sizeof(ShaderParams) / sizeof(int), &params, 0);
    d.commands->DrawInstanced(3, 1, 0, 0);
  }

  auto to_resource = transition(d.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  d.commands->ResourceBarrier(1, &to_resource);

  // ------------------------------------------ pass two: encode for display --

  const UINT index = d.swapchain->GetCurrentBackBufferIndex();
  auto back_to_target = transition(d.back_buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
  d.commands->ResourceBarrier(1, &back_to_target);

  D3D12_CPU_DESCRIPTOR_HANDLE back_rtv = d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  back_rtv.ptr += static_cast<SIZE_T>(index) * d.rtv_size;
  d.commands->OMSetRenderTargets(1, &back_rtv, FALSE, nullptr);

  D3D12_GPU_DESCRIPTOR_HANDLE scene_table = d.srv_heap->GetGPUDescriptorHandleForHeapStart();
  scene_table.ptr += static_cast<UINT64>(kScenePassSlot) * d.srv_size;

  d.commands->SetPipelineState(d.present_pipeline.Get());
  d.commands->SetGraphicsRootDescriptorTable(0, scene_table);
  d.commands->DrawInstanced(3, 1, 0, 0);

  auto back_to_present = transition(d.back_buffers[index].Get(),
                                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PRESENT);
  d.commands->ResourceBarrier(1, &back_to_present);

  if (HRESULT hr = d.commands->Close(); FAILED(hr)) {
    return std::unexpected(std::format("cannot close the command list: {}", hresult_string(hr)));
  }

  ID3D12CommandList* lists[] = {d.commands.Get()};
  d.queue->ExecuteCommandLists(1, lists);

  if (HRESULT hr = d.swapchain->Present(1, 0); FAILED(hr)) {
    return std::unexpected(std::format("present failed: {}", hresult_string(hr)));
  }

  // One frame in flight. A debug viewport does not need the complexity of a
  // ring, and a full sync makes upload aliasing impossible by construction.
  wait_for_idle();
  return {};
}

}  // namespace cutline::gpu
