#pragma once

/// Turning a project into something the timeline can draw, and turning what
/// the timeline reports back into an edit.
///
/// Both halves already exist and neither knows about the other: `cutline::ui`
/// draws plain structs and `cutline::core` performs operations on a project.
/// This is the whole of the join, kept in one file so there is exactly one
/// place where a clip becomes a rectangle.

#include "cutline/core/model.hpp"
#include "cutline/ui/timeline.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// Where a source's audio envelope comes from, asked per media and stream.
///
/// A lookup rather than a container of them, and this layer stays pure because
/// of it: computing an envelope means decoding a file, which belongs to the
/// media layer and to a thread that is not the one painting. Whoever has one
/// answers; anything else returns null and the clip is simply drawn without a
/// waveform, which is also what it looks like while one is still being decoded.
///
/// It returns a shared pointer because the answer is the same for every clip of
/// a source and the model is rebuilt on every gesture. Copying a pointer per
/// rebuild is what keeps a drag free.
using WaveformSource =
    std::function<std::shared_ptr<const ui::Waveform>(std::string_view media_id, int stream)>;

/// Where a source's filmstrip comes from. The same bargain as `WaveformSource`:
/// answer with what is already extracted, never decode here, and null simply
/// means the clip is drawn without one.
using FilmstripSource =
    std::function<std::shared_ptr<const ui::Filmstrip>(std::string_view media_id)>;

/// What the timeline can be given beyond the project itself.
///
/// A struct rather than more parameters, because both of these are optional,
/// both arrive from a worker, and a third would otherwise mean changing every
/// caller again.
struct TimelineMedia {
  WaveformSource waveforms;
  FilmstripSource filmstrips;
};

/// What the timeline should draw for this project.
///
/// Track order follows the project's, which is video first and topmost first —
/// the same order the timeline stacks rows in, so V2 sits above V1 the way it
/// does everywhere else.
[[nodiscard]] ui::TimelineModel timeline_model(const core::Project& project,
                                               std::span<const std::string> selection = {},
                                               const TimelineMedia& media = {});

/// The name a track shows when it has not been given one.
///
/// Numbered from the bottom for video and from the top for audio, which is
/// what every editor does: V1 is the base layer and A1 is the first audio
/// lane, and they meet in the middle.
[[nodiscard]] std::string default_track_label(const core::Project& project,
                                              std::size_t index);

/// Applies a gesture the timeline reported.
///
/// One function for all of them. The alternative is the caller switching on the
/// mode to decide which operation to call, which is the same switch one layer
/// further away from the operations it chooses between.
///
/// This is where the interface's units become the model's. A slip is dragged in
/// timeline seconds and stored in source seconds, and the two differ by the
/// clip's speed; a rate stretch and a trim are the same gesture and different
/// operations. Neither the timeline nor the core should have to know that.
///
/// Returns the project unchanged when the edit cannot apply — clamped against
/// a neighbour, or past the end of the source — which is what every core
/// operation does and what lets the caller skip the undo entry.
/// `selection` is every clip the gesture should carry with it, which matters for
/// exactly one mode: a move. Dragging one clip of a selection moves the whole
/// selection, because that is what selecting several of them was for — while a
/// trim, a slip and a fade stay on the clip whose edge or handle was grabbed,
/// which is the one being aimed at.
///
/// Empty, or not containing `clip_id`, means the gesture applies to that clip
/// alone — so a drag on something outside the selection does not sweep up
/// whatever happened to be highlighted elsewhere.
///
/// `made` collects the ids an alt-drag put down, so the caller can select the
/// copies rather than leaving the originals highlighted — which would make the
/// nudge that usually follows act on the wrong clips. Untouched by every other
/// mode, none of which creates anything.
[[nodiscard]] core::Project apply_timeline_edit(core::Project project,
                                                std::string_view clip_id,
                                                const ui::TimelineEdit& edit,
                                                std::span<const std::string> selection = {},
                                                std::vector<std::string>* made = nullptr);

/// Flips one of a track's header switches.
///
/// Takes the control rather than a value, and reads the current one out of the
/// project. The interface therefore never holds the truth about a switch and
/// cannot toggle from a stale copy of it — which is exactly what would happen
/// the first time two things changed a track between frames.
[[nodiscard]] core::Project toggle_track_switch(core::Project project,
                                                std::string_view track_id,
                                                ui::TrackControl control);

/// One of the colours a clip can be labelled with.
struct ClipLabel {
  std::string_view name;
  std::string_view color;
};

/// The labels offered, in the order a menu should list them.
///
/// Premiere's eight, by their names, because a label is something people say
/// out loud — "the violet ones are the interview" — and a hex nobody can
/// pronounce is a label that never gets used. The colours are the model's; the
/// names live here because `core` has no menus and no opinion about them.
[[nodiscard]] std::span<const ClipLabel> clip_labels();

/// The clip a block refers to, or nothing when the model has moved on.
[[nodiscard]] std::optional<std::string> block_clip_id(const ui::TimelineModel& model,
                                                       ui::BlockRef ref);

}  // namespace cutline::editor
