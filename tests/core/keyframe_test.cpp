#include "cutline/core/keyframe.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

namespace cutline::core {
namespace {

TEST(EaseFraction, LinearIsIdentity) {
  EXPECT_DOUBLE_EQ(ease_fraction(0.0, Interp::Linear), 0.0);
  EXPECT_DOUBLE_EQ(ease_fraction(0.25, Interp::Linear), 0.25);
  EXPECT_DOUBLE_EQ(ease_fraction(1.0, Interp::Linear), 1.0);
}

TEST(EaseFraction, HoldAlwaysCollapsesToZero) {
  EXPECT_DOUBLE_EQ(ease_fraction(0.0, Interp::Hold), 0.0);
  EXPECT_DOUBLE_EQ(ease_fraction(0.5, Interp::Hold), 0.0);
  EXPECT_DOUBLE_EQ(ease_fraction(0.999, Interp::Hold), 0.0);
}

TEST(EaseFraction, EaseIsSmoothstep) {
  EXPECT_DOUBLE_EQ(ease_fraction(0.0, Interp::Ease), 0.0);
  EXPECT_DOUBLE_EQ(ease_fraction(0.25, Interp::Ease), 0.15625);
  EXPECT_DOUBLE_EQ(ease_fraction(0.5, Interp::Ease), 0.5);
  EXPECT_DOUBLE_EQ(ease_fraction(1.0, Interp::Ease), 1.0);
}

TEST(EvalKeyframes, EmptyListIsZero) {
  EXPECT_DOUBLE_EQ(eval_keyframes({}, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(eval_keyframes({}, 42.0), 0.0);
}

TEST(EvalKeyframes, SingleKeyframeIsConstant) {
  const std::vector<Keyframe> kfs{{.t = 1.0, .v = 7.0}};
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 0.0), 7.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 1.0), 7.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 99.0), 7.0);
}

TEST(EvalKeyframes, ClampsOutsideTheRange) {
  const std::vector<Keyframe> kfs{{.t = 1.0, .v = 10.0}, {.t = 3.0, .v = 30.0}};
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 0.0), 10.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 1.0), 10.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 3.0), 30.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 100.0), 30.0);
}

TEST(EvalKeyframes, LinearInterpolatesBetween) {
  const std::vector<Keyframe> kfs{{.t = 0.0, .v = 0.0}, {.t = 2.0, .v = 100.0}};
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 0.5), 25.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 1.0), 50.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 1.5), 75.0);
}

TEST(EvalKeyframes, HoldStepsAtTheNextKeyframe) {
  const std::vector<Keyframe> kfs{{.t = 0.0, .v = 0.0, .e = Interp::Hold},
                                 {.t = 2.0, .v = 100.0}};
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 0.5), 0.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 1.999), 0.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 2.0), 100.0);
}

TEST(EvalKeyframes, EaseUsesSmoothstep) {
  const std::vector<Keyframe> kfs{{.t = 0.0, .v = 0.0, .e = Interp::Ease},
                                 {.t = 4.0, .v = 100.0}};
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 1.0), 15.625);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 2.0), 50.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 3.0), 84.375);
}

// The interpolation mode is a property of the segment's *starting* keyframe,
// so a list can mix modes segment by segment.
TEST(EvalKeyframes, ModeComesFromTheOutgoingKeyframe) {
  const std::vector<Keyframe> kfs{{.t = 0.0, .v = 0.0, .e = Interp::Hold},
                                 {.t = 1.0, .v = 10.0, .e = Interp::Linear},
                                 {.t = 2.0, .v = 20.0, .e = Interp::Ease}};
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 0.5), 0.0);    // held by keyframe 0
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 1.5), 15.0);   // linear from keyframe 1
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 2.0), 20.0);   // clamped to the last
}

// Two keyframes sharing a timestamp resolve to the earlier segment's endpoint
// rather than dividing by a zero span.
TEST(EvalKeyframes, DuplicateTimestampsDoNotDivideByZero) {
  const std::vector<Keyframe> kfs{{.t = 0.0, .v = 0.0},
                                 {.t = 1.0, .v = 10.0},
                                 {.t = 1.0, .v = 20.0},
                                 {.t = 2.0, .v = 30.0}};
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 1.0), 10.0);
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 0.5), 5.0);
}

}  // namespace

// ----------------------------------------------------------------- bezier --
//
// The general interpolation. What is worth pinning down is that it agrees with
// the fixed shapes where it should, that solving for x actually lands on x, and
// that a handle nobody has moved changes nothing.

TEST(Bezier, TheDefaultHandlesAreAStraightLine) {
  // The whole reason the defaults are a third and a third: grabbing a handle
  // has to feel like taking hold of the curve that was already there.
  for (const double f : {0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0}) {
    EXPECT_NEAR(bezier_fraction(f, kDefaultHandleX, kDefaultHandleY, 1.0 - kDefaultHandleX,
                                1.0 - kDefaultHandleY),
                f, 1e-5)
        << "at " << f;
  }
}

TEST(Bezier, ASegmentSwitchedToBezierAndLeftAloneAnimatesLinearly) {
  std::vector<Keyframe> straight{{.t = 0.0, .v = 0.0}, {.t = 4.0, .v = 100.0}};
  std::vector<Keyframe> curved = straight;
  curved[0].e = Interp::Bezier;

  for (const double t : {0.0, 0.5, 1.0, 2.0, 3.5, 4.0}) {
    EXPECT_NEAR(eval_keyframes(curved, t), eval_keyframes(straight, t), 1e-4) << "at " << t;
  }
}

TEST(Bezier, EndsAreExact) {
  EXPECT_DOUBLE_EQ(bezier_fraction(0.0, 0.9, 0.0, 0.1, 1.0), 0.0);
  EXPECT_DOUBLE_EQ(bezier_fraction(1.0, 0.9, 0.0, 0.1, 1.0), 1.0);
}

TEST(Bezier, PullingBothHandlesFlatMakesAnEaseInAndOut) {
  // (1/3, 0) and (2/3, 1): the classic ease. Slow away from the start, slow
  // into the end, and dead on the halfway point in the middle by symmetry.
  const double early = bezier_fraction(0.25, 1.0 / 3.0, 0.0, 2.0 / 3.0, 1.0);
  const double middle = bezier_fraction(0.5, 1.0 / 3.0, 0.0, 2.0 / 3.0, 1.0);
  const double late = bezier_fraction(0.75, 1.0 / 3.0, 0.0, 2.0 / 3.0, 1.0);

  EXPECT_LT(early, 0.25) << "slow to leave";
  EXPECT_NEAR(middle, 0.5, 1e-6);
  EXPECT_GT(late, 0.75) << "and slow to arrive";
}

TEST(Bezier, IsCloseToSmoothstepWhenShapedLikeIt) {
  // The Ease mode is smoothstep and stays smoothstep, so that existing projects
  // render to the same numbers. This says the two are the same *curve* to the
  // eye, which is what makes Ease a preset over this space rather than a
  // different feature.
  for (const double f : {0.1, 0.3, 0.5, 0.7, 0.9}) {
    EXPECT_NEAR(bezier_fraction(f, 1.0 / 3.0, 0.0, 2.0 / 3.0, 1.0), ease_fraction(f, Interp::Ease),
                0.03)
        << "at " << f;
  }
}

TEST(Bezier, SolvesForTheFractionAlongXRatherThanAlongTheCurve) {
  // A handle pulled far along in time. At the halfway point in *time* the value
  // must still be the curve's value there, not the curve's midpoint.
  const double x1 = 0.9;
  const double x2 = 0.95;
  // Both control points late, so the value hangs back and then rushes.
  EXPECT_LT(bezier_fraction(0.5, x1, 0.0, x2, 0.0), 0.2);
  EXPECT_NEAR(bezier_fraction(1.0, x1, 0.0, x2, 0.0), 1.0, 1e-9);
}

TEST(Bezier, AHandleDraggedBackwardsInTimeIsClampedRatherThanFolded) {
  // Left alone, x outside 0..1 makes the curve double back and be at two values
  // at the same instant, and which one came out would be wherever the solver
  // happened to land.
  const double folded = bezier_fraction(0.5, -2.0, 0.0, 3.0, 1.0);
  const double clamped = bezier_fraction(0.5, 0.0, 0.0, 1.0, 1.0);
  EXPECT_DOUBLE_EQ(folded, clamped);
}

TEST(Bezier, AnyHandlesGiveAFiniteCurveWithExactEnds) {
  // Deliberately *not* a monotonicity test on the value: a handle pulled past
  // the far keyframe makes the animation overshoot and come back, which is a
  // real shape somebody wants. What must hold for every handle is that the
  // solve terminates with a number, and that the two ends are still the two
  // keyframes — a curve that did not start where it starts would move the
  // keyframe rather than bend the segment.
  const std::array shapes{std::array{0.0, 2.0, 1.0, -1.0}, std::array{0.9, 0.1, 0.1, 0.9},
                          std::array{0.0, 0.0, 0.0, 0.0}, std::array{1.0, 1.0, 1.0, 1.0},
                          std::array{0.5, -3.0, 0.5, 4.0}};
  for (const auto& shape : shapes) {
    EXPECT_DOUBLE_EQ(bezier_fraction(0.0, shape[0], shape[1], shape[2], shape[3]), 0.0);
    EXPECT_DOUBLE_EQ(bezier_fraction(1.0, shape[0], shape[1], shape[2], shape[3]), 1.0);
    for (int i = 1; i < 50; ++i) {
      const double f = static_cast<double>(i) / 50.0;
      EXPECT_TRUE(std::isfinite(bezier_fraction(f, shape[0], shape[1], shape[2], shape[3])))
          << "at " << f;
    }
  }
}

TEST(Bezier, TheSegmentTakesOneHandleFromEachEnd) {
  // A's outgoing handle and B's incoming one. Setting only B's must still bend
  // the segment, which is what says the two are read from different keyframes.
  std::vector<Keyframe> kfs{{.t = 0.0, .v = 0.0, .e = Interp::Bezier},
                            {.t = 4.0, .v = 100.0}};
  const double before = eval_keyframes(kfs, 3.0);

  kfs[1].in_y = 0.0;  // flat into the end: it arrives slowly
  EXPECT_GT(eval_keyframes(kfs, 3.0), before);
}

TEST(Bezier, AModeThatIsNotBezierIgnoresTheHandles) {
  std::vector<Keyframe> kfs{{.t = 0.0, .v = 0.0}, {.t = 4.0, .v = 100.0}};
  kfs[0].out_y = 0.0;
  kfs[1].in_y = 0.0;
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 2.0), 50.0) << "still linear";
}

TEST(Bezier, HoldStillHolds) {
  // Hold is the one of the four that is not a curve at all, so it survives the
  // arrival of a general one rather than becoming a preset over it.
  std::vector<Keyframe> kfs{{.t = 0.0, .v = 0.0, .e = Interp::Hold}, {.t = 4.0, .v = 100.0}};
  EXPECT_DOUBLE_EQ(eval_keyframes(kfs, 3.9), 0.0);
}

}  // namespace cutline::core