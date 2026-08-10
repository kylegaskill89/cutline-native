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
  project.sequence().tracks.push_back(core::Track{.id = "v1", .kind = core::TrackKind::Video});

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

  project.sequence().tracks[0].clips.push_back(std::move(clip));
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

// ------------------------------------------------------------- handles --
//
// Dragging a handle both shapes the curve and switches the segment to Bezier.
// Which keyframe's mode changes is the part worth stating: interpolation lives
// on the keyframe a segment *leaves*, so the incoming handle of a keyframe
// changes the mode of the one before it.

TEST(SetKeyframeHandle, ShapesTheSegmentAndSwitchesItToBezier) {
  core::Project p = animated();
  p = set_keyframe_handle(std::move(p), "c1", x_ref(), 0.0, HandleSide::Out, 0.5, 0.0);

  const std::vector<core::Keyframe>& keys = x_keys(p);
  EXPECT_EQ(keys[0].e, core::Interp::Bezier);
  EXPECT_DOUBLE_EQ(keys[0].out_x, 0.5);
  EXPECT_DOUBLE_EQ(keys[0].out_y, 0.0);
}

TEST(SetKeyframeHandle, AnIncomingHandleSwitchesTheKeyframeBeforeIt) {
  // The segment it shapes is the one that *arrives*, and that segment's mode is
  // stored on the keyframe it left. Switching this keyframe instead would bend
  // the next segment along, which is not the one being pointed at.
  core::Project p = animated();
  p = set_keyframe_handle(std::move(p), "c1", x_ref(), 4.0, HandleSide::In, 0.25, 1.0);

  const std::vector<core::Keyframe>& keys = x_keys(p);
  EXPECT_EQ(keys[1].e, core::Interp::Bezier) << "the segment arriving at the last";
  EXPECT_EQ(keys[2].e, core::Interp::Linear) << "and not the one leaving it";
  EXPECT_DOUBLE_EQ(keys[2].in_x, 0.25) << "the handle stays on the keyframe it belongs to";
}

TEST(SetKeyframeHandle, BendsWhatTheEvaluatorProduces) {
  core::Project p = animated();
  const double before = core::animated_value(*core::find_clip(p, "c1"), core::AnimProp::X, 1.0);

  // Flat out of the start: it leaves slowly, so it is behind where it was.
  p = set_keyframe_handle(std::move(p), "c1", x_ref(), 0.0, HandleSide::Out, 1.0 / 3.0, 0.0);
  const double after = core::animated_value(*core::find_clip(p, "c1"), core::AnimProp::X, 1.0);

  EXPECT_LT(after, before);
}

TEST(SetKeyframeHandle, ClampsTheTimeButNotTheValue) {
  core::Project p = animated();
  p = set_keyframe_handle(std::move(p), "c1", x_ref(), 0.0, HandleSide::Out, -3.0, 2.5);

  const std::vector<core::Keyframe>& keys = x_keys(p);
  EXPECT_DOUBLE_EQ(keys[0].out_x, 0.0) << "a curve cannot be at two values in one instant";
  EXPECT_DOUBLE_EQ(keys[0].out_y, 2.5) << "overshoot is a shape somebody wants";
}

TEST(SetKeyframeHandle, RefusesAHandleWithNoSegmentToShape) {
  const core::Project p = animated();
  // The outgoing handle of the last keyframe, and the incoming one of the first.
  EXPECT_EQ(set_keyframe_handle(p, "c1", x_ref(), 4.0, HandleSide::Out, 0.5, 0.5), p);
  EXPECT_EQ(set_keyframe_handle(p, "c1", x_ref(), 0.0, HandleSide::In, 0.5, 0.5), p);
}

TEST(SetKeyframeHandle, RefusesWhenNothingIsNearTheTimeAsked) {
  const core::Project p = animated();
  EXPECT_EQ(set_keyframe_handle(p, "c1", x_ref(), 1.0, HandleSide::Out, 0.5, 0.5), p);
}

TEST(SetKeyframeHandle, SettingTheSameHandleAgainChangesNothing) {
  core::Project p = animated();
  p = set_keyframe_handle(std::move(p), "c1", x_ref(), 0.0, HandleSide::Out, 0.5, 0.25);
  EXPECT_EQ(set_keyframe_handle(p, "c1", x_ref(), 0.0, HandleSide::Out, 0.5, 0.25), p);
}

// -------------------------------------------------------- copy and paste --

[[nodiscard]] std::vector<KeyframeAddress> all_x() {
  return {KeyframeAddress{.ref = x_ref(), .t = 0.0}, KeyframeAddress{.ref = x_ref(), .t = 2.0},
          KeyframeAddress{.ref = x_ref(), .t = 4.0}};
}

TEST(CopyKeyframes, TakesTimesRelativeToTheEarliestOne) {
  // Absolute times would only ever go back where they came from. The shape of
  // an animation is the spacing between its points.
  const KeyframeClipboard clipboard =
      copy_keyframes(animated(), "c1", std::vector{KeyframeAddress{.ref = x_ref(), .t = 2.0},
                                                   KeyframeAddress{.ref = x_ref(), .t = 4.0}});

  ASSERT_EQ(clipboard.lanes.size(), 1u);
  ASSERT_EQ(clipboard.lanes[0].keys.size(), 2u);
  EXPECT_DOUBLE_EQ(clipboard.lanes[0].keys[0].t, 0.0);
  EXPECT_DOUBLE_EQ(clipboard.lanes[0].keys[1].t, 2.0);
}

TEST(CopyKeyframes, GroupsBySomethingThatCanBePastedBack) {
  core::Project project = animated();
  core::find_clip(project, "c1")->keyframes[core::anim_prop_index(core::AnimProp::Opacity)] = {
      Keyframe{.t = 1.0, .v = 0.5}};

  const KeyframeClipboard clipboard =
      copy_keyframes(project, "c1",
                     std::vector{KeyframeAddress{.ref = x_ref(), .t = 2.0},
                                 KeyframeAddress{.ref = ParamRef{.param = ClipParam::Opacity},
                                                 .t = 1.0}});
  EXPECT_EQ(clipboard.lanes.size(), 2u);
}

TEST(CopyKeyframes, CopyingNothingLeavesAnEmptyClipboard) {
  // So a paste after a failed copy puts nothing anywhere, rather than putting
  // back whatever was copied before.
  EXPECT_TRUE(copy_keyframes(animated(), "c1", {}).empty());
  EXPECT_TRUE(copy_keyframes(animated(), "c1",
                             std::vector{KeyframeAddress{.ref = x_ref(), .t = 1.5}})
                  .empty());
}

TEST(PasteKeyframes, PutsTheEarliestAtTheTimeAsked) {
  core::Project project = animated();
  // Two keyframes, 2 seconds apart, back down at half a second.
  const KeyframeClipboard clipboard =
      copy_keyframes(project, "c1", std::vector{KeyframeAddress{.ref = x_ref(), .t = 0.0},
                                                KeyframeAddress{.ref = x_ref(), .t = 2.0}});

  const core::Project pasted = paste_keyframes(project, "c1", clipboard, 0.5);
  const std::vector<Keyframe>& keys = x_keys(pasted);

  // 0, 0.5, 2, 2.5, 4.
  ASSERT_EQ(keys.size(), 5u);
  EXPECT_DOUBLE_EQ(keys[1].t, 0.5);
  EXPECT_DOUBLE_EQ(keys[3].t, 2.5);
}

TEST(PasteKeyframes, KeepsEachKeyframesOwnCurve) {
  // `upsert_keyframe` gives a new keyframe the *list's* mode, so a paste that
  // did not put the copied one back would quietly flatten every curve it moved.
  core::Project project = set_keyframe_interp(animated(), "c1", x_ref(), 2.0, Interp::Hold);
  const KeyframeClipboard clipboard =
      copy_keyframes(project, "c1", std::vector{KeyframeAddress{.ref = x_ref(), .t = 2.0}});

  const core::Project pasted = paste_keyframes(project, "c1", clipboard, 1.0);
  const std::vector<Keyframe>& keys = x_keys(pasted);
  ASSERT_EQ(keys.size(), 4u);
  EXPECT_EQ(keys[1].e, Interp::Hold) << "the pasted keyframe took the list's mode instead";
}

TEST(PasteKeyframes, DropsWhatWouldLandOutsideTheClip) {
  // Clamping would pile a whole curve onto the last frame, which is worse than
  // losing the part that did not fit.
  core::Project project = animated();
  const KeyframeClipboard clipboard =
      copy_keyframes(project, "c1", std::vector{KeyframeAddress{.ref = x_ref(), .t = 0.0},
                                                KeyframeAddress{.ref = x_ref(), .t = 4.0}});

  // At 3s the second one would land at 7s, past the clip's 4s end.
  const core::Project pasted = paste_keyframes(project, "c1", clipboard, 3.0);
  const std::vector<Keyframe>& keys = x_keys(pasted);
  ASSERT_EQ(keys.size(), 4u);
  EXPECT_DOUBLE_EQ(keys.back().t, 4.0);
}

TEST(PasteKeyframes, OverwritesWhatIsAlreadyAtThatInstant) {
  core::Project project = animated();
  const KeyframeClipboard clipboard =
      copy_keyframes(project, "c1", std::vector{KeyframeAddress{.ref = x_ref(), .t = 0.0}});

  const core::Project pasted = paste_keyframes(project, "c1", clipboard, 2.0);
  const std::vector<Keyframe>& keys = x_keys(pasted);
  EXPECT_EQ(keys.size(), 3u) << "it added a second keyframe at the same instant";
  EXPECT_DOUBLE_EQ(keys[1].v, 0.0) << "the copied value did not go in";
}

TEST(PasteKeyframes, SkipsAPropertyThatIsNoLongerAnimated) {
  // Switching animation on is the stopwatch's job, and a paste that silently
  // did it would change the picture in a way nobody asked for.
  const KeyframeClipboard clipboard =
      copy_keyframes(animated(), "c1", std::vector{KeyframeAddress{.ref = x_ref(), .t = 0.0}});

  core::Project bare = animated();
  core::find_clip(bare, "c1")->keyframes[core::anim_prop_index(core::AnimProp::X)].clear();

  EXPECT_EQ(paste_keyframes(bare, "c1", clipboard, 1.0), bare);
}

TEST(PasteKeyframes, AnEmptyClipboardChangesNothing) {
  const core::Project before = animated();
  EXPECT_EQ(paste_keyframes(before, "c1", KeyframeClipboard{}, 1.0), before);
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
