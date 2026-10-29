#include "cutline/render/auto_reframe.hpp"

#include "cutline/core/effects.hpp"
#include "cutline/render/warp_stabilizer.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>

namespace cutline::render {
namespace {

/// A control's value at `local_t`, or its default when it was never set.
[[nodiscard]] double control(const core::ClipEffect& effect, std::string_view key,
                             double fallback, double local_t) {
  const bool stored =
      effect.params.contains(std::string(key)) || core::is_effect_param_animated(effect, key);
  const double value = stored ? core::effect_param_at(effect, key, local_t) : fallback;
  return std::isfinite(value) ? value : fallback;
}

/// Analyses read from their files, with when each file was written, and the
/// paths worked out from them for each Motion Tracking setting. Small, so kept
/// until the process ends; a file analysed again, or gone, is read afresh.
struct Cache {
  std::mutex lock;
  struct Read {
    std::filesystem::file_time_type written;
    std::shared_ptr<const track::ReframeAnalysis> analysis;
    std::map<int, std::shared_ptr<const std::vector<track::Point>>> paths;
  };
  std::map<std::filesystem::path, Read> files;
};

Cache& cache() {
  static Cache instance;
  return instance;
}

}  // namespace

const core::ClipEffect* auto_reframe_of(const core::Clip& clip) noexcept {
  for (const core::ClipEffect& effect : clip.effects) {
    if (effect.enabled && effect.type == "autoreframe") return &effect;
  }
  return nullptr;
}

ReframeWindow reframe_window(const core::Project& project, const core::Clip& clip) {
  const auto media = std::ranges::find(project.media, clip.media_id, &core::Media::id);
  const double canvas_w = project.sequence().canvas_w;
  const double canvas_h = project.sequence().canvas_h;
  const core::Size natural =
      core::natural_size(media == project.media.end() ? nullptr : &*media, canvas_w, canvas_h);
  if (natural.width <= 0.0 || natural.height <= 0.0) return {};
  // Filled rather than fitted: the larger of the two scales, so one side
  // matches the frame and the other runs past it.
  const double fill = std::max(canvas_w / natural.width, canvas_h / natural.height);
  return {.width = std::min(1.0, canvas_w / (natural.width * fill)),
          .height = std::min(1.0, canvas_h / (natural.height * fill))};
}

std::string reframe_key(const core::Project& project, const core::Clip& clip) {
  const core::ClipEffect* effect = auto_reframe_of(clip);
  if (effect == nullptr) return {};
  // The source, its part and its speed, as Warp Stabilizer names them; and the
  // window's shape, which is what the looking is for.
  const std::string source = analysis_key(project, clip, core::ClipEffect{});
  if (source.empty()) return {};
  const ReframeWindow window = reframe_window(project, clip);
  return std::format("{}-{:04}x{:04}", source, std::lround(window.width * 1000.0),
                     std::lround(window.height * 1000.0));
}

std::filesystem::path reframe_analysis_path(const std::string& key) {
  return analysis_folder() / (key + ".cara");
}

bool has_reframe_analysis(const std::string& key) {
  std::error_code error;
  return !key.empty() && std::filesystem::exists(reframe_analysis_path(key), error);
}

track::ReframeMotion reframe_motion(const core::ClipEffect& effect) noexcept {
  const auto found = effect.params.find("motion_preset");
  const int chosen =
      found == effect.params.end() ? 1 : std::clamp(static_cast<int>(std::lround(found->second)), 0, 2);
  return static_cast<track::ReframeMotion>(chosen);
}

std::optional<track::Point> reframe_centre(const std::string& key, track::ReframeMotion motion,
                                           double local_t) {
  if (key.empty()) return std::nullopt;
  const std::filesystem::path file = reframe_analysis_path(key);
  std::error_code error;
  const auto written = std::filesystem::last_write_time(file, error);
  Cache& c = cache();
  if (error) {
    const std::scoped_lock lock(c.lock);
    c.files.erase(file);
    return std::nullopt;
  }

  std::shared_ptr<const track::ReframeAnalysis> analysis;
  std::shared_ptr<const std::vector<track::Point>> path;
  {
    const std::scoped_lock lock(c.lock);
    if (const auto found = c.files.find(file); found != c.files.end() && found->second.written == written) {
      analysis = found->second.analysis;
      if (const auto p = found->second.paths.find(static_cast<int>(motion));
          p != found->second.paths.end()) {
        path = p->second;
      }
    }
  }
  if (analysis == nullptr) {
    auto loaded = track::load_reframe(file);
    if (!loaded.has_value() || loaded->subjects.empty()) return std::nullopt;
    analysis = std::make_shared<const track::ReframeAnalysis>(std::move(*loaded));
    const std::scoped_lock lock(c.lock);
    c.files[file] = Cache::Read{.written = written, .analysis = analysis};
  }
  if (path == nullptr) {
    path = std::make_shared<const std::vector<track::Point>>(track::reframe_path(*analysis, motion));
    const std::scoped_lock lock(c.lock);
    if (const auto found = c.files.find(file); found != c.files.end()) {
      found->second.paths[static_cast<int>(motion)] = path;
    }
  }
  if (path->empty()) return std::nullopt;

  // Between the frames looked at, except across a cut, where the window
  // jumps with the picture rather than gliding into the next shot.
  const double at = std::max(0.0, local_t * analysis->fps);
  const auto i = std::min(static_cast<std::size_t>(at), path->size() - 1);
  if (i + 1 >= path->size() || std::ranges::find(analysis->cuts, static_cast<int>(i + 1)) !=
                                   analysis->cuts.end()) {
    return (*path)[i];
  }
  const double f = at - static_cast<double>(i);
  const track::Point& a = (*path)[i];
  const track::Point& b = (*path)[i + 1];
  return track::Point{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
}

void apply_auto_reframe(const core::Project& project, const core::Clip& clip,
                        const core::Media* media, double t, core::LayerBox& box) {
  const core::ClipEffect* effect = auto_reframe_of(clip);
  if (effect == nullptr) return;
  const double canvas_w = project.sequence().canvas_w;
  const double canvas_h = project.sequence().canvas_h;
  const core::Size natural = core::natural_size(media, canvas_w, canvas_h);
  if (natural.width <= 0.0 || natural.height <= 0.0) return;
  const double local_t = t - clip.start;

  const double fill = std::max(canvas_w / natural.width, canvas_h / natural.height);
  const double scale = std::max(0.01, control(*effect, "scale", 100.0, local_t) / 100.0);
  box.width *= fill * scale;
  box.height *= fill * scale;

  // The subject to the middle of the box's frame, which with Motion left alone
  // is the middle of the sequence's.
  const track::Point centre =
      reframe_centre(reframe_key(project, clip), reframe_motion(*effect), local_t)
          .value_or(track::Point{0.5, 0.5});
  box.center_x += (0.5 - centre.x) * box.width;
  box.center_y += (0.5 - centre.y) * box.height;

  // And never so far that the frame shows past the picture's edge, where the
  // picture still covers it at all; turned, the edges are Motion's business.
  const double rotation = control(*effect, "rotation", 0.0, local_t);
  if (std::abs(box.rotation_deg) < 1e-9 && std::abs(rotation) < 1e-9) {
    if (box.width >= canvas_w) {
      box.center_x = std::clamp(box.center_x, canvas_w - box.width / 2.0, box.width / 2.0);
    }
    if (box.height >= canvas_h) {
      box.center_y = std::clamp(box.center_y, canvas_h - box.height / 2.0, box.height / 2.0);
    }
  }

  box.center_x += control(*effect, "offset_x", 0.0, local_t);
  box.center_y += control(*effect, "offset_y", 0.0, local_t);
  box.rotation_deg += rotation;
}

}  // namespace cutline::render
