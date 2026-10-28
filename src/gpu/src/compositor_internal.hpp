#pragma once

/// The compositor's Direct3D state, in a header only because the presenter
/// needs to read the scene texture. Internal to `src/gpu`.

#include "cutline/gpu/compositor.hpp"

#include "d3d12_util.hpp"

#include <array>
#include <condition_variable>
#include <mutex>
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

/// A generation no texture in the process has had before. Shared by everything
/// that hands a texture to a painter — every compositor, and the frame ring —
/// because a painter's cache is keyed by address, and an address freed by one
/// and reused by another with an old number is the stale handle it guards.
[[nodiscard]] unsigned next_generation();

struct Compositor::Impl {
  std::shared_ptr<Device> owner;

  /// This compositor's own command list. Its own so that a compositor can live
  /// on a thread of its own, and waited on by its own fence so that it never
  /// waits for anybody else's work.
  Recorder recorder;

  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> pipeline_normal;
  ComPtr<ID3D12PipelineState> pipeline_add;
  ComPtr<ID3D12PipelineState> pipeline_blend;
  ComPtr<ID3D12PipelineState> pipeline_adjustment;
  /// The layer drawn with blending off into a scratch target, in its own
  /// space, which is where an effect chain starts.
  ComPtr<ID3D12PipelineState> pipeline_source;
  /// The same for an adjustment layer, whose source is the backdrop.
  ComPtr<ID3D12PipelineState> pipeline_adjust_source;
  /// One effect over the scratch. Which effect is a root constant, so there is
  /// one pipeline for all of them.
  ComPtr<ID3D12PipelineState> pipeline_effect;
  /// Ultra Key's passes, Transform and Warp Stabilizer, in a shader of their
  /// own; see `PSEffectHeavy`.
  ComPtr<ID3D12PipelineState> pipeline_heavy;
  ComPtr<ID3D12PipelineState> pipeline_present;
  ComPtr<ID3D12PipelineState> pipeline_present_hdr;
  /// The two halves of the encoder's conversion, at each depth: luma into a
  /// one-channel target, colour differences into a two-channel one.
  ComPtr<ID3D12PipelineState> pipeline_luma8;
  ComPtr<ID3D12PipelineState> pipeline_chroma8;
  ComPtr<ID3D12PipelineState> pipeline_luma16;
  ComPtr<ID3D12PipelineState> pipeline_chroma16;

  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> srv_heap;
  UINT rtv_size = 0;
  UINT srv_size = 0;
  UINT layer_capacity = 0;

  ComPtr<ID3D12Resource> scene;
  ComPtr<ID3D12Resource> backdrop;
  /// Ping-pong targets for the effect chain. Allocated only when a layer
  /// actually has an effect on it, because they are canvas-sized.
  ComPtr<ID3D12Resource> scratch[2];
  /// Where a `Keep` pass puts the layer aside, the same size and format as
  /// the scratch so a copy is all it takes.
  ComPtr<ID3D12Resource> kept;
  ComPtr<ID3D12Resource> display;
  /// The scRGB copy for an HDR display, made the first time one is asked for.
  ComPtr<ID3D12Resource> hdr_display;
  std::optional<float> hdr_sdr_white;
  DisplayOptions display_options;
  ComPtr<ID3D12Resource> readback;
  std::size_t readback_capacity = 0;

  /// The encoder's planes on the card, made when first asked for and again
  /// when the canvas or the layout changes.
  ComPtr<ID3D12Resource> yuv_luma;
  ComPtr<ID3D12Resource> yuv_chroma;
  YuvLayout yuv_layout = YuvLayout::Nv12;

  ColourSettings colour;
  /// The colour fields every draw carries, from `colour`.
  void colour_params(ShaderParams& params, WorkingColour output) const noexcept {
    params.working = static_cast<int>(colour.working);
    params.white_nits = std::max(colour.graphics_white_nits, 1.0f);
    params.tone_map = colour.tone_map ? 1.0f : 0.0f;
    params.output = static_cast<int>(output);
  }

  /// Where converted frames come back to, one buffer per frame in flight.
  struct YuvSlot {
    ComPtr<ID3D12Resource> buffer;
    std::size_t capacity = 0;
    UINT64 fence = 0;
    bool lent = false;
    YuvPlanes shape;
    std::size_t chroma_offset = 0;
  };
  std::array<YuvSlot, kYuvSlots> yuv_slots;
  std::mutex yuv_lock;
  std::condition_variable yuv_returned;

  /// Makes the planes for `layout` at the canvas size, if they are not there.
  [[nodiscard]] std::expected<void, std::string> ensure_yuv_targets(YuvLayout layout);

  /// Plane textures per layer slot, kept between composes because reallocating
  /// them every frame would dominate the cost of drawing.
  struct PlaneSet {
    std::vector<ComPtr<ID3D12Resource>> textures;
    std::vector<PlaneDescription> layout;
  };
  std::vector<PlaneSet> planes;

  /// Fences a decoder has to reach before this frame may be sampled, gathered
  /// while the layers are prepared and waited on just before the work is
  /// submitted. One per hardware-decoded layer, and usually none.
  struct DecodedWait {
    ID3D12Fence* fence = nullptr;
    UINT64 value = 0;
  };
  std::vector<DecodedWait> decoded_waits;

  /// Points a layer's three descriptor slots at a decoder's own texture.
  ///
  /// The picture is NV12 in one resource of two plane slices, which is exactly
  /// the pair of textures the uploading path builds by hand — so the shader
  /// needs to know nothing about where a frame came from.
  [[nodiscard]] std::expected<void, std::string> view_decoded(const FrameView& frame,
                                                              UINT layer);

  ComPtr<ID3D12Resource> upload;
  std::size_t upload_capacity = 0;

  /// Every free-drawn mask's corners in the frame, one path after another.
  ///
  /// A path is the one mask too big for the root constants, so it goes in a
  /// buffer — and one buffer for the whole frame rather than one per pass,
  /// because the command list is recorded once and submitted once: a buffer
  /// rewritten between draws would be read by all of them with whatever it
  /// ended up holding. Each pass carries its first index and its count instead.
  ComPtr<ID3D12Resource> mask_points;
  std::size_t mask_points_capacity = 0;

  /// Every pass's own data in the frame, one run after another, for the same
  /// reason the corners share a buffer.
  ComPtr<ID3D12Resource> pass_data;
  std::size_t pass_data_capacity = 0;

  /// Makes a shared buffer hold at least `count` elements of `stride` bytes,
  /// and writes the descriptor every run reads it through at `slot`.
  [[nodiscard]] std::expected<void, std::string> ensure_shared_buffer(
      ComPtr<ID3D12Resource>& buffer, std::size_t& capacity, std::size_t count, UINT stride,
      UINT slot, std::string_view what);

  /// Copies `bytes` into a shared buffer, already big enough.
  [[nodiscard]] std::expected<void, std::string> fill_shared_buffer(ID3D12Resource* buffer,
                                                                    const void* bytes,
                                                                    std::size_t size,
                                                                    std::string_view what);

  int width = 0;
  int height = 0;
  /// Incremented by `create_targets`, so a cache downstream can tell a rebuilt
  /// target from the one it saw last even at the same address and size.
  ///
  /// Drawn from a counter shared by every compositor rather than starting at
  /// zero in each. A per-compositor count is only unique while there is one
  /// compositor: the preview builds a *new* one whenever the canvas changes
  /// size, and a fresh one starting at zero hands out generations that a
  /// caller has already seen from the old one — at which point the guard says
  /// "same texture" about memory that has been freed and reallocated, which is
  /// exactly the case it exists to catch.
  unsigned generation = 0;

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

  /// A layer's own runs for its effect passes, one per ping-pong buffer:
  /// t0 the buffer being read, t1 the layer's matte, t2 what was kept, t3 the
  /// backdrop. A run each, because a matte is one layer's own and two layers
  /// in a frame cannot share the descriptor that names it.
  [[nodiscard]] UINT pass_slot(UINT layer, int which) const noexcept {
    return (layer_capacity + 3 + layer * 2 + static_cast<UINT>(which)) * kSlotsPerLayer;
  }
  /// Every run in the table: the layers', the present pass's, the two scratch
  /// runs, and two pass runs per layer.
  [[nodiscard]] UINT run_count() const noexcept { return layer_capacity * 3 + 3; }

  /// Writes the pass runs' fixed views: the scratch buffers, what was kept and
  /// the backdrop, whichever of them exist yet.
  void write_pass_runs();

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
