/// The join between a project and the timeline that draws it.
///
/// Both halves are already tested on their own. What is left is whether they
/// agree: that a clip becomes a block in the right place, that a block dragged
/// somewhere becomes the right edit, and that the numbering on the track
/// headers is the one an editor expects.
///
/// **Hold what `timeline_model` returns by value.** It returns a model, and a
/// reference bound to a *member* of a temporary is not lifetime-extended — so
/// `const auto& x = timeline_model(p).tracks[0].something;` reads freed memory.
/// It has caught three tests in this suite so far, each time passing until an
/// unrelated change moved the heap under it.

#include "cutline/editor/timeline_binding.hpp"

#include "cutline/editor/transitions.hpp"

#include "cutline/core/animate.hpp"
#include "cutline/core/edit.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include <string>
#include <string_view>
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
  project.media = {
      Media{.id = "m1", .name = "wide.mp4", .duration = 60.0, .has_video = true},
      Media{.id = "m2", .name = "music.wav", .duration = 60.0, .audio_stream_count = 1},
  };

  Track upper{.id = "v2", .kind = TrackKind::Video};
  upper.clips = {Clip{.id = "title", .media_id = "m1", .source_in = 0.0, .source_out = 3.0,
                      .start = 4.0}};

  Track lower{.id = "v1", .kind = TrackKind::Video};
  lower.clips = {
      Clip{.id = "c1", .media_id = "m1", .source_in = 0.0, .source_out = 5.0, .start = 0.0},
      Clip{.id = "c2", .media_id = "m1", .source_in = 5.0, .source_out = 12.0, .start = 5.0},
  };

  Track audio{.id = "a1", .kind = TrackKind::Audio};
  audio.clips = {Clip{.id = "a", .media_id = "m2", .kind = TrackKind::Audio, .source_in = 0.0,
                      .source_out = 12.0, .start = 0.0}};

  project.sequence().tracks = {std::move(upper), std::move(lower), std::move(audio)};
  return project;
}

// ------------------------------------------------------------ the drawing --

TEST(Binding, EveryTrackBecomesARow) {
  const ui::TimelineModel model = timeline_model(sample_project());

  ASSERT_EQ(model.tracks.size(), 3u);
  EXPECT_EQ(model.tracks[0].id, "v2");
  EXPECT_FALSE(model.tracks[0].audio);
  EXPECT_TRUE(model.tracks[2].audio);
  EXPECT_DOUBLE_EQ(model.fps, 30.0);
}

TEST(Binding, ClipsBecomeBlocksAtTheirOwnTimes) {
  const ui::TimelineModel model = timeline_model(sample_project());
  const ui::TimelineBlock& block = model.tracks[1].blocks[1];

  EXPECT_EQ(block.id, "c2");
  EXPECT_DOUBLE_EQ(block.start, 5.0);
  EXPECT_DOUBLE_EQ(block.end, 12.0);
  EXPECT_EQ(block.label, "wide.mp4");
}

TEST(Binding, ABlockCarriesItsClipIdBack) {
  // Which is the whole reason the block has an id: it is how a drag finds its
  // way to the project without the timeline knowing a project exists.
  const ui::TimelineModel model = timeline_model(sample_project());
  EXPECT_EQ(block_clip_id(model, ui::BlockRef{1, 0}), "c1");
  EXPECT_FALSE(block_clip_id(model, ui::BlockRef{9, 0}).has_value());
  EXPECT_FALSE(block_clip_id(model, ui::BlockRef{1, 9}).has_value());
}

TEST(Binding, TheSelectionIsMarkedOnTheBlocks) {
  const std::vector<std::string> selection{"c2"};
  const ui::TimelineModel model = timeline_model(sample_project(), selection);

  EXPECT_FALSE(model.tracks[1].blocks[0].selected);
  EXPECT_TRUE(model.tracks[1].blocks[1].selected);
}

TEST(Binding, TheDurationIsTheProjectsOwn) {
  const Project project = sample_project();
  const ui::TimelineModel model = timeline_model(project);
  EXPECT_DOUBLE_EQ(model.duration, core::timeline_duration(project));
}

TEST(Binding, ABlockCarriesTheTimesItIsAnimatedAt) {
  Project project = sample_project();
  project = core::set_keyframe(std::move(project), "c1", core::AnimProp::Opacity, 1.0, 0.5);
  project = core::set_keyframe(std::move(project), "c1", core::AnimProp::Opacity, 3.0, 1.0);
  project = core::add_clip_effect(std::move(project), "c1", "blur", {{"amount", 5.0}});
  project = core::set_effect_keyframe(std::move(project), "c1", 0, "amount", 2.0, 10.0);

  // The model by value: a reference bound to a *member* of a temporary is not
  // lifetime-extended, and reads whatever is left at the semicolon.
  const ui::TimelineModel model = timeline_model(project);
  const ui::TimelineBlock& block = model.tracks[1].blocks[0];
  ASSERT_EQ(block.keyframes.size(), 3u);
  EXPECT_DOUBLE_EQ(block.keyframes[0], 1.0);
  EXPECT_DOUBLE_EQ(block.keyframes[1], 2.0) << "an effect's keyframes count too";
  EXPECT_DOUBLE_EQ(block.keyframes[2], 3.0) << "and they come out in time order";
}

TEST(Binding, TwoPropertiesKeyedTogetherAreOneMark) {
  // The block is a few pixels tall; two diamonds drawn on top of each other
  // are one diamond with the drawing done twice.
  Project project = sample_project();
  project = core::set_keyframe(std::move(project), "c1", core::AnimProp::X, 2.0, 0.25);
  project = core::set_keyframe(std::move(project), "c1", core::AnimProp::Y, 2.0, 0.75);

  EXPECT_EQ(timeline_model(project).tracks[1].blocks[0].keyframes.size(), 1u);
}

TEST(Binding, AClipWithNoAnimationHasNoMarks) {
  EXPECT_TRUE(timeline_model(sample_project()).tracks[1].blocks[0].keyframes.empty());
}

TEST(Binding, AClipWithNoMediaStillDraws) {
  // A project referring to media that is gone must open and be visible, or
  // there is no way to find the broken clip and fix it.
  Project project = sample_project();
  project.media.clear();

  const ui::TimelineModel model = timeline_model(project);
  EXPECT_FALSE(model.tracks[1].blocks[0].label.empty());
}

TEST(Binding, AnEmptyProjectMakesAnEmptyTimeline) {
  const ui::TimelineModel model = timeline_model(Project{});
  EXPECT_TRUE(model.tracks.empty());
  EXPECT_DOUBLE_EQ(model.content_duration(), 0.0);
}

// ------------------------------------------------------------- the labels --

TEST(Binding, VideoCountsUpFromTheBottomAndAudioDownFromTheTop) {
  // V1 is the base layer and A1 is the first lane, so the two numberings meet
  // in the middle. Numbering video from the top would put V1 on the overlay.
  const Project project = sample_project();

  EXPECT_EQ(default_track_label(project, 0), "V2");
  EXPECT_EQ(default_track_label(project, 1), "V1");
  EXPECT_EQ(default_track_label(project, 2), "A1");
}

TEST(Binding, AGivenLabelIsUsedAsIs) {
  Project project = sample_project();
  project.sequence().tracks[0].label = "Titles";
  EXPECT_EQ(default_track_label(project, 0), "Titles");
}

TEST(Binding, AnIndexPastTheEndIsHarmless) {
  EXPECT_TRUE(default_track_label(sample_project(), 99).empty());
}

// ----------------------------------------------------------- the mute flag --

TEST(Binding, AHiddenVideoTrackReadsAsMuted) {
  Project project = sample_project();
  project.sequence().tracks[0].hidden = true;
  EXPECT_TRUE(timeline_model(project).tracks[0].muted);
}

TEST(Binding, SoloingOneAudioTrackMutesTheOthers) {
  // Which is why this is not simply `track.muted`: an audio track can be
  // silenced by something happening on a different track entirely.
  Project project = sample_project();
  Track second{.id = "a2", .kind = TrackKind::Audio, .solo = true};
  project.sequence().tracks.push_back(std::move(second));

  const ui::TimelineModel model = timeline_model(project);
  EXPECT_TRUE(model.tracks[2].muted) << "a1 should be silenced by a2's solo";
  EXPECT_FALSE(model.tracks[3].muted);
}

// -------------------------------------------------------------- the edits --

/// A gesture that left a block spanning `start` to `end`.
///
/// Most modes report only that, and reading it as two numbers is clearer here
/// than assembling a struct nine times.
[[nodiscard]] Project drag(const Project& project, std::string_view clip_id, ui::DragMode mode,
                           double start, double end) {
  return apply_timeline_edit(
      project, clip_id,
      ui::TimelineEdit{.mode = mode,
                       .result = ui::TimelineBlock{.start = start, .end = end}});
}

/// The same, with a selection the gesture should carry along.
[[nodiscard]] Project drag_selected(const Project& project, std::string_view clip_id,
                                    std::span<const std::string> selection, double start,
                                    double end) {
  return apply_timeline_edit(
      project, clip_id,
      ui::TimelineEdit{.mode = ui::DragMode::Move,
                       .result = ui::TimelineBlock{.start = start, .end = end}},
      selection);
}

TEST(Binding, AMoveThatChangedLaneMovesTheClipBetweenTracks) {
  core::Project p = sample_project();
  const core::Project after = apply_timeline_edit(
      p, "c1",
      ui::TimelineEdit{.mode = ui::DragMode::Move,
                       .result = ui::TimelineBlock{.start = 0.0, .end = 5.0},
                       .lanes = -1});

  // v2 is the track above v1 in storage order, so one lane up is -1.
  EXPECT_TRUE(std::ranges::none_of(after.sequence().tracks[1].clips,
                                   [](const core::Clip& c) { return c.id == "c1"; }));
  EXPECT_TRUE(std::ranges::any_of(after.sequence().tracks[0].clips,
                                  [](const core::Clip& c) { return c.id == "c1"; }));
}

TEST(Binding, AMoveAlongTheTrackLeavesTheLaneAlone) {
  const core::Project after = apply_timeline_edit(
      sample_project(), "c1",
      ui::TimelineEdit{.mode = ui::DragMode::Move,
                       .result = ui::TimelineBlock{.start = 8.0, .end = 13.0}});

  EXPECT_TRUE(std::ranges::any_of(after.sequence().tracks[1].clips,
                                  [](const core::Clip& c) { return c.id == "c1"; }));
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c1")->start, 8.0);
}

TEST(Binding, ARippledEdgeUsesTheEdgeItWasDraggedTo) {
  // `result` cannot say where a rippled head went, so the edit carries it in
  // `at` and this is what reads it.
  const Project after = apply_timeline_edit(
      sample_project(), "c1",
      ui::TimelineEdit{.mode = ui::DragMode::RippleEnd,
                       .result = ui::TimelineBlock{.start = 0.0, .end = 3.0},
                       .at = 3.0});

  EXPECT_DOUBLE_EQ(core::clip_end(*core::find_clip(after, "c1")), 3.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->start, 3.0) << "c2 closed the gap";
}

TEST(Binding, ARolledJoinMovesBothSidesOfIt) {
  const Project after = apply_timeline_edit(
      sample_project(), "c1",
      ui::TimelineEdit{.mode = ui::DragMode::RollEnd,
                       .result = ui::TimelineBlock{.start = 0.0, .end = 7.0},
                       .at = 7.0});

  EXPECT_DOUBLE_EQ(core::clip_end(*core::find_clip(after, "c1")), 7.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->start, 7.0);
  EXPECT_DOUBLE_EQ(core::clip_end(*core::find_clip(after, "c2")), 12.0)
      << "the far end of the pair did not move";
}

TEST(Binding, MovingABlockMovesTheClip) {
  const Project before = sample_project();
  const Project after = drag(before, "title", ui::DragMode::Move, 9.0, 12.0);

  EXPECT_DOUBLE_EQ(core::find_clip(after, "title")->start, 9.0);
  EXPECT_DOUBLE_EQ(core::clip_duration(*core::find_clip(after, "title")), 3.0);
}

TEST(Binding, MovingGoesThroughTheModelsOwnClamp) {
  // Not by assignment: the model owns the rule that nothing starts before
  // zero, and a second opinion about it here would eventually disagree.
  const Project after =
      drag(sample_project(), "title", ui::DragMode::Move, -50.0, -47.0);
  EXPECT_GE(core::find_clip(after, "title")->start, 0.0);
}

TEST(Binding, TrimmingTheStartMovesOnlyThatEdge) {
  const Project after =
      drag(sample_project(), "c2", ui::DragMode::TrimStart, 7.0, 12.0);
  const core::Clip* clip = core::find_clip(after, "c2");

  EXPECT_DOUBLE_EQ(clip->start, 7.0);
  EXPECT_DOUBLE_EQ(core::clip_end(*clip), 12.0);
}

TEST(Binding, TrimmingTheEndMovesOnlyThatEdge) {
  const Project after =
      drag(sample_project(), "c2", ui::DragMode::TrimEnd, 5.0, 10.0);
  const core::Clip* clip = core::find_clip(after, "c2");

  EXPECT_DOUBLE_EQ(clip->start, 5.0);
  EXPECT_DOUBLE_EQ(core::clip_end(*clip), 10.0);
}

TEST(Binding, AnUnknownClipChangesNothing) {
  const Project before = sample_project();
  EXPECT_EQ(drag(before, "ghost", ui::DragMode::Move, 3.0, 5.0), before);
}

TEST(Binding, ScrubbingAndNothingAreNotEdits) {
  const Project before = sample_project();
  EXPECT_EQ(drag(before, "c1", ui::DragMode::Scrub, 3.0, 5.0), before);
  EXPECT_EQ(drag(before, "c1", ui::DragMode::None, 3.0, 5.0), before);
}

TEST(Binding, AnEditThatCannotApplyReturnsTheProjectUnchanged) {
  // Which is what lets the session skip the undo entry. Trimming an edge to
  // where it already is has nothing to do.
  const Project before = sample_project();
  const core::Clip* clip = core::find_clip(before, "c2");
  const Project after = drag(before, "c2", ui::DragMode::TrimStart,
                                            clip->start, core::clip_end(*clip));
  EXPECT_EQ(after, before);
}

// -------------------------------------------------- the header switches --

TEST(Switches, ShowWhatTheProjectHoldsRatherThanWhatTakesEffect) {
  // A track silenced by somebody else's solo is not muted, and its M must not
  // light up saying it is.
  Project project = sample_project();
  Track quiet{.id = "a2", .kind = TrackKind::Audio, .solo = true};
  project.sequence().tracks.push_back(std::move(quiet));

  const ui::TimelineModel model = timeline_model(project);
  EXPECT_TRUE(model.tracks[2].muted) << "a1 is not heard";
  EXPECT_FALSE(model.tracks[2].switches.mute) << "but nobody muted it";
  EXPECT_TRUE(model.tracks[3].switches.solo);
}

TEST(Switches, CarryEveryFlagAcross) {
  Project project = sample_project();
  project.sequence().tracks[0].hidden = true;
  project.sequence().tracks[0].locked = true;

  // By value. `timeline_model` returns a model, and a reference bound to a
  // *member* of a temporary is not lifetime-extended — see the note at the top.
  const ui::TrackSwitches on = timeline_model(project).tracks[0].switches;
  EXPECT_TRUE(on.hide);
  EXPECT_TRUE(on.lock);
  EXPECT_FALSE(on.mute);
}

TEST(Switches, TogglingReadsTheCurrentValueOutOfTheProject) {
  // The interface never holds the truth about a switch, so it cannot toggle
  // from a stale copy of one.
  const Project before = sample_project();
  ASSERT_FALSE(before.sequence().tracks[0].hidden);

  const Project on = toggle_track_switch(before, "v2", ui::TrackControl::Hide);
  EXPECT_TRUE(on.sequence().tracks[0].hidden);

  const Project off = toggle_track_switch(on, "v2", ui::TrackControl::Hide);
  EXPECT_FALSE(off.sequence().tracks[0].hidden);
}

TEST(Switches, TogglingOneLeavesTheOthersAlone) {
  Project before = sample_project();
  before.sequence().tracks[0].locked = true;

  const Project after = toggle_track_switch(before, "v2", ui::TrackControl::Hide);
  EXPECT_TRUE(after.sequence().tracks[0].hidden);
  EXPECT_TRUE(after.sequence().tracks[0].locked) << "the patch touches one field";
}

TEST(Switches, MuteAndSoloReachTheAudioTrack) {
  const Project muted = toggle_track_switch(sample_project(), "a1", ui::TrackControl::Mute);
  EXPECT_TRUE(muted.sequence().tracks[2].muted);
  EXPECT_FALSE(core::is_track_audible(muted, muted.sequence().tracks[2]));

  const Project soloed = toggle_track_switch(sample_project(), "a1", ui::TrackControl::Solo);
  EXPECT_TRUE(soloed.sequence().tracks[2].solo);
}

TEST(Switches, AnUnknownTrackChangesNothing) {
  const Project before = sample_project();
  EXPECT_EQ(toggle_track_switch(before, "nowhere", ui::TrackControl::Mute), before);
}

// ------------------------------------------------------------ the tools --

TEST(Binding, ARateStretchChangesTheSpeedAndNotTheSource) {
  // The whole difference from a trim: the footage shown is the same footage,
  // played at a different rate.
  const Project before = sample_project();
  const core::Clip* was = core::find_clip(before, "c2");
  const double source_span = was->source_out - was->source_in;

  // c2 runs 5 to 12; pulled back to end at 8.5, which is half its length.
  const Project after = drag(before, "c2", ui::DragMode::RateEnd, 5.0, 8.5);
  const core::Clip* now = core::find_clip(after, "c2");

  EXPECT_DOUBLE_EQ(now->source_in, was->source_in);
  EXPECT_DOUBLE_EQ(now->source_out, was->source_out);
  EXPECT_DOUBLE_EQ(now->source_out - now->source_in, source_span);
  EXPECT_DOUBLE_EQ(core::clip_speed(*now), 2.0);
  EXPECT_DOUBLE_EQ(core::clip_end(*now), 8.5);
}

TEST(Binding, ARateStretchCanMakeAClipLongerThanItsFootage) {
  // A trim could not: it runs out of source. This is what the tool is for.
  const Project before = sample_project();
  const Project after = drag(before, "title", ui::DragMode::RateEnd, 4.0, 20.0);
  const core::Clip* clip = core::find_clip(after, "title");

  EXPECT_DOUBLE_EQ(core::clip_end(*clip), 20.0);
  EXPECT_LT(core::clip_speed(*clip), 1.0) << "slowed down to fill the extra length";
}

TEST(Binding, DraggingATransitionsEdgeChangesHowLongItRuns) {
  Project before = sample_project();
  before = set_transition(std::move(before), "c1", core::TransitionKind::Dissolve, 1.0);
  ASSERT_TRUE(clip_transition(before, "c1").present);

  const Project after = apply_timeline_edit(
      before, "c1",
      ui::TimelineEdit{.mode = ui::DragMode::TransitionLength,
                       .result = ui::TimelineBlock{.transition = {.duration = 2.0}}});

  const TransitionRow row = clip_transition(after, "c1");
  EXPECT_TRUE(row.present);
  EXPECT_DOUBLE_EQ(row.duration, 2.0);
  // The kind is the panel's business. Turning a dissolve into something else by
  // pulling its edge would be a second meaning nobody asked this gesture for.
  EXPECT_EQ(row.kind, core::TransitionKind::Dissolve);
}

TEST(Binding, ATransitionDragIsClampedToWhatTheJoinCanManage) {
  // Half of one sits either side of the cut and neither half may swallow its
  // clip or run past the source there is to borrow. Stopping at the longest
  // that works is what a trim does at the end of its footage.
  Project before = sample_project();
  before = set_transition(std::move(before), "c1", core::TransitionKind::Dissolve, 1.0);
  const double longest = longest_transition(before, "c1", core::TransitionKind::Dissolve);
  ASSERT_GT(longest, 0.0);

  const Project after = apply_timeline_edit(
      before, "c1",
      ui::TimelineEdit{.mode = ui::DragMode::TransitionLength,
                       .result = ui::TimelineBlock{.transition = {.duration = longest * 10.0}}});

  EXPECT_DOUBLE_EQ(clip_transition(after, "c1").duration, longest);
}

TEST(Binding, DraggingAnEdgeWhereThereIsNoTransitionChangesNothing) {
  // The gesture cannot be started without one, but the edit is a value that can
  // arrive after the join has been changed underneath it.
  const Project before = sample_project();
  ASSERT_FALSE(clip_transition(before, "c1").present);

  EXPECT_EQ(apply_timeline_edit(
                before, "c1",
                ui::TimelineEdit{.mode = ui::DragMode::TransitionLength,
                                 .result = ui::TimelineBlock{.transition = {.duration = 2.0}}}),
            before);
}

TEST(Binding, ASlipMovesTheSourceAndLeavesTheClipWhereItIs) {
  const Project before = sample_project();
  const core::Clip* was = core::find_clip(before, "c2");

  const Project after = apply_timeline_edit(
      before, "c2", ui::TimelineEdit{.mode = ui::DragMode::Slip, .delta = -1.0});
  const core::Clip* now = core::find_clip(after, "c2");

  EXPECT_DOUBLE_EQ(now->start, was->start);
  EXPECT_DOUBLE_EQ(core::clip_duration(*now), core::clip_duration(*was));
  // Dragged left, so later footage: the window moves forward through the source.
  EXPECT_DOUBLE_EQ(now->source_in, was->source_in + 1.0);
  EXPECT_DOUBLE_EQ(now->source_out, was->source_out + 1.0);
}

TEST(Binding, ASlipFollowsTheContentRatherThanTheWindow) {
  // Dragging right shows *earlier* footage. The clip is a window onto a strip
  // of film and the strip moves with the hand, which is what every editor does
  // and the opposite of what the naive sign gives.
  const Project before = sample_project();
  const core::Clip* was = core::find_clip(before, "c2");
  const Project after = apply_timeline_edit(
      before, "c2", ui::TimelineEdit{.mode = ui::DragMode::Slip, .delta = 2.0});

  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->source_in, was->source_in - 2.0);
}

TEST(Binding, ASlipIsInSourceSecondsNotTimelineOnes) {
  // A clip at 2x shows two seconds of footage for every second of timeline, so
  // the same gesture has to move the source twice as far.
  Project before = core::set_clip_speed(sample_project(), "c2", 2.0);
  const core::Clip* was = core::find_clip(before, "c2");

  const Project after = apply_timeline_edit(
      before, "c2", ui::TimelineEdit{.mode = ui::DragMode::Slip, .delta = -1.0});

  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->source_in, was->source_in + 2.0);
}

TEST(Binding, ASlideMovesTheClipAndItsNeighboursAbsorbIt) {
  // c1 runs 0 to 5 and c2 runs 5 to 12, abutting. Sliding c2 later grows c1 and
  // leaves the end of the sequence where it was.
  const Project before = sample_project();
  const Project after = drag(before, "c2", ui::DragMode::Slide, 6.0, 13.0);

  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->start, 6.0);
  EXPECT_DOUBLE_EQ(core::clip_end(*core::find_clip(after, "c1")), 6.0)
      << "the clip before grew into the gap";
  EXPECT_DOUBLE_EQ(core::clip_duration(*core::find_clip(after, "c2")),
                   core::clip_duration(*core::find_clip(before, "c2")))
      << "a slide does not trim the clip being slid";
}

TEST(Binding, ARazorCutsTheClipItWasUsedOn) {
  const Project before = sample_project();
  const Project after = apply_timeline_edit(
      before, "c1", ui::TimelineEdit{.mode = ui::DragMode::Razor, .at = 2.0});

  const std::vector<core::Clip>& lower = core::track_of_clip(after, "c1")->clips;
  EXPECT_EQ(lower.size(), 3u) << "c1 became two, beside c2";
  EXPECT_DOUBLE_EQ(core::clip_end(*core::find_clip(after, "c1")), 2.0);
}

// A razor took the one clip it was pointed at, which on a linked pair cut the
// picture and left the sound whole. It then made that worse: the right half of
// a cut is put in a *new* group so the two halves are not linked to each other,
// so cutting one of a pair left the new group holding a single clip — the
// picture after the cut linked to nothing, and the sound still linked to the
// picture before it. One stroke both failed to cut the sound and unlinked it.
TEST(Binding, ARazorCutsEverythingLinkedToWhatItWasUsedOn) {
  Project before = sample_project();
  // Link the picture on v1 to the sound on a1, the way a placement does.
  for (core::Track& track : before.sequence().tracks) {
    for (core::Clip& clip : track.clips) {
      if (clip.id == "c1" || clip.id == "a") clip.group_id = "g1";
    }
  }

  const Project after = apply_timeline_edit(
      before, "c1", ui::TimelineEdit{.mode = ui::DragMode::Razor, .at = 2.0});

  EXPECT_EQ(core::track_of_clip(after, "c1")->clips.size(), 3u) << "the picture was cut";
  EXPECT_EQ(core::track_of_clip(after, "a")->clips.size(), 2u) << "and so was the sound";

  // And the halves either side of the cut are still pairs: the two left halves
  // share a group, the two right halves share a different one.
  const core::Clip* left_picture = core::find_clip(after, "c1");
  const core::Clip* left_sound = core::find_clip(after, "a");
  ASSERT_NE(left_picture, nullptr);
  ASSERT_NE(left_sound, nullptr);
  EXPECT_EQ(left_picture->group_id, left_sound->group_id) << "the halves before the cut";

  const std::vector<std::string> right = core::group_members(after, "c1");
  EXPECT_EQ(right.size(), 2u) << "the group either side of the cut holds a pair";
}

TEST(Binding, ARazorOnAnUnlinkedClipStillCutsOnlyThatOne) {
  const Project before = sample_project();
  const Project after = apply_timeline_edit(
      before, "c1", ui::TimelineEdit{.mode = ui::DragMode::Razor, .at = 2.0});
  EXPECT_EQ(core::track_of_clip(after, "a")->clips.size(), 1u);
}

TEST(Binding, ARazorLeavesEveryOtherClipAlone) {
  const Project before = sample_project();
  const Project after = apply_timeline_edit(
      before, "c1", ui::TimelineEdit{.mode = ui::DragMode::Razor, .at = 2.0});

  // The title on v2 spans four to seven and was not the clip clicked.
  EXPECT_EQ(core::track_of_clip(after, "title")->clips.size(), 1u);
}

TEST(Binding, ARazorAcrossEveryTrackCutsWhateverTheCutFallsInside) {
  const Project before = sample_project();
  // Five seconds in: inside the title on v2 and exactly on the join between c1
  // and c2 below, which is not inside either of them.
  const Project after = apply_timeline_edit(
      before, "title",
      ui::TimelineEdit{.mode = ui::DragMode::Razor, .at = 5.0, .all_tracks = true});

  EXPECT_EQ(core::track_of_clip(after, "title")->clips.size(), 2u);
  EXPECT_EQ(core::track_of_clip(after, "c1")->clips.size(), 2u)
      << "a cut landing on a join has nothing to divide";
}

TEST(Binding, ARazorOutsideTheClipDoesNothing) {
  const Project before = sample_project();
  EXPECT_EQ(apply_timeline_edit(before, "c1",
                                ui::TimelineEdit{.mode = ui::DragMode::Razor, .at = 40.0}),
            before);
}

// ----------------------------------------------------------- round tripping --

TEST(Binding, WhatIsDrawnAfterAnEditIsWhatWasAskedFor) {
  // The loop closed: project to blocks, drag a block, edit back to the
  // project, and the block that comes out is where the drag left it.
  const Project before = sample_project();
  const ui::TimelineModel drawn = timeline_model(before);
  const std::optional<std::string> id = block_clip_id(drawn, ui::BlockRef{0, 0});
  ASSERT_TRUE(id.has_value());

  const Project after = drag(before, *id, ui::DragMode::Move, 20.0, 23.0);
  const ui::TimelineModel redrawn = timeline_model(after);

  EXPECT_DOUBLE_EQ(redrawn.tracks[0].blocks[0].start, 20.0);
  EXPECT_DOUBLE_EQ(redrawn.tracks[0].blocks[0].end, 23.0);
  EXPECT_EQ(redrawn.tracks[0].blocks[0].id, *id);
}

// ------------------------------------------------------- the volume band --

TEST(Binding, OnlyAnAudioClipCarriesAVolumeBand) {
  const ui::TimelineModel model = timeline_model(sample_project());

  EXPECT_FALSE(model.tracks[0].blocks[0].gain.has_value());
  EXPECT_FALSE(model.tracks[1].blocks[0].gain.has_value());
  ASSERT_TRUE(model.tracks[2].blocks[0].gain.has_value());
}

TEST(Binding, TheBandStartsAtTheClipsOwnGain) {
  Project project = core::set_clip_gain(sample_project(), "a", 0.4);
  const ui::TimelineModel model = timeline_model(project);

  ASSERT_TRUE(model.tracks[2].blocks[0].gain.has_value());
  EXPECT_DOUBLE_EQ(model.tracks[2].blocks[0].gain->level, 0.4);
  EXPECT_TRUE(model.tracks[2].blocks[0].gain->points.empty());
}

TEST(Binding, TheBandsPointsAreTheClipsGainKeyframes) {
  Project project = sample_project();
  project = core::set_gain_keyframe(std::move(project), "a", 1.0, 0.25);
  project = core::set_gain_keyframe(std::move(project), "a", 4.0, 1.0);

  const ui::TimelineModel model = timeline_model(project);
  const std::vector<ui::GainPoint>& points = model.tracks[2].blocks[0].gain->points;

  ASSERT_EQ(points.size(), 2u);
  EXPECT_DOUBLE_EQ(points[0].t, 1.0);
  EXPECT_DOUBLE_EQ(points[0].v, 0.25);
  EXPECT_DOUBLE_EQ(points[1].t, 4.0);
  EXPECT_DOUBLE_EQ(points[1].v, 1.0);
}

// The band says where the automation is *and* what it does, which is strictly
// more than a diamond says. Both would be two marks on one clip for one
// keyframe.
TEST(Binding, AGainKeyframeIsNotAlsoDrawnAsADiamond) {
  Project project = core::set_gain_keyframe(sample_project(), "a", 2.0, 0.5);
  const ui::TimelineModel model = timeline_model(project);

  EXPECT_TRUE(model.tracks[2].blocks[0].keyframes.empty());
  EXPECT_EQ(model.tracks[2].blocks[0].gain->points.size(), 1u);
}

// A video clip has no band, so its gain keyframes have nowhere else to show.
TEST(Binding, AVideoClipsGainKeyframeIsStillADiamond) {
  Project project = core::set_gain_keyframe(sample_project(), "c1", 2.0, 0.5);
  const ui::TimelineModel model = timeline_model(project);

  EXPECT_EQ(model.tracks[1].blocks[0].keyframes.size(), 1u);
}

TEST(Binding, TheTopOfTheBandIsTheGainTheModelWillStore) {
  // Or the last part of the band's travel would be refused by the clamp in the
  // core, and dragging to the top would quietly land somewhere else.
  EXPECT_DOUBLE_EQ(timeline_model(sample_project()).max_gain, core::kMaxGain);
}

TEST(Binding, DraggingTheBandSetsTheClipsGain) {
  const Project after = apply_timeline_edit(
      sample_project(), "a", ui::TimelineEdit{.mode = ui::DragMode::GainLevel, .gain = 0.3});

  EXPECT_DOUBLE_EQ(core::find_clip(after, "a")->gain, 0.3);
}

TEST(Binding, MovingAPointRelocatesTheKeyframe) {
  Project project = core::set_gain_keyframe(sample_project(), "a", 2.0, 0.5);
  project = apply_timeline_edit(std::move(project), "a",
                                ui::TimelineEdit{.mode = ui::DragMode::GainPointDrag,
                                                 .gain_from = {2.0, 0.5},
                                                 .gain_to = {5.0, 0.8}});

  const std::vector<core::Keyframe>& kfs = core::find_clip(project, "a")->gain_keyframes;
  ASSERT_EQ(kfs.size(), 1u);
  EXPECT_DOUBLE_EQ(kfs[0].t, 5.0);
  EXPECT_DOUBLE_EQ(kfs[0].v, 0.8);
}

// A point that was just created reports the same place in both, and the one
// operation has to cope: the remove half finds the keyframe it is replacing and
// the set half puts it back.
TEST(Binding, AddingAPointIsTheSameEditAsMovingOne) {
  const Project after =
      apply_timeline_edit(sample_project(), "a",
                          ui::TimelineEdit{.mode = ui::DragMode::GainPointDrag,
                                           .gain_from = {3.0, 0.6},
                                           .gain_to = {3.0, 0.6}});

  const std::vector<core::Keyframe>& kfs = core::find_clip(after, "a")->gain_keyframes;
  ASSERT_EQ(kfs.size(), 1u);
  EXPECT_DOUBLE_EQ(kfs[0].t, 3.0);
  EXPECT_DOUBLE_EQ(kfs[0].v, 0.6);
}

TEST(Binding, DraggingAStretchSetsEveryPointItCarried) {
  Project project = core::set_gain_keyframe(sample_project(), "a", 2.0, 0.5);
  project = core::set_gain_keyframe(std::move(project), "a", 6.0, 1.0);

  project = apply_timeline_edit(
      std::move(project), "a",
      ui::TimelineEdit{.mode = ui::DragMode::GainSegment,
                       .gain_moved = {{2.0, 0.4}, {6.0, 0.8}}});

  const std::vector<core::Keyframe>& kfs = core::find_clip(project, "a")->gain_keyframes;
  ASSERT_EQ(kfs.size(), 2u) << "a stretch moves points, it does not add any";
  EXPECT_DOUBLE_EQ(kfs[0].v, 0.4);
  EXPECT_DOUBLE_EQ(kfs[1].v, 0.8);
  EXPECT_DOUBLE_EQ(kfs[0].t, 2.0);
  EXPECT_DOUBLE_EQ(kfs[1].t, 6.0);
}

// The times do not change, so each point is set where it already is — an
// upsert, which is what keeps the interpolation the keyframe was carrying.
TEST(Binding, DraggingAStretchKeepsEachKeyframesInterpolation) {
  Project project = core::set_gain_keyframe(sample_project(), "a", 2.0, 0.5);
  project = core::set_gain_keyframe(std::move(project), "a", 6.0, 1.0);
  project = core::set_gain_keyframe_interp(std::move(project), "a", core::Interp::Ease);

  project = apply_timeline_edit(
      std::move(project), "a",
      ui::TimelineEdit{.mode = ui::DragMode::GainSegment, .gain_moved = {{2.0, 0.4}}});

  EXPECT_EQ(core::gain_keyframe_interp_of(*core::find_clip(project, "a")), core::Interp::Ease);
}

TEST(Binding, RemovingAPointTakesTheKeyframeAway) {
  Project project = core::set_gain_keyframe(sample_project(), "a", 2.0, 0.5);
  project = core::set_gain_keyframe(std::move(project), "a", 6.0, 1.0);
  project = apply_timeline_edit(std::move(project), "a",
                                ui::TimelineEdit{.mode = ui::DragMode::GainPointRemove,
                                                 .gain_from = {2.0, 0.5},
                                                 .gain_to = {2.0, 0.5}});

  const std::vector<core::Keyframe>& kfs = core::find_clip(project, "a")->gain_keyframes;
  ASSERT_EQ(kfs.size(), 1u);
  EXPECT_DOUBLE_EQ(kfs[0].t, 6.0);
}

TEST(Binding, AVolumeEditOnAMissingClipIsANoOp) {
  const Project before = sample_project();
  EXPECT_EQ(apply_timeline_edit(before, "nope",
                                ui::TimelineEdit{.mode = ui::DragMode::GainLevel, .gain = 0.1}),
            before);
  EXPECT_EQ(apply_timeline_edit(before, "nope",
                               ui::TimelineEdit{.mode = ui::DragMode::GainPointDrag,
                                                .gain_to = {1.0, 0.5}}),
            before);
}

TEST(Binding, WhatIsDrawnAfterAVolumeEditIsWhatWasAskedFor) {
  Project project = apply_timeline_edit(
      sample_project(), "a",
      ui::TimelineEdit{
          .mode = ui::DragMode::GainPointDrag, .gain_from = {2.0, 0.5}, .gain_to = {2.0, 0.5}});
  project = apply_timeline_edit(std::move(project), "a",
                                ui::TimelineEdit{.mode = ui::DragMode::GainPointDrag,
                                                 .gain_from = {2.0, 0.5},
                                                 .gain_to = {4.0, 0.9}});

  const ui::TimelineModel model = timeline_model(project);
  const std::vector<ui::GainPoint>& points = model.tracks[2].blocks[0].gain->points;
  ASSERT_EQ(points.size(), 1u);
  EXPECT_DOUBLE_EQ(points[0].t, 4.0);
  EXPECT_DOUBLE_EQ(points[0].v, 0.9);
}

// ------------------------------------------------- moving a whole selection --

TEST(Binding, MovingOneOfASelectionMovesAllOfIt) {
  // Selecting several clips and having only the one under the pointer move
  // makes the selection a decoration.
  const std::vector<std::string> selection{"c1", "c2"};
  // c1 runs 0 to 5, so landing it at 3 is a move of three seconds.
  const Project after = drag_selected(sample_project(), "c1", selection, 3.0, 8.0);

  EXPECT_DOUBLE_EQ(core::find_clip(after, "c1")->start, 3.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->start, 8.0) << "it started at 5";
}

TEST(Binding, MovingAClipOutsideTheSelectionLeavesTheSelectionAlone) {
  // Dragging something that is not selected must not sweep up whatever happened
  // to be highlighted elsewhere.
  const std::vector<std::string> selection{"c2"};
  const Project after = drag_selected(sample_project(), "c1", selection, 3.0, 8.0);

  EXPECT_DOUBLE_EQ(core::find_clip(after, "c1")->start, 3.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->start, 5.0) << "it should not have moved";
}

// One clip stopped at the start of the timeline has to stop the rest with it,
// or the shape of the selection changes as it hits the edge.
TEST(Binding, ASelectionKeepsItsShapeAgainstTheStartOfTheTimeline) {
  const std::vector<std::string> selection{"c1", "c2"};
  // c1 already starts at zero, so a move backwards is refused for the whole set
  // rather than for c1 alone — which would have slid c2 under it.
  const Project after = drag_selected(sample_project(), "c1", selection, -10.0, -5.0);

  EXPECT_DOUBLE_EQ(core::find_clip(after, "c1")->start, 0.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->start, 5.0) << "the gap between them changed";
}

TEST(Binding, WithNoSelectionOnlyTheDraggedClipMoves) {
  const Project after = drag(sample_project(), "c1", ui::DragMode::Move, 3.0, 8.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c1")->start, 3.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c2")->start, 5.0);
}

// -------------------------------------------------------- the fade handles --

TEST(Binding, ABlockCarriesItsFades) {
  Project project = core::set_clip_fade(sample_project(), "c1", core::ClipEdge::In, 1.5);
  project = core::set_clip_fade(std::move(project), "c1", core::ClipEdge::Out, 0.5);

  const ui::TimelineModel model = timeline_model(project);
  EXPECT_DOUBLE_EQ(model.tracks[1].blocks[0].fade_in, 1.5);
  EXPECT_DOUBLE_EQ(model.tracks[1].blocks[0].fade_out, 0.5);
}

TEST(Binding, DraggingAFadeHandleSetsTheFade) {
  const Project after = apply_timeline_edit(
      sample_project(), "c1", ui::TimelineEdit{.mode = ui::DragMode::FadeIn, .fade = 1.25});
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c1")->fade_in, 1.25);

  const Project out = apply_timeline_edit(
      sample_project(), "c1", ui::TimelineEdit{.mode = ui::DragMode::FadeOut, .fade = 2.0});
  EXPECT_DOUBLE_EQ(core::find_clip(out, "c1")->fade_out, 2.0);
}

// The model owns the rule that the two fades together cannot exceed the clip;
// the handle is a convenience, not a second opinion about it.
TEST(Binding, TheModelStillBoundsAFadeDraggedTooFar) {
  const Project after = apply_timeline_edit(
      sample_project(), "c1", ui::TimelineEdit{.mode = ui::DragMode::FadeIn, .fade = 500.0});
  EXPECT_LE(core::find_clip(after, "c1")->fade_in, core::clip_duration(*core::find_clip(after, "c1")));
}

// ---------------------------------------------------------- the waveform --

[[nodiscard]] std::shared_ptr<const ui::Waveform> some_waveform() {
  auto wave = std::make_shared<ui::Waveform>();
  wave->buckets_per_second = 10.0;
  wave->minimum.assign(100, -0.5f);
  wave->maximum.assign(100, 0.5f);
  return wave;
}

TEST(Binding, AnAudioClipTakesTheEnvelopeOfItsOwnSourceAndStream) {
  std::vector<std::pair<std::string, int>> asked;
  const WaveformSource source = [&](std::string_view media_id, int stream) {
    asked.emplace_back(std::string(media_id), stream);
    return some_waveform();
  };

  const ui::TimelineModel model =
      timeline_model(sample_project(), {}, TimelineMedia{.waveforms = source});

  ASSERT_EQ(asked.size(), 1u);
  EXPECT_EQ(asked[0].first, "m2");
  EXPECT_EQ(asked[0].second, 0);
  EXPECT_NE(model.tracks[2].blocks[0].waveform, nullptr);
}

// A video clip draws its picture, not its sound. The audio it was linked to is
// a clip of its own, on its own track, and that is the one with the envelope.
TEST(Binding, AVideoClipIsNotAskedForAnEnvelope) {
  const ui::TimelineModel model =
      timeline_model(sample_project(), {},
                     TimelineMedia{.waveforms = [](std::string_view, int) {
                       return some_waveform();
                     }});

  EXPECT_EQ(model.tracks[0].blocks[0].waveform, nullptr);
  EXPECT_EQ(model.tracks[1].blocks[0].waveform, nullptr);
}

TEST(Binding, NoSourceOfEnvelopesIsSimplyAClipWithoutOne) {
  // What the timeline looks like while the decoding is still happening, and
  // under a build with no media layer at all.
  const ui::TimelineModel model = timeline_model(sample_project());
  EXPECT_EQ(model.tracks[2].blocks[0].waveform, nullptr);
}

TEST(Binding, ABlockCarriesWhatMapsItOntoItsSource) {
  Project project = sample_project();
  project.sequence().tracks[2].clips[0].source_in = 4.0;
  project.sequence().tracks[2].clips[0].speed = 2.0;
  project.sequence().tracks[2].clips[0].reverse = true;

  const ui::TimelineModel model = timeline_model(project);
  const ui::TimelineBlock& block = model.tracks[2].blocks[0];

  EXPECT_DOUBLE_EQ(block.source_in, 4.0);
  EXPECT_DOUBLE_EQ(block.speed, 2.0);
  EXPECT_TRUE(block.reverse);
}

// A clip with no speed set must read as 1, not 0, or every block would map its
// whole length onto a single instant of its source.
TEST(Binding, AClipWithNoSpeedSetRunsAtOne) {
  const ui::TimelineModel model = timeline_model(sample_project());
  EXPECT_DOUBLE_EQ(model.tracks[2].blocks[0].speed, 1.0);
}

// One envelope, however many clips of the source there are: it describes the
// file, and cutting a clip in half does not give it two shapes.
TEST(Binding, EveryClipOfASourceSharesOneEnvelope) {
  Project project = sample_project();
  project.sequence().tracks[2].clips.push_back(Clip{.id = "a2",
                                         .media_id = "m2",
                                         .kind = TrackKind::Audio,
                                         .source_in = 20.0,
                                         .source_out = 30.0,
                                         .start = 20.0});

  const std::shared_ptr<const ui::Waveform> shared = some_waveform();
  const ui::TimelineModel model =
      timeline_model(project, {},
                     TimelineMedia{.waveforms = [&](std::string_view, int) { return shared; }});

  EXPECT_EQ(model.tracks[2].blocks[0].waveform, model.tracks[2].blocks[1].waveform);
}

}  // namespace
}  // namespace cutline::editor
