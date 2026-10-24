#include "cutline/gpu/frame_ring.hpp"

#include "compositor_internal.hpp"

#include <cstring>
#include <mutex>
#include <vector>

namespace cutline::gpu {
namespace {

/// The display encoding, which is what the compositor's display target holds
/// and what the interface's painter is told it is sampling.
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

/// The HDR display's copy beside it, when the compositor made one: scRGB.
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

/// The state every texture here rests in between copies. The same one the
/// compositor leaves its display target in, and the one a painter is told.
constexpr D3D12_RESOURCE_STATES kResting = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

[[nodiscard]] std::expected<ComPtr<ID3D12Resource>, std::string> make_texture(
    ID3D12Device* device, int width, int height, DXGI_FORMAT format = kFormat) {
  const D3D12_HEAP_PROPERTIES heap = heap_of(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = static_cast<UINT64>(width);
  desc.Height = static_cast<UINT>(height);
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;

  ComPtr<ID3D12Resource> texture;
  if (HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kResting,
                                                   nullptr, IID_PPV_ARGS(&texture));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a frame texture: {}", hresult_string(hr)));
  }
  return texture;
}

/// One whole-texture copy, with the transitions either side of it.
void record_copy(ID3D12GraphicsCommandList* commands, ID3D12Resource* from,
                 ID3D12Resource* to) {
  const D3D12_RESOURCE_BARRIER before[] = {
      transition(from, kResting, D3D12_RESOURCE_STATE_COPY_SOURCE),
      transition(to, kResting, D3D12_RESOURCE_STATE_COPY_DEST),
  };
  commands->ResourceBarrier(2, before);
  commands->CopyResource(to, from);
  const D3D12_RESOURCE_BARRIER after[] = {
      transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE, kResting),
      transition(to, D3D12_RESOURCE_STATE_COPY_DEST, kResting),
  };
  commands->ResourceBarrier(2, after);
}

}  // namespace

struct FrameRing::Impl {
  std::shared_ptr<Device> owner;

  struct Slot {
    ComPtr<ID3D12Resource> texture;
    /// The HDR copy, and whether this frame has one: only when the compositor
    /// was asked for it, which is when the window is on an HDR display.
    ComPtr<ID3D12Resource> hdr;
    bool has_hdr = false;
    int width = 0;
    int height = 0;
    bool stored = false;
    /// The fence value the interface's copy out of this slot reaches once it is
    /// done, on the interface's queue. The next write into the slot waits for
    /// it on the card, and freeing the slot for a new size waits for it here.
    UINT64 released = 0;
  };
  std::vector<Slot> slots;
  /// Guards which slots are stored. The textures themselves are only ever
  /// touched by one side at a time, and that is what the flag says.
  mutable std::mutex lock;

  /// One command list per side, because each side records from its own thread.
  Recorder producer;
  Recorder consumer;

  /// The texture the interface samples, and only the interface's thread
  /// touches it.
  ComPtr<ID3D12Resource> shown;
  ComPtr<ID3D12Resource> shown_hdr;
  int shown_width = 0;
  int shown_height = 0;
  unsigned generation = 0;

  ComPtr<ID3D12Resource> readback;
  std::size_t readback_capacity = 0;

  [[nodiscard]] Device::Impl& gpu() noexcept { return owner->internals(); }
};

FrameRing::FrameRing(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FrameRing::~FrameRing() {
  // Both sides' copies, and the interface's drawing from `shown`, finished
  // before anything here is freed.
  if (impl_ && impl_->owner) impl_->owner->wait_for_idle();
}

std::expected<std::unique_ptr<FrameRing>, std::string> FrameRing::create(
    std::shared_ptr<Device> device, std::size_t slots, std::shared_ptr<Queue> producer) {
  if (!device) return std::unexpected("a frame ring needs a device");
  if (slots == 0) return std::unexpected("a frame ring needs at least one slot");

  auto impl = std::make_unique<Impl>();
  impl->owner = std::move(device);
  impl->slots.resize(slots);
  if (auto ok = impl->producer.create(impl->gpu(), std::move(producer)); !ok) {
    return std::unexpected(ok.error());
  }
  if (auto ok = impl->consumer.create(impl->gpu()); !ok) return std::unexpected(ok.error());
  return std::unique_ptr<FrameRing>(new FrameRing(std::move(impl)));
}

std::size_t FrameRing::capacity() const noexcept { return impl_->slots.size(); }

bool FrameRing::has_room() const {
  const std::lock_guard<std::mutex> held(impl_->lock);
  for (const Impl::Slot& slot : impl_->slots) {
    if (!slot.stored) return true;
  }
  return false;
}

std::expected<std::size_t, std::string> FrameRing::store(Compositor& from) {
  Impl& d = *impl_;
  Compositor::Impl& source = from.internals();
  if (source.display == nullptr) return std::unexpected("nothing has been composited yet");

  std::size_t which = d.slots.size();
  {
    const std::lock_guard<std::mutex> held(d.lock);
    for (std::size_t i = 0; i < d.slots.size(); ++i) {
      if (!d.slots[i].stored) {
        which = i;
        break;
      }
    }
  }
  if (which == d.slots.size()) return std::unexpected("every slot is waiting to be shown");

  Impl::Slot& slot = d.slots[which];
  if (slot.texture == nullptr || slot.width != source.width || slot.height != source.height) {
    // The interface's last copy out of it may still be running, and this is
    // the one place that frees a slot rather than writing it.
    d.gpu().wait_for(slot.released);
    slot.texture.Reset();
    slot.hdr.Reset();
    auto made = make_texture(d.gpu().device.Get(), source.width, source.height);
    if (!made) return std::unexpected(made.error());
    slot.texture = std::move(*made);
    slot.width = source.width;
    slot.height = source.height;
  }
  const bool hdr = source.hdr_sdr_white.has_value() && source.hdr_display != nullptr;
  if (hdr && slot.hdr == nullptr) {
    d.gpu().wait_for(slot.released);
    auto made = make_texture(d.gpu().device.Get(), source.width, source.height, kHdrFormat);
    if (!made) return std::unexpected(made.error());
    slot.hdr = std::move(*made);
  }
  slot.has_hdr = hdr;

  const auto opened = d.producer.begin();
  if (!opened) return std::unexpected(opened.error());
  record_copy(*opened, source.display.Get(), slot.texture.Get());
  if (hdr) record_copy(*opened, source.hdr_display.Get(), slot.hdr.Get());
  // The interface's last copy *out* of this slot went to the interface's
  // queue, and on a queue of its own this write is not ordered after it. A
  // wait on the card rather than here: the copy is almost always long done,
  // and when it is not, it is the card that should hold, not this thread.
  d.producer.target->queue->Wait(d.gpu().direct->fence.Get(), slot.released);
  if (auto ok = d.producer.submit(); !ok) return std::unexpected(ok.error());
  // Waited for so the compositor may render its next frame over the display
  // target straight away. Its own fence, and a copy: a fraction of a
  // millisecond.
  d.producer.finish();

  const std::lock_guard<std::mutex> held(d.lock);
  slot.stored = true;
  return which;
}

void FrameRing::discard(std::size_t slot) {
  const std::lock_guard<std::mutex> held(impl_->lock);
  if (slot < impl_->slots.size()) impl_->slots[slot].stored = false;
}

std::expected<SceneTexture, std::string> FrameRing::show(std::size_t which) {
  Impl& d = *impl_;
  if (which >= d.slots.size()) return std::unexpected("no such slot");

  int width = 0;
  int height = 0;
  ID3D12Resource* stored = nullptr;
  ID3D12Resource* stored_hdr = nullptr;
  {
    const std::lock_guard<std::mutex> held(d.lock);
    const Impl::Slot& slot = d.slots[which];
    if (!slot.stored) return std::unexpected("that slot holds nothing");
    width = slot.width;
    height = slot.height;
    stored = slot.texture.Get();
    if (slot.has_hdr) stored_hdr = slot.hdr.Get();
  }

  if (d.shown == nullptr || d.shown_width != width || d.shown_height != height) {
    // Everybody's work, not just this side's: the painter has been drawing
    // from the old texture, and that is somebody else's submission.
    d.gpu().wait_for_idle();
    d.shown.Reset();
    d.shown_hdr.Reset();
    auto made = make_texture(d.gpu().device.Get(), width, height);
    if (!made) return std::unexpected(made.error());
    d.shown = std::move(*made);
    d.shown_width = width;
    d.shown_height = height;
    d.generation = next_generation();
  }
  if (stored_hdr != nullptr && d.shown_hdr == nullptr) {
    auto made = make_texture(d.gpu().device.Get(), width, height, kHdrFormat);
    if (!made) return std::unexpected(made.error());
    d.shown_hdr = std::move(*made);
    // A new texture for the painter to wrap, which the generation tells it.
    d.generation = next_generation();
  }

  const auto opened = d.consumer.begin();
  if (!opened) return std::unexpected(opened.error());
  record_copy(*opened, stored, d.shown.Get());
  if (stored_hdr != nullptr) record_copy(*opened, stored_hdr, d.shown_hdr.Get());
  if (auto ok = d.consumer.submit(); !ok) return std::unexpected(ok.error());
  // Not waited for. The painter's own submission comes after this one on the
  // same queue, which is all the ordering a paint needs, and the interface
  // thread is the one thread here that must never stand still.

  {
    const std::lock_guard<std::mutex> held(d.lock);
    d.slots[which].released = d.consumer.submitted;
    d.slots[which].stored = false;
  }

  return SceneTexture{.resource = d.shown.Get(),
                      .width = width,
                      .height = height,
                      .format = static_cast<unsigned>(kFormat),
                      .state = static_cast<unsigned>(kResting),
                      .generation = d.generation,
                      .hdr_resource = stored_hdr != nullptr ? d.shown_hdr.Get() : nullptr,
                      .hdr_format = stored_hdr != nullptr ? static_cast<unsigned>(kHdrFormat) : 0u};
}

std::expected<Image, std::string> FrameRing::read_shown() {
  Impl& d = *impl_;
  if (d.shown == nullptr) return std::unexpected("nothing has been shown yet");

  const UINT row_bytes = static_cast<UINT>(d.shown_width) * 4;
  const UINT pitch = aligned_pitch(row_bytes);
  const std::size_t needed = static_cast<std::size_t>(pitch) * d.shown_height;

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

  const auto opened = d.consumer.begin();
  if (!opened) return std::unexpected(opened.error());
  ID3D12GraphicsCommandList* commands = *opened;

  auto to_source = transition(d.shown.Get(), kResting, D3D12_RESOURCE_STATE_COPY_SOURCE);
  commands->ResourceBarrier(1, &to_source);

  D3D12_TEXTURE_COPY_LOCATION source{};
  source.pResource = d.shown.Get();
  source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = d.readback.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  destination.PlacedFootprint.Footprint.Format = kFormat;
  destination.PlacedFootprint.Footprint.Width = static_cast<UINT>(d.shown_width);
  destination.PlacedFootprint.Footprint.Height = static_cast<UINT>(d.shown_height);
  destination.PlacedFootprint.Footprint.Depth = 1;
  destination.PlacedFootprint.Footprint.RowPitch = pitch;
  commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

  auto back = transition(d.shown.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kResting);
  commands->ResourceBarrier(1, &back);

  if (auto ok = d.consumer.submit(); !ok) return std::unexpected(ok.error());
  d.consumer.finish();

  std::byte* mapped = nullptr;
  const D3D12_RANGE everything{0, needed};
  if (HRESULT hr = d.readback->Map(0, &everything, reinterpret_cast<void**>(&mapped));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot map the readback buffer: {}", hresult_string(hr)));
  }
  Image image;
  image.width = d.shown_width;
  image.height = d.shown_height;
  image.pixels.resize(static_cast<std::size_t>(row_bytes) * d.shown_height);
  for (int y = 0; y < d.shown_height; ++y) {
    std::memcpy(image.pixels.data() + static_cast<std::size_t>(y) * row_bytes,
                mapped + static_cast<std::size_t>(y) * pitch, row_bytes);
  }
  const D3D12_RANGE nothing{0, 0};
  d.readback->Unmap(0, &nothing);
  return image;
}

}  // namespace cutline::gpu
