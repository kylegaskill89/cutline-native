#pragma once

/// Filmstrip thumbnails for timeline clips.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::media {

/// A decoded frame scaled for display, as tightly packed RGBA.
struct Thumbnail {
  int width = 0;
  int height = 0;
  double timestamp = 0.0;  ///< source seconds the frame was taken from
  std::vector<std::uint8_t> rgba;
};

struct ThumbnailOptions {
  /// Thumbnails are sized by height; width follows from the source's aspect
  /// ratio, so a filmstrip of mixed sources still lines up.
  int height = 72;
  /// Range to sample across, in source seconds. An unset end means the whole
  /// file, letting a trimmed clip ask only for the part it shows.
  double start = 0.0;
  double end = -1.0;

  /// Decoding threads; 0 lets libav take the machine.
  ///
  /// Taking the machine is the right default for one extraction in a tool and
  /// the wrong one for a background worker. Measured on a ten-minute 4K
  /// capture: dropping it on the timeline cost about a hundred seconds of
  /// processor time in ten seconds of wall clock — every core busy — and while
  /// the interface never actually stopped, a machine in that state is
  /// indistinguishable from one that is about to fall over.
  int threads = 0;
  /// Whether to decode on the card where it can. It is the fast way by a long
  /// way — eleven and a half seconds a source in software against about one —
  /// and it costs the processor nothing, so it is only turned off to compare
  /// the two.
  bool hardware = true;
};

/// Extracts `count` frames evenly spaced across the requested range.
///
/// Seeks to a thumbnail only when the seek lands ahead of where decoding has
/// got to. A handful of frames spread across a long file would otherwise mean
/// decoding everything between them; but on a short one, or one with keyframes
/// seconds apart, seeking for every frame went back to the same keyframe and
/// decoded the same pictures again — 1,380 frames of a 480-frame source for
/// twelve thumbnails.
[[nodiscard]] std::expected<std::vector<Thumbnail>, std::string> extract_thumbnails(
    std::string_view path, int count, ThumbnailOptions options = {});

}  // namespace cutline::media
