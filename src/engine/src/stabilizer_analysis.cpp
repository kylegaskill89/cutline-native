#include "cutline/engine/stabilizer_analysis.hpp"

#include "cutline/core/query.hpp"
#include "cutline/engine/clip_pictures.hpp"

#include <cmath>

namespace cutline::engine {

std::expected<track::Analysis, std::string> analyse_for_stabilizer(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const std::string& clip_id, std::size_t effect, const AnalysisProgress& progress) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return std::unexpected("the clip is not in the sequence");
  if (effect >= clip->effects.size()) return std::unexpected("the effect is not there");
  const auto detailed = clip->effects[effect].params.find("detailed");
  const bool fine = detailed != clip->effects[effect].params.end() && detailed->second >= 0.5;

  // Smaller than mask tracking draws, because every frame is analysed and a
  // camera's shake is many pixels; Detailed Analysis is the larger size.
  auto pictures = ClipPictures::create(std::move(device), project, *clip, fine ? 960.0 : 640.0);
  if (!pictures.has_value()) return std::unexpected(pictures.error());

  const double fps = project.sequence().fps > 0.0 ? project.sequence().fps : 30.0;
  const auto total = std::max(
      1, static_cast<int>(std::floor(core::clip_duration(*clip) * fps - 1e-6)) + 1);

  track::Analysis analysis{.width = (*pictures)->width(),
                           .height = (*pictures)->height(),
                           .fps = fps};
  analysis.frames.reserve(static_cast<std::size_t>(total));
  track::FeatureTracker tracker(fine ? 400 : 200, fine ? 8.0 : 12.0);
  for (int i = 0; i < total; ++i) {
    auto image = (*pictures)->at(i / fps);
    if (!image.has_value()) return std::unexpected(image.error());
    analysis.frames.push_back(tracker.next(*image));
    if (progress && !progress(i + 1, total)) return std::unexpected("it was stopped");
  }
  return analysis;
}

}  // namespace cutline::engine
