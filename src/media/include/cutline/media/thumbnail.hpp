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
};

/// Extracts `count` frames evenly spaced across the requested range.
///
/// This seeks per thumbnail, which is the one place that access pattern is the
/// right choice: a handful of frames spread across a long file would otherwise
/// mean decoding everything between them.
[[nodiscard]] std::expected<std::vector<Thumbnail>, std::string> extract_thumbnails(
    std::string_view path, int count, ThumbnailOptions options = {});

}  // namespace cutline::media
