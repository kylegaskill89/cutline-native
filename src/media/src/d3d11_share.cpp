#include "d3d11_share.hpp"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <memory>

namespace cutline::media::detail {
namespace {

using Microsoft::WRL::ComPtr;

/// Closes an NT handle, which both shared objects hand back and neither owns
/// afterwards: opening a resource from a handle does not consume it.
struct HandleCloser {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr) CloseHandle(handle);
  }
};
using Handle = std::unique_ptr<void, HandleCloser>;

}  // namespace

struct D3D11Share::Impl {
  ComPtr<ID3D11Device5> source;
  ComPtr<ID3D11DeviceContext4> context;

  /// The texture both APIs can see, and the same memory addressed twice.
  ComPtr<ID3D11Texture2D> shared11;
  ComPtr<ID3D12Resource> shared12;

  /// One fence, likewise. D3D11 signals it after the copy and D3D12 waits on
  /// it before sampling — which is the whole of the synchronisation, and the
  /// reason a shared *fence* is worth the trouble over a keyed mutex: a mutex
  /// would make the two devices take turns, and this only needs an ordering.
  ComPtr<ID3D11Fence> fence11;
  ComPtr<ID3D12Fence> fence12;
  std::uint64_t signalled = 0;

  int width = 0;
  int height = 0;
};

D3D11Share::D3D11Share() : impl_(std::make_unique<Impl>()) {}
D3D11Share::~D3D11Share() = default;

bool D3D11Share::ready() const noexcept { return impl_->shared12 != nullptr; }

bool D3D11Share::open(ID3D11Device* source, ID3D12Device* target, int width, int height) {
  if (source == nullptr || target == nullptr || width <= 0 || height <= 0) return false;

  Impl& d = *impl_;
  // Both interfaces are the newer ones: fences arrived in 11.4, and signalling
  // one needs the 11.4 context. A driver without them cannot do this at all,
  // which is a clean answer rather than a half-working one.
  if (FAILED(source->QueryInterface(IID_PPV_ARGS(&d.source)))) return false;

  ComPtr<ID3D11DeviceContext> immediate;
  d.source->GetImmediateContext(&immediate);
  if (!immediate || FAILED(immediate.As(&d.context))) return false;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_NV12;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  // An NT handle rather than the older shared handle: `OpenSharedHandle` on the
  // D3D12 device takes nothing else.
  desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

  if (FAILED(d.source->CreateTexture2D(&desc, nullptr, &d.shared11))) return false;

  ComPtr<IDXGIResource1> shareable;
  if (FAILED(d.shared11.As(&shareable))) return false;

  Handle texture_handle;
  {
    HANDLE raw = nullptr;
    if (FAILED(shareable->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                             &raw))) {
      return false;
    }
    texture_handle.reset(raw);
  }
  if (FAILED(target->OpenSharedHandle(texture_handle.get(), IID_PPV_ARGS(&d.shared12)))) {
    return false;
  }

  if (FAILED(d.source->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d.fence11)))) {
    return false;
  }
  Handle fence_handle;
  {
    HANDLE raw = nullptr;
    if (FAILED(d.fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &raw))) {
      return false;
    }
    fence_handle.reset(raw);
  }
  if (FAILED(target->OpenSharedHandle(fence_handle.get(), IID_PPV_ARGS(&d.fence12)))) {
    return false;
  }

  d.width = width;
  d.height = height;
  return true;
}

std::optional<HardwareTexture> D3D11Share::copy(ID3D11Texture2D* frame, unsigned int slice) {
  Impl& d = *impl_;
  if (frame == nullptr || !ready()) return std::nullopt;

  // Whole subresource, both planes. NV12's planes are slices of one resource,
  // so copying the resource copies the picture — there is no per-plane loop to
  // get wrong here, unlike on the way in from system memory.
  d.context->CopySubresourceRegion(d.shared11.Get(), 0, 0, 0, 0, frame, slice, nullptr);

  ++d.signalled;
  if (FAILED(d.context->Signal(d.fence11.Get(), d.signalled))) return std::nullopt;
  // Submitted rather than left sitting in the context's queue: the value has
  // been promised to the other device, and a signal nobody sent is a wait
  // nobody satisfies.
  d.context->Flush();

  return HardwareTexture{
      .resource = d.shared12.Get(),
      .subresource = 0,
      .fence = d.fence12.Get(),
      .fence_value = d.signalled,
  };
}

}  // namespace cutline::media::detail
