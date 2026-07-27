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
};

/// Extracts `count` frames evenly spaced across the requested range.
///
/// This seeks per thumbnail, which is the one place that access pattern is the
/// right choice: a handful of frames spread across a long file would otherwise
/// mean decoding everything between them.
[[nodiscard]] std::expected<std::vector<Thumbnail>, std::string> extract_thumbnails(
    std::string_view path, int count, ThumbnailOptions options = {});

}  // namespace cutline::media
