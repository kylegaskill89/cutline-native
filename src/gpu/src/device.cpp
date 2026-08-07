#include "d3d12_util.hpp"

#include <fstream>

namespace cutline::gpu {
namespace {

/// Converts an adapter description to UTF-8. DXGI reports wide strings and the
/// rest of the program speaks UTF-8.
[[nodiscard]] std::string narrow(const wchar_t* wide) {
  const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 1) return {};

  std::string out(static_cast<std::size_t>(length - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), length, nullptr, nullptr);
  return out;
}

}  // namespace

std::filesystem::path module_directory() {
  std::wstring buffer(MAX_PATH, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}

std::expected<std::vector<std::byte>, std::string> read_file(const std::filesystem::path& path) {
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

std::expected<ComPtr<ID3D12RootSignature>, std::string> create_root_signature(
    ID3D12Device* device) {
  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = kSlotsPerLayer;
  range.BaseShaderRegister = 0;

  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable.NumDescriptorRanges = 1;
  parameters[0].DescriptorTable.pDescriptorRanges = &range;
  parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  // Root constants rather than a buffer: this is a handful of bytes that
  // changes every draw, and the vertex stage builds the quad from it, so the
  // visibility has to cover both stages.
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[1].Constants.Num32BitValues = kShaderParamCount;
  parameters[1].Constants.ShaderRegister = 0;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  D3D12_ROOT_SIGNATURE_DESC desc{};
  desc.NumParameters = 2;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 1;
  desc.pStaticSamplers = &sampler;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

  ComPtr<ID3DBlob> serialized;
  ComPtr<ID3DBlob> error;
  if (HRESULT hr =
          D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
      FAILED(hr)) {
    const std::string detail =
        error ? std::string(static_cast<const char*>(error->GetBufferPointer())) : "";
    return std::unexpected(
        std::format("cannot serialise the root signature: {} {}", hresult_string(hr), detail));
  }

  ComPtr<ID3D12RootSignature> signature;
  if (HRESULT hr = device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                               serialized->GetBufferSize(),
                                               IID_PPV_ARGS(&signature));
      FAILED(hr)) {
    return std::unexpected(
        std::format("cannot create the root signature: {}", hresult_string(hr)));
  }
  return signature;
}

// ------------------------------------------------------------------ Device --

Device::Impl::~Impl() {
  if (fence_event != nullptr) CloseHandle(fence_event);
}

std::expected<void, std::string> Device::Impl::begin() {
  if (HRESULT hr = allocator->Reset(); FAILED(hr)) {
    return std::unexpected(std::format("cannot reset the allocator: {}", hresult_string(hr)));
  }
  if (HRESULT hr = commands->Reset(allocator.Get(), nullptr); FAILED(hr)) {
    return std::unexpected(std::format("cannot reset the command list: {}", hresult_string(hr)));
  }
  return {};
}

std::expected<void, std::string> Device::Impl::submit() {
  if (HRESULT hr = commands->Close(); FAILED(hr)) {
    return std::unexpected(std::format("cannot close the command list: {}", hresult_string(hr)));
  }
  ID3D12CommandList* lists[] = {commands.Get()};
  queue->ExecuteCommandLists(1, lists);
  return {};
}

void Device::Impl::wait_for_idle() {
  if (!queue || !fence) return;

  const UINT64 target = ++fence_value;
  if (FAILED(queue->Signal(fence.Get(), target))) return;
  if (fence->GetCompletedValue() < target) {
    fence->SetEventOnCompletion(target, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
  }
}

Device::Device(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Device::~Device() {
  if (impl_) impl_->wait_for_idle();
}

const std::string& Device::adapter_name() const noexcept { return impl_->adapter; }
bool Device::is_software() const noexcept { return impl_->software; }
void* Device::native_device() const noexcept { return impl_->device.Get(); }
void* Device::native_adapter() const noexcept { return impl_->adapter_object.Get(); }
void* Device::native_queue() const noexcept { return impl_->queue.Get(); }
void Device::wait_for_idle() { impl_->wait_for_idle(); }

std::expected<std::shared_ptr<Device>, std::string> Device::create(DeviceOptions options) {
  auto impl = std::make_unique<Impl>();

  UINT factory_flags = 0;
  if (options.debug_layer) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
    // A missing debug layer is not fatal: the graphics tools feature is often
    // absent on machines that only run the app.
  }

  if (HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&impl->factory)); FAILED(hr)) {
    return std::unexpected(std::format("cannot create a DXGI factory: {}", hresult_string(hr)));
  }

  // Skipped entirely when the software rasteriser was asked for by name. The
  // point of asking is to take the hardware adapter out of the picture, and a
  // device created on it first — even one immediately dropped — has already run
  // the driver code somebody is trying to rule out.
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; !options.force_software &&
                   impl->factory->EnumAdapterByGpuPreference(
                       i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                       IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf())) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&impl->device)))) {
      impl->adapter = narrow(desc.Description);
      impl->adapter_object = adapter;
      break;
    }
  }

  if (!impl->device && (options.allow_software || options.force_software)) {
    ComPtr<IDXGIAdapter1> warp;
    if (SUCCEEDED(impl->factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
        SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                    IID_PPV_ARGS(&impl->device)))) {
      impl->adapter = "Microsoft Basic Render Driver (WARP)";
      impl->adapter_object = warp;
      impl->software = true;
    }
  }

  if (!impl->device) return std::unexpected("no Direct3D 12 capable adapter found");

  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (HRESULT hr = impl->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&impl->queue));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a command queue: {}", hresult_string(hr)));
  }

  if (HRESULT hr = impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&impl->allocator));
      FAILED(hr)) {
    return std::unexpected(
        std::format("cannot create a command allocator: {}", hresult_string(hr)));
  }
  if (HRESULT hr =
          impl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl->allocator.Get(),
                                          nullptr, IID_PPV_ARGS(&impl->commands));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a command list: {}", hresult_string(hr)));
  }
  impl->commands->Close();

  if (HRESULT hr =
          impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->fence));
      FAILED(hr)) {
    return std::unexpected(std::format("cannot create a fence: {}", hresult_string(hr)));
  }
  impl->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (impl->fence_event == nullptr) return std::unexpected("cannot create a fence event");

  return std::shared_ptr<Device>(new Device(std::move(impl)));
}

}  // namespace cutline::gpu
