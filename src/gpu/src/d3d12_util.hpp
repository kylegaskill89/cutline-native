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
  int pass_kind = 0;
  float pass_pad = 0.0f;

  float solid[4]{};

  float gradient[4]{};      ///< linear rgb of the far stop; w is 1 when set
  float gradient_dir[2]{};  ///< cos, sin of the gradient angle
  float gradient_pad[2]{};

  /// Whatever the pass is. Eight floats, shared by every kind because only one
  /// runs at a time — which is what took the ceiling off the catalogue: an
  /// effect no longer needs a permanent field here.
  float pass_a[4]{};
  float pass_b[4]{};

  /// Where the pass applies. Zero shape is everywhere, which is what nearly
  /// every pass says and costs one comparison to answer.
  float mask_shape = 0.0f;
  float mask_center[2]{0.5f, 0.5f};
  float mask_feather = 0.0f;

  float mask_size[2]{0.25f, 0.25f};
  float mask_rotation[2]{1.0f, 0.0f};

  float mask_opacity = 1.0f;
  float mask_inverted = 0.0f;
  /// How much of the scratch is empty border, per side, as a fraction of it.
  /// Zero for everything that is not a layer being drawn through the passes.
  float margin = 0.0f;
  /// A free-drawn path's corners: where they start in the shared point buffer,
  /// and how many. A path is the one mask too big for the root constants, which
  /// is the whole reason that buffer exists.
  float path_first = 0.0f;
  float path_count = 0.0f;
  float path_pad[2]{};  ///< to the next sixteen-byte row, which HLSL requires
};

/// The most of the scratch a margin may take, per side.
///
/// A margin is resolution the layer does not get, so an enormous blur has to
/// stop asking for more at some point and start being clipped instead. A fifth
/// each side leaves the layer three fifths of the scratch, which is the point
/// where the softness it buys stops being worth the sharpness it costs.
inline constexpr float kMaxScratchMargin = 0.2f;
static_assert(sizeof(ShaderParams) == 52 * sizeof(float),
              "root constants must match the shader's cbuffer exactly");

inline constexpr UINT kShaderParamCount = sizeof(ShaderParams) / sizeof(float);

/// The descriptor table every pass binds: three plane slots, a backdrop, and
/// the shared buffer of mask-path corners.
inline constexpr UINT kSlotsPerLayer = 5;

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
  /// Kept rather than dropped after the device is made, because Skia wants the
  /// adapter as well as the device in order to share this one. `IDXGIAdapter1`
  /// specifically, including for WARP, because that is the type Skia's backend
  /// context holds and handing it the base interface would mean a cast that
  /// happens to work.
  ComPtr<IDXGIAdapter1> adapter_object;
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
