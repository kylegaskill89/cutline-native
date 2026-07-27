#include "cutline/core/keyframe.hpp"

#include <gtest/gtest.h>

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
}  // namespace cutline::core
