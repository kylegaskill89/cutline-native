#pragma once

/// Bringing a Direct3D 11 decoder's frames onto a Direct3D 12 device.
///
/// D3D12 video decode is the path that needs none of this: hand libav the
/// compositor's own device and the picture is decoded straight into memory the
/// compositor can sample. On the machine this was written for that path does
/// not work — the driver fails every HEVC picture and removes the device with
/// it, and the stock `ffmpeg.exe` fails identically, so it is not something
/// this application is doing wrong. D3D11 decodes the same file at 558 fps.
///
/// So the frames arrive on a device the compositor knows nothing about, and
/// this is the crossing. It is a **GPU-side copy**, not a round trip through
/// system memory: one `CopySubresourceRegion` into a texture that both APIs can
/// see, which for 4K NV12 is about twelve megabytes of video memory moving
/// inside the card. That is a fraction of what decoding the frame cost and
/// nothing like what downloading it would.
///
/// Two things are shared, and both have to be: the texture, so D3D12 has
/// something to sample, and a **fence**, so it knows when the copy has actually
/// happened. Without the second one D3D12 would sample whatever was in the
/// texture when it got there, which on a busy card is the previous frame — some
/// of the time, which is the worst way for it to be wrong.

#include "cutline/media/decoder.hpp"

#include <cstdint>
#include <optional>

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D12Device;

namespace cutline::media::detail {

/// A shared NV12 texture and a shared fence, and the copy between them.
///
/// One of these serves a decoder for its lifetime: the shared texture is made
/// once, at the size the first frame turns out to be, and every frame after is
/// a copy into it. That is safe because `compose` waits for the GPU before it
/// returns — the compositor is finished with frame N before frame N+1 is asked
/// for, so one texture is enough and the alternative would be a pool nothing
/// needs.
class D3D11Share {
 public:
  D3D11Share();
  ~D3D11Share();

  D3D11Share(const D3D11Share&) = delete;
  D3D11Share& operator=(const D3D11Share&) = delete;

  /// Prepares the crossing for frames of this size. False when the pair of
  /// devices cannot share — an old driver, a format neither will take — in
  /// which case the caller has software to fall back on and should.
  [[nodiscard]] bool open(ID3D11Device* source, ID3D12Device* target, int width, int height);

  [[nodiscard]] bool ready() const noexcept;

  /// Copies one decoder frame across and reports where it landed.
  ///
  /// `slice` is the array index within the decoder's frame pool, which is how
  /// D3D11 decoders hand out frames: one texture array, a slice per frame in
  /// flight.
  [[nodiscard]] std::optional<HardwareTexture> copy(ID3D11Texture2D* frame,
                                                    unsigned int slice);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::media::detail
