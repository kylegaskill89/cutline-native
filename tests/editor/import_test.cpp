/// Bringing files into a project.
///
/// The conversion is dull; the pool is not. Importing the same file twice must
/// not put it in twice, or a project accumulates a media entry per drag and the
/// browser fills with duplicates of one clip.

#include "cutline/editor/import.hpp"

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

}  // namespace
}  // namespace cutline::editor
