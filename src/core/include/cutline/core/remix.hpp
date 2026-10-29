#pragma once

/// Putting a remix on a clip and taking it off. See `Remix` for what one is;
/// finding the stretches is `audio::plan_remix`'s business, and this is only
/// what the project does with them.

#include "cutline/core/model.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace cutline::core {

/// Why a clip cannot be remixed, or nothing when it can.
///
/// A clip of sound read from a file: not a picture, not a nest — whose sound
/// is a mix of its own rather than a song — and not a clip whose speed is
/// animated, since the length a remix is asked for is then not any one
/// length of the song. The words are for the person who asked.
[[nodiscard]] std::optional<std::string> why_not_remix(const Project& p,
                                                       std::string_view clip_id);

/// The part of the file a remix of the clip is made from: the song as the
/// clip had it before any remix, in source seconds.
struct RemixSource {
  std::string media_id;
  int audio_stream = 0;
  double source_in = 0.0;
  double source_out = 0.0;
};
[[nodiscard]] std::optional<RemixSource> remix_source(const Project& p, std::string_view clip_id);

/// How long the clip may become on the timeline before it runs into the next
/// clip on its track: the wall a remix's length cannot pass. Unbounded when
/// nothing follows it.
[[nodiscard]] double remix_room(const Project& p, std::string_view clip_id) noexcept;

/// Makes the clip `remix`, whose stretches are in source seconds and whose
/// `source_in` and `source_out` are ignored in favour of the song the clip
/// already plays. The clip keeps its start and lasts the remix's length at
/// its speed. Nothing changes for a clip `why_not_remix` refuses or a remix
/// with nothing in it.
[[nodiscard]] Project apply_remix(Project p, std::string_view clip_id, Remix remix);

/// Takes the remix off: the clip is its song again, as it was before, cut
/// short where something now follows it too closely to have it whole.
[[nodiscard]] Project clear_remix(Project p, std::string_view clip_id);

}  // namespace cutline::core
