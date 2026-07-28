#include "cutline/editor/timeline_binding.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/query.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace cutline::editor {
namespace {

[[nodiscard]] const core::Media* media_of(const core::Project& project,
                                          std::string_view media_id) noexcept {
  const auto found = std::ranges::find(project.media, media_id, &core::Media::id);
  return found == project.media.end() ? nullptr : &*found;
}

/// What a clip should be called on the timeline. The media's name, falling
/// back to something rather than a blank block nobody can identify.
[[nodiscard]] std::string label_for(const core::Project& project, const core::Clip& clip) {
  const core::Media* media = media_of(project, clip.media_id);
  if (media == nullptr) return "(missing)";
  if (!media->name.empty()) return media->name;
  return media->path.empty() ? "(untitled)" : media->path;
}

}  // namespace

std::string default_track_label(const core::Project& project, std::size_t index) {
  if (index >= project.tracks.size()) return {};
  const core::Track& track = project.tracks[index];
  if (!track.label.empty()) return track.label;

  if (track.kind == core::TrackKind::Video) {
    // Counted from the bottom: V1 is the base layer everything stacks onto, so
    // the topmost track has the highest number.
    std::size_t below = 0;
    for (std::size_t i = index + 1; i < project.tracks.size(); ++i) {
      if (project.tracks[i].kind == core::TrackKind::Video) ++below;
    }
    return "V" + std::to_string(below + 1);
  }

  // Audio counts from the top, so A1 is the first lane and the two numbering
  // schemes meet in the middle of the timeline.
  std::size_t above = 0;
  for (std::size_t i = 0; i < index; ++i) {
    if (project.tracks[i].kind == core::TrackKind::Audio) ++above;
  }
  return "A" + std::to_string(above + 1);
}

ui::TimelineModel timeline_model(const core::Project& project,
                                 std::span<const std::string> selection) {
  ui::TimelineModel model;
  model.fps = project.fps;
  model.duration = core::timeline_duration(project);
  model.tracks.reserve(project.tracks.size());

  for (std::size_t i = 0; i < project.tracks.size(); ++i) {
    const core::Track& track = project.tracks[i];
    const bool audio = track.kind == core::TrackKind::Audio;

    ui::TimelineTrack row;
    row.id = track.id;
    row.name = default_track_label(project, i);
    row.audio = audio;
    // Two different flags, one appearance: a hidden video track and an
    // inaudible audio one are both "this contributes nothing right now".
    // Solo elsewhere in the project is what makes the audio case not simply
    // `track.muted`.
    row.muted = audio ? !core::is_track_audible(project, track) : track.hidden;

    row.blocks.reserve(track.clips.size());
    for (const core::Clip& clip : track.clips) {
      row.blocks.push_back(ui::TimelineBlock{
          .id = clip.id,
          .start = clip.start,
          .end = core::clip_end(clip),
          .label = label_for(project, clip),
          .selected = std::ranges::find(selection, clip.id) != selection.end(),
      });
    }
    model.tracks.push_back(std::move(row));
  }
  return model;
}

core::Project apply_timeline_edit(core::Project project, std::string_view clip_id,
                                  ui::DragMode mode, double start, double end) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  switch (mode) {
    case ui::DragMode::Move: {
      // Through move_clips rather than by assignment, so the whole linked
      // group travels and the clamp against the start of the timeline is the
      // model's rather than a second opinion about it.
      const std::array<std::string, 1> ids{std::string(clip_id)};
      return core::move_clips(std::move(project), ids, start - clip->start);
    }

    case ui::DragMode::TrimStart:
      return core::set_clip_edge(std::move(project), clip_id, core::ClipEdge::In, start);

    case ui::DragMode::TrimEnd:
      return core::set_clip_edge(std::move(project), clip_id, core::ClipEdge::Out, end);

    case ui::DragMode::None:
    case ui::DragMode::Scrub:
      break;
  }
  return project;
}

std::optional<std::string> block_clip_id(const ui::TimelineModel& model, ui::BlockRef ref) {
  if (ref.track >= model.tracks.size()) return std::nullopt;
  const std::vector<ui::TimelineBlock>& blocks = model.tracks[ref.track].blocks;
  if (ref.block >= blocks.size()) return std::nullopt;
  return blocks[ref.block].id;
}

}  // namespace cutline::editor
