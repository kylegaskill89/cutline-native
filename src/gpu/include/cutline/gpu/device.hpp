#pragma once

/// The Direct3D 12 device, shared by everything that draws.
///
/// This exists apart from any window because most rendering does not have one.
/// Export composites to an offscreen target and encodes it; the golden-image
/// tests composite to an offscreen target and read it back. Only the preview
/// puts pixels on screen. Binding the device to a swapchain would have made the
/// two paths structurally different, which is exactly the divergence between
/// preview and export this rewrite exists to remove.

#include <expected>
#include <memory>
#include <string>

namespace cutline::gpu {

struct DeviceOptions {
  /// Permit Microsoft's software rasteriser when no hardware adapter is usable.
  /// CI runners have no GPU, and a slow correct device is what lets the
  /// golden-image tests run there at all. Off by default: silently falling back
  /// to software on a real machine would look like a mysterious slowdown.
  bool allow_software = false;

  /// Enable the debug layer, which turns silent API misuse into a message.
  /// Defaults to on in debug builds.
  bool debug_layer =
#ifndef NDEBUG
      true;
#else
      false;
#endif
};

class Device {
 public:
  [[nodiscard]] static std::expected<std::shared_ptr<Device>, std::string> create(
      DeviceOptions options = {});

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  ~Device();

  /// The adapter the device was created on, for logging.
  [[nodiscard]] const std::string& adapter_name() const noexcept;

  /// True when this is the software rasteriser rather than a real GPU.
  [[nodiscard]] bool is_software() const noexcept;

  /// Blocks until the GPU has finished everything submitted so far.
  void wait_for_idle();

  /// The Direct3D objects. Defined in an internal header: callers outside this
  /// library have no use for them, and keeping the type opaque is what stops
  /// d3d12.h leaking into the rest of the program.
  struct Impl;
  [[nodiscard]] Impl& internals() noexcept { return *impl_; }

 private:
  explicit Device(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::gpu
