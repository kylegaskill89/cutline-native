#pragma once

/// A clip's own pictures, for analysing: the clip drawn alone, with no effects,
/// no Motion and nothing above or below it, at the size of its own frame.
///
/// That is the space its effects work in, so what is found in these pictures
/// is in the effects' terms without converting it. Drawn by the ordinary
/// renderer, so a nest, a title or a retimed shot is analysed as it is seen.
/// Mask tracking and Warp Stabilizer's analysis both read clips this way.

#include "cutline/core/model.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/track/tracker.hpp"

#include <expected>
#include <memory>
#include <string>

namespace cutline::engine {

class FrameRenderer;

class ClipPictures {
 public:
  /// At most `width` pixels across, keeping the frame's shape.
  [[nodiscard]] static std::expected<std::unique_ptr<ClipPictures>, std::string> create(
      std::shared_ptr<gpu::Device> device, const core::Project& project, const core::Clip& clip,
      double width);
  ~ClipPictures();

  /// The picture at clip-local `t`, in grey.
  [[nodiscard]] std::expected<track::Image, std::string> at(double t);
  /// The same picture in colour, as 8-bit RGBA.
  [[nodiscard]] std::expected<std::vector<std::uint8_t>, std::string> colour_at(double t);

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }

 private:
  ClipPictures() = default;
  core::Project alone_;
  double start_ = 0.0;
  int width_ = 0;
  int height_ = 0;
  std::unique_ptr<FrameRenderer> renderer_;
};

}  // namespace cutline::engine
