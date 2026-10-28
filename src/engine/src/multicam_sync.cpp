#include "cutline/engine/multicam_sync.hpp"

#include "cutline/audio/sync.hpp"
#include "cutline/media/audio.hpp"

#include <algorithm>
#include <format>

namespace cutline::engine {
namespace {

/// Mono at 8 kHz: an onset is a rise in loudness, and loudness is all there
/// in the low frequencies, at a sixth of the cost of reading it at 48.
constexpr int kSyncRate = 8000;

}  // namespace

std::expected<std::vector<double>, std::string> multicam_audio_starts(
    const core::Project& project, std::span<const std::string> media_ids,
    const SyncProgress& progress) {
  std::vector<std::vector<float>> envelopes;
  std::vector<std::string> names;
  const int total = static_cast<int>(media_ids.size());
  for (const std::string& id : media_ids) {
    const auto media = std::ranges::find(project.media, id, &core::Media::id);
    if (media == project.media.end()) return std::unexpected(std::format("{} is not in the project", id));
    if (media->audio_stream_count <= 0 || media->path.empty()) {
      return std::unexpected(std::format("{} has no sound to synchronise by", media->name));
    }
    const auto sound = media::decode_audio(media->path, 0, {.sample_rate = kSyncRate, .channels = 1});
    if (!sound.has_value()) {
      return std::unexpected(std::format("Could not read the sound of {}: {}", media->name,
                                         sound.error()));
    }
    envelopes.push_back(audio::onset_envelope(sound->samples, kSyncRate));
    names.push_back(media->name);
    if (progress && !progress(static_cast<int>(envelopes.size()), total)) {
      return std::unexpected(std::string("Stopped"));
    }
  }
  if (envelopes.empty()) return std::vector<double>{};

  // Each against the first, which is Camera 1 and the one Premiere measures
  // the others by.
  std::vector<double> starts{0.0};
  for (std::size_t k = 1; k < envelopes.size(); ++k) {
    const auto match = audio::match_envelopes(envelopes.front(), envelopes[k]);
    if (!match.has_value()) {
      return std::unexpected(std::format("Could not find where the sound of {} meets the sound of {}",
                                         names[k], names.front()));
    }
    starts.push_back(match->offset);
  }
  const double earliest = std::ranges::min(starts);
  for (double& start : starts) start -= earliest;
  return starts;
}

}  // namespace cutline::engine
