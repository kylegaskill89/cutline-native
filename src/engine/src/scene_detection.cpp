#include "cutline/engine/scene_detection.hpp"

#include "cutline/core/query.hpp"
#include "cutline/engine/clip_pictures.hpp"
#include "cutline/track/scenes.hpp"

#include <cmath>

namespace cutline::engine {
namespace {

/// Wide enough to see a cut in, small enough that drawing a frame costs
/// little next to decoding it, and small enough that grain and compression
/// noise average out rather than reading as change.
constexpr double kWidth = 256.0;

}  // namespace

std::expected<std::vector<double>, std::string> detect_scene_edits(
    std::shared_ptr<gpu::Device> device, const core::Project& project,
    const std::string& clip_id, const AnalysisProgress& progress) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return std::unexpected("the clip is not in the sequence");
  if (clip->kind != core::TrackKind::Video) return std::unexpected("the clip has no picture");
  auto pictures = ClipPictures::create(std::move(device), project, *clip, kWidth);
  if (!pictures.has_value()) return std::unexpected(pictures.error());

  const double fps = project.sequence().fps > 0.0 ? project.sequence().fps : 30.0;
  const auto total = std::max(
      1, static_cast<int>(std::floor(core::clip_duration(*clip) * fps - 1e-6)) + 1);
  track::SceneDetector detector(fps);
  for (int i = 0; i < total; ++i) {
    auto image = (*pictures)->colour_at(i / fps);
    if (!image.has_value()) return std::unexpected(image.error());
    detector.add(*image, (*pictures)->width(), (*pictures)->height());
    if (progress && !progress(i + 1, total)) return std::unexpected("it was stopped");
  }
  std::vector<double> out;
  for (const int frame : detector.cuts()) out.push_back(clip->start + frame / fps);
  return out;
}

}  // namespace cutline::engine
