#pragma once

/// The compositor's Direct3D state, in a header only because the presenter
/// needs to read the scene texture. Internal to `src/gpu`.

#include "cutline/gpu/compositor.hpp"

#include "d3d12_util.hpp"

#include <span>
#include <vector>

namespace cutline::gpu {

/// A plane of a decoded frame as a GPU texture.
struct PlaneDescription {
  UINT width = 0;
  UINT height = 0;
  UINT bytes_per_pixel = 1;
  DXGI_FORMAT format = DXGI_FORMAT_R8_UNORM;

  [[nodiscard]] bool same_shape_as(const PlaneDescription& other) const noexcept {
    return width == other.width && height == other.height && format == other.format;
  }
};

struct Compositor::Impl {
  std::shared_ptr<Device> owner;

  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> pipeline_normal;
  ComPtr<ID3D12PipelineState> pipeline_add;
  ComPtr<ID3D12PipelineState> pipeline_blend;
  ComPtr<ID3D12PipelineState> pipeline_adjustment;
  /// The layer drawn with blending off, into a scratch target, which is how
  /// a blurred layer is captured before it is filtered.
  ComPtr<ID3D12PipelineState> pipeline_raw;
  ComPtr<ID3D12PipelineState> pipeline_blur;
  ComPtr<ID3D12PipelineState> pipeline_present;

  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> srv_heap;
  UINT rtv_size = 0;
  UINT srv_size = 0;
  UINT layer_capacity = 0;

  ComPtr<ID3D12Resource> scene;
  ComPtr<ID3D12Resource> backdrop;
  /// Ping-pong targets for the separable blur. Allocated only when a layer
  /// actually asks for one, because they are canvas-sized.
  ComPtr<ID3D12Resource> scratch[2];
  ComPtr<ID3D12Resource> display;
  ComPtr<ID3D12Resource> readback;
  std::size_t readback_capacity = 0;

  /// Plane textures per layer slot, kept between composes because reallocating
  /// them every frame would dominate the cost of drawing.
  struct PlaneSet {
    std::vector<ComPtr<ID3D12Resource>> textures;
    std::vector<PlaneDescription> layout;
  };
  std::vector<PlaneSet> planes;

  ComPtr<ID3D12Resource> upload;
  std::size_t upload_capacity = 0;

  int width = 0;
  int height = 0;

  [[nodiscard]] Device::Impl& gpu() noexcept { return owner->internals(); }

  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT index) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * rtv_size;
    return handle;
  }

  [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(UINT index) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * srv_size;
    return handle;
  }

  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(UINT index) const noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = srv_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * srv_size;
    return handle;
  }

  /// The descriptor run reserved for the present pass, past every layer's.
  [[nodiscard]] UINT present_slot() const noexcept { return layer_capacity * kSlotsPerLayer; }

  /// The run of descriptors that binds one blur scratch target as t0. Two more
  /// runs sit past the present pass's, one per ping-pong buffer.
  [[nodiscard]] UINT scratch_slot(int which) const noexcept {
    return (layer_capacity + 1 + static_cast<UINT>(which)) * kSlotsPerLayer;
  }

  /// Writes a descriptor that reads as transparent black, so a table slot the
  /// current layout does not use still has something valid behind it.
  void write_null_srv(D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    gpu().device->CreateShaderResourceView(nullptr, &desc, where);
  }

  [[nodiscard]] std::expected<void, std::string> ensure_capacity(UINT layers);
  [[nodiscard]] std::expected<void, std::string> create_targets();
  [[nodiscard]] std::expected<void, std::string> upload_planes(std::span<const Layer> layers);
  /// Creates the blur scratch targets on first use.
  [[nodiscard]] std::expected<void, std::string> ensure_scratch();
};

}  // namespace cutline::gpu
