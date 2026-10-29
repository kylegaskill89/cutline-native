#include "cutline/core/scene_edits.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/pool.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/subclips.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace cutline::core {
namespace {

/// Nearer than this to an end of the clip or to another cut is the same
/// place, so a cut found on the clip's first frame changes nothing.
constexpr double kSamePlace = 1e-4;

}  // namespace

Project apply_scene_edits(Project p, std::string_view clip_id, std::span<const double> cuts,
                          const SceneEditOptions& options) {
  const Clip* found = find_clip(p, clip_id);
  if (found == nullptr) return p;
  const Clip clip = *found;  // copied: the cuts below change the track it lives in

  std::vector<double> times;
  for (const double t : cuts) {
    if (t > clip.start + kSamePlace && t < clip_end(clip) - kSamePlace) times.push_back(t);
  }
  std::ranges::sort(times);
  const auto [first, last] = std::ranges::unique(
      times, [](double a, double b) { return b - a < kSamePlace; });
  times.erase(first, last);

  const auto media_of = [&p, &clip]() -> const Media* {
    const auto it = std::ranges::find(p.media, clip.media_id, &Media::id);
    return it == p.media.end() ? nullptr : &*it;
  };

  if (options.markers && media_of() != nullptr) {
    for (const double t : times) {
      const double at = source_time_at(clip, t);
      if (source_marker_near(*media_of(), at, kSamePlace) != nullptr) continue;
      p = add_source_marker(std::move(p), clip.media_id, at);
    }
  }

  if (options.subclips && media_of() != nullptr && can_make_subclip(*media_of())) {
    // A bin beside the source, named after it, with a subclip for every shot
    // from the first frame of the clip to the last.
    const Media source = *media_of();
    p = create_bin(std::move(p), source.name, source.bin);
    const std::string bin = p.bins.back().id;
    std::vector<double> edges{clip.start};
    edges.insert(edges.end(), times.begin(), times.end());
    edges.push_back(clip_end(clip));
    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
      const SourceRange span = clip_sub_source(clip, edges[i], edges[i + 1]);
      p = make_subclip(std::move(p), source.id, std::format("{} {:03}", subclip_name(source), i + 1),
                       std::min(span.source_in, span.source_out),
                       std::max(span.source_in, span.source_out), true, bin);
    }
  }

  if (options.cut) {
    // The latest first, so the clip that keeps its id is always the one that
    // still covers every earlier time, and so is the one to cut next.
    const std::vector<std::string> members = group_members(p, clip.id);
    for (auto t = times.rbegin(); t != times.rend(); ++t) p = split_at(std::move(p), *t, members);
  }
  return p;
}

}  // namespace cutline::core
