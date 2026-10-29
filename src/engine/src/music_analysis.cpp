#include "cutline/engine/music_analysis.hpp"

#include "cutline/core/interpret.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/remix.hpp"
#include "cutline/media/audio.hpp"

#include <algorithm>

namespace cutline::engine {

std::expected<audio::MusicAnalysis, std::string> analyse_music_file(const std::string& path,
                                                                   int audio_stream, double from,
                                                                   double to) {
  if (!(to > from)) return std::unexpected("There is no music in this clip to listen to.");
  auto decoded = media::decode_audio(path, audio_stream,
                                     {.sample_rate = audio::kMusicAnalysisRate,
                                      .channels = 1,
                                      .start = from,
                                      .duration = to - from});
  if (!decoded) return std::unexpected(decoded.error());
  // From exactly `from`: a decoder starts where it can, which may be a little
  // before, and the analysis's seconds have to be the clip's.
  const auto skip = static_cast<std::size_t>(
      std::max(0.0, (from - decoded->start_time) * audio::kMusicAnalysisRate));
  const auto want = static_cast<std::size_t>((to - from) * audio::kMusicAnalysisRate);
  const std::size_t first = std::min(skip, decoded->samples.size());
  const std::size_t count = std::min(want, decoded->samples.size() - first);
  return audio::analyse_music(std::span<const float>(decoded->samples).subspan(first, count),
                              audio::kMusicAnalysisRate);
}

std::expected<audio::MusicAnalysis, std::string> analyse_clip_music(
    const core::Project& project, const std::string& clip_id) {
  if (auto why = core::why_not_remix(project, clip_id)) return std::unexpected(*why);
  const std::optional<core::RemixSource> source = core::remix_source(project, clip_id);
  const auto media = std::ranges::find(project.media, source->media_id, &core::Media::id);
  if (media == project.media.end() || media->path.empty()) {
    return std::unexpected("This clip's media is offline.");
  }
  // In file seconds, which a conformed source's are not.
  const double conform = core::conform_speed(*media);
  return analyse_music_file(media->path, source->audio_stream, source->source_in * conform,
                            source->source_out * conform);
}

std::optional<core::Remix> make_remix(const core::Project& project, const std::string& clip_id,
                                      const audio::MusicAnalysis& analysis, double target,
                                      int segments, int variations) {
  if (core::why_not_remix(project, clip_id).has_value()) return std::nullopt;
  const core::Clip& clip = *core::find_clip(project, clip_id);
  const std::optional<core::RemixSource> source = core::remix_source(project, clip_id);
  const auto media = std::ranges::find(project.media, source->media_id, &core::Media::id);
  const double conform = media != project.media.end() ? core::conform_speed(*media) : 1.0;

  // Along the song in source seconds, which is the timeline's length at the
  // clip's speed, and the file's seconds over the conform.
  const double speed = core::clip_speed(clip);
  const audio::RemixPlan plan = audio::plan_remix(
      analysis, {.target = target * speed * conform,
                 .maximum = core::remix_room(project, clip_id) * speed * conform,
                 .segments = segments,
                 .variations = variations});
  if (plan.spans.empty()) return std::nullopt;

  core::Remix remix{.target = target, .segments = segments, .variations = variations};
  for (const audio::MusicSpan& span : plan.spans) {
    remix.spans.push_back({source->source_in + span.from / conform,
                           source->source_in + span.to / conform});
  }
  return remix;
}

}  // namespace cutline::engine
