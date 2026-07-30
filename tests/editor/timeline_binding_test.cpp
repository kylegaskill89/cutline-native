/// The join between a project and the timeline that draws it.
///
/// Both halves are already tested on their own. What is left is whether they
/// agree: that a clip becomes a block in the right place, that a block dragged
/// somewhere becomes the right edit, and that the numbering on the track
/// headers is the one an editor expects.

#include "cutline/editor/timeline_binding.hpp"

#include "cutline/core/animate.hpp"
#include "cutline/core/edit.hpp"
#include "cutline/core/effects.hpp"
#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

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
  project.fps = 30.0;
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

  project.tracks = {std::move(upper), std::move(lower), std::move(audio)};
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
  project.tracks[0].label = "Titles";
  EXPECT_EQ(default_track_label(project, 0), "Titles");
}

TEST(Binding, AnIndexPastTheEndIsHarmless) {
  EXPECT_TRUE(default_track_label(sample_project(), 99).empty());
}

// ----------------------------------------------------------- the mute flag --

TEST(Binding, AHiddenVideoTrackReadsAsMuted) {
  Project project = sample_project();
  project.tracks[0].hidden = true;
  EXPECT_TRUE(timeline_model(project).tracks[0].muted);
}

TEST(Binding, SoloingOneAudioTrackMutesTheOthers) {
  // Which is why this is not simply `track.muted`: an audio track can be
  // silenced by something happening on a different track entirely.
  Project project = sample_project();
  Track second{.id = "a2", .kind = TrackKind::Audio, .solo = true};
  project.tracks.push_back(std::move(second));

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

}  // namespace
}  // namespace cutline::editor
