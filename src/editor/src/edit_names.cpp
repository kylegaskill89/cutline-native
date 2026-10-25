#include "cutline/editor/edit_names.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <vector>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Sequence;
using core::Track;

/// Things by id, for comparing two lists that may have gained, lost or
/// reordered entries.
template <typename T>
[[nodiscard]] std::map<std::string_view, const T*> by_id(const std::vector<T>& items) {
  std::map<std::string_view, const T*> found;
  for (const T& item : items) found.emplace(item.id, &item);
  return found;
}

/// What a pool entry that has just appeared is, in Premiere's words for making
/// one: a file is imported, everything else is a new item of its kind.
[[nodiscard]] std::string arrival(const Media& media) {
  if (media.is_graphic) return "New Graphic";
  if (media.is_color) return "New Colour Matte";
  if (media.is_adjustment) return "New Adjustment Layer";
  if (!media.sequence_id.empty()) return "Nest";
  return "Import";
}

[[nodiscard]] std::string describe_pool(const Project& before, const Project& after) {
  const auto was = by_id(before.media);
  const auto now = by_id(after.media);
  for (const auto& [id, media] : now) {
    if (!was.contains(id)) return arrival(*media);
  }
  for (const auto& [id, media] : was) {
    if (!now.contains(id)) return "Clear";
  }
  for (const auto& [id, media] : now) {
    const Media& old = *was.at(id);
    if (*media == old) continue;
    if (media->label_color != old.label_color) return "Label";
    if (media->name != old.name) return "Rename";
    if (media->path != old.path) return "Link Media";
    if (media->bin != old.bin) return "Move to Bin";
    if (media->assumed_fps != old.assumed_fps || media->colour_override != old.colour_override) {
      return "Interpret Footage";
    }
    if (media->in_point != old.in_point || media->out_point != old.out_point) {
      return "Mark Source";
    }
    if (media->graphic != old.graphic) return "Edit Graphic";
    if (media->color != old.color || media->gradient != old.gradient) return "Edit Colour Matte";
    // Anything else on an entry is bookkeeping the application keeps up to
    // date — a proxy's path, a nest's length — and is named by whatever edit
    // caused it, if anything here is. Left to fall through.
  }
  return {};
}

[[nodiscard]] std::string describe_bins(const Project& before, const Project& after) {
  const auto was = by_id(before.bins);
  const auto now = by_id(after.bins);
  for (const auto& [id, bin] : now) {
    if (!was.contains(id)) return "New Bin";
  }
  for (const auto& [id, bin] : was) {
    if (!now.contains(id)) return "Delete Bin";
  }
  for (const auto& [id, bin] : now) {
    const core::Bin& old = *was.at(id);
    if (bin->name != old.name) return "Rename Bin";
    if (bin->parent != old.parent) return "Move Bin";
  }
  return {};
}

/// One clip against what it was, the most telling difference first.
[[nodiscard]] std::string describe_clip(const Clip& old, const Clip& now) {
  if (now.speed != old.speed || now.reverse != old.reverse) return "Speed/Duration";
  if (now.source_in != old.source_in || now.source_out != old.source_out) return "Trim";
  if (now.start != old.start) return "Move";
  if (now.hold != old.hold) return "Frame Hold";
  if (now.disabled != old.disabled) return "Enable";
  if (now.group_id != old.group_id) return now.group_id.has_value() ? "Link" : "Unlink";
  if (now.transition_out != old.transition_out) {
    if (!old.transition_out.has_value()) return "Add Transition";
    if (!now.transition_out.has_value()) return "Remove Transition";
    return "Transition";
  }
  if (now.effects.size() != old.effects.size() ||
      now.audio_effects.size() != old.audio_effects.size()) {
    return now.effects.size() + now.audio_effects.size() >
                   old.effects.size() + old.audio_effects.size()
               ? "Add Effect"
               : "Remove Effect";
  }
  if (now.effects != old.effects || now.audio_effects != old.audio_effects) {
    // A reorder keeps every effect and changes only where each one is.
    const auto types = [](const Clip& clip) {
      std::vector<std::string> names;
      for (const core::ClipEffect& effect : clip.effects) names.push_back(effect.type);
      for (const core::AudioClipEffect& effect : clip.audio_effects) names.push_back(effect.type);
      return names;
    };
    if (types(now) != types(old)) return "Reorder Effects";
    for (std::size_t i = 0; i < now.effects.size(); ++i) {
      if (now.effects[i].enabled != old.effects[i].enabled) return "Effect Enabled";
      if (now.effects[i].mask != old.effects[i].mask) return "Mask";
    }
    return "Effect Change";
  }
  if (now.transform != old.transform) return "Motion";
  if (now.opacity != old.opacity) return "Opacity";
  if (now.blend != old.blend) return "Blend Mode";
  if (now.gain != old.gain || now.gain_keyframes != old.gain_keyframes) return "Volume";
  if (now.pan != old.pan) return "Pan";
  if (now.fade_in != old.fade_in || now.fade_out != old.fade_out) return "Fade";
  if (now.keyframes != old.keyframes) return "Keyframe";
  if (now.channel_map != old.channel_map || now.audio_stream != old.audio_stream) {
    return "Audio Channels";
  }
  if (now.role != old.role) return "Essential Sound";
  if (now.label_color != old.label_color) return "Label";
  return {};
}

[[nodiscard]] std::string describe_track(const Track& old, const Track& now) {
  if (now.label != old.label) return "Rename Track";
  if (now.muted != old.muted) return "Mute Track";
  if (now.solo != old.solo) return "Solo Track";
  if (now.locked != old.locked) return "Lock Track";
  if (now.hidden != old.hidden) return "Toggle Track Output";
  if (now.targeted != old.targeted) return "Target Track";
  if (now.sync_locked != old.sync_locked) return "Sync Lock";
  if (now.height != old.height) return "Track Height";
  if (now.gain != old.gain || now.gain_keyframes != old.gain_keyframes) return "Track Volume";
  if (now.pan != old.pan || now.pan_keyframes != old.pan_keyframes) return "Track Pan";
  if (now.automation != old.automation) return "Automation Mode";
  if (now.audio_effects != old.audio_effects) {
    if (now.audio_effects.size() > old.audio_effects.size()) return "Add Track Effect";
    if (now.audio_effects.size() < old.audio_effects.size()) return "Remove Track Effect";
    return "Track Effect Change";
  }
  if (now.output != old.output || now.sends != old.sends) return "Track Routing";
  return {};
}

[[nodiscard]] std::string describe_sequence(const Sequence& old, const Sequence& now) {
  // Clips first: what happens on the tracks is what an edit is, nearly always.
  std::map<std::string_view, const Clip*> was;
  std::map<std::string_view, const Clip*> is;
  for (const Track& track : old.tracks) {
    for (const Clip& clip : track.clips) was.emplace(clip.id, &clip);
  }
  for (const Track& track : now.tracks) {
    for (const Clip& clip : track.clips) is.emplace(clip.id, &clip);
  }

  // Tracks coming and going before clips: removing a track takes its clips
  // with it, and that is a track being deleted rather than a clear.
  if (now.tracks.size() > old.tracks.size()) {
    const auto old_tracks = by_id(old.tracks);
    for (const Track& track : now.tracks) {
      if (old_tracks.contains(track.id)) continue;
      if (track.submix) return "Add Submix Track";
      return track.kind == core::TrackKind::Audio ? "Add Audio Track" : "Add Video Track";
    }
  }
  if (now.tracks.size() < old.tracks.size()) return "Delete Track";

  std::size_t added = 0;
  std::size_t removed = 0;
  for (const auto& [id, clip] : is) {
    if (!was.contains(id)) ++added;
  }
  for (const auto& [id, clip] : was) {
    if (!is.contains(id)) ++removed;
  }
  if (added > 0 && removed == 0) {
    // A cut leaves the clip it cut, shortened, and its other half beside it.
    for (const auto& [id, clip] : is) {
      const auto found = was.find(id);
      if (found != was.end() && found->second->source_out != clip->source_out) return "Razor";
    }
    return "Add Clip";
  }
  if (removed > 0 && added == 0) return "Clear";
  if (added > 0 && removed > 0) return "Replace";

  // The same clips. Which one changed, and how — and if a clip is on a
  // different track, it was moved there.
  std::map<std::string_view, std::string_view> track_of;
  for (const Track& track : old.tracks) {
    for (const Clip& clip : track.clips) track_of.emplace(clip.id, track.id);
  }
  for (const Track& track : now.tracks) {
    for (const Clip& clip : track.clips) {
      if (const auto found = track_of.find(clip.id);
          found != track_of.end() && found->second != track.id) {
        return "Move";
      }
    }
  }
  for (const auto& [id, clip] : is) {
    if (std::string name = describe_clip(*was.at(id), *clip); !name.empty()) return name;
  }

  const auto old_tracks = by_id(old.tracks);
  for (const Track& track : now.tracks) {
    const auto found = old_tracks.find(track.id);
    if (found == old_tracks.end()) continue;
    if (std::string name = describe_track(*found->second, track); !name.empty()) return name;
  }
  if (now.tracks != old.tracks) return "Reorder Tracks";

  if (now.markers != old.markers) {
    if (now.markers.size() > old.markers.size()) return "Add Marker";
    if (now.markers.empty()) return "Clear All Markers";
    if (now.markers.size() < old.markers.size()) return "Clear Marker";
    return "Edit Marker";
  }
  if (now.in_point != old.in_point || now.out_point != old.out_point) {
    if (!now.in_point.has_value() && !now.out_point.has_value()) return "Clear In and Out";
    if (now.in_point != old.in_point) return "Mark In";
    return "Mark Out";
  }
  if (now.master_gain != old.master_gain ||
      now.master_gain_keyframes != old.master_gain_keyframes) {
    return "Master Volume";
  }
  if (now.master_effects != old.master_effects) return "Master Effect";
  if (now.name != old.name) return "Rename Sequence";
  return "Sequence Settings";
}

}  // namespace

std::string describe_edit(const Project& before, const Project& after) {
  if (std::string name = describe_pool(before, after); !name.empty()) return name;
  if (std::string name = describe_bins(before, after); !name.empty()) return name;

  if (after.sequences.size() > before.sequences.size()) return "New Sequence";
  if (after.sequences.size() < before.sequences.size()) return "Delete Sequence";

  const auto was = by_id(before.sequences);
  for (const Sequence& sequence : after.sequences) {
    const auto found = was.find(sequence.id);
    if (found == was.end()) return "Replace Sequence";
    if (*found->second == sequence) continue;
    return describe_sequence(*found->second, sequence);
  }

  if (after.use_proxies != before.use_proxies) return "Use Proxies";
  return "Edit";
}

}  // namespace cutline::editor
