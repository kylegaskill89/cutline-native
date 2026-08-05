/// The editing commands.
///
/// The point of naming them is that a keystroke, a menu item and a toolbar
/// button are the same edit. These check the command, so all three are checked
/// at once, and check `can_run` agrees with `run` — a menu item that is enabled
/// and then does nothing is worse than one that was greyed out.

#include "cutline/editor/commands.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/properties.hpp"
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
    Command::Split,         Command::Delete,        Command::RippleDelete,
    Command::NudgeLeft,     Command::NudgeRight,    Command::MarkIn,
    Command::MarkOut,       Command::ClearMarks,    Command::AddMarker,
    Command::ClearMarkers,  Command::NextMarker,    Command::PreviousMarker,
    Command::SelectAll,     Command::SelectNone,    Command::GoToStart,
    Command::GoToEnd,       Command::PreviousFrame, Command::NextFrame,
    Command::LinkClips,     Command::UnlinkClips,   Command::AddVideoTrack,
    Command::AddAudioTrack, Command::RemoveTrack,
    Command::Copy,          Command::Cut,           Command::Paste,
    Command::PasteInsert,   Command::Insert,        Command::Overwrite,
    Command::Undo,          Command::Redo,
    Command::TrimPreviousToPlayhead,
    Command::TrimNextToPlayhead,
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
    // Moving the playhead past the end is always allowed — a timeline does not
    // stop at its last clip — and adding a track to an empty sequence is how
    // one is started, so neither is idle here.
    if (command == Command::NextFrame || command == Command::GoToEnd ||
        command == Command::AddVideoTrack || command == Command::AddAudioTrack) {
      continue;
    }
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

// -------------------------------------------------- trimming to the playhead --

TEST(Commands, TrimPreviousTakesTheHeadAndClosesUp) {
  Session session(sample_project());  // c1 [0,5), c2 [5,12)
  session.set_playhead(2.0);

  ASSERT_TRUE(can_run(session, Command::TrimPreviousToPlayhead));
  ASSERT_TRUE(run(session, Command::TrimPreviousToPlayhead));

  const Clip* first = core::find_clip(session.project(), "c1");
  const Clip* second = core::find_clip(session.project(), "c2");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  // Two seconds went from the head, and the clip stayed where it was rather
  // than leaving a hole in front of itself.
  EXPECT_DOUBLE_EQ(first->start, 0.0);
  EXPECT_DOUBLE_EQ(core::clip_duration(*first), 3.0);
  EXPECT_DOUBLE_EQ(second->start, 3.0) << "and everything after came with it";
}

TEST(Commands, TrimNextTakesTheTailAndClosesUp) {
  Session session(sample_project());
  session.set_playhead(2.0);

  ASSERT_TRUE(run(session, Command::TrimNextToPlayhead));

  const Clip* first = core::find_clip(session.project(), "c1");
  const Clip* second = core::find_clip(session.project(), "c2");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_DOUBLE_EQ(core::clip_duration(*first), 2.0);
  EXPECT_DOUBLE_EQ(second->start, 2.0);
}

TEST(Commands, TrimmingNeedsSomethingUnderThePlayhead) {
  Session session(sample_project());
  session.set_playhead(40.0);
  EXPECT_FALSE(can_run(session, Command::TrimPreviousToPlayhead));
  EXPECT_FALSE(can_run(session, Command::TrimNextToPlayhead));
  EXPECT_FALSE(run(session, Command::TrimNextToPlayhead));
}

// An edit aimed at a lane: targeting is how the sequence is told which one.
TEST(Commands, TrimmingFollowsTheTargetedTrack) {
  Project project = sample_project();
  Track second{.id = "v2", .kind = TrackKind::Video};
  second.clips = {
      Clip{.id = "d1", .media_id = "m1", .source_in = 0.0, .source_out = 9.0, .start = 0.0}};
  // Stored above v1, and the only one targeted.
  project.tracks.insert(project.tracks.begin(), std::move(second));
  project.tracks[0].targeted = true;

  Session session(std::move(project));
  session.set_playhead(2.0);
  ASSERT_TRUE(run(session, Command::TrimNextToPlayhead));

  EXPECT_DOUBLE_EQ(core::clip_duration(*core::find_clip(session.project(), "d1")), 2.0);
  EXPECT_DOUBLE_EQ(core::clip_duration(*core::find_clip(session.project(), "c1")), 5.0)
      << "the untargeted lane kept its clip whole";
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

// ------------------------------------------------------------- in and out --

TEST(Marks, MarkInPutsTheInAtThePlayhead) {
  Session session(sample_project());
  session.set_playhead(3.0);

  ASSERT_TRUE(run(session, Command::MarkIn));
  ASSERT_TRUE(session.project().in_point.has_value());
  EXPECT_DOUBLE_EQ(*session.project().in_point, 3.0);
}

TEST(Marks, MarkingWhereTheMarkAlreadyIsTakesItAway) {
  // The same key undoing itself, which is how a mark is removed without a
  // third control that exists only to take one away.
  Session session(sample_project());
  session.set_playhead(3.0);
  ASSERT_TRUE(run(session, Command::MarkIn));

  ASSERT_TRUE(run(session, Command::MarkIn));
  EXPECT_FALSE(session.project().in_point.has_value());
}

TEST(Marks, MarkingSomewhereElseMovesIt) {
  Session session(sample_project());
  session.set_playhead(3.0);
  ASSERT_TRUE(run(session, Command::MarkIn));

  session.set_playhead(6.0);
  ASSERT_TRUE(run(session, Command::MarkIn));
  EXPECT_DOUBLE_EQ(*session.project().in_point, 6.0);
}

TEST(Marks, MarkOutIsTheSameTheOtherWayRound) {
  Session session(sample_project());
  session.set_playhead(8.0);

  ASSERT_TRUE(run(session, Command::MarkOut));
  EXPECT_DOUBLE_EQ(*session.project().out_point, 8.0);
  EXPECT_FALSE(session.project().in_point.has_value());
}

TEST(Marks, ClearingIsOnlyOfferedWhenThereIsSomethingToClear) {
  Session session(sample_project());
  EXPECT_FALSE(can_run(session, Command::ClearMarks));

  session.set_playhead(2.0);
  ASSERT_TRUE(run(session, Command::MarkIn));
  EXPECT_TRUE(can_run(session, Command::ClearMarks));

  ASSERT_TRUE(run(session, Command::ClearMarks));
  EXPECT_FALSE(core::has_marks(session.project()));
}

TEST(Marks, MarkingIsOfferedWhenThereIsSomethingToMark) {
  const Session session(sample_project());
  EXPECT_TRUE(can_run(session, Command::MarkIn));
  EXPECT_TRUE(can_run(session, Command::MarkOut));
}

TEST(Marks, AMarkCanStillBeRemovedFromASequenceWithNothingLeftInIt) {
  // Pressing the key where the mark already is removes it, so an empty
  // timeline that still carries a mark has to keep offering that — otherwise
  // the only way to take one away is greyed out.
  Session session(sample_project());
  session.set_playhead(2.0);
  ASSERT_TRUE(run(session, Command::MarkIn));

  session.select(std::vector<std::string>{"c1", "c2"});
  ASSERT_TRUE(run(session, Command::Delete));
  ASSERT_DOUBLE_EQ(core::timeline_duration(session.project()), 0.0);

  EXPECT_TRUE(can_run(session, Command::MarkIn));
  EXPECT_FALSE(can_run(session, Command::MarkOut)) << "there was never an out";
}

// --------------------------------------------------------------- markers --

TEST(Markers, AreDroppedAtThePlayhead) {
  Session session(sample_project());
  session.set_playhead(3.0);

  ASSERT_TRUE(run(session, Command::AddMarker));
  ASSERT_EQ(session.project().markers.size(), 1u);
  EXPECT_DOUBLE_EQ(session.project().markers.front().time, 3.0);
}

TEST(Markers, DroppingOneWhereOneAlreadyIsTakesItAway) {
  // The same toggle as the in and out points, and for the same reason: one
  // key, and no second control that exists only to undo it.
  Session session(sample_project());
  session.set_playhead(3.0);
  ASSERT_TRUE(run(session, Command::AddMarker));

  ASSERT_TRUE(run(session, Command::AddMarker));
  EXPECT_TRUE(session.project().markers.empty());
}

TEST(Markers, AreNotConfusedWithOneAFrameAway) {
  Session session(sample_project());
  session.set_playhead(3.0);
  ASSERT_TRUE(run(session, Command::AddMarker));

  session.set_playhead(3.0 + 1.0 / kFps);
  ASSERT_TRUE(run(session, Command::AddMarker));
  EXPECT_EQ(session.project().markers.size(), 2u) << "a frame apart is two markers";
}

TEST(Markers, KeepTheirOrderWhateverOrderTheyWereMadeIn) {
  Session session(sample_project());
  for (const double at : {6.0, 2.0, 4.0}) {
    session.set_playhead(at);
    ASSERT_TRUE(run(session, Command::AddMarker));
  }

  ASSERT_EQ(session.project().markers.size(), 3u);
  EXPECT_DOUBLE_EQ(session.project().markers[0].time, 2.0);
  EXPECT_DOUBLE_EQ(session.project().markers[1].time, 4.0);
  EXPECT_DOUBLE_EQ(session.project().markers[2].time, 6.0);
}

TEST(Markers, TheJumpsWalkFromWhereThePlayheadIs) {
  Session session(sample_project());
  for (const double at : {2.0, 4.0, 6.0}) {
    session.set_playhead(at);
    ASSERT_TRUE(run(session, Command::AddMarker));
  }

  session.set_playhead(0.0);
  ASSERT_TRUE(run(session, Command::NextMarker));
  EXPECT_DOUBLE_EQ(session.playhead(), 2.0);
  ASSERT_TRUE(run(session, Command::NextMarker));
  EXPECT_DOUBLE_EQ(session.playhead(), 4.0);

  ASSERT_TRUE(run(session, Command::PreviousMarker));
  EXPECT_DOUBLE_EQ(session.playhead(), 2.0);
}

TEST(Markers, TheJumpsStopAtTheEnds) {
  Session session(sample_project());
  session.set_playhead(2.0);
  ASSERT_TRUE(run(session, Command::AddMarker));

  EXPECT_FALSE(can_run(session, Command::NextMarker)) << "sitting on the last one";
  EXPECT_FALSE(run(session, Command::NextMarker));

  session.set_playhead(0.0);
  EXPECT_FALSE(can_run(session, Command::PreviousMarker));
}

TEST(Markers, ClearingIsOnlyOfferedWhenThereAreSome) {
  Session session(sample_project());
  EXPECT_FALSE(can_run(session, Command::ClearMarkers));

  session.set_playhead(2.0);
  ASSERT_TRUE(run(session, Command::AddMarker));
  EXPECT_TRUE(can_run(session, Command::ClearMarkers));

  ASSERT_TRUE(run(session, Command::ClearMarkers));
  EXPECT_TRUE(session.project().markers.empty());
}

TEST(Markers, AJumpIsNotAnEditAndDoesNotGoOnTheUndoStack) {
  // Moving the playhead changes nothing about the document. An undo entry per
  // jump would bury the edit somebody actually wants back.
  Session session(sample_project());
  session.set_playhead(2.0);
  ASSERT_TRUE(run(session, Command::AddMarker));

  const Project before = session.project();
  session.set_playhead(0.0);
  ASSERT_TRUE(run(session, Command::NextMarker));
  EXPECT_EQ(session.project(), before);
}

// ------------------------------------------------------------- the linking --

TEST(Commands, LinkingNeedsTwoClips) {
  Session session(sample_project());
  EXPECT_FALSE(can_run(session, Command::LinkClips)) << "nothing selected";

  session.select({"c1"});
  EXPECT_FALSE(can_run(session, Command::LinkClips)) << "one clip is not a group";

  session.select({"c1", "c2"});
  EXPECT_TRUE(can_run(session, Command::LinkClips));
}

TEST(Commands, LinkedClipsMoveTogether) {
  Session session(sample_project());
  session.select({"c1", "c2"});
  ASSERT_TRUE(run(session, Command::LinkClips));

  // The proof that the link took: an edit aimed at one now reaches both, which
  // is what `selected_group` is for and what every operation goes through.
  session.select({"c1"});
  const std::vector<std::string> group = session.selected_group();
  EXPECT_EQ(group.size(), 2u);
}

TEST(Commands, ThereIsNothingToUnlinkUntilSomethingIsLinked) {
  Session session(sample_project());
  session.select({"c1", "c2"});
  EXPECT_FALSE(can_run(session, Command::UnlinkClips));

  ASSERT_TRUE(run(session, Command::LinkClips));
  EXPECT_TRUE(can_run(session, Command::UnlinkClips));
}

// Unlinking one clip out of a group takes the whole group apart. Leaving the
// others tied to each other is not what anybody means by removing a link.
TEST(Commands, UnlinkingOneMemberUndoesTheWholeGroup) {
  Session session(sample_project());
  session.select({"c1", "c2"});
  ASSERT_TRUE(run(session, Command::LinkClips));

  session.select({"c1"});
  ASSERT_TRUE(run(session, Command::UnlinkClips));

  EXPECT_FALSE(core::find_clip(session.project(), "c1")->group_id.has_value());
  EXPECT_FALSE(core::find_clip(session.project(), "c2")->group_id.has_value());
}

TEST(Commands, LinkingIsOneUndoStep) {
  Session session(sample_project());
  const Project before = session.project();
  session.select({"c1", "c2"});
  ASSERT_TRUE(run(session, Command::LinkClips));

  ASSERT_TRUE(session.undo());
  EXPECT_EQ(session.project(), before);
}

// -------------------------------------------------------------- the tracks --

TEST(Commands, AVideoTrackGoesOnTopAndAnAudioTrackAtTheBottom) {
  Session session(sample_project());
  ASSERT_TRUE(run(session, Command::AddVideoTrack));
  ASSERT_TRUE(run(session, Command::AddAudioTrack));

  const std::vector<Track>& tracks = session.project().tracks;
  ASSERT_EQ(tracks.size(), 3u);
  EXPECT_EQ(tracks.front().kind, TrackKind::Video) << "the new video layer is the topmost";
  EXPECT_EQ(tracks.back().kind, TrackKind::Audio) << "the new lane is the last one";
}

TEST(Commands, ATrackCanBeAddedToAnEmptySequence) {
  // Which is how one is started, so this must not need a clip to exist first.
  Session session{};
  EXPECT_TRUE(can_run(session, Command::AddVideoTrack));
  EXPECT_TRUE(run(session, Command::AddVideoTrack));
  EXPECT_EQ(session.project().tracks.size(), 1u);
}

TEST(Commands, RemovingATrackNeedsToKnowWhichOne) {
  Session session(sample_project());
  EXPECT_FALSE(can_run(session, Command::RemoveTrack)) << "nothing says which track";

  session.select({"c1"});
  EXPECT_TRUE(can_run(session, Command::RemoveTrack));
}

TEST(Commands, RemovingATrackTakesItsClipsWithIt) {
  Session session(sample_project());
  session.select({"c1"});
  ASSERT_TRUE(run(session, Command::RemoveTrack));

  EXPECT_TRUE(session.project().tracks.empty());
  EXPECT_EQ(clip_count(session.project()), 0u);
}

// A selection spanning two tracks has no single answer, and deleting a track is
// not something to guess at.
TEST(Commands, ASelectionAcrossTwoTracksNamesNoTrack) {
  Project project = sample_project();
  Track second{.id = "v2", .kind = TrackKind::Video};
  second.clips = {Clip{.id = "c3", .media_id = "m1", .source_in = 0.0, .source_out = 4.0,
                       .start = 0.0}};
  project.tracks.push_back(std::move(second));

  Session session(std::move(project));
  session.select({"c1", "c3"});
  EXPECT_FALSE(can_run(session, Command::RemoveTrack));
  EXPECT_FALSE(run(session, Command::RemoveTrack));
}

TEST(Commands, RemovingATrackDropsTheSelectionThatNamedIt) {
  // The clips are gone, so a selection still holding their ids would be a
  // reference to nothing — which every later command would have to check for.
  Session session(sample_project());
  session.select({"c1"});
  ASSERT_TRUE(run(session, Command::RemoveTrack));
  EXPECT_TRUE(session.selection().empty());
}

// ------------------------------------------------------------- the clipboard --

TEST(Commands, NothingIsOfferedToCopyUntilSomethingIsSelected) {
  Session session(sample_project());
  EXPECT_FALSE(can_run(session, Command::Copy));
  EXPECT_FALSE(can_run(session, Command::Cut));
  session.select({"c1"});
  EXPECT_TRUE(can_run(session, Command::Copy));
}

TEST(Commands, PasteIsOfferedOnlyOnceSomethingHasBeenCopied) {
  Session session(sample_project());
  session.select({"c1"});
  EXPECT_FALSE(can_run(session, Command::Paste));
  EXPECT_FALSE(can_run(session, Command::PasteInsert));

  ASSERT_TRUE(run(session, Command::Copy));
  EXPECT_TRUE(can_run(session, Command::Paste));
  EXPECT_TRUE(can_run(session, Command::PasteInsert));
}

TEST(Commands, CopyingLeavesTheProjectAlone) {
  Session session(sample_project());
  session.select({"c1"});
  const Project before = session.project();

  ASSERT_TRUE(run(session, Command::Copy));
  EXPECT_EQ(session.project(), before);
  EXPECT_FALSE(session.can_undo()) << "copying is not an edit and has nothing to undo";
}

TEST(Commands, PastingPutsTheCopyAtThePlayhead) {
  Session session(sample_project());
  session.select({"c1"});
  ASSERT_TRUE(run(session, Command::Copy));

  session.set_playhead(20.0);
  ASSERT_TRUE(run(session, Command::Paste));

  EXPECT_EQ(clip_count(session.project()), 3u);
  const Track& video = session.project().tracks[0];
  EXPECT_DOUBLE_EQ(video.clips.back().start, 20.0);
}

TEST(Commands, CutTakesTheClipAndLeavesTheGap) {
  Session session(sample_project());
  session.select({"c1"});
  ASSERT_TRUE(run(session, Command::Cut));

  EXPECT_EQ(clip_count(session.project()), 1u);
  // c2 has not moved: cutting lifts, and closing the gap is Ripple Delete.
  EXPECT_DOUBLE_EQ(session.project().tracks[0].clips[0].start, 5.0);
  EXPECT_FALSE(session.clipboard().empty());
}

TEST(Commands, WhatWasCutCanBePastedBack) {
  Session session(sample_project());
  session.select({"c1"});
  ASSERT_TRUE(run(session, Command::Cut));

  session.set_playhead(0.0);
  ASSERT_TRUE(run(session, Command::Paste));
  EXPECT_EQ(clip_count(session.project()), 2u);
  EXPECT_DOUBLE_EQ(session.project().tracks[0].clips[0].start, 0.0);
}

TEST(Commands, WhatWasPastedIsWhatIsSelectedAfterwards) {
  Session session(sample_project());
  session.select({"c1"});
  ASSERT_TRUE(run(session, Command::Copy));

  session.set_playhead(20.0);
  ASSERT_TRUE(run(session, Command::Paste));

  ASSERT_EQ(session.selection().size(), 1u);
  EXPECT_NE(session.selection()[0], "c1");
  EXPECT_DOUBLE_EQ(core::find_clip(session.project(), session.selection()[0])->start, 20.0);
}

TEST(Commands, PasteInsertMovesWhatWasAlreadyThere) {
  Session session(sample_project());
  session.select({"c1"});  // five seconds long
  ASSERT_TRUE(run(session, Command::Copy));

  session.set_playhead(5.0);
  ASSERT_TRUE(run(session, Command::PasteInsert));

  const Track& video = session.project().tracks[0];
  ASSERT_EQ(video.clips.size(), 3u);
  EXPECT_DOUBLE_EQ(video.clips[1].start, 5.0);
  EXPECT_DOUBLE_EQ(video.clips[2].start, 10.0) << "c2 was pushed along";
}

TEST(Commands, TheClipboardSurvivesOpeningAnotherDocument) {
  // What makes copying between two projects work, and the reason the clipboard
  // is not part of the document.
  Session session(sample_project());
  session.select({"c1"});
  ASSERT_TRUE(run(session, Command::Copy));

  session.reset(sample_project());
  EXPECT_TRUE(can_run(session, Command::Paste));
}

// ------------------------------------------------- targeting and placing --

/// The sample project with a second video lane, an audio lane, and a media in
/// the pool that has not been placed.
[[nodiscard]] Project targetable_project() {
  Project project = sample_project();
  project.media.push_back(
      Media{.id = "m2", .name = "insert.mp4", .duration = 8.0, .has_video = true});
  project.tracks.insert(project.tracks.begin(), Track{.id = "v2", .kind = TrackKind::Video});
  project.tracks.push_back(Track{.id = "a1", .kind = TrackKind::Audio});
  return project;
}

TEST(Commands, PlacingNeedsSomethingToPlace) {
  Session session(targetable_project());
  EXPECT_FALSE(can_run(session, Command::Insert));
  EXPECT_FALSE(can_run(session, Command::Overwrite));
}

TEST(Commands, PlacingNeedsATargetedTrack) {
  // A sequence with nothing targeted has no answer to *where*, and guessing is
  // how an edit lands on a track nobody was looking at.
  Session session(targetable_project());
  session.set_source_media("m2");
  EXPECT_FALSE(can_run(session, Command::Insert));

  Project project = session.project();
  project.tracks[0].targeted = true;
  session.reset(std::move(project));
  session.set_source_media("m2");
  EXPECT_TRUE(can_run(session, Command::Insert));
}

TEST(Commands, AnOverwriteLandsOnTheTargetedTrack) {
  Project project = targetable_project();
  project.tracks[0].targeted = true;  // v2, above the one holding the clips
  Session session(std::move(project));
  session.set_source_media("m2");
  session.set_playhead(2.0);

  ASSERT_TRUE(run(session, Command::Overwrite));
  EXPECT_EQ(session.project().tracks[0].clips.size(), 1u);
  EXPECT_DOUBLE_EQ(session.project().tracks[0].clips[0].start, 2.0);
  // v1's clips are untouched: the overwrite carved the track it was aimed at.
  EXPECT_EQ(session.project().tracks[1].clips.size(), 2u);
}

TEST(Commands, AnInsertRipplesWhatWasAlreadyThere) {
  Project project = targetable_project();
  project.tracks[1].targeted = true;  // v1, which holds c1 and c2
  Session session(std::move(project));
  session.set_source_media("m2");
  session.set_playhead(5.0);

  ASSERT_TRUE(run(session, Command::Insert));

  const Track& lane = session.project().tracks[1];
  ASSERT_EQ(lane.clips.size(), 3u);
  EXPECT_DOUBLE_EQ(lane.clips[1].start, 5.0);
  EXPECT_EQ(lane.clips[1].media_id, "m2");
  EXPECT_DOUBLE_EQ(lane.clips[2].start, 13.0) << "c2 was pushed along by eight seconds";
}

TEST(Commands, AnInsertPlacesOnlyTheMarkedPartOfTheSource) {
  // The other half of three-point editing. `PlacementRange` had been honoured
  // by every placement operation and set by nothing, so however a source was
  // marked the whole of it went down.
  Project project = targetable_project();
  project.tracks[1].targeted = true;
  project = core::set_source_in_point(std::move(project), "m2", 2.0);
  project = core::set_source_out_point(std::move(project), "m2", 5.0);
  Session session(std::move(project));
  session.set_source_media("m2");
  session.set_playhead(5.0);

  ASSERT_TRUE(run(session, Command::Insert));

  const Track& lane = session.project().tracks[1];
  ASSERT_EQ(lane.clips.size(), 3u);
  EXPECT_DOUBLE_EQ(lane.clips[1].source_in, 2.0);
  EXPECT_DOUBLE_EQ(lane.clips[1].source_out, 5.0);
  EXPECT_DOUBLE_EQ(lane.clips[2].start, 8.0) << "rippled by three seconds, not eight";
}

TEST(Commands, AnOverwriteTakesTheMarkedPartToo) {
  Project project = targetable_project();
  project.tracks[0].targeted = true;
  project = core::set_source_out_point(std::move(project), "m2", 3.0);
  Session session(std::move(project));
  session.set_source_media("m2");
  session.set_playhead(2.0);

  ASSERT_TRUE(run(session, Command::Overwrite));
  const Clip& placed = session.project().tracks[0].clips.at(0);
  EXPECT_DOUBLE_EQ(placed.source_in, 0.0);
  EXPECT_DOUBLE_EQ(placed.source_out, 3.0);
}

TEST(Commands, AnOverwriteLeavesTheSequenceTheLengthItWas) {
  Project project = targetable_project();
  project.tracks[1].targeted = true;
  Session session(std::move(project));
  session.set_source_media("m2");
  session.set_playhead(0.0);

  const double was = core::timeline_duration(session.project());
  ASSERT_TRUE(run(session, Command::Overwrite));
  EXPECT_DOUBLE_EQ(core::timeline_duration(session.project()), was);
}

TEST(Commands, SourceThatIsNoLongerInThePoolIsNotOffered) {
  Project project = targetable_project();
  project.tracks[0].targeted = true;
  Session session(std::move(project));
  session.set_source_media("gone");
  EXPECT_FALSE(can_run(session, Command::Insert));
  EXPECT_FALSE(run(session, Command::Insert));
}

}  // namespace
}  // namespace cutline::editor
