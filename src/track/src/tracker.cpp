#include "cutline/track/tracker.hpp"

#include "linear.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace cutline::track {
namespace {

/// Half the side of the patch a point is matched by.
constexpr int kWindow = 7;
/// How far a point may land from where it started when followed back, in
/// pixels, and still be believed.
constexpr double kRoundTrip = 1.0;
/// How far from the fitted motion a point may be and still agree with it.
constexpr double kAgreement = 1.5;
/// The fewest points a fit is believed on.
constexpr std::size_t kFewestPoints = 4;

/// Half the size, each pixel the average of the four it covers.
[[nodiscard]] Image halved(const Image& image) {
  Image out;
  out.width = std::max(1, image.width / 2);
  out.height = std::max(1, image.height / 2);
  out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      const int x0 = std::min(2 * x, image.width - 1);
      const int y0 = std::min(2 * y, image.height - 1);
      const int x1 = std::min(x0 + 1, image.width - 1);
      const int y1 = std::min(y0 + 1, image.height - 1);
      out.pixels[static_cast<std::size_t>(y) * out.width + x] =
          0.25f * (image.at(x0, y0) + image.at(x1, y0) + image.at(x0, y1) + image.at(x1, y1));
    }
  }
  return out;
}

/// Follows one point from `a` to `b` at one level, starting from `guess`.
/// Returns where it ended, or nothing when the patch had no detail to match.
[[nodiscard]] std::optional<Point> follow(const Image& a, const Image& b, Point p, Point guess) {
  // The gradient and the structure of the patch in the first picture, which
  // stay the same while the second is searched.
  constexpr int kSide = 2 * kWindow + 1;
  std::vector<float> ix(kSide * kSide);
  std::vector<float> iy(kSide * kSide);
  std::vector<float> va(kSide * kSide);
  double gxx = 0.0;
  double gxy = 0.0;
  double gyy = 0.0;
  for (int j = -kWindow; j <= kWindow; ++j) {
    for (int i = -kWindow; i <= kWindow; ++i) {
      const double x = p.x + i;
      const double y = p.y + j;
      const std::size_t at = static_cast<std::size_t>((j + kWindow) * kSide + (i + kWindow));
      ix[at] = 0.5f * (a.sample(x + 1, y) - a.sample(x - 1, y));
      iy[at] = 0.5f * (a.sample(x, y + 1) - a.sample(x, y - 1));
      va[at] = a.sample(x, y);
      gxx += ix[at] * ix[at];
      gxy += ix[at] * iy[at];
      gyy += iy[at] * iy[at];
    }
  }
  const double det = gxx * gyy - gxy * gxy;
  if (det < 1e-9) return std::nullopt;

  Point q = guess;
  for (int iteration = 0; iteration < 20; ++iteration) {
    double bx = 0.0;
    double by = 0.0;
    for (int j = -kWindow; j <= kWindow; ++j) {
      for (int i = -kWindow; i <= kWindow; ++i) {
        const std::size_t at = static_cast<std::size_t>((j + kWindow) * kSide + (i + kWindow));
        const double difference = va[at] - b.sample(q.x + i, q.y + j);
        bx += difference * ix[at];
        by += difference * iy[at];
      }
    }
    const double sx = (gyy * bx - gxy * by) / det;
    const double sy = (gxx * by - gxy * bx) / det;
    q.x += sx;
    q.y += sy;
    if (sx * sx + sy * sy < 1e-4) break;
  }
  return q;
}

/// Follows a point down the pyramid, coarsest first.
[[nodiscard]] std::optional<Point> follow_pyramid(const Pyramid& from, const Pyramid& to,
                                                  Point p) {
  const int levels = std::min(from.levels(), to.levels());
  Point guess{p.x / std::exp2(levels - 1), p.y / std::exp2(levels - 1)};
  for (int level = levels - 1; level >= 0; --level) {
    const double factor = std::exp2(level);
    const Point here{p.x / factor, p.y / factor};
    const std::optional<Point> found = follow(from.level(level), to.level(level), here, guess);
    if (!found.has_value()) return std::nullopt;
    guess = level > 0 ? Point{found->x * 2.0, found->y * 2.0} : *found;
  }
  return guess;
}

/// The least-squares motion of the pairs named by `use`.
[[nodiscard]] Similarity least_squares(std::span<const Point> from, std::span<const Point> to,
                                       std::span<const std::size_t> use, Motion motion,
                                       Point origin) {
  Point cf{};
  Point ct{};
  for (const std::size_t i : use) {
    cf.x += from[i].x;
    cf.y += from[i].y;
    ct.x += to[i].x;
    ct.y += to[i].y;
  }
  const auto n = static_cast<double>(use.size());
  cf = {cf.x / n, cf.y / n};
  ct = {ct.x / n, ct.y / n};

  double rotation = 0.0;
  double scale = 1.0;
  if (motion != Motion::Translation && use.size() >= 2) {
    double a = 0.0;
    double b = 0.0;
    double spread = 0.0;
    for (const std::size_t i : use) {
      const double dx = from[i].x - cf.x;
      const double dy = from[i].y - cf.y;
      const double ex = to[i].x - ct.x;
      const double ey = to[i].y - ct.y;
      a += dx * ex + dy * ey;
      b += dx * ey - dy * ex;
      spread += dx * dx + dy * dy;
    }
    rotation = std::atan2(b, a);
    if (motion == Motion::Similarity && spread > 1e-9) scale = std::hypot(a, b) / spread;
  }
  // The offset that puts the centre of the one set on the centre of the other.
  Similarity fit{.origin = origin, .rotation = rotation, .scale = scale};
  const Point moved = fit.apply(cf);
  fit.dx = ct.x - moved.x;
  fit.dy = ct.y - moved.y;
  return fit;
}

[[nodiscard]] double miss(const Similarity& fit, Point from, Point to) {
  const Point p = fit.apply(from);
  return std::hypot(p.x - to.x, p.y - to.y);
}

}  // namespace

float Image::sample(double x, double y) const noexcept {
  x = std::clamp(x, 0.0, static_cast<double>(width - 1));
  y = std::clamp(y, 0.0, static_cast<double>(height - 1));
  const int x0 = static_cast<int>(x);
  const int y0 = static_cast<int>(y);
  const int x1 = std::min(x0 + 1, width - 1);
  const int y1 = std::min(y0 + 1, height - 1);
  const auto fx = static_cast<float>(x - x0);
  const auto fy = static_cast<float>(y - y0);
  const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
  const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
  return top + (bottom - top) * fy;
}

Image grey_of_rgba(std::span<const std::uint8_t> rgba, int width, int height) {
  Image out;
  out.width = width;
  out.height = height;
  out.pixels.resize(static_cast<std::size_t>(width) * height);
  for (std::size_t i = 0; i < out.pixels.size() && i * 4 + 2 < rgba.size(); ++i) {
    out.pixels[i] = (0.2126f * rgba[i * 4] + 0.7152f * rgba[i * 4 + 1] +
                     0.0722f * rgba[i * 4 + 2]) / 255.0f;
  }
  return out;
}

bool inside(std::span<const Point> outline, Point p) noexcept {
  bool in = false;
  for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
    const Point& a = outline[i];
    const Point& b = outline[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      in = !in;
    }
  }
  return in;
}

std::vector<Point> find_features(const Image& image, std::span<const Point> region, int most,
                                 double spacing) {
  const int w = image.width;
  const int h = image.height;
  if (w < 8 || h < 8 || most <= 0) return {};

  // The structure of each pixel's neighbourhood, summed over a window, and its
  // smaller eigenvalue: Shi and Tomasi's measure of a corner. Large only where
  // the picture changes in two directions.
  std::vector<float> xx(static_cast<std::size_t>(w) * h);
  std::vector<float> xy(xx.size());
  std::vector<float> yy(xx.size());
  for (int y = 1; y < h - 1; ++y) {
    for (int x = 1; x < w - 1; ++x) {
      const float gx = 0.5f * (image.at(x + 1, y) - image.at(x - 1, y));
      const float gy = 0.5f * (image.at(x, y + 1) - image.at(x, y - 1));
      const std::size_t at = static_cast<std::size_t>(y) * w + x;
      xx[at] = gx * gx;
      xy[at] = gx * gy;
      yy[at] = gy * gy;
    }
  }
  // Summed over 5 by 5 through running sums, one direction then the other.
  const auto box = [w, h](std::vector<float>& field) {
    std::vector<float> across(field.size());
    for (int y = 0; y < h; ++y) {
      for (int x = 2; x < w - 2; ++x) {
        float sum = 0.0f;
        for (int i = -2; i <= 2; ++i) sum += field[static_cast<std::size_t>(y) * w + x + i];
        across[static_cast<std::size_t>(y) * w + x] = sum;
      }
    }
    for (int x = 0; x < w; ++x) {
      for (int y = 2; y < h - 2; ++y) {
        float sum = 0.0f;
        for (int j = -2; j <= 2; ++j) sum += across[static_cast<std::size_t>(y + j) * w + x];
        field[static_cast<std::size_t>(y) * w + x] = sum;
      }
    }
  };
  box(xx);
  box(xy);
  box(yy);

  std::vector<float> strength(xx.size(), 0.0f);
  float strongest = 0.0f;
  const int margin = kWindow + 2;
  for (int y = margin; y < h - margin; ++y) {
    for (int x = margin; x < w - margin; ++x) {
      const std::size_t at = static_cast<std::size_t>(y) * w + x;
      const float half = 0.5f * (xx[at] + yy[at]);
      const float spread = std::sqrt(0.25f * (xx[at] - yy[at]) * (xx[at] - yy[at]) + xy[at] * xy[at]);
      strength[at] = half - spread;
      strongest = std::max(strongest, strength[at]);
    }
  }
  if (strongest <= 0.0f) return {};

  // The local peaks, strong enough to matter, inside the region.
  struct Candidate {
    float strength;
    Point at;
  };
  std::vector<Candidate> candidates;
  const float floor = std::max(strongest * 0.01f, 1e-7f);
  for (int y = margin; y < h - margin; ++y) {
    for (int x = margin; x < w - margin; ++x) {
      const float s = strength[static_cast<std::size_t>(y) * w + x];
      if (s < floor) continue;
      bool peak = true;
      for (int j = -1; j <= 1 && peak; ++j) {
        for (int i = -1; i <= 1; ++i) {
          if ((i != 0 || j != 0) && strength[static_cast<std::size_t>(y + j) * w + x + i] > s) {
            peak = false;
            break;
          }
        }
      }
      const Point p{static_cast<double>(x), static_cast<double>(y)};
      if (peak && (region.empty() || inside(region, p))) candidates.push_back({s, p});
    }
  }
  std::ranges::sort(candidates, std::greater{}, &Candidate::strength);

  std::vector<Point> out;
  for (const Candidate& candidate : candidates) {
    const bool clear = std::ranges::none_of(out, [&](const Point& kept) {
      return std::hypot(kept.x - candidate.at.x, kept.y - candidate.at.y) < spacing;
    });
    if (!clear) continue;
    out.push_back(candidate.at);
    if (static_cast<int>(out.size()) >= most) break;
  }
  return out;
}

Pyramid::Pyramid(const Image& image, int levels) {
  images_.push_back(image);
  while (static_cast<int>(images_.size()) < levels && images_.back().width >= 32 &&
         images_.back().height >= 32) {
    images_.push_back(halved(images_.back()));
  }
}

std::vector<std::optional<Point>> track_points(const Pyramid& from, const Pyramid& to,
                                               std::span<const Point> points) {
  std::vector<std::optional<Point>> out;
  out.reserve(points.size());
  const Image& next = to.level(0);
  for (const Point& p : points) {
    const std::optional<Point> there = follow_pyramid(from, to, p);
    if (!there.has_value() || there->x < 0.0 || there->y < 0.0 ||
        there->x > next.width - 1 || there->y > next.height - 1) {
      out.push_back(std::nullopt);
      continue;
    }
    // Followed back again, it has to come home.
    const std::optional<Point> back = follow_pyramid(to, from, *there);
    if (!back.has_value() || std::hypot(back->x - p.x, back->y - p.y) > kRoundTrip) {
      out.push_back(std::nullopt);
      continue;
    }
    out.push_back(there);
  }
  return out;
}

Point Similarity::apply(Point p) const noexcept {
  const double c = std::cos(rotation);
  const double s = std::sin(rotation);
  const double x = p.x - origin.x;
  const double y = p.y - origin.y;
  return {origin.x + dx + scale * (c * x - s * y), origin.y + dy + scale * (s * x + c * y)};
}

std::optional<Similarity> fit_motion(std::span<const Point> from, std::span<const Point> to,
                                     Motion motion, Point origin) {
  const std::size_t n = std::min(from.size(), to.size());
  if (n < kFewestPoints) return std::nullopt;

  // Random pairs proposed and the proposal most of the rest agree with kept.
  // Seeded the same way every time, so the same pictures track the same way.
  std::mt19937 random(12345);
  std::uniform_int_distribution<std::size_t> pick(0, n - 1);
  const std::size_t sample = motion == Motion::Translation ? 1 : 2;
  std::vector<std::size_t> best;
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::vector<std::size_t> chosen;
    while (chosen.size() < sample) {
      const std::size_t i = pick(random);
      if (std::ranges::find(chosen, i) == chosen.end()) chosen.push_back(i);
    }
    const Similarity guess = least_squares(from, to, chosen, motion, origin);
    std::vector<std::size_t> agree;
    for (std::size_t i = 0; i < n; ++i) {
      if (miss(guess, from[i], to[i]) <= kAgreement) agree.push_back(i);
    }
    if (agree.size() > best.size()) best = std::move(agree);
    if (best.size() == n) break;
  }
  if (best.size() < kFewestPoints) return std::nullopt;
  return least_squares(from, to, best, motion, origin);
}

std::optional<Similarity> track_region(const Pyramid& from, const Pyramid& to,
                                       std::span<const Point> region, Motion motion,
                                       Point origin) {
  const std::vector<Point> corners = find_features(from.level(0), region, 200, 6.0);
  const std::vector<std::optional<Point>> followed = track_points(from, to, corners);
  std::vector<Point> a;
  std::vector<Point> b;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    if (!followed[i].has_value()) continue;
    a.push_back(corners[i]);
    b.push_back(*followed[i]);
  }
  return fit_motion(a, b, motion, origin);
}

// ------------------------------------------------------------ perspective --

Point Homography::apply(Point p) const noexcept {
  const double w = m[6] * p.x + m[7] * p.y + m[8];
  const double d = std::abs(w) < 1e-12 ? 1e-12 : w;
  return {(m[0] * p.x + m[1] * p.y + m[2]) / d, (m[3] * p.x + m[4] * p.y + m[5]) / d};
}

Homography Homography::inverse() const noexcept {
  const auto& a = m;
  const double c00 = a[4] * a[8] - a[5] * a[7];
  const double c01 = a[2] * a[7] - a[1] * a[8];
  const double c02 = a[1] * a[5] - a[2] * a[4];
  const double c10 = a[5] * a[6] - a[3] * a[8];
  const double c11 = a[0] * a[8] - a[2] * a[6];
  const double c12 = a[2] * a[3] - a[0] * a[5];
  const double c20 = a[3] * a[7] - a[4] * a[6];
  const double c21 = a[1] * a[6] - a[0] * a[7];
  const double c22 = a[0] * a[4] - a[1] * a[3];
  const double det = a[0] * c00 + a[1] * c10 + a[2] * c20;
  if (std::abs(det) < 1e-18) return {};
  Homography out;
  out.m = {c00 / det, c01 / det, c02 / det, c10 / det, c11 / det, c12 / det,
           c20 / det, c21 / det, c22 / det};
  if (std::abs(out.m[8]) > 1e-18) {
    const double k = out.m[8];
    for (double& v : out.m) v /= k;
  }
  return out;
}

Homography Homography::operator*(const Homography& other) const noexcept {
  Homography out;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 3; ++k) sum += m[r * 3 + k] * other.m[k * 3 + c];
      out.m[r * 3 + c] = sum;
    }
  }
  if (std::abs(out.m[8]) > 1e-18) {
    const double k = out.m[8];
    for (double& v : out.m) v /= k;
  }
  return out;
}

Homography Homography::of(const Similarity& s) noexcept {
  const double c = s.scale * std::cos(s.rotation);
  const double n = s.scale * std::sin(s.rotation);
  // origin + offset + R (p - origin), written out.
  Homography out;
  out.m = {c, -n, s.origin.x + s.dx - c * s.origin.x + n * s.origin.y,
           n, c,  s.origin.y + s.dy - n * s.origin.x - c * s.origin.y,
           0.0, 0.0, 1.0};
  return out;
}

namespace {

/// The direct linear fit of a perspective to the pairs named by `use`, with
/// both sets moved and scaled to a unit spread first so the arithmetic stays
/// well conditioned.
[[nodiscard]] std::optional<Homography> direct_fit(std::span<const Point> from,
                                                   std::span<const Point> to,
                                                   std::span<const std::size_t> use) {
  const auto normaliser = [&](std::span<const Point> points) {
    Point c{};
    for (const std::size_t i : use) {
      c.x += points[i].x;
      c.y += points[i].y;
    }
    c = {c.x / use.size(), c.y / use.size()};
    double spread = 0.0;
    for (const std::size_t i : use) spread += std::hypot(points[i].x - c.x, points[i].y - c.y);
    spread /= use.size();
    const double k = spread > 1e-12 ? std::sqrt(2.0) / spread : 1.0;
    Homography t;
    t.m = {k, 0.0, -k * c.x, 0.0, k, -k * c.y, 0.0, 0.0, 1.0};
    return t;
  };
  const Homography tf = normaliser(from);
  const Homography tt = normaliser(to);

  std::vector<double> a(64, 0.0);
  std::vector<double> b(8, 0.0);
  for (const std::size_t i : use) {
    const Point p = tf.apply(from[i]);
    const Point q = tt.apply(to[i]);
    const double rows[2][9] = {{p.x, p.y, 1.0, 0.0, 0.0, 0.0, -p.x * q.x, -p.y * q.x, q.x},
                               {0.0, 0.0, 0.0, p.x, p.y, 1.0, -p.x * q.y, -p.y * q.y, q.y}};
    for (const auto& row : rows) {
      for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) a[r * 8 + c] += row[r] * row[c];
        b[r] += row[r] * row[8];
      }
    }
  }
  if (!detail::solve(a, b, 8)) return std::nullopt;
  Homography h;
  h.m = {b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], 1.0};
  return tt.inverse() * h * tf;
}

}  // namespace

std::optional<Homography> fit_homography(std::span<const Point> from, std::span<const Point> to) {
  const std::size_t n = std::min(from.size(), to.size());
  if (n < 8) return std::nullopt;
  std::mt19937 random(54321);
  std::uniform_int_distribution<std::size_t> pick(0, n - 1);
  std::vector<std::size_t> best;
  for (int attempt = 0; attempt < 300; ++attempt) {
    std::vector<std::size_t> chosen;
    while (chosen.size() < 4) {
      const std::size_t i = pick(random);
      if (std::ranges::find(chosen, i) == chosen.end()) chosen.push_back(i);
    }
    const std::optional<Homography> guess = direct_fit(from, to, chosen);
    if (!guess.has_value()) continue;
    std::vector<std::size_t> agree;
    for (std::size_t i = 0; i < n; ++i) {
      const Point p = guess->apply(from[i]);
      if (std::hypot(p.x - to[i].x, p.y - to[i].y) <= 2.0) agree.push_back(i);
    }
    if (agree.size() > best.size()) best = std::move(agree);
    if (best.size() == n) break;
  }
  if (best.size() < 8) return std::nullopt;
  return direct_fit(from, to, best);
}

// --------------------------------------------------------- feature tracks --

FeatureTracker::FeatureTracker(int most, double spacing) : most_(most), spacing_(spacing) {}

std::vector<TrackPoint> FeatureTracker::next(const Image& image) {
  Pyramid now(image);
  std::vector<TrackPoint> kept;
  if (previous_.has_value() && !points_.empty()) {
    std::vector<Point> was;
    was.reserve(points_.size());
    for (const TrackPoint& p : points_) was.push_back({p.x, p.y});
    const std::vector<std::optional<Point>> followed = track_points(*previous_, now, was);
    for (std::size_t i = 0; i < points_.size(); ++i) {
      if (!followed[i].has_value()) continue;
      kept.push_back(TrackPoint{.id = points_[i].id,
                                .x = static_cast<float>(followed[i]->x),
                                .y = static_cast<float>(followed[i]->y)});
    }
  }
  // Topped up when a quarter has been lost, with corners away from those
  // still held, so the picture stays covered as things leave it.
  if (static_cast<int>(kept.size()) < most_ * 3 / 4) {
    for (const Point& corner : find_features(image, {}, most_ * 2, spacing_)) {
      if (static_cast<int>(kept.size()) >= most_) break;
      const bool clear = std::ranges::none_of(kept, [&](const TrackPoint& p) {
        return std::hypot(p.x - corner.x, p.y - corner.y) < spacing_;
      });
      if (clear) {
        kept.push_back(TrackPoint{.id = next_id_++,
                                  .x = static_cast<float>(corner.x),
                                  .y = static_cast<float>(corner.y)});
      }
    }
  }
  previous_ = std::move(now);
  points_ = kept;
  return kept;
}

}  // namespace cutline::track
