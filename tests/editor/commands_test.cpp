/// The editing commands.
///
/// The point of naming them is that a keystroke, a menu item and a toolbar
/// button are the same edit. These check the command, so all three are checked
/// at once, and check `can_run` agrees with `run` — a menu item that is enabled
/// and then does nothing is worse than one that was greyed out.

#include "cutline/editor/commands.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

constexpr double kFps = 30.0;

[[nodiscard]] Project sample_project() {
  Project project;
  project.fps = kFps;
  project.media.push_back(
      Media{.id = "m1", .name = "wide.mp4", .duration = 60.0, .has_video = true});

  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {
      Clip{.id = "c1", .media_id = "m1", .source_in = 0.0, .source_out = 5.0, .start = 0.0},
      Clip{.id = "c2", .media_id = "m1", .source_in = 5.0, .source_out = 12.0, .start = 5.0},
  };
  project.tracks.push_back(std::move(video));
  return project;
}

[[nodiscard]] std::size_t clip_count(const Project& project) {
  std::size_t count = 0;
  for (const Track& track : project.tracks) count += track.clips.size();
  return count;
}

constexpr std::array kAllCommands{
    Command::Split,     Command::Delete,        Command::RippleDelete, Command::NudgeLeft,
    Command::NudgeRight, Command::SelectAll,    Command::SelectNone,   Command::GoToStart,
    Command::GoToEnd,   Command::PreviousFrame, Command::NextFrame,    Command::Undo,
    Command::Redo,
};

// ----------------------------------------------------------- the contract --

TEST(Commands, EveryCommandHasAName) {
  for (const Command command : kAllCommands) {
    EXPECT_NE(to_string(command), "unknown");
  }
}

TEST(Commands, NamesAreDistinct) {
  for (std::size_t i = 0; i < kAllCommands.size(); ++i) {
    for (std::size_t j = i + 1; j < kAllCommands.size(); ++j) {
      EXPECT_NE(to_string(kAllCommands[i]), to_string(kAllCommands[j]));
    }
  }
}

TEST(Commands, WhatCannotRunDoesNothingWhenRunAnyway) {
  // A command may always be pressed hopefully. The two answers have to agree,
  // or a greyed-out menu item is greyed out for the wrong reason.
  Session session(sample_project());
  for (const Command command : kAllCommands) {
    if (can_run(session, command)) continue;
    EXPECT_FALSE(run(session, command)) << to_string(command) << " ran when it said it could not";
  }
}

TEST(Commands, NothingWorksOnAnEmptyProject) {
  Session session{};
  for (const Command command : kAllCommands) {
    if (command == Command::NextFrame || command == Command::GoToEnd) continue;
    EXPECT_FALSE(can_run(session, command)) << to_string(command);
    EXPECT_FALSE(run(session, command)) << to_string(command);
  }
}

// -------------------------------------------------------------- the razor --

TEST(Commands, SplitCutsEverythingUnderThePlayhead) {
  Session session(sample_project());
  session.set_playhead(2.0);

  ASSERT_TRUE(can_run(session, Command::Split));
  ASSERT_TRUE(run(session, Command::Split));
  EXPECT_EQ(clip_count(session.project()), 3u);
}

TEST(Commands, SplitOnAnEdgeIsNotACut) {
  // A cut exactly on a boundary would make a piece of zero length, which is
  // not a cut at all.
  Session session(sample_project());
  session.set_playhead(5.0);

  EXPECT_FALSE(can_run(session, Command::Split));
  EXPECT_FALSE(run(session, Command::Split));
}

TEST(Commands, SplitInEmptySpaceDoesNothing) {
  Session session(sample_project());
  session.set_playhead(40.0);
  EXPECT_FALSE(can_run(session, Command::Split));
}

TEST(Commands, SplitPrefersTheSelectionWhenThereIsOne) {
  Session session(sample_project());
  // Two clips would be under the playhead if they overlapped; here select the
  // one that is, and check the other is left alone.
  session.set_playhead(2.0);
  session.select_one("c1");

  ASSERT_TRUE(run(session, Command::Split));
  EXPECT_EQ(clip_count(session.project()), 3u);
  EXPECT_NE(core::find_clip(session.project(), "c2"), nullptr) << "the unselected clip was cut";
}

TEST(Commands, SplitIgnoresASelectionThePlayheadIsNowhereNear) {
  // Cutting a selected clip the playhead does not cross would be a surprise.
  Session session(sample_project());
  session.set_playhead(2.0);
  session.select_one("c2");

  EXPECT_FALSE(can_run(session, Command::Split));
  EXPECT_EQ(clip_count(session.project()), 2u);
}

// ------------------------------------------------------------- removing --

TEST(Commands, DeleteLeavesTheGap) {
  Session session(sample_project());
  session.select_one("c1");

  ASSERT_TRUE(run(session, Command::Delete));
  EXPECT_EQ(clip_count(session.project()), 1u);
  // The clip after it did not move, which is the difference from a ripple.
  EXPECT_DOUBLE_EQ(core::find_clip(session.project(), "c2")->start, 5.0);
}

TEST(Commands, RippleDeleteClosesTheGap) {
  Session session(sample_project());
  session.select_one("c1");

  ASSERT_TRUE(run(session, Command::RippleDelete));
  EXPECT_EQ(clip_count(session.project()), 1u);
  EXPECT_DOUBLE_EQ(core::find_clip(session.project(), "c2")->start, 0.0);
}

TEST(Commands, RemovingSomethingDeselectsIt) {
  Session session(sample_project());
  session.select_one("c1");
  run(session, Command::Delete);

  EXPECT_TRUE(session.selection().empty());
}

TEST(Commands, DeletingWithNothingSelectedDoesNothing) {
  Session session(sample_project());
  EXPECT_FALSE(can_run(session, Command::Delete));
  EXPECT_EQ(clip_count(session.project()), 2u);
}

// -------------------------------------------------------------- nudging --

TEST(Commands, NudgingMovesBySingleFrames) {
  Session session(sample_project());
  session.select_one("c2");

  ASSERT_TRUE(run(session, Command::NudgeRight));
  EXPECT_NEAR(core::find_clip(session.project(), "c2")->start, 5.0 + 1.0 / kFps, 1e-9);

  ASSERT_TRUE(run(session, Command::NudgeLeft));
  EXPECT_NEAR(core::find_clip(session.project(), "c2")->start, 5.0, 1e-9);
}

TEST(Commands, NudgingIntoTheStartIsRefusedByTheModel) {
  Session session(sample_project());
  session.select_one("c1");  // already at zero
  EXPECT_FALSE(run(session, Command::NudgeLeft));
}

TEST(Commands, EachNudgeIsItsOwnUndoStep) {
  Session session(sample_project());
  session.select_one("c2");
  run(session, Command::NudgeRight);
  run(session, Command::NudgeRight);

  ASSERT_TRUE(run(session, Command::Undo));
  EXPECT_NEAR(core::find_clip(session.project(), "c2")->start, 5.0 + 1.0 / kFps, 1e-9);
}

// ------------------------------------------------------------ selection --

TEST(Commands, SelectAllTakesEveryClip) {
  Session session(sample_project());
  ASSERT_TRUE(run(session, Command::SelectAll));
  EXPECT_EQ(session.selection().size(), 2u);
}

TEST(Commands, SelectNoneClearsIt) {
  Session session(sample_project());
  run(session, Command::SelectAll);

  ASSERT_TRUE(run(session, Command::SelectNone));
  EXPECT_TRUE(session.selection().empty());
  EXPECT_FALSE(run(session, Command::SelectNone)) << "clearing nothing is not a change";
}

// ------------------------------------------------------------- playhead --

TEST(Commands, TheFrameStepsMoveOneFrame) {
  Session session(sample_project());
  ASSERT_TRUE(run(session, Command::NextFrame));
  EXPECT_NEAR(session.playhead(), 1.0 / kFps, 1e-9);

  ASSERT_TRUE(run(session, Command::PreviousFrame));
  EXPECT_DOUBLE_EQ(session.playhead(), 0.0);
}

TEST(Commands, ThePlayheadStopsAtTheStart) {
  Session session(sample_project());
  EXPECT_FALSE(can_run(session, Command::PreviousFrame));
  EXPECT_FALSE(run(session, Command::PreviousFrame));
  EXPECT_DOUBLE_EQ(session.playhead(), 0.0);
}

TEST(Commands, GoToEndLandsOnTheLastFrameOfContent) {
  Session session(sample_project());
  ASSERT_TRUE(run(session, Command::GoToEnd));
  EXPECT_DOUBLE_EQ(session.playhead(), core::timeline_duration(session.project()));

  EXPECT_FALSE(run(session, Command::GoToEnd)) << "it was already there";
}

TEST(Commands, GoToStartComesBack) {
  Session session(sample_project());
  run(session, Command::GoToEnd);
  ASSERT_TRUE(run(session, Command::GoToStart));
  EXPECT_DOUBLE_EQ(session.playhead(), 0.0);
}

// -------------------------------------------------------------- history --

TEST(Commands, UndoAndRedoAreCommandsLikeAnyOther) {
  Session session(sample_project());
  session.select_one("c1");

  EXPECT_FALSE(can_run(session, Command::Undo));
  run(session, Command::Delete);
  EXPECT_TRUE(can_run(session, Command::Undo));

  ASSERT_TRUE(run(session, Command::Undo));
  EXPECT_EQ(clip_count(session.project()), 2u);
  ASSERT_TRUE(run(session, Command::Redo));
  EXPECT_EQ(clip_count(session.project()), 1u);
}

TEST(Commands, EveryEditIsUndoable) {
  // Whatever a command did, one undo has to put it back. A command that edits
  // the project without going through the session would fail this.
  for (const Command command :
       {Command::Split, Command::Delete, Command::RippleDelete, Command::NudgeRight}) {
    Session session(sample_project());
    session.set_playhead(2.0);
    session.select_one("c1");

    const Project before = session.project();
    if (!run(session, command)) continue;
    ASSERT_NE(session.project(), before) << to_string(command);

    ASSERT_TRUE(run(session, Command::Undo)) << to_string(command);
    EXPECT_EQ(session.project(), before) << to_string(command) << " did not undo cleanly";
  }
}

}  // namespace
}  // namespace cutline::editor
