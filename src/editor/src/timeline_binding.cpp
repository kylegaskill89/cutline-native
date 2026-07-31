#include "cutline/editor/timeline_binding.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/keyframe.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/editor/transitions.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

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

/// Every moment a clip is animated at, from every property and every effect,
/// deduplicated and in order.
///
/// One list rather than one per property: the block is a few pixels tall, and
/// what it can usefully say is "something happens here", not which of eleven
/// parameters it was. The panel is where a keyframe is identified.
[[nodiscard]] std::vector<double> keyframe_times(const core::Clip& clip) {
  std::vector<double> times = core::effect_keyframe_times(clip);

  for (const core::AnimProp prop : core::kAnimProps) {
    for (const core::Keyframe& frame : clip.keyframes[core::anim_prop_index(prop)]) {
      times.push_back(frame.t);
    }
  }
  for (const core::Keyframe& frame : clip.gain_keyframes) times.push_back(frame.t);

  std::ranges::sort(times);
  // Two properties keyed at the same instant are one mark, not two drawn on
  // top of each other.
  const auto duplicates = std::ranges::unique(times, [](double a, double b) {
    return std::abs(a - b) <= core::kKeyframeMatchEps;
  });
  times.erase(duplicates.begin(), duplicates.end());
  return times;
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
  model.in_point = project.in_point;
  model.out_point = project.out_point;

  model.markers.reserve(project.markers.size());
  for (const core::Marker& marker : project.markers) {
    model.markers.push_back(ui::TimelineMarker{
        .time = marker.time, .label = marker.label, .color = marker.color});
  }
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
    // The switches as the project holds them, which is not the same as the line
    // above: a track silenced by somebody else's solo is not muted, and its M
    // must not light up saying it is.
    row.switches = ui::TrackSwitches{.mute = track.muted,
                                     .solo = track.solo,
                                     .lock = track.locked,
                                     .hide = track.hidden};

    row.blocks.reserve(track.clips.size());
    for (const core::Clip& clip : track.clips) {
      ui::BlockTransition transition;
      // Only when the renderer would honour it. A transition stored on the last
      // clip of a track, or on one with a gap after it, resolves to nothing —
      // and drawing it would be the timeline claiming something the picture
      // does not do.
      if (const TransitionRow at_join = clip_transition(project, clip.id);
          at_join.joins && at_join.present) {
        transition.duration = at_join.duration;
        transition.label = std::string(transition_name(at_join.kind));
      }

      row.blocks.push_back(ui::TimelineBlock{
          .id = clip.id,
          .start = clip.start,
          .end = core::clip_end(clip),
          .label = label_for(project, clip),
          .selected = std::ranges::find(selection, clip.id) != selection.end(),
          .keyframes = keyframe_times(clip),
          .transition = std::move(transition),
      });
    }
    model.tracks.push_back(std::move(row));
  }
  return model;
}

core::Project apply_timeline_edit(core::Project project, std::string_view clip_id,
                                  const ui::TimelineEdit& edit) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  const std::array<std::string, 1> ids{std::string(clip_id)};

  switch (edit.mode) {
    case ui::DragMode::Move:
      // Through move_clips rather than by assignment, so the whole linked
      // group travels and the clamp against the start of the timeline is the
      // model's rather than a second opinion about it.
      return core::move_clips(std::move(project), ids, edit.result.start - clip->start);

    case ui::DragMode::TrimStart:
      return core::set_clip_edge(std::move(project), clip_id, core::ClipEdge::In,
                                 edit.result.start);

    case ui::DragMode::TrimEnd:
      return core::set_clip_edge(std::move(project), clip_id, core::ClipEdge::Out,
                                 edit.result.end);

    case ui::DragMode::RateStart:
      return core::rate_stretch_edge(std::move(project), clip_id, core::ClipEdge::In,
                                     edit.result.start);

    case ui::DragMode::RateEnd:
      return core::rate_stretch_edge(std::move(project), clip_id, core::ClipEdge::Out,
                                     edit.result.end);

    case ui::DragMode::Slip: {
      // The gesture is in timeline seconds and the core wants source seconds,
      // which the clip's own speed converts: a clip at 2x shows twice as much
      // footage for the same distance, so a second of dragging is two seconds
      // of source. Dragging right shows *earlier* footage, as Premiere does —
      // the clip's content follows the hand, so the window into it moves the
      // other way.
      const double source = -edit.delta * core::clip_speed(*clip);
      return core::slip_clip(std::move(project), clip_id, source);
    }

    case ui::DragMode::Slide:
      return core::slide_clip(std::move(project), clip_id, edit.result.start - clip->start);

    case ui::DragMode::Razor: {
      if (!edit.all_tracks) return core::split_at(std::move(project), edit.at, ids);
      // Every clip in the project. `split_at` ignores the ones the cut does not
      // fall inside, so this is "cut through everything" without the caller
      // having to work out what that means.
      std::vector<std::string> every;
      for (const core::Track& track : project.tracks) {
        for (const core::Clip& c : track.clips) every.push_back(c.id);
      }
      return core::split_at(std::move(project), edit.at, every);
    }

    case ui::DragMode::None:
    case ui::DragMode::Scrub:
      break;
  }
  return project;
}

core::Project toggle_track_switch(core::Project project, std::string_view track_id,
                                  ui::TrackControl control) {
  const auto found = std::ranges::find(project.tracks, track_id, &core::Track::id);
  if (found == project.tracks.end()) return project;

  // Read then flip, so the interface never has to hold the current value and
  // cannot get out of step with the document by holding a stale one.
  core::TrackPropsPatch patch;
  switch (control) {
    case ui::TrackControl::Mute: patch.muted = !found->muted; break;
    case ui::TrackControl::Solo: patch.solo = !found->solo; break;
    case ui::TrackControl::Lock: patch.locked = !found->locked; break;
    case ui::TrackControl::Hide: patch.hidden = !found->hidden; break;
  }
  return core::update_track(std::move(project), track_id, patch);
}

std::optional<std::string> block_clip_id(const ui::TimelineModel& model, ui::BlockRef ref) {
  if (ref.track >= model.tracks.size()) return std::nullopt;
  const std::vector<ui::TimelineBlock>& blocks = model.tracks[ref.track].blocks;
  if (ref.block >= blocks.size()) return std::nullopt;
  return blocks[ref.block].id;
}

}  // namespace cutline::editor
