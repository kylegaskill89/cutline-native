#pragma once

/// Following a picture from one frame to the next: what mask tracking does,
/// and what Warp Stabilizer's analysis does over the whole frame.
///
/// The method is the usual one, because it works and is well understood.
/// Corners are found where the picture has detail in two directions, since a
/// point on a plain area or along an edge cannot be told from its neighbours.
/// Each is followed into the next frame by pyramidal Lucas–Kanade, which
/// matches the patch around it from coarse to fine so a large move is found
/// as readily as a small one, and kept only if following it back again lands
/// where it started. The motion is then fitted to the points that agree with
/// each other, so a hand passing in front of what is tracked moves a few of
/// them and not the answer.
///
/// Everything is in the pixels of the images given, on the processor, with
/// nothing to set up: a pure function of two pictures, which is what lets it be
/// tested with pictures made in the test.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cutline::track {

/// A grey picture, 0 to 1, row after row.
struct Image {
  int width = 0;
  int height = 0;
  std::vector<float> pixels;

  [[nodiscard]] float at(int x, int y) const noexcept {
    return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(x)];
  }
  /// Between pixels, read bilinearly, with the edge carried outward.
  [[nodiscard]] float sample(double x, double y) const noexcept;
};

/// The luma of 8-bit RGBA, Rec.709 weighted.
[[nodiscard]] Image grey_of_rgba(std::span<const std::uint8_t> rgba, int width, int height);

struct Point {
  double x = 0.0;
  double y = 0.0;

  friend bool operator==(const Point&, const Point&) = default;
};

/// Whether `p` is inside the closed polygon `outline`, by the even-odd rule.
[[nodiscard]] bool inside(std::span<const Point> outline, Point p) noexcept;

/// The strongest corners, at least `spacing` apart, strongest first. Inside
/// `region` when one is given, anywhere in the picture otherwise.
[[nodiscard]] std::vector<Point> find_features(const Image& image,
                                               std::span<const Point> region, int most,
                                               double spacing);

/// A picture at successively halved sizes, coarsest last.
class Pyramid {
 public:
  explicit Pyramid(const Image& image, int levels = 4);
  [[nodiscard]] int levels() const noexcept { return static_cast<int>(images_.size()); }
  [[nodiscard]] const Image& level(int at) const noexcept { return images_[at]; }

 private:
  std::vector<Image> images_;
};

/// Where each point went from `from` to `to`, or nothing for a point that was
/// lost: carried off the picture, or not where it started when followed back.
[[nodiscard]] std::vector<std::optional<Point>> track_points(const Pyramid& from,
                                                             const Pyramid& to,
                                                             std::span<const Point> points);

/// What a fit may use, as Premiere's Tracking Method names it.
enum class Motion { Translation, TranslationRotation, Similarity };

/// A move, a turn and a scale about `origin`: a point p goes to
/// origin + offset + scale · R(rotation) · (p − origin). The rotation is
/// clockwise on a picture whose y runs down, in radians.
struct Similarity {
  Point origin;
  double dx = 0.0;
  double dy = 0.0;
  double rotation = 0.0;
  double scale = 1.0;

  [[nodiscard]] Point apply(Point p) const noexcept;
};

/// The motion that takes `from` to `to` best, among the pairs that agree with
/// each other, or nothing when too few do. `from` and `to` are pairs, the same
/// length.
[[nodiscard]] std::optional<Similarity> fit_motion(std::span<const Point> from,
                                                   std::span<const Point> to, Motion motion,
                                                   Point origin);

/// One step of tracking a region: its corners found in `from`, followed into
/// `to`, and the motion fitted to them about `origin`. Nothing when the
/// region has too little detail to follow, or too little of it was followed.
[[nodiscard]] std::optional<Similarity> track_region(const Pyramid& from, const Pyramid& to,
                                                     std::span<const Point> region,
                                                     Motion motion, Point origin);

}  // namespace cutline::track
