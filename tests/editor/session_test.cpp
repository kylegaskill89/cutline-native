/// The editing session: document, history, selection, playhead.
///
/// The interesting behaviour is all about what happens to a selection when the
/// project moves underneath it — an undo that removes the clip you had
/// selected, a rejected drag that should not land in the history at all.

#include "cutline/editor/session.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

[[nodiscard]] Project sample_project() {
  Project project;
  project.sequence().fps = 30.0;
  project.media.push_back(Media{.id = "m1", .name = "wide.mp4", .duration = 60.0,
                                .has_video = true});

  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {
      Clip{.id = "c1", .media_id = "m1", .source_in = 0.0, .source_out = 5.0, .start = 0.0},
      Clip{.id = "c2", .media_id = "m1", .source_in = 5.0, .source_out = 12.0, .start = 5.0},
  };
  project.sequence().tracks.push_back(std::move(video));
  return project;
}

// -------------------------------------------------------------- documents --

TEST(Session, StartsWithNoHistory) {
  const Session session(sample_project());
  EXPECT_FALSE(session.can_undo());
  EXPECT_FALSE(session.can_redo());
  EXPECT_EQ(session.revision(), 0u);
}

TEST(Session, ApplyingAnEditRecordsIt) {
  Session session(sample_project());
  const std::vector<std::string> ids{"c1"};

  EXPECT_TRUE(session.apply(core::move_clips(session.project(), ids, 2.0)));
  EXPECT_TRUE(session.can_undo());
  EXPECT_EQ(session.revision(), 1u);
  EXPECT_DOUBLE_EQ(core::find_clip(session.project(), "c1")->start, 2.0);
}

TEST(Session, AnEditThatChangedNothingIsNotRecorded) {
  // Every core operation returns the project unchanged when it cannot apply.
  // Without this the undo stack fills with entries that undo nothing, and the
  // user presses undo three times before anything moves.
  Session session(sample_project());

  EXPECT_FALSE(session.apply(session.project()));
  EXPECT_FALSE(session.can_undo());
  EXPECT_EQ(session.revision(), 0u);

  const std::vector<std::string> nobody{"does-not-exist"};
  EXPECT_FALSE(session.apply(core::move_clips(session.project(), nobody, 5.0)));
  EXPECT_FALSE(session.can_undo());
}

TEST(Session, UndoAndRedoStepThroughEdits) {
  Session session(sample_project());
  const std::vector<std::string> ids{"c1"};
  session.apply(core::move_clips(session.project(), ids, 2.0));

  ASSERT_TRUE(session.undo());
  EXPECT_DOUBLE_EQ(core::find_clip(session.project(), "c1")->start, 0.0);
  EXPECT_TRUE(session.can_redo());

  ASSERT_TRUE(session.redo());
  EXPECT_DOUBLE_EQ(core::find_clip(session.project(), "c1")->start, 2.0);
}

TEST(Session, UndoWithNoHistoryDoesNothing) {
  Session session(sample_project());
  EXPECT_FALSE(session.undo());
  EXPECT_FALSE(session.redo());
  EXPECT_EQ(session.revision(), 0u);
}

TEST(Session, ResettingDropsTheHistoryWithTheDocument) {
  // The history belongs to the file being closed. Carrying it forward would
  // let undo walk a new project back into an old one's state.
  Session session(sample_project());
  const std::vector<std::string> ids{"c1"};
  session.apply(core::move_clips(session.project(), ids, 2.0));
  ASSERT_TRUE(session.can_undo());

  session.reset(sample_project());
  EXPECT_FALSE(session.can_undo());
  EXPECT_FALSE(session.can_redo());
  EXPECT_TRUE(session.selection().empty());
}

TEST(Session, TheRevisionMovesWheneverSomethingChanges) {
  Session session(sample_project());
  const std::uint64_t start = session.revision();

  session.select_one("c1");
  EXPECT_GT(session.revision(), start);

  const std::uint64_t after_select = session.revision();
  const std::vector<std::string> ids{"c1"};
  session.apply(core::move_clips(session.project(), ids, 1.0));
  EXPECT_GT(session.revision(), after_select);
}

// -------------------------------------------------------------- selection --

TEST(Session, SelectingKeepsOnlyClipsThatExist) {
  Session session(sample_project());
  session.select({"c1", "ghost", "c2"});

  EXPECT_EQ(session.selection().size(), 2u);
  EXPECT_TRUE(session.is_selected("c1"));
  EXPECT_FALSE(session.is_selected("ghost"));
}

TEST(Session, RemovingAClipDeselectsIt) {
  Session session(sample_project());
  session.select_one("c1");

  const std::vector<std::string> ids{"c1"};
  session.apply(core::remove_clips(session.project(), ids));

  EXPECT_FALSE(session.is_selected("c1"));
  EXPECT_TRUE(session.selection().empty());
}

TEST(Session, UndoingBackPastAClipDeselectsIt) {
  // The selection must never outlive what it points at. This is the case that
  // gets missed, because nothing was deleted — the clip simply stopped
  // existing when history stepped backwards.
  Session session(sample_project());

  const std::vector<std::string> ids{"c1"};
  session.apply(core::split_at(session.project(), 2.0, ids));

  // The split produced a second piece; select it, then undo the split away.
  const core::Track& track = session.project().sequence().tracks[0];
  ASSERT_GE(track.clips.size(), 3u);
  const std::string fresh = track.clips[1].id;
  session.select_one(fresh);
  ASSERT_TRUE(session.is_selected(fresh));

  ASSERT_TRUE(session.undo());
  EXPECT_FALSE(session.is_selected(fresh)) << "the selection outlived the clip";
}

TEST(Session, ClearingAnEmptySelectionIsNotAChange) {
  Session session(sample_project());
  const std::uint64_t before = session.revision();
  session.clear_selection();
  EXPECT_EQ(session.revision(), before);
}

TEST(Session, TheSelectedGroupBringsLinkedClipsAlong) {
  // Dragging a video clip has to bring its audio, or the two drift apart.
  Project project = sample_project();
  project.media.push_back(Media{.id = "m2", .name = "take.mp4", .duration = 30.0,
                                .has_video = true, .audio_stream_count = 1});
  project = core::place_media(std::move(project), "m2", 20.0);

  Session session(std::move(project));
  const core::Clip* placed = nullptr;
  for (const core::Track& track : session.project().sequence().tracks) {
    for (const core::Clip& clip : track.clips) {
      if (clip.media_id == "m2" && clip.kind == core::TrackKind::Video) placed = &clip;
    }
  }
  ASSERT_NE(placed, nullptr);
  ASSERT_TRUE(placed->group_id.has_value()) << "placement did not link the audio";

  session.select_one(placed->id);
  EXPECT_GT(session.selected_group().size(), 1u);
}

TEST(Session, TheSelectedGroupHasNoDuplicates) {
  Session session(sample_project());
  session.select({"c1", "c1", "c2"});
  const std::vector<std::string> group = session.selected_group();

  for (std::size_t i = 0; i < group.size(); ++i) {
    for (std::size_t j = i + 1; j < group.size(); ++j) {
      EXPECT_NE(group[i], group[j]);
    }
  }
}

// --------------------------------------------------------------- playhead --

TEST(Session, ThePlayheadLandsOnAFrame) {
  Session session(sample_project());
  session.set_playhead(1.0 / 30.0 * 4.5);

  const double frames = session.playhead() * 30.0;
  EXPECT_NEAR(frames, std::round(frames), 1e-9);
}

TEST(Session, ThePlayheadNeverGoesNegative) {
  Session session(sample_project());
  session.set_playhead(-10.0);
  EXPECT_DOUBLE_EQ(session.playhead(), 0.0);
}

}  // namespace
}  // namespace cutline::editor
