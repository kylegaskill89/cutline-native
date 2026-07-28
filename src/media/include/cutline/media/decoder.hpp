#pragma once

/// Sequential video decoding — the whole point of the rewrite.
///
/// The TypeScript version rendered each output frame by seeking a `<video>`
/// element to that time, and a browser re-decodes from the nearest keyframe on
/// every seek. On 4K long-GOP footage that cost roughly 4.5 seconds per frame.
///
/// Export does not need random access. It walks time forwards, which is exactly
/// the access pattern a decoder is built for: one pass, every frame emitted in
/// order, keyframes decoded once. `seek` exists for scrubbing the preview, not
/// for rendering.

#include "cutline/media/probe.hpp"

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct AVFrame;

namespace cutline::media {

/// Where decoded frames end up.
enum class Acceleration {
  /// Frames land in system memory. Always available.
  Software,
  /// Frames stay in GPU memory as D3D11 textures. Reaching the Direct3D 12
  /// compositor from there needs a shared handle, so this is the older path.
  D3D11Va,
  /// Frames stay in GPU memory as Direct3D 12 resources. Given the
  /// compositor's own device they are that device's own resources — nothing to
  /// share, and nothing to copy.
  D3D12Va,
};

[[nodiscard]] std::string_view to_string(Acceleration acceleration) noexcept;

/// A decoded frame that never left the GPU.
///
/// The texture is NV12: an 8-bit luma plane and a half-resolution interleaved
/// chroma plane, addressed as plane slices of one resource.
struct HardwareTexture {
  /// `ID3D12Resource*`, untyped so d3d12.h stays out of this header. Owned by
  /// the decoder and valid until the next `next_frame`.
  void* resource = nullptr;
  /// Array slice, when the frame pool was allocated as a texture array.
  int subresource = 0;

  /// `ID3D12Fence*` the decoder signals when this frame is done, and the value
  /// to wait for. Sampling before that point reads a half-decoded picture —
  /// intermittently, depending on how busy the GPU is, which is the worst kind
  /// of bug to find later.
  void* fence = nullptr;
  unsigned long long fence_value = 0;
};

class VideoDecoder {
 public:
  struct Options {
    /// Hardware decoding is attempted first and falls back to software, since
    /// codec and driver support varies by machine, driver and file.
    Acceleration preferred = Acceleration::D3D12Va;
    /// Software decoding thread count; 0 lets libav choose.
    int threads = 0;

    /// An `ID3D12Device*` to decode onto, untyped for the same reason. Handing
    /// over the compositor's device is what makes decoding zero-copy; without
    /// one, D3D12VA creates its own and the frames would need copying across
    /// before anything could draw them.
    void* d3d12_device = nullptr;
  };

  /// Opens the file's primary video stream.
  [[nodiscard]] static std::expected<std::unique_ptr<VideoDecoder>, std::string> open(
      std::string_view path, Options options = {});

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;
  ~VideoDecoder();

  /// Decodes the next frame in presentation order. False means end of stream.
  /// The frame stays valid until the next call.
  [[nodiscard]] std::expected<bool, std::string> next_frame();

  /// The most recently decoded frame, or null before the first successful
  /// `next_frame`. Hardware frames carry a GPU handle rather than pixels.
  [[nodiscard]] const AVFrame* frame() const noexcept;

  /// Presentation timestamp of the current frame, in seconds.
  [[nodiscard]] double timestamp() const noexcept;

  /// Jumps to the keyframe at or before `seconds` and flushes the decoder.
  /// Decoding continues sequentially from there, so reaching an exact frame
  /// means seeking once and then decoding forward — never seeking per frame.
  [[nodiscard]] std::expected<void, std::string> seek(double seconds);

  /// The current frame as a GPU texture, or nothing when decoding in software.
  /// Only meaningful for `D3D12Va`.
  [[nodiscard]] std::optional<HardwareTexture> hardware_texture() const noexcept;

  /// What the decoder actually got, which may not be what was asked for.
  [[nodiscard]] Acceleration acceleration() const noexcept;

  [[nodiscard]] const VideoStreamInfo& stream() const noexcept;

 private:
  struct Impl;
  explicit VideoDecoder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::media
