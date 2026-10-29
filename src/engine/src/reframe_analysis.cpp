#include "cutline/engine/reframe_analysis.hpp"

#include "cutline/core/query.hpp"
#include "cutline/engine/clip_pictures.hpp"
#include "cutline/render/auto_reframe.hpp"
#include "cutline/track/scenes.hpp"

#include <cmath>

namespace cutline::engine {

std::expected<track::ReframeAnalysis, std::string> analyse_for_reframe(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const std::string& clip_id, const AnalysisProgress& progress) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return std::unexpected("the clip is not in the sequence");
  if (render::auto_reframe_of(*clip) == nullptr) return std::unexpected("the clip has no Auto Reframe");

  // Small: what is looked for is where things are, not what they are.
  auto pictures = ClipPictures::create(std::move(device), project, *clip, 320.0);
  if (!pictures.has_value()) return std::unexpected(pictures.error());

  const render::ReframeWindow window = render::reframe_window(project, *clip);
  track::ReframeAnalysis analysis{.fps = kReframeAnalysisRate,
                                  .window_width = window.width,
                                  .window_height = window.height};
  const auto total = std::max(
      1, static_cast<int>(std::floor(core::clip_duration(*clip) * kReframeAnalysisRate - 1e-6)) + 1);
  analysis.subjects.reserve(static_cast<std::size_t>(total));
  track::SubjectFinder finder(window.width, window.height);
  track::SceneDetector scenes(kReframeAnalysisRate);
  for (int i = 0; i < total; ++i) {
    auto rgba = (*pictures)->colour_at(i / kReframeAnalysisRate);
    if (!rgba.has_value()) return std::unexpected(rgba.error());
    const int w = (*pictures)->width();
    const int h = (*pictures)->height();
    scenes.add(*rgba, w, h);
    analysis.subjects.push_back(finder.add(*rgba, w, h, i == 0));
    if (progress && !progress(i + 1, total)) return std::unexpected("it was stopped");
  }
  analysis.cuts = scenes.cuts();
  return analysis;
}

}  // namespace cutline::engine
