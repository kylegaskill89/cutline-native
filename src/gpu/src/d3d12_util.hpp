#pragma once

/// Direct3D 12 plumbing shared inside the GPU library. Internal: this header is
/// not installed and nothing outside `src/gpu` should include it.

#include "cutline/gpu/device.hpp"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace cutline::gpu {

using Microsoft::WRL::ComPtr;

[[nodiscard]] inline std::string hresult_string(HRESULT hr) {
  return std::format("HRESULT 0x{:08X}", static_cast<unsigned>(hr));
}

[[nodiscard]] inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
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
[[nodiscard]] inline UINT aligned_pitch(UINT bytes) {
  constexpr UINT alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  return (bytes + alignment - 1) & ~(alignment - 1);
}

/// Matches `Params` in composite.hlsl. Laid out in 16-byte rows so no member
/// straddles a boundary, which is how HLSL packs a constant buffer. Shared
/// because the compositor and the presenter use the same shader file and so
/// must agree on the root signature exactly.
struct alignas(16) ShaderParams {
  float canvas[2]{};
  float center[2]{};

  float size[2]{};
  float rotation[2]{};

  float opacity = 1.0f;
  int layout = -1;
  int color_space = 0;
  int transfer = 0;

  int full_range = 0;
  int blend = 0;
  float flip[2]{1.0f, 1.0f};

  float solid[4]{};

  float brightness = 0.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float hue_radians = 0.0f;

  float invert = 0.0f;
  float vignette = 0.0f;
  float chroma_similarity = 0.0f;
  float chroma_blend = 0.0f;

  float crop[4]{};         ///< left, top, right, bottom
  float chroma_color[4]{};  ///< rgb, then 1 when keying is on
};
static_assert(sizeof(ShaderParams) == 36 * sizeof(float),
              "root constants must match the shader's cbuffer exactly");

inline constexpr UINT kShaderParamCount = sizeof(ShaderParams) / sizeof(float);

/// The descriptor table every pass binds: three plane slots plus a backdrop.
inline constexpr UINT kSlotsPerLayer = 4;

/// Builds the root signature both the compositor and the presenter use.
[[nodiscard]] std::expected<ComPtr<ID3D12RootSignature>, std::string> create_root_signature(
    ID3D12Device* device);

[[nodiscard]] inline D3D12_HEAP_PROPERTIES heap_of(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = type;
  return heap;
}

/// The directory the running executable lives in. Compiled shaders are built
/// alongside it, so this is how they are found without hard-coding a layout.
[[nodiscard]] std::filesystem::path module_directory();

[[nodiscard]] std::expected<std::vector<std::byte>, std::string> read_file(
    const std::filesystem::path& path);

/// The device, its queue, and the single command list everything records into.
///
/// One command list, submitted and waited on per operation, is deliberate for
/// now: it makes upload aliasing impossible by construction. Overlapping frames
/// is a throughput optimisation to make once there is something to measure.
struct Device::Impl {
  ComPtr<IDXGIFactory6> factory;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> commands;

  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;

  std::string adapter;
  bool software = false;

  /// Resets the allocator and opens the command list for recording.
  [[nodiscard]] std::expected<void, std::string> begin();

  /// Closes the command list and submits it. Does not wait.
  [[nodiscard]] std::expected<void, std::string> submit();

  void wait_for_idle();

  ~Impl();
};

}  // namespace cutline::gpu
