/// What the History panel calls each edit.
///
/// Most edits arrive as a new project with nothing said about what happened, so
/// these are named by comparing the two. What matters is that the name is the
/// one an editor would give the gesture, not merely the first field that
/// differs: a ripple delete moves everything after the gap and is still a
/// clear, and a razor adds a clip and is still a cut.

#include "cutline/editor/edit_names.hpp"

#include "cutline/editor/commands.hpp"
#include "cutline/editor/session.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/properties.hpp"
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

[[nodiscard]] Project sample() {
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
  project.sequence().tracks.push_back(Track{.id = "a1", .kind = TrackKind::Audio});
  return project;
}

[[nodiscard]] Clip& clip(Project& project, std::size_t index) {
  return project.sequence().tracks[0].clips[index];
}

TEST(EditNames, AClipThatOnlyChangedPlaceWasMoved) {
  const Project before = sample();
  const Project after = core::move_clips(before, std::vector<std::string>{"c2"}, 3.0);
  EXPECT_EQ(describe_edit(before, after), "Move");
}

TEST(EditNames, AClipWhoseSourceSpanChangedWasTrimmed) {
  const Project before = sample();
  Project after = before;
  clip(after, 0).source_out = 4.0;
  EXPECT_EQ(describe_edit(before, after), "Trim");
}

TEST(EditNames, AClipThatWentWasCleared) {
  const Project before = sample();
  const Project after = core::remove_clips(before, std::vector<std::string>{"c1"});
  EXPECT_EQ(describe_edit(before, after), "Clear");
}

TEST(EditNames, AClipCutInTwoIsARazorRatherThanAnAddition) {
  const Project before = sample();
  const Project after = core::split_at(before, 2.0, std::vector<std::string>{"c1"});
  ASSERT_NE(before, after);
  EXPECT_EQ(describe_edit(before, after), "Razor");
}

TEST(EditNames, AnEffectAddedIsNamedAsOne) {
  const Project before = sample();
  Project after = before;
  clip(after, 0).effects.push_back(core::ClipEffect{.type = "brightness"});
  EXPECT_EQ(describe_edit(before, after), "Add Effect");

  Project switched = after;
  clip(switched, 0).effects[0].enabled = false;
  EXPECT_EQ(describe_edit(after, switched), "Effect Enabled");
}

TEST(EditNames, AFileArrivingInThePoolIsAnImport) {
  const Project before = sample();
  Project after = before;
  after.media.push_back(Media{.id = "m2", .path = "C:/b.mp4", .name = "b.mp4"});
  EXPECT_EQ(describe_edit(before, after), "Import");

  Project titled = before;
  titled.media.push_back(Media{.id = "t1", .name = "Title", .is_text = true});
  EXPECT_EQ(describe_edit(before, titled), "New Title");
}

TEST(EditNames, ATrackAddedIsNamedForItsKind) {
  const Project before = sample();
  const Project after = core::add_audio_track(before);
  EXPECT_EQ(describe_edit(before, after), "Add Audio Track");
}

TEST(EditNames, ATrackSwitchIsNamedForTheSwitch) {
  const Project before = sample();
  Project after = before;
  after.sequence().tracks[1].muted = true;
  EXPECT_EQ(describe_edit(before, after), "Mute Track");
}

TEST(EditNames, AMarkerAddedIsToldFromAnInPointSet) {
  const Project before = sample();
  Project marked = before;
  marked.sequence().markers.push_back(core::Marker{.id = "k1", .time = 1.0});
  EXPECT_EQ(describe_edit(before, marked), "Add Marker");

  Project in = before;
  in.sequence().in_point = 1.0;
  EXPECT_EQ(describe_edit(before, in), "Mark In");
}

TEST(EditNames, AnEditNothingParticularDescribesIsStillNamed) {
  const Project before = sample();
  Project after = before;
  after.sequence().canvas_w = 1280;
  EXPECT_FALSE(describe_edit(before, after).empty());
}

// ---------------------------------------------------------- the session --

TEST(EditNames, TheSessionListsEachStepByName) {
  Session session(sample());
  session.apply(core::move_clips(session.project(), std::vector<std::string>{"c2"}, 3.0));
  session.apply(core::remove_clips(session.project(), std::vector<std::string>{"c1"}));

  EXPECT_EQ(session.undo_names(), (std::vector<std::string>{"Move", "Clear"}));
  EXPECT_TRUE(session.redo_names().empty());
}

TEST(EditNames, AStepUndoneKeepsItsNameToBeRedone) {
  Session session(sample());
  session.apply(core::move_clips(session.project(), std::vector<std::string>{"c2"}, 3.0));
  session.apply(core::remove_clips(session.project(), std::vector<std::string>{"c1"}));

  ASSERT_TRUE(session.undo());
  EXPECT_EQ(session.undo_names(), (std::vector<std::string>{"Move"}));
  EXPECT_EQ(session.redo_names(), (std::vector<std::string>{"Clear"}));

  ASSERT_TRUE(session.redo());
  EXPECT_EQ(session.undo_names(), (std::vector<std::string>{"Move", "Clear"}));
}

TEST(EditNames, ACommandIsListedUnderItsOwnName) {
  Session session(sample());
  session.select(std::vector<std::string>{"c1"});
  ASSERT_TRUE(run(session, Command::RippleDelete));
  EXPECT_EQ(session.undo_names(), (std::vector<std::string>{"Ripple Delete"}))
      << "a ripple moves every clip after the gap, and is still a ripple delete";
}

TEST(EditNames, ACommandThatChangedNothingLendsItsNameToNothing) {
  Session session(sample());
  ASSERT_FALSE(run(session, Command::RippleDelete)) << "nothing was selected";
  session.apply(core::move_clips(session.project(), std::vector<std::string>{"c2"}, 3.0));
  EXPECT_EQ(session.undo_names(), (std::vector<std::string>{"Move"}));
}

TEST(EditNames, SteppingToAPointUndoesOrRedoesTheWayThere) {
  Session session(sample());
  session.apply(core::move_clips(session.project(), std::vector<std::string>{"c2"}, 3.0));
  session.apply(core::remove_clips(session.project(), std::vector<std::string>{"c1"}));
  session.apply(core::add_audio_track(session.project()));

  EXPECT_TRUE(session.step_to(1));
  EXPECT_EQ(session.undo_names().size(), 1u);
  EXPECT_EQ(session.redo_names().size(), 2u);
  EXPECT_NE(core::find_clip(session.project(), "c1"), nullptr) << "the clear is undone";

  EXPECT_TRUE(session.step_to(3));
  EXPECT_EQ(session.undo_names().size(), 3u);
  EXPECT_FALSE(session.step_to(3)) << "already there";
}

TEST(EditNames, ClearingTheHistoryKeepsTheProject) {
  Session session(sample());
  session.apply(core::move_clips(session.project(), std::vector<std::string>{"c2"}, 3.0));
  const Project now = session.project();

  session.clear_history();
  EXPECT_FALSE(session.can_undo());
  EXPECT_EQ(session.project(), now);
}

}  // namespace
}  // namespace cutline::editor
