#pragma once

/// Multi-camera editing, as Premiere does it.
///
/// A shoot with several cameras running at once becomes one *multi-camera
/// source sequence*: each camera on a video track of its own, the first on V1,
/// lined up so that the same moment is at the same time on every track. A clip
/// of that sequence is a nest that shows one camera, and cutting a scene is
/// playing it and choosing a camera at each moment, which cuts the clip and
/// sets the camera on the piece after the cut. See `Clip::camera` and
/// `Sequence::multicam`.
///
/// Everything takes a project by value and hands one back, like the rest of
/// core, so a pass of switches made while playing is one entry in the undo
/// stack however many cuts it made.

#include "cutline/core/model.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::core {

/// The multi-camera source sequence a clip is of, whether or not the clip has
/// multicam switched on; null for a clip of anything else.
[[nodiscard]] const Sequence* multicam_source(const Project& p, const Clip& clip) noexcept;

/// Whether a clip shows one camera of a multi-camera source sequence, rather
/// than being an ordinary clip or a multicam drawn as a plain nest.
[[nodiscard]] bool shows_camera(const Project& p, const Clip& clip) noexcept;

/// The video tracks of a sequence in camera order, as indexes into its
/// tracks: the first camera is the bottom track, V1, as Premiere numbers them.
[[nodiscard]] std::vector<std::size_t> camera_tracks(const Sequence& s);

/// What each camera is called: its track's label, or "Camera N".
[[nodiscard]] std::vector<std::string> camera_names(const Sequence& s);

/// Whether a track of a multi-camera source sequence is drawn or heard for a
/// clip showing `camera`.
///
/// A video track is drawn when it is that camera's. An audio track is heard
/// when it is that camera's, counting A1 as the first camera's, when it is
/// past the last camera's (sound recorded on its own), or when the sequence
/// mixes every camera's sound.
[[nodiscard]] bool camera_uses_track(const Sequence& s, std::size_t track, int camera);

/// What a clip is called on the timeline: Premiere's "[MC2]" in front of a
/// multicam's name, saying which camera it shows. Empty for anything else.
[[nodiscard]] std::string camera_tag(const Project& p, const Clip& clip);

/// How many cells across the Multi-Camera view's grid has for this many
/// cameras: square, as Premiere's are, so 2 by 2 for up to four.
[[nodiscard]] int camera_grid_columns(int cameras) noexcept;

/// What the Multi-Camera view draws on its left: every camera of the
/// multicam source `media_id` in a cell of its own, as a project whose open
/// sequence is that grid, at most `width` pixels across and in the source
/// sequence's time. Each cell is a clip of the multicam showing one camera,
/// so it is drawn exactly as a cut to that camera would be.
[[nodiscard]] Project camera_grid(const Project& p, std::string_view media_id, int width);

// --------------------------------------------------------------- making --

/// What the cameras are lined up by: Premiere's Synchronize Point.
enum class MulticamSync {
  /// Each source's marked In point is the same moment.
  InPoints,
  /// Each source's marked Out point is the same moment.
  OutPoints,
  /// The timecode each file records.
  Timecode,
  /// Each source's first clip marker is the same moment: a clap marked in
  /// the Source Monitor on every camera.
  ClipMarker,
  /// The sound each camera recorded, matched; measured by the engine, which
  /// can decode it, and handed to `create_multicam_sequence` as start times.
  Audio,
};

/// What the cameras are called.
enum class CameraNaming {
  /// Camera 1, Camera 2, and so on.
  Enumerate,
  /// The name of each camera's source.
  ClipNames,
};

/// Premiere's Create Multi-Camera Source Sequence dialog.
struct MulticamOptions {
  /// Empty for the first source's name with " Multicam" after it.
  std::string name;
  MulticamSync sync = MulticamSync::InPoints;
  /// For timecode: compare minutes, seconds and frames only, for cameras
  /// whose clocks were set the same apart from the hour.
  bool ignore_hours = false;
  MulticamAudio audio = MulticamAudio::Camera1;
  CameraNaming naming = CameraNaming::Enumerate;
  /// Files the sources in a "Processed Clips" bin once they are used.
  bool move_to_processed = false;
};

/// Where each source starts in the sequence to be made, in seconds, so that
/// its synchronise point lands at the same moment as every other's. The
/// earliest starts at zero. Fails for audio, which needs the sound decoded,
/// for timecode when a source records none, and for clip markers when a
/// source has none.
[[nodiscard]] std::expected<std::vector<double>, std::string> multicam_starts(
    const Project& p, std::span<const std::string> media_ids, MulticamSync sync,
    bool ignore_hours = false);

/// Adds a multi-camera source sequence of these sources, starting where
/// `starts` says, and its entry in the pool.
///
/// Each source with a picture is a camera, in the order given: its picture on
/// a video track of its own and its sound on the audio track of the same
/// number, linked. A source with only sound is not a camera; it goes on an
/// audio track after the cameras' and is heard whichever camera shows. The
/// sequence takes the first camera's size and rate, as Premiere's Automatic
/// preset does. The open sequence stays open.
///
/// Returns the project unchanged when no source has a picture.
[[nodiscard]] Project create_multicam_sequence(Project p, std::span<const std::string> media_ids,
                                               std::span<const double> starts,
                                               const MulticamOptions& options,
                                               std::string* made_media = nullptr);

// ------------------------------------------------------------ switching --

/// Shows another camera on a whole multicam clip, as choosing one in the
/// Multi-Camera view does while stopped. With `audio_follows`, or when the
/// sequence's sound switches with the picture, the clips linked to it change
/// camera too.
[[nodiscard]] Project switch_camera(Project p, std::string_view clip_id, int camera,
                                    bool audio_follows);

/// One choice made while playing: from `time`, show `camera`.
struct CameraSwitch {
  double time = 0.0;
  int camera = 0;

  friend bool operator==(const CameraSwitch&, const CameraSwitch&) = default;
};

/// Cuts the multicam on a track at each switch and shows the chosen camera
/// from there, as switching while playing does. A switch at a clip's own
/// start changes that clip without cutting it; one to the camera already
/// showing does nothing; one over no multicam is ignored. Linked clips are
/// cut with it, and change camera on the terms `switch_camera` sets.
[[nodiscard]] Project record_camera_switches(Project p, std::string_view track_id,
                                             std::span<const CameraSwitch> switches,
                                             bool audio_follows);

/// Premiere's Multi-Camera > Enable: whether these clips show one camera or
/// are drawn as an ordinary nest.
[[nodiscard]] Project set_multicam_enabled(Project p, std::span<const std::string> clip_ids,
                                           bool enabled);

/// Premiere's Flatten Multi-Camera: each multicam clip is replaced by the
/// clips of the camera it shows, cut to the same stretch, so the cut no longer
/// depends on the source sequence. A sound clip is replaced by the clips it
/// was hearing. The multicam's own effects are added to each clip that
/// replaces it. Reversed clips are left as they are, since a reversed nest has
/// no flat equivalent short of reversing every clip in it.
[[nodiscard]] Project flatten_multicam(Project p, std::span<const std::string> clip_ids);

}  // namespace cutline::core
