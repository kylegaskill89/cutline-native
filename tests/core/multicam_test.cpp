/// Multi-camera source sequences and the clips that show one camera of them.
///
/// What matters is the bookkeeping Premiere's arrangement depends on: cameras
/// numbered from V1 and A1, start times that put the same moment at the same
/// place, a cut while playing that leaves the earlier piece alone, and a
/// flatten that shows what the multicam showed.

#include "cutline/core/multicam.hpp"

#include "cutline/core/nesting.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/core/sequences.hpp"
#include "cutline/core/serialize.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::core {
namespace {

/// Three cameras, each with sound, and a recorder with sound alone.
[[nodiscard]] Project a_shoot() {
  Project p = empty_project();
  p.sequences.front().id = "edit";
  p.sequences.front().fps = 25.0;
  p.media = {
      Media{.id = "a", .name = "wide.mp4", .duration = 60.0, .has_video = true,
            .audio_stream_count = 1, .width = 1920, .height = 1080, .fps = 25.0,
            .timecode = 36000.0, .in_point = 2.0},
      Media{.id = "b", .name = "close.mp4", .duration = 50.0, .has_video = true,
            .audio_stream_count = 1, .width = 3840, .height = 2160, .fps = 50.0,
            .timecode = 36004.0, .in_point = 6.0},
      Media{.id = "c", .name = "side.mp4", .duration = 40.0, .has_video = true,
            .audio_stream_count = 0, .timecode = 39601.0, .in_point = 1.0},
      Media{.id = "r", .name = "recorder.wav", .duration = 70.0, .audio_stream_count = 1,
            .timecode = 35990.0},
  };
  return p;
}

/// The shoot made into a multicam, synchronised on In points, with one clip of
/// it on the edit: video on V1 and sound on A1, linked.
[[nodiscard]] Project cutting(MulticamAudio audio = MulticamAudio::Camera1) {
  Project p = a_shoot();
  const std::vector<std::string> ids{"a", "b", "c"};
  const auto starts = multicam_starts(p, ids, MulticamSync::InPoints);
  std::string made;
  p = create_multicam_sequence(std::move(p), ids, *starts,
                               MulticamOptions{.audio = audio}, &made);
  Track v1{.id = "v1", .kind = TrackKind::Video};
  v1.clips = {Clip{.id = "mc", .media_id = made, .source_in = 10.0, .source_out = 30.0,
                   .start = 0.0, .group_id = "pair"}};
  Track a1{.id = "a1", .kind = TrackKind::Audio};
  a1.clips = {Clip{.id = "mcs", .media_id = made, .kind = TrackKind::Audio, .source_in = 10.0,
                   .source_out = 30.0, .start = 0.0, .group_id = "pair"}};
  p.sequences.front().tracks = {v1, a1};
  return p;
}

[[nodiscard]] const Sequence& source_of(const Project& p) {
  return *multicam_source(p, *find_clip(p, "mc"));
}

}  // namespace

TEST(Multicam, InPointsLineTheSourcesUpWithTheEarliestAtZero) {
  const Project p = a_shoot();
  const std::vector<std::string> ids{"a", "b", "c"};
  const auto starts = multicam_starts(p, ids, MulticamSync::InPoints);
  ASSERT_TRUE(starts.has_value());
  // The In points are 2, 6 and 1 seconds in, so the one marked latest starts
  // first and each other is pushed along by the difference.
  EXPECT_DOUBLE_EQ((*starts)[0], 4.0);
  EXPECT_DOUBLE_EQ((*starts)[1], 0.0);
  EXPECT_DOUBLE_EQ((*starts)[2], 5.0);
}

TEST(Multicam, TimecodePutsTheEarliestClockFirst) {
  const Project p = a_shoot();
  const std::vector<std::string> ids{"a", "b", "r"};
  const auto starts = multicam_starts(p, ids, MulticamSync::Timecode);
  ASSERT_TRUE(starts.has_value());
  EXPECT_DOUBLE_EQ((*starts)[0], 10.0);
  EXPECT_DOUBLE_EQ((*starts)[1], 14.0);
  EXPECT_DOUBLE_EQ((*starts)[2], 0.0);
}

TEST(Multicam, IgnoringHoursComparesTheRestOfTheClock) {
  const Project p = a_shoot();
  // Camera C's clock reads an hour ahead and a second on from A's.
  const std::vector<std::string> ids{"a", "c"};
  const auto starts = multicam_starts(p, ids, MulticamSync::Timecode, true);
  ASSERT_TRUE(starts.has_value());
  EXPECT_DOUBLE_EQ((*starts)[0], 0.0);
  EXPECT_DOUBLE_EQ((*starts)[1], 1.0);
}

TEST(Multicam, TimecodeFailsForASourceThatRecordsNone) {
  Project p = a_shoot();
  p.media[1].timecode.reset();
  const std::vector<std::string> ids{"a", "b"};
  const auto starts = multicam_starts(p, ids, MulticamSync::Timecode);
  ASSERT_FALSE(starts.has_value());
  EXPECT_NE(starts.error().find("close.mp4"), std::string::npos);
}

TEST(Multicam, EachCameraGetsAVideoTrackFromV1AndAnAudioTrackFromA1) {
  Project p = a_shoot();
  const std::vector<std::string> ids{"a", "b", "c", "r"};
  const std::vector<double> starts{4.0, 0.0, 5.0, 1.0};
  std::string made;
  p = create_multicam_sequence(std::move(p), ids, starts, MulticamOptions{}, &made);
  ASSERT_EQ(p.sequences.size(), 2u);
  EXPECT_EQ(p.open, 0u) << "making one does not open it";
  const Sequence& s = p.sequences.back();
  EXPECT_TRUE(s.multicam);
  EXPECT_EQ(s.name, "wide.mp4 Multicam");
  // The first camera's shape and rate.
  EXPECT_EQ(s.canvas_w, 1920);
  EXPECT_DOUBLE_EQ(s.fps, 25.0);

  const std::vector<std::size_t> cameras = camera_tracks(s);
  ASSERT_EQ(cameras.size(), 3u);
  EXPECT_EQ(s.tracks[cameras[0]].clips.front().media_id, "a");
  EXPECT_EQ(s.tracks[cameras[1]].clips.front().media_id, "b");
  EXPECT_EQ(s.tracks[cameras[2]].clips.front().media_id, "c");
  EXPECT_DOUBLE_EQ(s.tracks[cameras[0]].clips.front().start, 4.0);
  EXPECT_EQ(camera_names(s), (std::vector<std::string>{"Camera 1", "Camera 2", "Camera 3"}));

  // A1 and A2 are the first two cameras', A3 is the silent third's and so
  // empty, and the recorder comes after.
  std::vector<const Track*> audio;
  for (const Track& t : s.tracks) {
    if (t.kind == TrackKind::Audio) audio.push_back(&t);
  }
  ASSERT_EQ(audio.size(), 4u);
  EXPECT_EQ(audio[0]->clips.front().media_id, "a");
  EXPECT_EQ(audio[1]->clips.front().media_id, "b");
  EXPECT_TRUE(audio[2]->clips.empty());
  EXPECT_EQ(audio[3]->clips.front().media_id, "r");
  EXPECT_EQ(audio[0]->clips.front().group_id, s.tracks[cameras[0]].clips.front().group_id);

  const auto entry = std::ranges::find(p.media, made, &Media::id);
  ASSERT_NE(entry, p.media.end());
  EXPECT_EQ(entry->sequence_id, s.id);
  EXPECT_DOUBLE_EQ(entry->duration, 71.0) << "the recorder runs from 1 to 71";
}

TEST(Multicam, CameraNamesCanBeTheSourcesNames) {
  Project p = a_shoot();
  const std::vector<std::string> ids{"a", "b"};
  const std::vector<double> starts{0.0, 0.0};
  p = create_multicam_sequence(std::move(p), ids, starts,
                               MulticamOptions{.name = "Interview",
                                               .naming = CameraNaming::ClipNames});
  EXPECT_EQ(p.sequences.back().name, "Interview");
  EXPECT_EQ(camera_names(p.sequences.back()),
            (std::vector<std::string>{"wide.mp4", "close.mp4"}));
}

TEST(Multicam, TheSourcesCanBeFiledInAProcessedClipsBin) {
  Project p = a_shoot();
  const std::vector<std::string> ids{"a", "b"};
  const std::vector<double> starts{0.0, 0.0};
  p = create_multicam_sequence(std::move(p), ids, starts,
                               MulticamOptions{.move_to_processed = true});
  ASSERT_EQ(p.bins.size(), 1u);
  EXPECT_EQ(p.bins[0].name, "Processed Clips");
  EXPECT_EQ(p.media[0].bin, p.bins[0].id);
  EXPECT_EQ(p.media[1].bin, p.bins[0].id);
  EXPECT_EQ(p.media[2].bin, "") << "a source not used stays where it was";
}

TEST(Multicam, NothingIsMadeWithoutACamera) {
  const Project before = a_shoot();
  const std::vector<std::string> ids{"r"};
  const std::vector<double> starts{0.0};
  EXPECT_EQ(create_multicam_sequence(before, ids, starts, MulticamOptions{}), before);
}

TEST(Multicam, AClipShowsTheFirstCameraAndIsTaggedSo) {
  const Project p = cutting();
  const Clip& clip = *find_clip(p, "mc");
  EXPECT_TRUE(shows_camera(p, clip));
  EXPECT_EQ(camera_tag(p, clip), "[MC1]");
  const Sequence& s = source_of(p);
  const std::vector<std::size_t> cameras = camera_tracks(s);
  EXPECT_TRUE(camera_uses_track(s, cameras[0], 0));
  EXPECT_FALSE(camera_uses_track(s, cameras[1], 0));
}

TEST(Multicam, SoundIsTheShowingCamerasUnlessEveryCameraIsMixed) {
  Project p = cutting();
  const Sequence& s = source_of(p);
  std::vector<std::size_t> audio;
  for (std::size_t t = 0; t < s.tracks.size(); ++t) {
    if (s.tracks[t].kind == TrackKind::Audio) audio.push_back(t);
  }
  EXPECT_TRUE(camera_uses_track(s, audio[0], 0));
  EXPECT_FALSE(camera_uses_track(s, audio[1], 0));
  EXPECT_TRUE(camera_uses_track(s, audio[1], 1));

  const Project all = cutting(MulticamAudio::AllCameras);
  const Sequence& mixed = source_of(all);
  for (std::size_t t = 0; t < mixed.tracks.size(); ++t) {
    if (mixed.tracks[t].kind == TrackKind::Audio) EXPECT_TRUE(camera_uses_track(mixed, t, 0));
  }
}

TEST(Multicam, SwitchingWhileStoppedChangesTheWholeClip) {
  const Project p = switch_camera(cutting(), "mc", 2, false);
  EXPECT_EQ(find_clip(p, "mc")->camera, 2);
  EXPECT_EQ(find_clip(p, "mcs")->camera, 0) << "the first camera's sound stays";
  EXPECT_EQ(camera_tag(p, *find_clip(p, "mc")), "[MC3]");
}

TEST(Multicam, SoundFollowsWhenAskedOrWhenTheSequenceSwitchesIt) {
  EXPECT_EQ(find_clip(switch_camera(cutting(), "mc", 1, true), "mcs")->camera, 1);
  EXPECT_EQ(find_clip(switch_camera(cutting(MulticamAudio::Switch), "mc", 1, false), "mcs")
                ->camera,
            1);
}

TEST(Multicam, ACameraThatIsNotThereIsRefused) {
  const Project before = cutting();
  EXPECT_EQ(switch_camera(before, "mc", 3, false), before);
  EXPECT_EQ(switch_camera(before, "mc", -1, false), before);
}

TEST(Multicam, SwitchesWhilePlayingCutTheClipAndShowTheCameraAfterEach) {
  const std::vector<CameraSwitch> switches{{.time = 4.0, .camera = 1},
                                           {.time = 9.0, .camera = 2},
                                           {.time = 12.0, .camera = 2}};
  const Project p = record_camera_switches(cutting(), "v1", switches, true);
  const auto& clips = p.sequence().tracks[0].clips;
  ASSERT_EQ(clips.size(), 3u) << "a switch to the camera already showing makes no cut";
  EXPECT_EQ(clips[0].id, "mc");
  EXPECT_EQ(clips[0].camera, 0);
  EXPECT_DOUBLE_EQ(clip_end(clips[0]), 4.0);
  EXPECT_EQ(clips[1].camera, 1);
  EXPECT_DOUBLE_EQ(clips[1].source_in, 14.0);
  EXPECT_EQ(clips[2].camera, 2);
  EXPECT_DOUBLE_EQ(clip_end(clips[2]), 20.0);
  // The sound was cut with it, and followed.
  const auto& sound = p.sequence().tracks[1].clips;
  ASSERT_EQ(sound.size(), 3u);
  EXPECT_EQ(sound[1].camera, 1);
  EXPECT_EQ(sound[2].camera, 2);
  EXPECT_EQ(sound[1].group_id, clips[1].group_id);
}

TEST(Multicam, ASwitchAtAClipsStartChangesItWithoutACut) {
  const std::vector<CameraSwitch> switches{{.time = 0.0, .camera = 1}};
  const Project p = record_camera_switches(cutting(), "v1", switches, false);
  ASSERT_EQ(p.sequence().tracks[0].clips.size(), 1u);
  EXPECT_EQ(p.sequence().tracks[0].clips[0].camera, 1);
}

TEST(Multicam, SwitchesLandOnAFrame) {
  const std::vector<CameraSwitch> switches{{.time = 4.013, .camera = 1}};
  const Project p = record_camera_switches(cutting(), "v1", switches, false);
  ASSERT_EQ(p.sequence().tracks[0].clips.size(), 2u);
  EXPECT_NEAR(p.sequence().tracks[0].clips[1].start, 4.0, 1e-9);
}

TEST(Multicam, TurnedOffItIsAnOrdinaryNest) {
  const std::vector<std::string> ids{"mc"};
  const Project p = set_multicam_enabled(cutting(), ids, false);
  EXPECT_FALSE(shows_camera(p, *find_clip(p, "mc")));
  EXPECT_EQ(camera_tag(p, *find_clip(p, "mc")), "");
  const Project back = set_multicam_enabled(p, ids, true);
  EXPECT_TRUE(shows_camera(back, *find_clip(back, "mc")));
}

TEST(Multicam, FlatteningPutsTheShowingCamerasFootageInItsPlace) {
  Project p = switch_camera(cutting(), "mc", 1, true);
  p.sequence().tracks[0].clips[0].effects.push_back(ClipEffect{.type = "blur"});
  const std::vector<std::string> ids{"mc", "mcs"};
  p = flatten_multicam(std::move(p), ids);

  // Camera 2 starts at 0 in the source sequence, so 10 to 30 of it is 10 to 30
  // of close.mp4.
  const auto& picture = p.sequence().tracks[0].clips;
  ASSERT_EQ(picture.size(), 1u);
  EXPECT_EQ(picture[0].media_id, "b");
  EXPECT_DOUBLE_EQ(picture[0].source_in, 10.0);
  EXPECT_DOUBLE_EQ(picture[0].source_out, 30.0);
  EXPECT_DOUBLE_EQ(picture[0].start, 0.0);
  ASSERT_EQ(picture[0].effects.size(), 1u);
  EXPECT_EQ(picture[0].effects[0].type, "blur");

  const auto& sound = p.sequence().tracks[1].clips;
  ASSERT_EQ(sound.size(), 1u);
  EXPECT_EQ(sound[0].media_id, "b");
  EXPECT_EQ(sound[0].group_id, picture[0].group_id) << "the camera's halves stay linked";
}

TEST(Multicam, FlatteningCutsToWhereTheCameraRanOut) {
  // Camera 3 starts at 5 in the source sequence, so the clip's first 5
  // seconds showed nothing of it, and the flattened clip starts later.
  Project p = cutting();
  p.sequence().tracks[0].clips[0].source_in = 0.0;
  p = switch_camera(std::move(p), "mc", 2, false);
  const std::vector<std::string> ids{"mc"};
  p = flatten_multicam(std::move(p), ids);
  const auto& picture = p.sequence().tracks[0].clips;
  ASSERT_EQ(picture.size(), 1u);
  EXPECT_EQ(picture[0].media_id, "c");
  EXPECT_DOUBLE_EQ(picture[0].start, 5.0);
  EXPECT_DOUBLE_EQ(picture[0].source_in, 0.0);
}

TEST(Multicam, FlatteningSoundThatHearsSeveralTracksSpreadsThem) {
  Project p = cutting(MulticamAudio::AllCameras);
  const std::vector<std::string> ids{"mcs"};
  p = flatten_multicam(std::move(p), ids);
  std::vector<std::string> heard;
  for (const Track& t : p.sequence().tracks) {
    if (t.kind != TrackKind::Audio) continue;
    for (const Clip& c : t.clips) heard.push_back(c.media_id);
  }
  EXPECT_EQ(heard, (std::vector<std::string>{"a", "b"}));
}

TEST(Multicam, TheGridIsSquareWithACellForEachCamera) {
  EXPECT_EQ(camera_grid_columns(1), 1);
  EXPECT_EQ(camera_grid_columns(2), 2);
  EXPECT_EQ(camera_grid_columns(4), 2);
  EXPECT_EQ(camera_grid_columns(5), 3);
  EXPECT_EQ(camera_grid_columns(16), 4);

  const Project p = cutting();
  const std::string media = find_clip(p, "mc")->media_id;
  const Project grid = camera_grid(p, media, 960);
  const Sequence& cells = grid.sequence();
  EXPECT_EQ(cells.canvas_w, 960);
  EXPECT_EQ(cells.canvas_h, 540);
  ASSERT_EQ(cells.tracks.size(), 3u);
  const Clip& third = cells.tracks[2].clips[0];
  EXPECT_EQ(third.camera, 2);
  EXPECT_TRUE(shows_camera(grid, third));
  // Bottom left of two by two.
  EXPECT_DOUBLE_EQ(third.transform.x, 0.25);
  EXPECT_DOUBLE_EQ(third.transform.y, 0.75);
  EXPECT_DOUBLE_EQ(third.transform.scale_x, 0.5);
  EXPECT_EQ(grid.sequences.size(), p.sequences.size() + 1) << "the rest is untouched";
}

TEST(Multicam, ItAllSurvivesSaving) {
  Project p = record_camera_switches(cutting(MulticamAudio::Switch), "v1",
                                     std::vector<CameraSwitch>{{.time = 5.0, .camera = 2}},
                                     false);
  const std::vector<std::string> ids{"mc"};
  p = set_multicam_enabled(std::move(p), ids, false);
  const auto back = from_json(to_json(p));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->project, p);
}

}  // namespace cutline::core
