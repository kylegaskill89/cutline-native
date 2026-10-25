#pragma once

/// A file's compressed video, a frame at a time, without decoding it.
///
/// What smart rendering is built on. A frame of an intermediate -- ProRes,
/// DNxHR -- is complete in itself, so where an export would only decode a
/// source frame and encode the same picture again, it can take the frame as the
/// file holds it and put that in the output instead: no generation lost, and
/// no time spent. This reads those frames, and says what they are, so the
/// export can tell whether one belongs in the file it is writing.

#include "cutline/media/encoder.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::media {

/// What a file's picture stream is, as far as copying its frames goes.
struct PacketStream {
  /// The codec, when it is one an export can write; empty otherwise.
  std::optional<VideoCodec> codec;
  /// ProRes 422 HQ, or DNxHR HQ: the profile the export writes. A frame of
  /// another profile is a different shape of bitstream under the same name.
  bool export_profile = false;
  int width = 0;
  int height = 0;
  double fps = 0.0;
  /// Whole frames rather than fields. An interlaced frame is two pictures,
  /// and splicing one into a progressive file would be wrong.
  bool progressive = false;
};

class PacketReader {
 public:
  [[nodiscard]] static std::expected<std::unique_ptr<PacketReader>, std::string> open(
      std::string_view path);

  PacketReader(const PacketReader&) = delete;
  PacketReader& operator=(const PacketReader&) = delete;
  ~PacketReader();

  [[nodiscard]] const PacketStream& stream() const noexcept;

  /// The compressed frame shown at `time` seconds, in the file's own timing
  /// -- the frame a decoder hands back for that moment. For a stream where
  /// every frame is a keyframe; one with frames that depend on others has no
  /// frame that stands alone to give.
  [[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string> frame_at(double time);

 private:
  struct Impl;
  explicit PacketReader(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::media
