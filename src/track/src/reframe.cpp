#include "cutline/track/reframe.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace cutline::track {
namespace {

/// How finely a frame is looked at: wide enough to place a window to within a
/// sixtieth of the frame before smoothing, coarse enough to cost nothing.
constexpr int kColumns = 64;

/// A map's level at its 95th percentile.
[[nodiscard]] float high_level(std::vector<float> values) {
  if (values.empty()) return 0.0f;
  const auto at = values.begin() + static_cast<std::ptrdiff_t>(values.size() * 95 / 100);
  std::nth_element(values.begin(), at, values.end());
  return *at;
}

/// Scaled so what stands out reaches one: by the 95th percentile, or by half
/// the brightest cell when that is higher, since a subject smaller than a
/// twentieth of the frame would otherwise be under the percentile and scaled
/// to nothing with the background.
void normalise(std::vector<float>& map) {
  const float brightest = map.empty() ? 0.0f : *std::ranges::max_element(map);
  const float level = std::max(high_level(map), 0.5f * brightest);
  if (!(level > 1e-6f)) {
    std::ranges::fill(map, 0.0f);
    return;
  }
  for (float& v : map) v = std::min(1.0f, v / level);
}

template <typename T>
void put(std::ofstream& out, T value) {
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
[[nodiscard]] bool take(std::ifstream& in, T& value) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

constexpr char kMagic[4] = {'C', 'A', 'R', 'A'};
constexpr std::uint32_t kVersion = 1;

}  // namespace

SubjectFinder::SubjectFinder(double window_width, double window_height)
    : window_width_(std::clamp(window_width, 0.01, 1.0)),
      window_height_(std::clamp(window_height, 0.01, 1.0)) {}

Point SubjectFinder::add(std::span<const std::uint8_t> rgba, int width, int height,
                         bool new_shot) {
  if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4) {
    return {0.5, 0.5};
  }
  const int columns = std::min(kColumns, width);
  const int rows = std::max(4, std::min(height, static_cast<int>(std::lround(
                                                    static_cast<double>(columns) * height / width))));
  const auto cells = static_cast<std::size_t>(columns) * rows;

  // Each cell the average of the pixels under it, in luma and the two chroma
  // differences, which is where skin is easiest to tell.
  std::vector<float> luma(cells, 0.0f);
  std::vector<float> blue(cells, 0.0f);
  std::vector<float> red(cells, 0.0f);
  for (int r = 0; r < rows; ++r) {
    const int y0 = r * height / rows;
    const int y1 = std::max(y0 + 1, (r + 1) * height / rows);
    for (int c = 0; c < columns; ++c) {
      const int x0 = c * width / columns;
      const int x1 = std::max(x0 + 1, (c + 1) * width / columns);
      double sr = 0.0;
      double sg = 0.0;
      double sb = 0.0;
      for (int y = y0; y < y1; ++y) {
        const std::uint8_t* row = rgba.data() + (static_cast<std::size_t>(y) * width + x0) * 4;
        for (int x = x0; x < x1; ++x, row += 4) {
          sr += row[0];
          sg += row[1];
          sb += row[2];
        }
      }
      const double n = static_cast<double>((y1 - y0) * (x1 - x0));
      sr /= n;
      sg /= n;
      sb /= n;
      const std::size_t i = static_cast<std::size_t>(r) * columns + c;
      luma[i] = static_cast<float>(0.299 * sr + 0.587 * sg + 0.114 * sb);
      blue[i] = static_cast<float>(128.0 - 0.1687 * sr - 0.3313 * sg + 0.5 * sb);
      red[i] = static_cast<float>(128.0 + 0.5 * sr - 0.4187 * sg - 0.0813 * sb);
    }
  }

  // Colour that stands out from the frame's own average: the frequency-tuned
  // saliency of Achanta and others, on cells that are already a blur.
  double mean_y = 0.0;
  double mean_b = 0.0;
  double mean_r = 0.0;
  for (std::size_t i = 0; i < cells; ++i) {
    mean_y += luma[i];
    mean_b += blue[i];
    mean_r += red[i];
  }
  mean_y /= static_cast<double>(cells);
  mean_b /= static_cast<double>(cells);
  mean_r /= static_cast<double>(cells);
  std::vector<float> standout(cells);
  std::vector<float> skin(cells);
  for (std::size_t i = 0; i < cells; ++i) {
    // Brightness counts for less than colour: a bright sky is not a subject.
    const double dy = (luma[i] - mean_y) * 0.5;
    const double db = blue[i] - mean_b;
    const double dr = red[i] - mean_r;
    standout[i] = static_cast<float>(std::sqrt(dy * dy + db * db + dr * dr));
    // Skin of every complexion sits in one small patch of the two chroma
    // differences; brightness says nothing about it, except that black is not.
    const double near_b = std::clamp(1.0 - std::abs(blue[i] - 108.0) / 22.0, 0.0, 1.0);
    const double near_r = std::clamp(1.0 - std::abs(red[i] - 152.0) / 18.0, 0.0, 1.0);
    skin[i] = luma[i] > 40.0f ? static_cast<float>(near_b * near_r) : 0.0f;
  }

  // Detail: a face, a sign, a hand, rather than a wall.
  std::vector<float> detail(cells, 0.0f);
  for (int r = 1; r + 1 < rows; ++r) {
    for (int c = 1; c + 1 < columns; ++c) {
      const std::size_t i = static_cast<std::size_t>(r) * columns + c;
      detail[i] = std::abs(luma[i + 1] - luma[i - 1]) +
                  std::abs(luma[i + columns] - luma[i - columns]);
    }
  }

  // Movement against the camera's: the whole frame's shift since the last one
  // is found first, so a pan does not make everything look like a subject.
  std::vector<float> motion(cells, 0.0f);
  const bool compare = !new_shot && last_luma_.size() == cells && columns_ == columns &&
                       rows_ == rows;
  if (compare) {
    constexpr int kReach = 3;
    double best = std::numeric_limits<double>::infinity();
    int best_dx = 0;
    int best_dy = 0;
    for (int dy = -kReach; dy <= kReach; ++dy) {
      for (int dx = -kReach; dx <= kReach; ++dx) {
        double sum = 0.0;
        int count = 0;
        for (int r = kReach; r < rows - kReach; ++r) {
          for (int c = kReach; c < columns - kReach; ++c) {
            const std::size_t now = static_cast<std::size_t>(r) * columns + c;
            const std::size_t then = static_cast<std::size_t>(r - dy) * columns + (c - dx);
            sum += std::abs(luma[now] - last_luma_[then]);
            ++count;
          }
        }
        if (count > 0 && sum / count < best) {
          best = sum / count;
          best_dx = dx;
          best_dy = dy;
        }
      }
    }
    for (int r = 0; r < rows; ++r) {
      for (int c = 0; c < columns; ++c) {
        const int pr = std::clamp(r - best_dy, 0, rows - 1);
        const int pc = std::clamp(c - best_dx, 0, columns - 1);
        motion[static_cast<std::size_t>(r) * columns + c] =
            std::abs(luma[static_cast<std::size_t>(r) * columns + c] -
                     last_luma_[static_cast<std::size_t>(pr) * columns + pc]);
      }
    }
  }
  // How much each cell has changed lately where it sits on the screen, camera
  // and all, kept across shots: a HUD, a scoreboard or a watermark hardly ever
  // changes while everything round it does, and is drawn over the picture
  // rather than being in it.
  if (change_.size() != cells) {
    change_.assign(cells, 0.0f);
    changes_seen_ = 0;
  }
  if (compare) {
    for (std::size_t i = 0; i < cells; ++i) {
      change_[i] = 0.9f * change_[i] + 0.1f * std::abs(luma[i] - last_luma_[i]);
    }
    ++changes_seen_;
  }
  last_luma_ = luma;
  columns_ = columns;
  rows_ = rows;

  normalise(standout);
  normalise(skin);
  normalise(detail);
  normalise(motion);
  // Skin and movement say "somebody is here" most plainly, colour that stands
  // out next, detail least: a patterned shirt is detail too.
  std::vector<double> weight(cells);
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < columns; ++c) {
      const std::size_t i = static_cast<std::size_t>(r) * columns + c;
      const double x = (c + 0.5) / columns - 0.5;
      const double y = (r + 0.5) / rows - 0.5;
      const double centre = 0.4 + 0.6 * std::exp(-(x * x + y * y) / (2.0 * 0.25 * 0.25));
      const double moving = compare ? 0.3 * motion[i] : 0.0;
      weight[i] = centre * (0.3 * standout[i] + 0.35 * skin[i] + moving + 0.1 * detail[i]);
    }
  }

  // Overlays left out, once there has been time to see what changes. Only
  // where most of the frame is changing: in a locked-off interview nearly
  // nothing does, and the speaker is not an overlay.
  if (changes_seen_ >= 5) {
    std::vector<float> sorted = change_;
    std::ranges::nth_element(sorted, sorted.begin() + static_cast<std::ptrdiff_t>(cells / 2));
    const float typical = sorted[cells / 2];
    if (typical > 1.0f) {
      for (std::size_t i = 0; i < cells; ++i) {
        weight[i] *= std::clamp(change_[i] / (0.25f * typical), 0.0f, 1.0f);
      }
    }
  }

  // Less what the frame has everywhere, so a patterned wall behind the
  // subject is background rather than a little of the subject spread wide.
  {
    std::vector<double> sorted = weight;
    std::ranges::nth_element(sorted, sorted.begin() + static_cast<std::ptrdiff_t>(cells / 2));
    const double floor = sorted[cells / 2];
    for (double& w : weight) w = std::max(0.0, w - floor);
  }

  // The window over the most of it, from a summed table so every position
  // costs four lookups.
  std::vector<double> summed(static_cast<std::size_t>(columns + 1) * (rows + 1), 0.0);
  const auto at = [&](int r, int c) -> double& {
    return summed[static_cast<std::size_t>(r) * (columns + 1) + c];
  };
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < columns; ++c) {
      at(r + 1, c + 1) = weight[static_cast<std::size_t>(r) * columns + c] + at(r, c + 1) +
                         at(r + 1, c) - at(r, c);
    }
  }
  const int wide = std::clamp(static_cast<int>(std::lround(window_width_ * columns)), 1, columns);
  const int tall = std::clamp(static_cast<int>(std::lround(window_height_ * rows)), 1, rows);
  const auto sum_at = [&](int r, int c) {
    return at(r + tall, c + wide) - at(r, c + wide) - at(r + tall, c) + at(r, c);
  };
  const int centre_c = (columns - wide) / 2;
  const int centre_r = (rows - tall) / 2;
  double best = sum_at(centre_r, centre_c);
  int best_c = centre_c;
  int best_r = centre_r;
  // Only a clearly better place moves the window off the middle, so a frame
  // with nothing in particular in it leaves the framing where it was shot.
  const double needed = best * 1.03;
  for (int r = 0; r + tall <= rows; ++r) {
    for (int c = 0; c + wide <= columns; ++c) {
      const double s = sum_at(r, c);
      if (s > needed && s > best) {
        best = s;
        best_c = c;
        best_r = r;
      }
    }
  }
  // Then centred on the middle of what it holds, weighted, rather than left
  // wherever the edge of the window first took all of it in: Premiere puts
  // the subject in the middle of the new frame, not just inside it.
  double sum = 0.0;
  double sx = 0.0;
  double sy = 0.0;
  for (int r = best_r; r < best_r + tall; ++r) {
    for (int c = best_c; c < best_c + wide; ++c) {
      const double w = weight[static_cast<std::size_t>(r) * columns + c];
      sum += w;
      sx += w * (c + 0.5);
      sy += w * (r + 0.5);
    }
  }
  if (!(sum > 0.0)) return {(best_c + wide / 2.0) / columns, (best_r + tall / 2.0) / rows};
  const double half_w = wide / 2.0;
  const double half_h = tall / 2.0;
  return {std::clamp(sx / sum, half_w, columns - half_w) / columns,
          std::clamp(sy / sum, half_h, rows - half_h) / rows};
}

std::vector<Point> reframe_path(const ReframeAnalysis& analysis, ReframeMotion motion) {
  const std::size_t count = analysis.subjects.size();
  std::vector<Point> path(count);
  if (count == 0) return path;

  // How long the window takes to follow, as a Gaussian's width in seconds, and
  // how far the subject may wander inside a still window, as a share of it.
  double sigma = 0.8;
  double still = 0.08;
  switch (motion) {
    case ReframeMotion::Slower:
      sigma = 2.0;
      still = 0.25;
      break;
    case ReframeMotion::Default:
      break;
    case ReframeMotion::Faster:
      sigma = 0.25;
      still = 0.0;
      break;
  }
  const double fps = analysis.fps > 0.0 ? analysis.fps : 10.0;
  const double spread = std::max(0.5, sigma * fps);
  const int reach = static_cast<int>(std::ceil(3.0 * spread));

  std::vector<int> bounds{0};
  for (const int cut : analysis.cuts) {
    if (cut > bounds.back() && static_cast<std::size_t>(cut) < count) bounds.push_back(cut);
  }
  bounds.push_back(static_cast<int>(count));

  const auto smooth_axis = [&](int from, int to, auto get, double window, auto set) {
    const int n = to - from;
    // A median of five first, so one frame that looked somewhere else is not
    // followed at all.
    std::vector<double> raw(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      std::vector<double> near;
      for (int k = std::max(0, i - 2); k <= std::min(n - 1, i + 2); ++k) {
        near.push_back(get(analysis.subjects[static_cast<std::size_t>(from + k)]));
      }
      std::ranges::nth_element(near, near.begin() + static_cast<std::ptrdiff_t>(near.size() / 2));
      raw[static_cast<std::size_t>(i)] = near[near.size() / 2];
    }
    // Held still when the subject stays well inside a still window for nearly
    // all of the shot: what an operator on sticks would have done.
    std::vector<double> sorted = raw;
    std::ranges::sort(sorted);
    const double middle = sorted[sorted.size() / 2];
    const double allowed = still * window;
    const auto inside = std::ranges::count_if(raw, [&](double v) {
      return std::abs(v - middle) <= allowed;
    });
    const double lo = window / 2.0;
    const double hi = 1.0 - window / 2.0;
    if (allowed > 0.0 && inside * 10 >= n * 9) {
      for (int i = 0; i < n; ++i) set(path[static_cast<std::size_t>(from + i)], std::clamp(middle, lo, hi));
      return;
    }
    for (int i = 0; i < n; ++i) {
      double sum = 0.0;
      double total = 0.0;
      for (int k = std::max(0, i - reach); k <= std::min(n - 1, i + reach); ++k) {
        const double d = (k - i) / spread;
        const double w = std::exp(-0.5 * d * d);
        sum += w * raw[static_cast<std::size_t>(k)];
        total += w;
      }
      set(path[static_cast<std::size_t>(from + i)], std::clamp(sum / total, lo, hi));
    }
  };

  for (std::size_t s = 0; s + 1 < bounds.size(); ++s) {
    const int from = bounds[s];
    const int to = bounds[s + 1];
    if (to <= from) continue;
    smooth_axis(from, to, [](const Point& p) { return p.x; }, analysis.window_width,
                [](Point& p, double v) { p.x = v; });
    smooth_axis(from, to, [](const Point& p) { return p.y; }, analysis.window_height,
                [](Point& p, double v) { p.y = v; });
  }
  return path;
}

bool save_reframe(const ReframeAnalysis& analysis, const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(kMagic, 4);
  put(out, kVersion);
  put(out, analysis.fps);
  put(out, analysis.window_width);
  put(out, analysis.window_height);
  put(out, static_cast<std::uint32_t>(analysis.subjects.size()));
  for (const Point& p : analysis.subjects) {
    put(out, static_cast<float>(p.x));
    put(out, static_cast<float>(p.y));
  }
  put(out, static_cast<std::uint32_t>(analysis.cuts.size()));
  for (const int cut : analysis.cuts) put(out, static_cast<std::int32_t>(cut));
  return static_cast<bool>(out);
}

std::optional<ReframeAnalysis> load_reframe(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  char magic[4] = {};
  std::uint32_t version = 0;
  ReframeAnalysis a;
  std::uint32_t count = 0;
  if (!in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !take(in, version) ||
      version != kVersion || !take(in, a.fps) || !take(in, a.window_width) ||
      !take(in, a.window_height) || !take(in, count) || count > 10'000'000) {
    return std::nullopt;
  }
  a.subjects.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    float x = 0.0f;
    float y = 0.0f;
    if (!take(in, x) || !take(in, y)) return std::nullopt;
    a.subjects.push_back({x, y});
  }
  std::uint32_t cuts = 0;
  if (!take(in, cuts) || cuts > count) return std::nullopt;
  for (std::uint32_t i = 0; i < cuts; ++i) {
    std::int32_t cut = 0;
    if (!take(in, cut)) return std::nullopt;
    a.cuts.push_back(cut);
  }
  return a;
}

}  // namespace cutline::track
