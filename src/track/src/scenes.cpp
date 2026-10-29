#include "cutline/track/scenes.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::track {
namespace {

/// How many times its neighbours' difference a frame's must be to be a cut.
constexpr double kAgainstNeighbours = 3.0;
/// And the least it must be whatever its neighbours do, so a flicker in a
/// still shot is not taken for one.
constexpr double kLeast = 15.0;
/// How many frames either side make the neighbourhood.
constexpr int kNeighbours = 2;

}  // namespace

SceneDetector::SceneDetector(double fps)
    : shortest_(std::max(2, static_cast<int>(std::lround((fps > 0.0 ? fps : 30.0) * 0.25)))) {}

std::vector<float> SceneDetector::hsv_of(std::span<const std::uint8_t> rgba, int width,
                                         int height) {
  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::vector<float> out(count * 3);
  for (std::size_t i = 0; i < count && i * 4 + 3 < rgba.size(); ++i) {
    const float r = rgba[i * 4];
    const float g = rgba[i * 4 + 1];
    const float b = rgba[i * 4 + 2];
    const float high = std::max({r, g, b});
    const float low = std::min({r, g, b});
    const float range = high - low;
    float hue = 0.0F;
    if (range > 0.0F) {
      if (high == r) hue = std::fmod((g - b) / range + 6.0F, 6.0F);
      else if (high == g) hue = (b - r) / range + 2.0F;
      else hue = (r - g) / range + 4.0F;
    }
    // On the scale the other two use, so each counts as much as the others.
    out[i * 3] = hue * (255.0F / 6.0F);
    out[i * 3 + 1] = high > 0.0F ? range / high * 255.0F : 0.0F;
    out[i * 3 + 2] = high;
  }
  return out;
}

double SceneDetector::difference(const std::vector<float>& a,
                                 const std::vector<float>& b) noexcept {
  if (a.size() != b.size() || a.empty()) return 0.0;
  double hue = 0.0;
  double saturation = 0.0;
  double value = 0.0;
  for (std::size_t i = 0; i < a.size(); i += 3) {
    // Hue goes round, so red at either end is the same red; and it means
    // little where there is no colour, so it counts for as much colour as
    // there is, or the noise in a grey shot would be cuts.
    float dh = std::abs(a[i] - b[i]);
    dh = std::min(dh, 255.0F - dh) * 2.0F;
    hue += dh * std::min(a[i + 1], b[i + 1]) / 255.0F;
    saturation += std::abs(a[i + 1] - b[i + 1]);
    value += std::abs(a[i + 2] - b[i + 2]);
  }
  const double pixels = static_cast<double>(a.size() / 3);
  return (hue + saturation + value) / (3.0 * pixels);
}

void SceneDetector::add(std::span<const std::uint8_t> rgba, int width, int height) {
  std::vector<float> now = hsv_of(rgba, width, height);
  scores_.push_back(last_.size() == now.size() ? difference(now, last_) : 0.0);
  skips_.push_back(before_.size() == now.size() ? difference(now, before_) : 0.0);
  before_ = std::move(last_);
  last_ = std::move(now);
}

std::vector<int> SceneDetector::cuts() const {
  std::vector<int> out;
  const int count = static_cast<int>(scores_.size());
  int last_cut = 0;
  for (int i = 1; i < count; ++i) {
    const double score = scores_[static_cast<std::size_t>(i)];
    if (score < kLeast) continue;
    double around = 0.0;
    int counted = 0;
    for (int j = i - kNeighbours; j <= i + kNeighbours; ++j) {
      if (j == i || j < 1 || j >= count) continue;
      around += scores_[static_cast<std::size_t>(j)];
      ++counted;
    }
    if (counted > 0 && score < kAgainstNeighbours * std::max(around / counted, 1.0)) continue;
    // Across it both ways: the new shot differs from the frame before the
    // last one too, and the frame after still differs from the old shot. A
    // flash fails one or the other.
    if (i >= 2 && skips_[static_cast<std::size_t>(i)] < kLeast) continue;
    if (i + 1 < count && skips_[static_cast<std::size_t>(i) + 1] < kLeast) continue;
    if (i - last_cut < shortest_ && last_cut > 0) continue;
    out.push_back(i);
    last_cut = i;
  }
  return out;
}

}  // namespace cutline::track
