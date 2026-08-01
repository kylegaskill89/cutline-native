/// A clip's animation as lanes, and the edits a lane view makes.
///
/// The interesting cases are all refusals. Every edit here can decline — no
/// keyframe there, the last one, a move onto another — and an edit that half
/// applied instead would leave an animation nobody asked for and no way back.

#include "cutline/editor/keyframes.hpp"

#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::editor {
namespace {

using core::Interp;
using core::Keyframe;

/// A project with one video clip, four seconds long, animated on X.
[[nodiscard]] core::Project animated() {
  core::Project project;
  project.tracks.push_back(core::Track{.id = "v1", .kind = core::TrackKind::Video});

  core::Clip clip;
  clip.id = "c1";
  clip.media_id = "m1";
  clip.kind = core::TrackKind::Video;
  clip.source_in = 0.0;
  clip.source_out = 4.0;
  clip.keyframes[core::anim_prop_index(core::AnimProp::X)] = {
      Keyframe{.t = 0.0, .v = 0.0, .e = Interp::Linear},
      Keyframe{.t = 2.0, .v = 0.5, .e = Interp::Linear},
      Keyframe{.t = 4.0, .v = 1.0, .e = Interp::Linear}};

  project.tracks[0].clips.push_back(std::move(clip));
  return project;
}

[[nodiscard]] const std::vector<Keyframe>& x_keys(const core::Project& project) {
  return core::find_clip(project, "c1")->keyframes[core::anim_prop_index(core::AnimProp::X)];
}

[[nodiscard]] ParamRef x_ref() { return ParamRef{.param = ClipParam::X}; }

// ----------------------------------------------------------------- lanes --

TEST(ClipKeyframes, ListsOnlyWhatIsAnimated) {
  // A lane for a property with no keyframes would be a row of nothing, and
  // there are six transform properties to have empty rows for.
  const KeyframeModel model = clip_keyframes(animated(), "c1");
  ASSERT_EQ(model.lanes.size(), 1u);
  EXPECT_EQ(model.lanes[0].name, param_name(ClipParam::X));
  EXPECT_EQ(model.lanes[0].keys.size(), 3u);
  EXPECT_DOUBLE_EQ(model.duration, 4.0);
}

TEST(ClipKeyframes, AClipThatIsNotThereHasNoLanes) {
  const KeyframeModel model = clip_keyframes(animated(), "nope");
  EXPECT_TRUE(model.empty());
  EXPECT_DOUBLE_EQ(model.duration, 0.0);
}

TEST(ClipKeyframes, TheDurationIsTheClipsOwnLengthRatherThanTheSequences) {
  core::Project project = animated();
  core::find_clip(project, "c1")->start = 30.0;
  EXPECT_DOUBLE_EQ(clip_keyframes(project, "c1").duration, 4.0);
}

TEST(ClipKeyframes, AnEffectsAnimatedParameterGetsALaneNamedAfterBoth) {
  // "Amount" alone says nothing when three effects in the stack have one.
  core::Project project = animated();
  core::ClipEffect blur;
  blur.type = "blur";
  blur.keyframes["amount"] = {Keyframe{.t = 1.0, .v = 4.0}};
  core::find_clip(project, "c1")->effects.push_back(std::move(blur));

  const KeyframeModel model = clip_keyframes(project, "c1");
  ASSERT_EQ(model.lanes.size(), 2u);
  EXPECT_FALSE(model.lanes[1].ref.motion());
  EXPECT_EQ(model.lanes[1].ref.effect, 0u);
  EXPECT_EQ(model.lanes[1].ref.key, "amount");
  EXPECT_NE(model.lanes[1].name.find("Amount"), std::string::npos);
  EXPECT_NE(model.lanes[1].name.find("Blur"), std::string::npos);
}

TEST(ClipKeyframes, AnEffectTheRegistryNoLongerHasContributesNoLanes) {
  core::Project project = animated();
  core::ClipEffect unknown;
  unknown.type = "not-an-effect";
  unknown.keyframes["whatever"] = {Keyframe{.t = 1.0, .v = 1.0}};
  core::find_clip(project, "c1")->effects.push_back(std::move(unknown));

  EXPECT_EQ(clip_keyframes(project, "c1").lanes.size(), 1u);
}

// ------------------------------------------------------------------ move --

TEST(MoveKeyframe, MovesTheOneAskedForAndKeepsTheListSorted) {
  const core::Project moved = move_keyframe(animated(), "c1", x_ref(), 2.0, 3.5);
  const std::vector<Keyframe>& keys = x_keys(moved);

  ASSERT_EQ(keys.size(), 3u);
  EXPECT_DOUBLE_EQ(keys[0].t, 0.0);
  EXPECT_DOUBLE_EQ(keys[1].t, 3.5);
  EXPECT_DOUBLE_EQ(keys[2].t, 4.0);
}

TEST(MoveKeyframe, ReordersWhenAKeyframeIsDraggedPastAnother) {
  // The evaluator walks the list in order, so an unsorted one animates
  // backwards through the middle of itself.
  const core::Project moved = move_keyframe(animated(), "c1", x_ref(), 0.0, 3.0);
  const std::vector<Keyframe>& keys = x_keys(moved);
  EXPECT_DOUBLE_EQ(keys[0].t, 2.0);
  EXPECT_DOUBLE_EQ(keys[1].t, 3.0);
  EXPECT_DOUBLE_EQ(keys[2].t, 4.0);
}

TEST(MoveKeyframe, ClampsInsideTheClip) {
  // A keyframe past either end is one that can never be reached again.
  EXPECT_DOUBLE_EQ(x_keys(move_keyframe(animated(), "c1", x_ref(), 2.0, -9.0))[0].t, 0.0);
  EXPECT_DOUBLE_EQ(x_keys(move_keyframe(animated(), "c1", x_ref(), 2.0, 99.0))[2].t, 4.0);
}

TEST(MoveKeyframe, RefusesToLandOnTopOfAnotherOne) {
  // Two keyframes at the same instant have no meaningful order, and which one
  // survived would be whichever the sort happened to keep.
  const core::Project before = animated();
  const core::Project after = move_keyframe(before, "c1", x_ref(), 2.0, 4.0);
  EXPECT_EQ(after, before);
}

TEST(MoveKeyframe, DoesNothingWhenThereIsNoKeyframeThere) {
  const core::Project before = animated();
  EXPECT_EQ(move_keyframe(before, "c1", x_ref(), 1.0, 3.0), before);
}

TEST(MoveKeyframe, DoesNothingForAPropertyThatIsNotAnimated) {
  const core::Project before = animated();
  EXPECT_EQ(move_keyframe(before, "c1", ParamRef{.param = ClipParam::Rotation}, 0.0, 1.0),
            before);
}

TEST(MoveKeyframe, DoesNothingForAClipThatIsNotThere) {
  const core::Project before = animated();
  EXPECT_EQ(move_keyframe(before, "nope", x_ref(), 2.0, 3.0), before);
}

// ---------------------------------------------------------- interpolation --

TEST(SetKeyframeInterp, SetsTheModeOnOneKeyframeAndLeavesTheRest) {
  // The whole point, and the thing the property-wide setter cannot do. The
  // evaluator has always read the outgoing keyframe's own mode, so a list with
  // a different curve out of each point already renders correctly.
  const core::Project eased = set_keyframe_interp(animated(), "c1", x_ref(), 2.0, Interp::Ease);
  const std::vector<Keyframe>& keys = x_keys(eased);

  EXPECT_EQ(keys[0].e, Interp::Linear);
  EXPECT_EQ(keys[1].e, Interp::Ease);
  EXPECT_EQ(keys[2].e, Interp::Linear);
}

TEST(SetKeyframeInterp, DoesNothingWhenItIsAlreadyThatMode) {
  const core::Project before = animated();
  EXPECT_EQ(set_keyframe_interp(before, "c1", x_ref(), 2.0, Interp::Linear), before);
}

TEST(SetKeyframeInterp, DoesNothingWhenThereIsNoKeyframeThere) {
  const core::Project before = animated();
  EXPECT_EQ(set_keyframe_interp(before, "c1", x_ref(), 1.0, Interp::Hold), before);
}

// ---------------------------------------------------------------- remove --

TEST(RemoveKeyframe, TakesTheOneNearestTheTimeAsked) {
  // The project is held rather than passed straight through: `x_keys` returns a
  // reference into it, and a temporary would be gone before it was read.
  const core::Project shorter = remove_keyframe(animated(), "c1", x_ref(), 2.0);
  const std::vector<Keyframe>& keys = x_keys(shorter);
  ASSERT_EQ(keys.size(), 2u);
  EXPECT_DOUBLE_EQ(keys[0].t, 0.0);
  EXPECT_DOUBLE_EQ(keys[1].t, 4.0);
}

TEST(RemoveKeyframe, RefusesToTakeTheLastOne) {
  // A property with animation on and no keyframes evaluates to zero, which is
  // not what removing a point should mean. Turning animation off is the
  // stopwatch's job.
  core::Project project = animated();
  core::find_clip(project, "c1")->keyframes[core::anim_prop_index(core::AnimProp::X)] = {
      Keyframe{.t = 1.0, .v = 0.5}};

  EXPECT_EQ(remove_keyframe(project, "c1", x_ref(), 1.0), project);
}

// ------------------------------------------------------------- neighbours --

TEST(KeyframeNeighbours, FindTheOnesEitherSide) {
  const std::vector<Keyframe> keys = x_keys(animated());
  EXPECT_DOUBLE_EQ(*keyframe_before(keys, 3.0), 2.0);
  EXPECT_DOUBLE_EQ(*keyframe_after(keys, 3.0), 4.0);
}

TEST(KeyframeNeighbours, DoNotStickOnTheOneTheyJustLandedOn) {
  // Otherwise pressing "next" repeatedly goes to the same keyframe for ever.
  const std::vector<Keyframe> keys = x_keys(animated());
  EXPECT_DOUBLE_EQ(*keyframe_after(keys, 2.0), 4.0);
  EXPECT_DOUBLE_EQ(*keyframe_before(keys, 2.0), 0.0);
}

TEST(KeyframeNeighbours, ReportNothingAtTheEnds) {
  const std::vector<Keyframe> keys = x_keys(animated());
  EXPECT_FALSE(keyframe_before(keys, 0.0).has_value());
  EXPECT_FALSE(keyframe_after(keys, 4.0).has_value());
  EXPECT_FALSE(keyframe_before({}, 1.0).has_value());
}

}  // namespace
}  // namespace cutline::editor
