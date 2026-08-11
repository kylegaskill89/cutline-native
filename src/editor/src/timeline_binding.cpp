#include "cutline/editor/timeline_binding.hpp"

#include "cutline/core/animate.hpp"
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
///
/// `banded` leaves the gain keyframes out, for a clip whose volume is drawn as
/// a rubber band instead. The band says where the automation is and what it is
/// doing, which is strictly more than a diamond says; drawing both puts two
/// marks on one clip for one keyframe.
[[nodiscard]] std::vector<double> keyframe_times(const core::Clip& clip, bool banded) {
  std::vector<double> times = core::effect_keyframe_times(clip);

  for (const core::AnimProp prop : core::kAnimProps) {
    for (const core::Keyframe& frame : clip.keyframes[core::anim_prop_index(prop)]) {
      times.push_back(frame.t);
    }
  }
  if (!banded) {
    for (const core::Keyframe& frame : clip.gain_keyframes) times.push_back(frame.t);
  }

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
  if (index >= project.sequence().tracks.size()) return {};
  const core::Track& track = project.sequence().tracks[index];
  if (!track.label.empty()) return track.label;

  if (track.kind == core::TrackKind::Video) {
    // Counted from the bottom: V1 is the base layer everything stacks onto, so
    // the topmost track has the highest number.
    std::size_t below = 0;
    for (std::size_t i = index + 1; i < project.sequence().tracks.size(); ++i) {
      if (project.sequence().tracks[i].kind == core::TrackKind::Video) ++below;
    }
    return "V" + std::to_string(below + 1);
  }

  // Audio counts from the top, so A1 is the first lane and the two numbering
  // schemes meet in the middle of the timeline.
  std::size_t above = 0;
  for (std::size_t i = 0; i < index; ++i) {
    if (project.sequence().tracks[i].kind == core::TrackKind::Audio) ++above;
  }
  return "A" + std::to_string(above + 1);
}

ui::TimelineModel timeline_model(const core::Project& project,
                                 std::span<const std::string> selection,
                                 const TimelineMedia& media) {
  ui::TimelineModel model;
  model.fps = project.sequence().fps;
  model.drop_frame = project.sequence().drop_frame;
  model.duration = core::timeline_duration(project);
  model.in_point = project.sequence().in_point;
  model.out_point = project.sequence().out_point;
  // Taken from the model rather than left at the timeline's own default, so the
  // top of a volume band is the loudest gain the core will actually store. A
  // band that reached higher would refuse the last part of its own travel.
  model.max_gain = core::kMaxGain;

  model.markers.reserve(project.sequence().markers.size());
  for (const core::Marker& marker : project.sequence().markers) {
    model.markers.push_back(ui::TimelineMarker{.time = marker.time,
                                               .label = marker.label,
                                               .color = marker.color,
                                               .duration = marker.duration,
                                               .comment = marker.comment});
  }
  model.tracks.reserve(project.sequence().tracks.size());

  for (std::size_t i = 0; i < project.sequence().tracks.size(); ++i) {
    const core::Track& track = project.sequence().tracks[i];
    const bool audio = track.kind == core::TrackKind::Audio;

    ui::TimelineTrack row;
    row.id = track.id;
    row.name = default_track_label(project, i);
    row.audio = audio;
    row.height = track.height;
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
                                     .hide = track.hidden,
                                     .target = track.targeted};

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

      // The volume band, on audio clips only. A video clip's gain is not what
      // anybody means by its volume — the audio it was linked to has its own
      // clip, on its own track, and that is the one carrying the level.
      std::optional<ui::GainBand> band;
      if (audio) {
        ui::GainBand gain;
        gain.level = clip.gain;
        gain.points.reserve(clip.gain_keyframes.size());
        for (const core::Keyframe& frame : clip.gain_keyframes) {
          gain.points.push_back(ui::GainPoint{.t = frame.t, .v = frame.v});
        }
        band = std::move(gain);
      }

      // The envelope, on audio clips only — a video clip draws its picture, not
      // its sound, and the audio it was linked to is a clip of its own.
      std::shared_ptr<const ui::Waveform> waveform;
      if (audio && media.waveforms) waveform = media.waveforms(clip.media_id, clip.audio_stream);

      // And the filmstrip on the ones that draw a picture. `has_video` rather
      // than the track's kind, because that is the flag meaning "contributes
      // picture" — an adjustment layer sets it and has nothing to show, and a
      // generated source has no file to take frames from either way.
      std::shared_ptr<const ui::Filmstrip> filmstrip;
      if (!audio && media.filmstrips) {
        const core::Media* source = media_of(project, clip.media_id);
        if (source != nullptr && source->has_video && !core::is_generated_media(*source)) {
          filmstrip = media.filmstrips(clip.media_id);
        }
      }

      row.blocks.push_back(ui::TimelineBlock{
          .id = clip.id,
          .group = clip.group_id.value_or(std::string{}),
          .start = clip.start,
          .end = core::clip_end(clip),
          .label = label_for(project, clip),
          .selected = std::ranges::find(selection, clip.id) != selection.end(),
          .disabled = clip.disabled,
          .color = clip.label_color,
          // Either stack: an audio clip's filters are as much a reason to mark
          // it as a video clip's are.
          .has_effects = !clip.effects.empty() || !clip.audio_effects.empty(),
          .keyframes = keyframe_times(clip, audio),
          .transition = std::move(transition),
          .gain = std::move(band),
          .waveform = std::move(waveform),
          .filmstrip = std::move(filmstrip),
          .fade_in = clip.fade_in,
          .fade_out = clip.fade_out,
          // What turns a position along the block into a position in the
          // source's envelope. Taken through the core's accessors so a clip
          // with no explicit speed reads as 1 rather than 0.
          .source_in = clip.source_in,
          .speed = core::clip_speed(clip),
          .reverse = clip.reverse,
      });
    }
    model.tracks.push_back(std::move(row));
  }
  return model;
}

core::Project apply_timeline_edit(core::Project project, std::string_view clip_id,
                                  const ui::TimelineEdit& edit,
                                  std::span<const std::string> selection,
                                  std::vector<std::string>* made) {
  const core::Clip* clip = core::find_clip(project, clip_id);
  if (clip == nullptr) return project;

  const std::array<std::string, 1> ids{std::string(clip_id)};

  switch (edit.mode) {
    case ui::DragMode::Move: {
      // Everything selected, when the clip being dragged is one of them.
      // Selecting several clips and having only the one under the pointer move
      // makes the selection a decoration.
      //
      // `move_clips` takes the whole set at once rather than being called per
      // clip, which is what keeps the clamp against the start of the timeline a
      // single answer: one clip stopped at zero has to stop the rest with it, or
      // the shape of the selection changes as it hits the edge.
      const bool carries_selection =
          std::ranges::find(selection, clip_id) != selection.end();
      const std::span<const std::string> moving =
          carries_selection ? selection : std::span<const std::string>{ids};
      const double shift = edit.result.start - clip->start;

      // Alt-drag: the originals stay and copies make the journey. Its own
      // operation rather than a move followed by a paste, so the copies land
      // exactly where dragging the originals there would have put them.
      if (edit.copy) {
        return core::duplicate_clips(std::move(project), moving, shift, edit.lanes,
                                     clip->kind, made);
      }

      // Straight along the track, which is most moves, and the one that must
      // not disturb which lane anything is on.
      if (edit.lanes == 0) return core::move_clips(std::move(project), moving, shift);

      // Up or down as well. `move_clips_layered` rather than `move_clips`
      // because a video clip changing lane is setting up an overlay, and its
      // sound has to go somewhere that is not already somebody else's — which
      // is a question only the layered version answers.
      return core::move_clips_layered(std::move(project), moving, shift, edit.lanes,
                                      clip->kind);
    }

    case ui::DragMode::TrimStart:
      return core::set_clip_edge(std::move(project), clip_id, core::ClipEdge::In,
                                 edit.result.start);

    case ui::DragMode::TrimEnd:
      return core::set_clip_edge(std::move(project), clip_id, core::ClipEdge::Out,
                                 edit.result.end);

    // The edge these ended on is `at` rather than something read off `result`:
    // a rippled head leaves the clip's start where it was, because the gap
    // behind it closed, so the result cannot say where the trim went.
    case ui::DragMode::RippleStart:
      return core::ripple_trim_edge(std::move(project), clip_id, core::ClipEdge::In, edit.at);

    case ui::DragMode::RippleEnd:
      return core::ripple_trim_edge(std::move(project), clip_id, core::ClipEdge::Out, edit.at);

    case ui::DragMode::RollStart:
      return core::roll_edit(std::move(project), clip_id, core::ClipEdge::In, edit.at);

    case ui::DragMode::RollEnd:
      return core::roll_edit(std::move(project), clip_id, core::ClipEdge::Out, edit.at);

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

    case ui::DragMode::TransitionLength: {
      // The kind it already is. This gesture changes how long a transition
      // runs and nothing else — which one it is belongs to the panel, and
      // turning a push into a dissolve by dragging its edge would be a second
      // meaning nobody asked this gesture for.
      const TransitionRow row = clip_transition(project, clip_id);
      if (!row.present) return project;
      // Clamped by `set_transition` against what the join can actually manage:
      // half of one sits either side of the cut and neither half may swallow
      // its clip or run past the source there is to borrow. Stopping at the
      // longest that works is what a trim does at the end of its footage.
      return set_transition(std::move(project), clip_id, row.kind,
                            edit.result.transition.duration);
    }

    case ui::DragMode::Razor: {
      if (!edit.all_tracks) {
        // Whatever is linked to it, not only the clip under the blade.
        //
        // A razor took the one clip it was pointed at, which cut the picture
        // and left the sound whole — and then made it worse, because the right
        // half of a cut clip is put in a *new* group so that the two halves are
        // not linked to each other. Cutting one of a linked pair therefore left
        // that new group holding a single clip, so the picture after the cut
        // was linked to nothing and the sound was still linked to the picture
        // before it. One blade stroke both failed to cut the sound and unlinked
        // the picture from it.
        //
        // Handing `split_at` the whole group fixes both at once: it cuts every
        // member, and the right halves all take the same new group id, so the
        // pair either side of the cut stay pairs.
        // `group_members` answers with the clip itself when it is in no group,
        // so this is one call rather than a special case for each.
        const std::vector<std::string> linked = core::group_members(project, clip_id);
        return core::split_at(std::move(project), edit.at, linked);
      }
      // Every clip in the project. `split_at` ignores the ones the cut does not
      // fall inside, so this is "cut through everything" without the caller
      // having to work out what that means.
      std::vector<std::string> every;
      for (const core::Track& track : project.sequence().tracks) {
        for (const core::Clip& c : track.clips) every.push_back(c.id);
      }
      return core::split_at(std::move(project), edit.at, every);
    }

    case ui::DragMode::GainLevel:
      return core::set_clip_gain(std::move(project), clip_id, edit.gain);

    case ui::DragMode::GainPointDrag:
      // One operation for both adding a point and moving one. A point that was
      // just created reports the same time in `gain_from` and `gain_to`, so the
      // remove half finds the keyframe it is about to replace and the set half
      // puts it back — which is the same answer as adding it outright, without
      // the caller having to know which happened.
      return core::move_gain_keyframe(std::move(project), clip_id, edit.gain_from.t,
                                      edit.gain_to.t, edit.gain_to.v);

    case ui::DragMode::FadeIn:
      return core::set_clip_fade(std::move(project), clip_id, core::ClipEdge::In, edit.fade);
    case ui::DragMode::FadeOut:
      return core::set_clip_fade(std::move(project), clip_id, core::ClipEdge::Out, edit.fade);

    case ui::DragMode::GainSegment:
      // Upserts rather than moves: a segment drag changes levels and not times,
      // so each point is set where it already is — which is what keeps the
      // interpolation mode the keyframe was carrying.
      for (const ui::GainPoint& point : edit.gain_moved) {
        project = core::set_gain_keyframe(std::move(project), clip_id, point.t, point.v);
      }
      return project;

    case ui::DragMode::GainPointRemove:
      return core::remove_gain_keyframe_at(std::move(project), clip_id, edit.gain_from.t);

    case ui::DragMode::None:
    case ui::DragMode::Scrub:
      break;
  }
  return project;
}

core::Project toggle_track_switch(core::Project project, std::string_view track_id,
                                  ui::TrackControl control) {
  const auto found = std::ranges::find(project.sequence().tracks, track_id, &core::Track::id);
  if (found == project.sequence().tracks.end()) return project;

  // Read then flip, so the interface never has to hold the current value and
  // cannot get out of step with the document by holding a stale one.
  core::TrackPropsPatch patch;
  switch (control) {
    case ui::TrackControl::Target: patch.targeted = !found->targeted; break;
    case ui::TrackControl::Mute: patch.muted = !found->muted; break;
    case ui::TrackControl::Solo: patch.solo = !found->solo; break;
    case ui::TrackControl::Lock: patch.locked = !found->locked; break;
    case ui::TrackControl::Hide: patch.hidden = !found->hidden; break;
  }
  return core::update_track(std::move(project), track_id, patch);
}

std::span<const ClipLabel> clip_labels() {
  // Premiere's eight, and its names for them. Muted rather than saturated: a
  // label sits behind a filmstrip and a waveform, and a colour loud enough to
  // win that fight is one that makes the picture unreadable.
  static constexpr std::array<ClipLabel, 8> kLabels{{
      {"Violet", "#8f7bb8"},
      {"Iris", "#6f8fc4"},
      {"Caribbean", "#4f9e9e"},
      {"Lavender", "#a887bd"},
      {"Cerulean", "#4a86b8"},
      {"Forest", "#5f8f5f"},
      {"Rose", "#c07a92"},
      {"Mango", "#c39a5a"},
  }};
  return kLabels;
}

std::optional<std::string> block_clip_id(const ui::TimelineModel& model, ui::BlockRef ref) {
  if (ref.track >= model.tracks.size()) return std::nullopt;
  const std::vector<ui::TimelineBlock>& blocks = model.tracks[ref.track].blocks;
  if (ref.block >= blocks.size()) return std::nullopt;
  return blocks[ref.block].id;
}

}  // namespace cutline::editor
