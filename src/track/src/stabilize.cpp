#include "cutline/track/stabilize.hpp"

#include "linear.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <numbers>

namespace cutline::track {
namespace {

// ------------------------------------------------------------------ file --

constexpr char kMagic[4] = {'C', 'W', 'S', 'A'};
constexpr std::uint32_t kVersion = 1;

template <typename T>
void put(std::ofstream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
[[nodiscard]] bool take(std::ifstream& in, T& value) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

// ------------------------------------------------------------- smoothing --

/// A Gaussian average over `sigma` frames each side, the weights made up to
/// one again where the run ends, so the first and last frames are averaged
/// over what there is rather than pulled towards nothing.
[[nodiscard]] std::vector<double> smoothed(const std::vector<double>& values, double sigma) {
  if (sigma < 0.5 || values.size() < 2) return values;
  const auto reach = static_cast<std::ptrdiff_t>(std::ceil(3.0 * sigma));
  const auto n = static_cast<std::ptrdiff_t>(values.size());
  std::vector<double> out(values.size());
  for (std::ptrdiff_t i = 0; i < n; ++i) {
    double sum = 0.0;
    double weight = 0.0;
    for (std::ptrdiff_t j = std::max<std::ptrdiff_t>(0, i - reach);
         j <= std::min(n - 1, i + reach); ++j) {
      const double d = static_cast<double>(j - i);
      const double w = std::exp(-d * d / (2.0 * sigma * sigma));
      sum += values[static_cast<std::size_t>(j)] * w;
      weight += w;
    }
    out[static_cast<std::size_t>(i)] = sum / weight;
  }
  return out;
}

/// The pairs of corners two frames share, by their numbers.
void shared(const std::vector<TrackPoint>& a, const std::vector<TrackPoint>& b,
            std::vector<Point>& from, std::vector<Point>& to) {
  from.clear();
  to.clear();
  std::map<std::uint32_t, const TrackPoint*> earlier;
  for (const TrackPoint& p : a) earlier[p.id] = &p;
  for (const TrackPoint& p : b) {
    const auto found = earlier.find(p.id);
    if (found == earlier.end()) continue;
    from.push_back({found->second->x, found->second->y});
    to.push_back({p.x, p.y});
  }
}

/// The middle of the moves a frame's corners made, which is the camera's
/// speed as a rolling shutter feels it.
[[nodiscard]] Point velocity(const std::vector<Point>& from, const std::vector<Point>& to) {
  if (from.empty()) return {};
  std::vector<double> xs;
  std::vector<double> ys;
  for (std::size_t i = 0; i < from.size(); ++i) {
    xs.push_back(to[i].x - from[i].x);
    ys.push_back(to[i].y - from[i].y);
  }
  const auto middle = [](std::vector<double>& v) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
  };
  return {middle(xs), middle(ys)};
}

/// A point with the rolling shutter's lean taken out: a row read later in the
/// frame saw the scene later, by `readout` of a frame at the bottom.
[[nodiscard]] Point upright(Point p, Point speed, double readout, double height) {
  const double late = readout * (p.y / height - 0.5);
  return {p.x - speed.x * late, p.y - speed.y * late};
}

/// The camera's move from one frame to the next, as far as `method` follows.
[[nodiscard]] Homography step(const std::vector<Point>& from, const std::vector<Point>& to,
                              StabilizeMethod method, Point centre) {
  if (method == StabilizeMethod::Perspective || method == StabilizeMethod::SubspaceWarp) {
    if (auto h = fit_homography(from, to); h.has_value()) return *h;
  }
  const Motion motion =
      method == StabilizeMethod::Position ? Motion::Translation : Motion::Similarity;
  if (auto s = fit_motion(from, to, motion, centre); s.has_value()) return Homography::of(*s);
  return {};
}

/// How far a set of moves misses its corners at a readout: lower is a better
/// guess at the shutter.
[[nodiscard]] double misfit(const Analysis& a, const std::vector<Point>& speed, double readout,
                            std::size_t stride) {
  const Point centre{a.width / 2.0, a.height / 2.0};
  double total = 0.0;
  std::vector<Point> from;
  std::vector<Point> to;
  for (std::size_t i = 1; i < a.frames.size(); i += stride) {
    shared(a.frames[i - 1], a.frames[i], from, to);
    if (from.size() < 8) continue;
    // Each frame leans by its own speed.
    for (std::size_t k = 0; k < from.size(); ++k) {
      from[k] = upright(from[k], speed[i - 1], readout, a.height);
      to[k] = upright(to[k], speed[i], readout, a.height);
    }
    const auto fit = fit_motion(from, to, Motion::Similarity, centre);
    if (!fit.has_value()) continue;
    for (std::size_t k = 0; k < from.size(); ++k) {
      const Point p = fit->apply(from[k]);
      total += std::min(4.0, std::hypot(p.x - to[k].x, p.y - to[k].y));
    }
  }
  return total;
}

/// The size a perspective gives things about the middle of the frame.
[[nodiscard]] double scale_of(const Homography& h) {
  return std::sqrt(std::abs(h.m[0] * h.m[4] - h.m[1] * h.m[3]));
}

/// A grid's place at an output point, in pixels, from vertices in pixels.
[[nodiscard]] Point grid_at(const std::vector<Point>& vertices, double x, double y, double w,
                            double h) {
  const double gx = std::clamp(x / w * kWarpColumns, 0.0, static_cast<double>(kWarpColumns));
  const double gy = std::clamp(y / h * kWarpRows, 0.0, static_cast<double>(kWarpRows));
  const int c = std::min(static_cast<int>(gx), kWarpColumns - 1);
  const int r = std::min(static_cast<int>(gy), kWarpRows - 1);
  const auto v = [&](int cc, int rr) { return vertices[static_cast<std::size_t>(rr * (kWarpColumns + 1) + cc)]; };
  const Point a = v(c, r);
  const Point b = v(c + 1, r);
  const Point d = v(c, r + 1);
  const Point e = v(c + 1, r + 1);
  // Past the frame's edge the nearest cell is carried on, so a point just
  // outside is placed as the edge would place it.
  const double ux = x / w * kWarpColumns - c;
  const double uy = y / h * kWarpRows - r;
  return {(1 - uy) * ((1 - ux) * a.x + ux * b.x) + uy * ((1 - ux) * d.x + ux * e.x),
          (1 - uy) * ((1 - ux) * a.y + ux * b.y) + uy * ((1 - ux) * d.y + ux * e.y)};
}

}  // namespace

bool save_analysis(const Analysis& analysis, const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(kMagic, 4);
  put(out, kVersion);
  put(out, static_cast<std::int32_t>(analysis.width));
  put(out, static_cast<std::int32_t>(analysis.height));
  put(out, analysis.fps);
  put(out, static_cast<std::uint32_t>(analysis.frames.size()));
  for (const auto& frame : analysis.frames) {
    put(out, static_cast<std::uint32_t>(frame.size()));
    for (const TrackPoint& p : frame) {
      put(out, p.id);
      put(out, p.x);
      put(out, p.y);
    }
  }
  return static_cast<bool>(out);
}

std::optional<Analysis> load_analysis(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  char magic[4] = {};
  std::uint32_t version = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  Analysis a;
  std::uint32_t count = 0;
  if (!in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !take(in, version) ||
      version != kVersion || !take(in, width) || !take(in, height) || !take(in, a.fps) ||
      !take(in, count)) {
    return std::nullopt;
  }
  a.width = width;
  a.height = height;
  a.frames.resize(count);
  for (auto& frame : a.frames) {
    std::uint32_t points = 0;
    if (!take(in, points) || points > 100000) return std::nullopt;
    frame.resize(points);
    for (TrackPoint& p : frame) {
      if (!take(in, p.id) || !take(in, p.x) || !take(in, p.y)) return std::nullopt;
    }
  }
  return a;
}

Point WarpGrid::at(double x, double y) const noexcept {
  if (source.empty() || columns <= 0 || rows <= 0) return {x, y};
  const double gx = std::clamp(x, 0.0, 1.0) * columns;
  const double gy = std::clamp(y, 0.0, 1.0) * rows;
  const int c = std::min(static_cast<int>(gx), columns - 1);
  const int r = std::min(static_cast<int>(gy), rows - 1);
  const double fx = gx - c;
  const double fy = gy - r;
  const auto v = [&](int cc, int rr) { return source[static_cast<std::size_t>(rr * (columns + 1) + cc)]; };
  const Point a = v(c, r);
  const Point b = v(c + 1, r);
  const Point d = v(c, r + 1);
  const Point e = v(c + 1, r + 1);
  return {(1 - fy) * ((1 - fx) * a.x + fx * b.x) + fy * ((1 - fx) * d.x + fx * e.x),
          (1 - fy) * ((1 - fx) * a.y + fx * b.y) + fy * ((1 - fx) * d.y + fx * e.y)};
}

Stabilization stabilize(const Analysis& a, const StabilizeSettings& settings) {
  Stabilization out;
  out.width = a.width;
  out.height = a.height;
  const std::size_t n = a.frames.size();
  const double w = std::max(1, a.width);
  const double h = std::max(1, a.height);
  const Point centre{w / 2.0, h / 2.0};

  const auto identity = [] {
    WarpGrid grid{.columns = kWarpColumns, .rows = kWarpRows};
    for (int r = 0; r <= kWarpRows; ++r) {
      for (int c = 0; c <= kWarpColumns; ++c) {
        grid.source.push_back({static_cast<double>(c) / kWarpColumns,
                               static_cast<double>(r) / kWarpRows});
      }
    }
    return grid;
  };
  if (n < 2) {
    out.frames.assign(n, identity());
    out.camera.assign(n, Homography{});
    return out;
  }

  // The camera's speed on each frame, which is what a rolling shutter leans
  // the picture by.
  std::vector<Point> speed(n);
  {
    std::vector<Point> from;
    std::vector<Point> to;
    for (std::size_t i = 1; i < n; ++i) {
      shared(a.frames[i - 1], a.frames[i], from, to);
      speed[i] = velocity(from, to);
    }
    speed[0] = speed[1];
  }

  // The rolling shutter's readout. Automatic tries a few and takes the best,
  // over every fourth frame; Enhanced tries finely, over every one.
  const bool enhanced = settings.rolling_shutter == RollingShutter::Enhanced;
  {
    const double stride = enhanced ? 0.05 : 0.25;
    const std::size_t frames = enhanced ? 1 : 4;
    double best = misfit(a, speed, 0.0, frames);
    for (double r = stride; r <= 1.0 + 1e-9; r += stride) {
      const double miss = misfit(a, speed, r, frames);
      // Only for a clear improvement, so footage without the lean is left
      // alone rather than given one by noise.
      if (miss < best * 0.97) {
        best = miss;
        out.readout = r;
      }
    }
  }

  // Each frame's corners with the lean taken out.
  std::vector<std::vector<TrackPoint>> upright_frames = a.frames;
  {
    for (std::size_t i = 0; i < n; ++i) {
      for (TrackPoint& p : upright_frames[i]) {
        const Point q = upright({p.x, p.y}, speed[i], out.readout, h);
        p.x = static_cast<float>(q.x);
        p.y = static_cast<float>(q.y);
      }
    }
  }

  // The camera's path: where the first frame's world is on each frame.
  std::vector<Homography> camera(n);
  {
    std::vector<Point> from;
    std::vector<Point> to;
    for (std::size_t i = 1; i < n; ++i) {
      shared(upright_frames[i - 1], upright_frames[i], from, to);
      camera[i] = step(from, to, settings.method, centre) * camera[i - 1];
    }
  }

  // The path wanted. Cropping trades smoothness for less zoom, as Crop Less
  // <-> Smooth More says.
  const bool cropping = settings.framing == Framing::StabilizeCrop ||
                        settings.framing == Framing::StabilizeCropAutoScale;
  const double sigma = settings.smoothness * (a.fps > 0.0 ? a.fps : 30.0) *
                       (cropping ? 0.5 + settings.crop_less_smooth_more : 1.0);
  std::vector<Homography> wanted(n);
  const bool perspective = settings.method == StabilizeMethod::Perspective ||
                           settings.method == StabilizeMethod::SubspaceWarp;
  if (settings.result == StabilizeResult::NoMotion) {
    std::ranges::fill(wanted, camera[n / 2]);
  } else if (perspective) {
    std::array<std::vector<double>, 8> entries;
    for (const Homography& c : camera) {
      for (int k = 0; k < 8; ++k) entries[k].push_back(c.m[k]);
    }
    for (auto& e : entries) e = smoothed(e, sigma);
    for (std::size_t i = 0; i < n; ++i) {
      for (int k = 0; k < 8; ++k) wanted[i].m[k] = entries[k][i];
    }
  } else {
    // A move, a turn and a size about the middle, smoothed as such.
    std::array<std::vector<double>, 4> parts;
    for (const Homography& c : camera) {
      const Point moved = c.apply(centre);
      parts[0].push_back(moved.x - centre.x);
      parts[1].push_back(moved.y - centre.y);
      parts[2].push_back(std::atan2(c.m[3], c.m[0]));
      parts[3].push_back(std::log(std::max(1e-6, std::hypot(c.m[0], c.m[3]))));
    }
    // Turns are kept continuous before they are averaged.
    for (std::size_t i = 1; i < n; ++i) {
      while (parts[2][i] - parts[2][i - 1] > std::numbers::pi) parts[2][i] -= 2 * std::numbers::pi;
      while (parts[2][i] - parts[2][i - 1] < -std::numbers::pi) parts[2][i] += 2 * std::numbers::pi;
    }
    for (auto& p : parts) p = smoothed(p, sigma);
    for (std::size_t i = 0; i < n; ++i) {
      wanted[i] = Homography::of(Similarity{.origin = centre,
                                            .dx = parts[0][i],
                                            .dy = parts[1][i],
                                            .rotation = parts[2][i],
                                            .scale = std::exp(parts[3][i])});
    }
  }
  if (settings.preserve_scale && settings.method != StabilizeMethod::Position) {
    for (std::size_t i = 0; i < n; ++i) {
      const double k = scale_of(camera[i]) / std::max(1e-9, scale_of(wanted[i]));
      Homography zoom;
      zoom.m = {k, 0.0, centre.x * (1 - k), 0.0, k, centre.y * (1 - k), 0.0, 0.0, 1.0};
      wanted[i] = zoom * wanted[i];
    }
  }

  out.camera = camera;

  // Each output pixel is drawn from where the camera had it: camera, after the
  // wanted path undone.
  std::vector<Homography> drawn(n);
  for (std::size_t i = 0; i < n; ++i) drawn[i] = camera[i] * wanted[i].inverse();

  const std::size_t vertices = static_cast<std::size_t>((kWarpColumns + 1) * (kWarpRows + 1));
  const auto vertex_at = [&](std::size_t v) {
    return Point{static_cast<double>(v % (kWarpColumns + 1)) / kWarpColumns * w,
                 static_cast<double>(v / (kWarpColumns + 1)) / kWarpRows * h};
  };
  std::vector<std::vector<Point>> grids(n, std::vector<Point>(vertices));
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t v = 0; v < vertices; ++v) grids[i][v] = drawn[i].apply(vertex_at(v));
  }

  if (settings.method == StabilizeMethod::SubspaceWarp) {
    // Each corner's path, steadied by the camera's correction and then
    // smoothed along its own life, is where it should land.
    std::map<std::uint32_t, std::vector<std::pair<std::size_t, Point>>> paths;
    for (std::size_t i = 0; i < n; ++i) {
      const Homography steady = wanted[i] * camera[i].inverse();
      for (const TrackPoint& p : upright_frames[i]) {
        paths[p.id].push_back({i, steady.apply({p.x, p.y})});
      }
    }
    // Per frame: where each corner should land, and where it was.
    std::vector<std::vector<std::pair<Point, Point>>> landing(n);
    for (auto& [id, path] : paths) {
      if (path.size() < 3) continue;
      std::vector<double> xs;
      std::vector<double> ys;
      for (const auto& [frame, p] : path) {
        xs.push_back(p.x);
        ys.push_back(p.y);
      }
      xs = smoothed(xs, sigma);
      ys = smoothed(ys, sigma);
      for (std::size_t k = 0; k < path.size(); ++k) {
        const std::size_t frame = path[k].first;
        const auto source = std::ranges::find(upright_frames[frame], id, &TrackPoint::id);
        landing[frame].push_back({Point{xs[k], ys[k]}, Point{source->x, source->y}});
      }
    }

    // The mesh, bent from the camera's correction so each corner lands where
    // it should. A Laplacian keeps neighbouring cells bending alike, and a
    // small pull back to the camera's correction keeps empty cells in place.
    // Corners that miss by far more than the rest are on things moving in
    // the scene, and are left out.
    const auto neighbours = [](std::size_t v) {
      std::vector<std::size_t> out;
      const int c = static_cast<int>(v % (kWarpColumns + 1));
      const int r = static_cast<int>(v / (kWarpColumns + 1));
      if (c > 0) out.push_back(v - 1);
      if (c < kWarpColumns) out.push_back(v + 1);
      if (r > 0) out.push_back(v - (kWarpColumns + 1));
      if (r < kWarpRows) out.push_back(v + (kWarpColumns + 1));
      return out;
    };
    for (std::size_t i = 0; i < n; ++i) {
      const auto& lands = landing[i];
      if (lands.size() < 8) continue;
      std::vector<double> misses;
      for (const auto& [to, from] : lands) {
        const Point p = grid_at(grids[i], to.x, to.y, w, h);
        misses.push_back(std::hypot(p.x - from.x, p.y - from.y));
      }
      std::vector<double> sorted = misses;
      std::ranges::sort(sorted);
      const double limit = std::max(8.0, 3.0 * sorted[sorted.size() / 2]);

      std::vector<double> m(vertices * vertices, 0.0);
      std::vector<double> bx(vertices, 0.0);
      std::vector<double> by(vertices, 0.0);
      std::size_t used = 0;
      for (std::size_t k = 0; k < lands.size(); ++k) {
        if (misses[k] > limit) continue;
        const auto& [to, from] = lands[k];
        if (to.x < 0 || to.y < 0 || to.x > w || to.y > h) continue;
        const double gx = to.x / w * kWarpColumns;
        const double gy = to.y / h * kWarpRows;
        const int c = std::min(static_cast<int>(gx), kWarpColumns - 1);
        const int r = std::min(static_cast<int>(gy), kWarpRows - 1);
        const double fx = gx - c;
        const double fy = gy - r;
        const std::size_t base = static_cast<std::size_t>(r * (kWarpColumns + 1) + c);
        const std::size_t at[4] = {base, base + 1, base + kWarpColumns + 1, base + kWarpColumns + 2};
        const double weight[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
        // The residual the bend has to make up, from the camera's correction.
        double rx = from.x;
        double ry = from.y;
        for (int j = 0; j < 4; ++j) {
          rx -= weight[j] * grids[i][at[j]].x;
          ry -= weight[j] * grids[i][at[j]].y;
        }
        for (int j = 0; j < 4; ++j) {
          for (int l = 0; l < 4; ++l) m[at[j] * vertices + at[l]] += weight[j] * weight[l];
          bx[at[j]] += weight[j] * rx;
          by[at[j]] += weight[j] * ry;
        }
        ++used;
      }
      if (used < 8) continue;
      const double laplacian = 1.0 + static_cast<double>(used) / vertices;
      for (std::size_t v = 0; v < vertices; ++v) {
        const std::vector<std::size_t> around = neighbours(v);
        // (D_v - mean of neighbours)², expanded into the normal equations.
        std::vector<std::pair<std::size_t, double>> row{{v, 1.0}};
        for (const std::size_t u : around) row.push_back({u, -1.0 / around.size()});
        for (const auto& [p, a1] : row) {
          for (const auto& [q, a2] : row) m[p * vertices + q] += laplacian * a1 * a2;
        }
        m[v * vertices + v] += 0.01;
      }
      std::vector<double> m2 = m;
      if (!detail::solve(m, bx, vertices) || !detail::solve(m2, by, vertices)) continue;
      for (std::size_t v = 0; v < vertices; ++v) {
        grids[i][v].x += bx[v];
        grids[i][v].y += by[v];
      }
    }
  }

  // The lean put back, so each row is read from where that row saw it.
  if (out.readout > 0.0) {
    for (std::size_t i = 0; i < n; ++i) {
      for (Point& p : grids[i]) {
        const double late = out.readout * (p.y / h - 0.5);
        p.x += speed[i].x * late;
        p.y += speed[i].y * late;
      }
    }
  }

  // Framing. The zoom about the middle that keeps every frame covered, up to
  // Maximum Scale, with the action-safe margin allowed to go uncovered.
  const auto covered = [&](double zoom) {
    const double m = std::clamp(settings.safe_margin, 0.0, 0.45);
    for (std::size_t i = 0; i < n; ++i) {
      for (int k = 0; k < 64; ++k) {
        const double t = k / 16.0;
        const int side = k / 16;
        const double f = t - side;
        Point b;
        switch (side) {
          case 0: b = {m + (1 - 2 * m) * f, m}; break;
          case 1: b = {1 - m, m + (1 - 2 * m) * f}; break;
          case 2: b = {1 - m - (1 - 2 * m) * f, 1 - m}; break;
          default: b = {m, 1 - m - (1 - 2 * m) * f}; break;
        }
        const Point o{centre.x + (b.x * w - centre.x) / zoom, centre.y + (b.y * h - centre.y) / zoom};
        const Point s = grid_at(grids[i], o.x, o.y, w, h);
        if (s.x < -0.5 || s.y < -0.5 || s.x > w + 0.5 || s.y > h + 0.5) return false;
      }
    }
    return true;
  };
  double fit = 1.0;
  const bool autoscale = settings.framing == Framing::StabilizeCropAutoScale ||
                         settings.framing == Framing::StabilizeCrop;
  if (autoscale && !covered(1.0)) {
    const double most = std::max(1.0, settings.maximum_scale);
    if (!covered(most)) {
      fit = most;
    } else {
      double low = 1.0;
      double high = most;
      for (int k = 0; k < 24; ++k) {
        const double mid = (low + high) / 2.0;
        (covered(mid) ? high : low) = mid;
      }
      fit = high;
    }
  }
  double zoom = std::max(0.01, settings.additional_scale);
  if (settings.framing == Framing::StabilizeCropAutoScale) {
    zoom *= fit;
    out.auto_scale = fit;
  } else if (settings.framing == Framing::StabilizeCrop) {
    out.visible = 1.0 / fit;
  }

  out.frames.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    WarpGrid grid{.columns = kWarpColumns, .rows = kWarpRows};
    grid.source.reserve(vertices);
    for (std::size_t v = 0; v < vertices; ++v) {
      const Point o = vertex_at(v);
      const Point z{centre.x + (o.x - centre.x) / zoom, centre.y + (o.y - centre.y) / zoom};
      const Point s = grid_at(grids[i], z.x, z.y, w, h);
      grid.source.push_back({s.x / w, s.y / h});
    }
    out.frames.push_back(std::move(grid));
  }
  return out;
}

}  // namespace cutline::track
