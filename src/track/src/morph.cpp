#include "cutline/track/morph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace cutline::track {
namespace {

/// A move per pixel, in that level's pixels.
struct Field {
  int width = 0;
  int height = 0;
  std::vector<float> u;
  std::vector<float> v;

  Field() = default;
  Field(int w, int h)
      : width(w),
        height(h),
        u(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0.0f),
        v(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0.0f) {}

  [[nodiscard]] std::size_t index(int x, int y) const noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x);
  }
};

/// Each value replaced by the mean of the square `radius` either side of it,
/// through a summed-area table so a wide one costs no more than a narrow one.
void box(std::vector<float>& values, int width, int height, int radius) {
  if (radius <= 0) return;
  const auto w = static_cast<std::size_t>(width);
  std::vector<double> sums((w + 1) * static_cast<std::size_t>(height + 1), 0.0);
  for (int y = 0; y < height; ++y) {
    double row = 0.0;
    for (int x = 0; x < width; ++x) {
      row += values[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)];
      sums[static_cast<std::size_t>(y + 1) * (w + 1) + static_cast<std::size_t>(x + 1)] =
          sums[static_cast<std::size_t>(y) * (w + 1) + static_cast<std::size_t>(x + 1)] + row;
    }
  }
  for (int y = 0; y < height; ++y) {
    const int y0 = std::max(0, y - radius);
    const int y1 = std::min(height, y + radius + 1);
    for (int x = 0; x < width; ++x) {
      const int x0 = std::max(0, x - radius);
      const int x1 = std::min(width, x + radius + 1);
      const auto at = [&](int xx, int yy) {
        return sums[static_cast<std::size_t>(yy) * (w + 1) + static_cast<std::size_t>(xx)];
      };
      const double total = at(x1, y1) - at(x0, y1) - at(x1, y0) + at(x0, y0);
      values[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] =
          static_cast<float>(total / static_cast<double>((x1 - x0) * (y1 - y0)));
    }
  }
}

/// The middle of each three by three: one pixel's wrong answer, where the
/// window saw only an edge, does not survive it.
void median3(std::vector<float>& values, int width, int height) {
  const std::vector<float> was = values;
  std::array<float, 9> near{};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int n = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int xx = std::clamp(x + dx, 0, width - 1);
          const int yy = std::clamp(y + dy, 0, height - 1);
          near[static_cast<std::size_t>(n++)] =
              was[static_cast<std::size_t>(yy) * static_cast<std::size_t>(width) +
                  static_cast<std::size_t>(xx)];
        }
      }
      std::nth_element(near.begin(), near.begin() + 4, near.end());
      values[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(x)] = near[4];
    }
  }
}

/// A coarser level's field carried to the next size up: read between its
/// pixels, and the moves doubled with the pixels.
[[nodiscard]] Field upsampled(const Field& coarse, int width, int height) {
  Field out(width, height);
  const double sx = static_cast<double>(coarse.width) / width;
  const double sy = static_cast<double>(coarse.height) / height;
  Image u{coarse.width, coarse.height, coarse.u};
  Image v{coarse.width, coarse.height, coarse.v};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double cx = (x + 0.5) * sx - 0.5;
      const double cy = (y + 0.5) * sy - 0.5;
      out.u[out.index(x, y)] = static_cast<float>(u.sample(cx, cy) / sx);
      out.v[out.index(x, y)] = static_cast<float>(v.sample(cx, cy) / sy);
    }
  }
  return out;
}

/// How many times each level is refined, and over what window.
constexpr int kIterations = 5;
constexpr int kWindow = 2;
/// What keeps a window with no detail in it from answering at all: it moves
/// with its neighbours, through the smoothing, instead.
constexpr double kSteady = 1e-3;
/// The most one refinement may move a pixel, so a window that has gone wrong
/// is pulled back by its neighbours rather than flung across the frame.
constexpr float kMostStep = 2.0f;

void refine(const Image& a, const Image& b, float bias, Field& f) {
  const int w = a.width;
  const int h = a.height;
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  std::vector<float> warped(n);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t i = f.index(x, y);
      warped[i] = b.sample(x + f.u[i], y + f.v[i]);
    }
  }
  std::vector<float> xx(n), xy(n), yy(n), xt(n), yt(n);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::size_t i = f.index(x, y);
      const int l = std::max(0, x - 1);
      const int r = std::min(w - 1, x + 1);
      const int t = std::max(0, y - 1);
      const int d = std::min(h - 1, y + 1);
      // Both pictures' slopes, averaged: what either alone says is biased
      // towards its own frame.
      const float gx = 0.25f * (a.at(r, y) - a.at(l, y) + warped[f.index(r, y)] -
                                warped[f.index(l, y)]);
      const float gy = 0.25f * (a.at(x, d) - a.at(x, t) + warped[f.index(x, d)] -
                                warped[f.index(x, t)]);
      const float gt = warped[i] - a.pixels[i] - bias;
      xx[i] = gx * gx;
      xy[i] = gx * gy;
      yy[i] = gy * gy;
      xt[i] = gx * gt;
      yt[i] = gy * gt;
    }
  }
  for (std::vector<float>* sum : {&xx, &xy, &yy, &xt, &yt}) box(*sum, w, h, kWindow);
  // How sure each window is of its answer: the weaker of its two directions
  // of detail, which is nothing on a plain area and little along an edge.
  std::vector<float> sure(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double sxx = xx[i] + kSteady;
    const double syy = yy[i] + kSteady;
    const double det = sxx * syy - static_cast<double>(xy[i]) * xy[i];
    const double half_trace = 0.5 * (xx[i] + yy[i]);
    const double spread = std::sqrt(std::max(
        0.0, 0.25 * (xx[i] - yy[i]) * (xx[i] - yy[i]) + static_cast<double>(xy[i]) * xy[i]));
    sure[i] = static_cast<float>(std::max(0.0, half_trace - spread));
    if (det <= 0.0) continue;
    const auto du = static_cast<float>((-syy * xt[i] + xy[i] * yt[i]) / det);
    const auto dv = static_cast<float>((xy[i] * xt[i] - sxx * yt[i]) / det);
    f.u[i] += std::clamp(du, -kMostStep, kMostStep);
    f.v[i] += std::clamp(dv, -kMostStep, kMostStep);
  }
  // A plain area says nothing about how it moved, so it takes what the
  // detail around it says: a cheek moves with the eyes and the mouth either
  // side of it. Weighted by how sure each neighbour is, over a reach of a
  // sixteenth of the frame, and only as far as the area is unsure itself.
  const int reach = std::max(2, w / 16);
  std::vector<float> weighted_u(n), weighted_v(n);
  for (std::size_t i = 0; i < n; ++i) {
    weighted_u[i] = sure[i] * f.u[i];
    weighted_v[i] = sure[i] * f.v[i];
  }
  std::vector<float> weights = sure;
  box(weighted_u, w, h, reach);
  box(weighted_v, w, h, reach);
  box(weights, w, h, reach);
  for (std::size_t i = 0; i < n; ++i) {
    const float trust = sure[i] / (sure[i] + static_cast<float>(kSteady));
    const float total = weights[i] + 1e-6f;
    const float fill_u = (weighted_u[i] + 1e-6f * f.u[i]) / total;
    const float fill_v = (weighted_v[i] + 1e-6f * f.v[i]) / total;
    f.u[i] = trust * f.u[i] + (1.0f - trust) * fill_u;
    f.v[i] = trust * f.v[i] + (1.0f - trust) * fill_v;
  }
  median3(f.u, w, h);
  median3(f.v, w, h);
  box(f.u, w, h, 1);
  box(f.v, w, h, 1);
}

[[nodiscard]] double mean(const Image& image) {
  double sum = 0.0;
  for (const float p : image.pixels) sum += p;
  return image.pixels.empty() ? 0.0 : sum / static_cast<double>(image.pixels.size());
}

}  // namespace

Point FlowField::at(double x, double y) const noexcept {
  if (moves.empty() || columns <= 0 || rows <= 0) return {};
  const double gx = std::clamp(x, 0.0, 1.0) * columns;
  const double gy = std::clamp(y, 0.0, 1.0) * rows;
  const int cx = std::min(static_cast<int>(gx), columns - 1);
  const int cy = std::min(static_cast<int>(gy), rows - 1);
  const double tx = gx - cx;
  const double ty = gy - cy;
  const auto corner = [&](int i, int j) {
    return moves[static_cast<std::size_t>(j) * static_cast<std::size_t>(columns + 1) +
                 static_cast<std::size_t>(i)];
  };
  const Point a = corner(cx, cy);
  const Point b = corner(cx + 1, cy);
  const Point c = corner(cx, cy + 1);
  const Point d = corner(cx + 1, cy + 1);
  return {(a.x * (1 - tx) + b.x * tx) * (1 - ty) + (c.x * (1 - tx) + d.x * tx) * ty,
          (a.y * (1 - tx) + b.y * tx) * (1 - ty) + (c.y * (1 - tx) + d.y * tx) * ty};
}

FlowField dense_flow(const Image& from, const Image& to, int columns, int rows) {
  FlowField out{.columns = columns, .rows = rows};
  out.moves.assign(static_cast<std::size_t>(columns + 1) * static_cast<std::size_t>(rows + 1),
                   Point{});
  if (from.width < 2 || from.height < 2 || from.width != to.width || from.height != to.height) {
    return out;
  }

  // Down to about thirty pixels, where a head that has crossed a tenth of a
  // wide frame has moved a pixel or two.
  const Pyramid a(from, 6);
  const Pyramid b(to, 6);
  const int levels = std::min(a.levels(), b.levels());
  // A shot that has brightened between the two halves is still the same shot.
  const auto bias = static_cast<float>(mean(to) - mean(from));

  Field f;
  for (int level = levels - 1; level >= 0; --level) {
    const Image& la = a.level(level);
    const Image& lb = b.level(level);
    f = f.width == 0 ? Field(la.width, la.height) : upsampled(f, la.width, la.height);
    for (int i = 0; i < kIterations; ++i) refine(la, lb, bias, f);
  }

  // Each corner the mean of the cell around it, so the mesh bends smoothly
  // rather than following every pixel's answer.
  const int w = from.width;
  const int h = from.height;
  const int reach = std::max(1, std::min(w / columns, h / rows) / 2);
  box(f.u, w, h, reach);
  box(f.v, w, h, reach);
  Image u{w, h, f.u};
  Image v{w, h, f.v};
  for (int j = 0; j <= rows; ++j) {
    for (int i = 0; i <= columns; ++i) {
      const double x = static_cast<double>(i) / columns * w - 0.5;
      const double y = static_cast<double>(j) / rows * h - 0.5;
      out.moves[static_cast<std::size_t>(j) * static_cast<std::size_t>(columns + 1) +
                static_cast<std::size_t>(i)] = {u.sample(x, y) / w, v.sample(x, y) / h};
    }
  }
  return out;
}

namespace {

constexpr char kMagic[4] = {'C', 'M', 'C', 'A'};
constexpr std::uint32_t kVersion = 1;

template <typename T>
void put(std::ofstream& out, T value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
[[nodiscard]] bool take(std::ifstream& in, T& value) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

}  // namespace

bool save_morph(const MorphAnalysis& analysis, const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(kMagic, 4);
  put(out, kVersion);
  put(out, analysis.fps);
  put(out, static_cast<std::uint32_t>(analysis.frames.size()));
  for (const FlowField& field : analysis.frames) {
    put(out, static_cast<std::int32_t>(field.columns));
    put(out, static_cast<std::int32_t>(field.rows));
    for (const Point& p : field.moves) {
      put(out, static_cast<float>(p.x));
      put(out, static_cast<float>(p.y));
    }
  }
  return static_cast<bool>(out);
}

std::optional<MorphAnalysis> load_morph(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  char magic[4] = {};
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  MorphAnalysis a;
  if (!in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !take(in, version) ||
      version != kVersion || !take(in, a.fps) || !take(in, count) || count > 100'000) {
    return std::nullopt;
  }
  a.frames.reserve(count);
  for (std::uint32_t k = 0; k < count; ++k) {
    std::int32_t columns = 0;
    std::int32_t rows = 0;
    if (!take(in, columns) || !take(in, rows) || columns < 1 || rows < 1 || columns > 256 ||
        rows > 256) {
      return std::nullopt;
    }
    FlowField field{.columns = columns, .rows = rows};
    const auto corners = static_cast<std::size_t>(columns + 1) * static_cast<std::size_t>(rows + 1);
    field.moves.reserve(corners);
    for (std::size_t i = 0; i < corners; ++i) {
      float x = 0.0f;
      float y = 0.0f;
      if (!take(in, x) || !take(in, y)) return std::nullopt;
      field.moves.push_back({x, y});
    }
    a.frames.push_back(std::move(field));
  }
  return a;
}

}  // namespace cutline::track
