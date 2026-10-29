#pragma once

/// Finding the cuts in footage that was edited before it arrived: Premiere's
/// Scene Edit Detection, done by looking at how much each frame differs from
/// the one before.
///
/// A cut is a frame that differs from its predecessor far more than its
/// neighbours differ from theirs. Measuring against the neighbours rather than
/// against a fixed amount is what keeps a whip pan or a handheld shot from
/// reading as a string of cuts — every frame of those differs a lot — while a
/// cut between two dim shots that differ little still stands out from the
/// stillness either side of it. A flash, a frame or two of something else
/// between two frames of the same shot, differs going in and coming out, and
/// is told apart by comparing across it.

#include <cstdint>
#include <span>
#include <vector>

namespace cutline::track {

class SceneDetector {
 public:
  /// `fps` is the rate the frames come at, which decides how short a shot may
  /// be before it is taken for a flash.
  explicit SceneDetector(double fps);

  /// The next frame, as 8-bit RGBA. Every frame is expected to be the same
  /// size; one that is not starts a comparison afresh.
  void add(std::span<const std::uint8_t> rgba, int width, int height);

  /// The frames that begin a new shot, by index from the first added, in
  /// order. Never the first frame.
  [[nodiscard]] std::vector<int> cuts() const;

  /// How much each frame differs from the one before, on a scale where 255 is
  /// as different as two pictures can be; the first frame's is zero.
  [[nodiscard]] const std::vector<double>& scores() const noexcept { return scores_; }

 private:
  /// Hue, saturation and value of each pixel.
  [[nodiscard]] static std::vector<float> hsv_of(std::span<const std::uint8_t> rgba, int width,
                                                 int height);
  [[nodiscard]] static double difference(const std::vector<float>& a,
                                         const std::vector<float>& b) noexcept;

  int shortest_ = 2;
  std::vector<float> last_;
  std::vector<float> before_;
  std::vector<double> scores_;
  /// How much each frame differs from the one two before it.
  std::vector<double> skips_;
};

}  // namespace cutline::track
