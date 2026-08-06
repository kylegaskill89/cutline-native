/// Bringing files into a project.
///
/// The conversion is dull; the pool is not. Importing the same file twice must
/// not put it in twice, or a project accumulates a media entry per drag and the
/// browser fills with duplicates of one clip.

#include "cutline/editor/import.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/id.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::editor {
namespace {

using core::Project;
using core::TrackKind;

[[nodiscard]] MediaSource a_video(std::string path = "D:/footage/wide.mp4") {
  return MediaSource{.path = std::move(path),
                     .duration = 42.0,
                     .has_video = true,
                     .audio_stream_count = 1,
                     .width = 1920,
                     .height = 1080,
                     .fps = 30.0};
}

[[nodiscard]] Project with_tracks() { return core::empty_project(1, 2); }

// ------------------------------------------------------------- the pool --

TEST(Import, AFileBecomesMedia) {
  std::string id;
  const Project project = import_media(Project{}, a_video(), &id);

  ASSERT_EQ(project.media.size(), 1u);
  EXPECT_FALSE(id.empty());
  EXPECT_EQ(project.media[0].id, id);
  EXPECT_EQ(project.media[0].path, "D:/footage/wide.mp4");
  EXPECT_TRUE(project.media[0].has_video);
  EXPECT_EQ(project.media[0].audio_stream_count, 1);
  EXPECT_DOUBLE_EQ(project.media[0].duration, 42.0);
}

// --------------------------------------------------------------- relinking --

TEST(Relink, TheEntryKeepsItsIdSoEveryClipIsRepaired) {
  // The whole of why this works: clips name media by id, so one entry
  // repointed repairs every clip that used it, however many there are.
  std::string id;
  Project project = import_media(with_tracks(), a_video(), &id);
  project = core::place_media(std::move(project), id, 0.0);
  project = core::place_media(std::move(project), id, 60.0);
  const std::size_t clips = project.tracks[0].clips.size();
  ASSERT_EQ(clips, 2u);

  project = relink_media(std::move(project), id, a_video("E:/moved/wide.mp4"));

  EXPECT_EQ(project.media[0].id, id) << "the id changed, so the clips point at nothing";
  EXPECT_EQ(project.media[0].path, "E:/moved/wide.mp4");
  EXPECT_EQ(project.tracks[0].clips.size(), clips);
  for (const core::Clip& clip : project.tracks[0].clips) EXPECT_EQ(clip.media_id, id);
}

TEST(Relink, TheProbedFactsComeWithTheNewPath) {
  // A relink is only *usually* the same file in a new place. One that went on
  // reporting the old duration and size would describe something that is not
  // there, and nothing would say so.
  std::string id;
  Project project = import_media(Project{}, a_video(), &id);

  MediaSource replacement = a_video("E:/moved/tall.mov");
  replacement.duration = 7.5;
  replacement.width = 1080;
  replacement.height = 1920;
  replacement.audio_stream_count = 2;
  project = relink_media(std::move(project), id, replacement);

  EXPECT_DOUBLE_EQ(project.media[0].duration, 7.5);
  EXPECT_EQ(project.media[0].width, 1080);
  EXPECT_EQ(project.media[0].height, 1920);
  EXPECT_EQ(project.media[0].audio_stream_count, 2);
}

TEST(Relink, TheNameIsTheProjectsAndIsKept) {
  // Renameable in the browser, and somebody who renamed an entry has said what
  // they want it called. Relinking is not the moment to overrule that.
  std::string id;
  MediaSource named = a_video();
  named.name = "Wide shot, take 3";
  Project project = import_media(Project{}, named, &id);

  project = relink_media(std::move(project), id, a_video("E:/moved/wide.mp4"));
  EXPECT_EQ(project.media[0].name, "Wide shot, take 3");
}

TEST(Relink, MarksOnTheSourceSurviveIt) {
  // They are a decision about which part of the footage to use, and the
  // footage has not changed — only where it lives.
  std::string id;
  Project project = import_media(Project{}, a_video(), &id);
  project = core::set_source_in_point(std::move(project), id, 4.0);
  project = core::set_source_out_point(std::move(project), id, 9.0);

  project = relink_media(std::move(project), id, a_video("E:/moved/wide.mp4"));
  EXPECT_DOUBLE_EQ(*project.media[0].in_point, 4.0);
  EXPECT_DOUBLE_EQ(*project.media[0].out_point, 9.0);
}

TEST(Relink, NamingNothingChangesNothing) {
  const Project project = import_media(Project{}, a_video());
  EXPECT_EQ(relink_media(project, "nobody", a_video("E:/moved/wide.mp4")).media,
            project.media);
  // And a source with no path, which is what a cancelled dialog would give.
  EXPECT_EQ(relink_media(project, project.media[0].id, MediaSource{}).media, project.media);
}

TEST(Import, TheNameFallsBackToTheFilename) {
  const Project project = import_media(Project{}, a_video());
  EXPECT_EQ(project.media[0].name, "wide.mp4");
}

TEST(Import, AGivenNameIsKept) {
  MediaSource source = a_video();
  source.name = "Establishing shot";
  const Project project = import_media(Project{}, source);
  EXPECT_EQ(project.media[0].name, "Establishing shot");
}

TEST(Import, TheSameFileTwiceIsOneMedia) {
  std::string first;
  std::string second;

  Project project = import_media(Project{}, a_video(), &first);
  project = import_media(std::move(project), a_video(), &second);

  EXPECT_EQ(project.media.size(), 1u) << "the browser would show it twice";
  EXPECT_EQ(first, second) << "and the two ids could drift apart";
}

TEST(Import, TheSameFileDescribedDifferentlyIsStillTheSameFile) {
  // The path is the identity. A second probe that reported a slightly
  // different duration must not become a second entry.
  MediaSource again = a_video();
  again.duration = 41.5;
  again.name = "something else";

  Project project = import_media(Project{}, a_video());
  project = import_media(std::move(project), again);

  EXPECT_EQ(project.media.size(), 1u);
  EXPECT_DOUBLE_EQ(project.media[0].duration, 42.0) << "the first import should stand";
}

TEST(Import, DifferentFilesAreDifferentMedia) {
  Project project = import_media(Project{}, a_video("D:/footage/a.mp4"));
  project = import_media(std::move(project), a_video("D:/footage/b.mp4"));
  EXPECT_EQ(project.media.size(), 2u);
}

TEST(Import, LookingUpByPathFindsIt) {
  const Project project = import_media(Project{}, a_video());

  EXPECT_NE(find_media_by_path(project, "D:/footage/wide.mp4"), nullptr);
  EXPECT_EQ(find_media_by_path(project, "D:/footage/other.mp4"), nullptr);
  EXPECT_EQ(find_media_by_path(project, ""), nullptr) << "generated media have no path";
}

TEST(Import, GeneratedMediaAreNotMatchedByTheirEmptyPath) {
  // Titles and mattes have no path. Two of them must not collapse into one.
  Project project;
  project.media.push_back(core::Media{.id = "title1", .name = "Title", .is_text = true});
  project.media.push_back(core::Media{.id = "title2", .name = "Title", .is_text = true});

  std::string id;
  project = import_media(std::move(project), a_video(), &id);
  EXPECT_EQ(project.media.size(), 3u);
  EXPECT_NE(id, "title1");
}

// ------------------------------------------------------------ placement --

TEST(Import, PlacingPutsClipsOnTheTimeline) {
  const Project project = import_and_place(with_tracks(), a_video(), 5.0);

  int video_clips = 0;
  int audio_clips = 0;
  for (const core::Track& track : project.tracks) {
    for (const core::Clip& clip : track.clips) {
      (clip.kind == TrackKind::Video ? video_clips : audio_clips) += 1;
    }
  }
  EXPECT_EQ(video_clips, 1);
  EXPECT_EQ(audio_clips, 1) << "the file's audio stream should have come with it";
}

TEST(Import, PlacedClipsAreLinked) {
  // Video and its audio have to move together or they drift out of sync.
  const Project project = import_and_place(with_tracks(), a_video(), 0.0);

  for (const core::Track& track : project.tracks) {
    for (const core::Clip& clip : track.clips) {
      EXPECT_TRUE(clip.group_id.has_value()) << clip.id << " was placed unlinked";
    }
  }
}

TEST(Import, PlacingLandsWhereItWasAsked) {
  const Project project = import_and_place(with_tracks(), a_video(), 7.5);
  bool found = false;
  for (const core::Track& track : project.tracks) {
    for (const core::Clip& clip : track.clips) {
      if (clip.kind == TrackKind::Video) {
        EXPECT_DOUBLE_EQ(clip.start, 7.5);
        found = true;
      }
    }
  }
  EXPECT_TRUE(found);
}

TEST(Import, PlacingBeforeTheStartIsPulledForward) {
  const Project project = import_and_place(with_tracks(), a_video(), -10.0);
  for (const core::Track& track : project.tracks) {
    for (const core::Clip& clip : track.clips) EXPECT_GE(clip.start, 0.0);
  }
}

TEST(Import, PlacingTheSameFileTwiceReusesTheMediaButAddsClips) {
  Project project = import_and_place(with_tracks(), a_video(), 0.0);
  const std::size_t after_first = project.tracks[0].clips.size();
  project = import_and_place(std::move(project), a_video(), 60.0);

  EXPECT_EQ(project.media.size(), 1u);
  EXPECT_GT(project.tracks[0].clips.size(), after_first);
}

// ----------------------------------------------------------- extensions --

TEST(Import, StillsAreRecognisedByExtension) {
  // Some formats a decoder happily reports as a one-frame video are stills as
  // far as an editor is concerned, and treating them as video gives a clip one
  // frame long.
  for (const char* path : {"a.png", "b.JPG", "c.jpeg", "d.Tiff", "e.webp"}) {
    EXPECT_TRUE(looks_like_image(path)) << path;
  }
  for (const char* path : {"a.mp4", "b.wav", "c.txt", "d"}) {
    EXPECT_FALSE(looks_like_image(path)) << path;
  }
}

TEST(Import, MediaAreRecognisedByExtensionWhateverTheCase) {
  for (const char* path : {"a.MP4", "b.mov", "c.wav", "d.PNG", "e.mkv", "f.flac"}) {
    EXPECT_TRUE(looks_like_media(path)) << path;
  }
  for (const char* path : {"notes.txt", "project.cutline", "no-extension"}) {
    EXPECT_FALSE(looks_like_media(path)) << path;
  }
}

TEST(Import, AStillCarriesThatThroughToTheMedia) {
  MediaSource source = a_video("D:/stills/card.png");
  source.is_image = true;
  source.has_video = true;
  source.audio_stream_count = 0;

  const Project project = import_media(Project{}, source);
  EXPECT_TRUE(project.media[0].is_image);
}

// ------------------------------------------------- how long a still lasts --

TEST(Import, AStillIsGivenALengthWorthPlacing) {
  // The bug this exists for: libavformat calls a PNG a one-frame video at its
  // own default rate, and taking that at face value put a clip a fortieth of a
  // second long on the timeline — placed, counted as used, and invisible at
  // every zoom.
  MediaSource source = a_video("D:/stills/card.png");
  source.is_image = true;
  source.duration = 0.04;

  const Project project = import_media(Project{}, source);
  EXPECT_DOUBLE_EQ(project.media[0].duration, kStillLength);
}

TEST(Import, AnAnimatedStillKeepsItsOwnRunningTime) {
  // A GIF is a still by extension and not by nature. Overriding its length
  // would make it loop at the wrong speed.
  MediaSource source = a_video("D:/stills/spin.gif");
  source.is_image = true;
  source.is_animated = true;
  source.duration = 2.5;

  const Project project = import_media(Project{}, source);
  EXPECT_DOUBLE_EQ(project.media[0].duration, 2.5);
}

TEST(Import, VideoAndAudioAreLeftAlone) {
  EXPECT_DOUBLE_EQ(placement_duration(a_video()), 42.0);

  MediaSource sound = a_video("D:/audio/take.wav");
  sound.has_video = false;
  sound.duration = 7.5;
  EXPECT_DOUBLE_EQ(placement_duration(sound), 7.5);
}

TEST(Import, AStillPlacesAsSomethingYouCanSee) {
  // End to end, because the fault was only visible once a clip existed: the
  // duration reached the model correctly and the clip was still too short to
  // draw.
  MediaSource source = a_video("D:/stills/card.png");
  source.is_image = true;
  source.duration = 0.04;
  source.audio_stream_count = 0;

  const Project project = import_and_place(with_tracks(), source, 0.0);
  ASSERT_FALSE(project.tracks.front().clips.empty()) << "nothing was placed";
  const core::Clip& clip = project.tracks.front().clips.front();
  EXPECT_DOUBLE_EQ(clip.source_out - clip.source_in, kStillLength);
}

TEST(Import, RelinkingAStillGivesItAPlaceableLengthToo) {
  // Relinking rewrites the probed facts, so a still repointed at a moved file
  // would otherwise come back a fortieth of a second long.
  MediaSource source = a_video("D:/stills/card.png");
  source.is_image = true;
  source.duration = 0.04;

  std::string id;
  Project project = import_media(Project{}, source, &id);
  MediaSource moved = source;
  moved.path = "E:/stills/card.png";
  project = relink_media(std::move(project), id, moved);

  EXPECT_EQ(project.media[0].path, "E:/stills/card.png");
  EXPECT_DOUBLE_EQ(project.media[0].duration, kStillLength);
}

}  // namespace
}  // namespace cutline::editor
