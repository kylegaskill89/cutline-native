#pragma once

/// Remix, from a project's point of view: listening to the song a clip plays,
/// and turning a length into a remix of it.
///
/// Two calls rather than one because they cost very differently. Listening
/// decodes the song, which is a second or so for a long one and belongs on a
/// thread of its own; arranging is a few milliseconds, and is done again on
/// every change of the target, from what was heard the once.

#include "cutline/audio/music.hpp"
#include "cutline/core/model.hpp"

#include <expected>
#include <optional>
#include <string>

namespace cutline::engine {

/// Listens to `from` to `to` seconds of a file's audio stream.
[[nodiscard]] std::expected<audio::MusicAnalysis, std::string> analyse_music_file(
    const std::string& path, int audio_stream, double from, double to);

/// Listens to the song a clip plays: see `core::remix_source`.
[[nodiscard]] std::expected<audio::MusicAnalysis, std::string> analyse_clip_music(
    const core::Project& project, const std::string& clip_id);

/// The remix of the clip's song nearest `target` seconds on the timeline,
/// from the analysis of that song, with Premiere's Segments and Variations.
///
/// No longer than the room the clip has on its track. In timeline seconds, so
/// a clip at another speed is remixed to the length it will play for. Nothing
/// when the clip cannot be remixed.
[[nodiscard]] std::optional<core::Remix> make_remix(const core::Project& project,
                                                    const std::string& clip_id,
                                                    const audio::MusicAnalysis& analysis,
                                                    double target, int segments, int variations);

}  // namespace cutline::engine
