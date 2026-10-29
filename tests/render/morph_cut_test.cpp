/// Morph Cut as it is planned: a dissolve until the motion has been found,
/// then each side bent partway towards the other by Warp Stabilizer's mesh,
/// in step with the dissolve.

#include "cutline/render/morph_cut.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/render/plan.hpp"
#include "cutline/render/warp_stabilizer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <process.h>
#include <string>

namespace cutline::render {
namespace {

/// One interview, cut twice into: [5, 10) then [12, 17), a Morph Cut of a
/// second over the jump.
[[nodiscard]] core::Project jump_cut() {
  core::Project p = core::empty_project(1, 0);
  p.sequence().fps = 10.0;
  core::Media m;
  m.id = "m";
  m.path = "D:/no/such/interview.mp4";
  m.duration = 30.0;
  m.has_video = true;
  m.width = 1920;
  m.height = 1080;
  p.media = {m};
  core::Clip a{.id = "a", .media_id = "m", .kind = core::TrackKind::Video, .source_in = 5.0,
               .source_out = 10.0, .start = 0.0};
  a.transition_out = core::Transition{.kind = core::TransitionKind::MorphCut, .duration = 1.0};
  core::Clip b{.id = "b", .media_id = "m", .kind = core::TrackKind::Video, .source_in = 12.0,
               .source_out = 17.0, .start = 5.0};
  p.sequence().tracks[0].clips = {a, b};
  return p;
}

/// A field that moves everything `dx` across.
[[nodiscard]] track::FlowField moving(double dx) {
  track::FlowField field{.columns = 4, .rows = 2};
  field.moves.assign(15, track::Point{dx, 0.0});
  return field;
}

/// The source a mesh pass gives the corner at column `i` of row `j`.
[[nodiscard]] track::Point corner(const EffectPass& pass, int i, int j) {
  const auto columns = static_cast<std::size_t>(pass.data[0]);
  const std::size_t at = kWarpHeader + 2 * (static_cast<std::size_t>(j) * (columns + 1) +
                                            static_cast<std::size_t>(i));
  return {pass.data[at], pass.data[at + 1]};
}

class MorphCutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    folder_ = std::filesystem::temp_directory_path() /
              ("cutline_morph_" + std::to_string(_getpid()) + "_" +
               ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(folder_);
    set_analysis_folder(folder_);
  }
  void TearDown() override {
    set_analysis_folder({});
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

  void analysed(const core::Project& p, std::vector<track::FlowField> frames) {
    const std::optional<MorphJoin> join = morph_cut_of(p, "a");
    ASSERT_TRUE(join.has_value());
    ASSERT_FALSE(join->key.empty());
    ASSERT_TRUE(track::save_morph(track::MorphAnalysis{.fps = 10.0, .frames = std::move(frames)},
                                  morph_analysis_path(join->key)));
  }

  std::filesystem::path folder_;
};

TEST_F(MorphCutTest, IsFoundInTheOpenSequenceWithItsWindow) {
  const core::Project p = jump_cut();
  const std::vector<MorphJoin> joins = morph_cuts(p);
  ASSERT_EQ(joins.size(), 1u);
  EXPECT_EQ(joins[0].out.clip->id, "a");
  EXPECT_EQ(joins[0].in.clip->id, "b");
  EXPECT_DOUBLE_EQ(joins[0].start, 4.5);
  EXPECT_DOUBLE_EQ(joins[0].end, 5.5);
  EXPECT_EQ(morph_frames(p, joins[0].start, joins[0].end), 10);
  EXPECT_DOUBLE_EQ(morph_frame_time(p, 4.5, 0), 4.55);
}

TEST_F(MorphCutTest, IsAPlainDissolveUntilItHasBeenAnalysed) {
  const std::vector<PlannedLayer> layers = plan_frame(jump_cut(), 5.0);
  ASSERT_EQ(layers.size(), 2u);
  EXPECT_FALSE(layers[0].morph.has_value());
  EXPECT_FALSE(layers[1].morph.has_value());
  EXPECT_NEAR(layers[1].alpha, 0.5, 1e-9);
}

TEST_F(MorphCutTest, BendsBothSidesHalfwayTowardsEachOtherAtTheMiddle) {
  const core::Project p = jump_cut();
  analysed(p, std::vector<track::FlowField>(10, moving(0.1)));
  const std::vector<PlannedLayer> layers = plan_frame(p, 5.0);
  ASSERT_EQ(layers.size(), 2u);
  ASSERT_TRUE(layers[0].morph.has_value());
  ASSERT_TRUE(layers[1].morph.has_value());
  EXPECT_EQ(layers[0].morph->kind, EffectPassKind::WarpStabilizer);
  // The outgoing picture shows what was half the move back; the incoming one
  // what is half the move ahead. The middle corner, so nothing is clamped.
  EXPECT_NEAR(corner(*layers[0].morph, 2, 1).x, 0.5 - 0.05, 1e-6);
  EXPECT_NEAR(corner(*layers[1].morph, 2, 1).x, 0.5 + 0.05, 1e-6);
  EXPECT_NEAR(corner(*layers[1].morph, 2, 1).y, 0.5, 1e-6);
}

TEST_F(MorphCutTest, KeepsStepWithTheDissolve) {
  const core::Project p = jump_cut();
  analysed(p, std::vector<track::FlowField>(10, moving(0.1)));
  // A quarter of the way through: the outgoing picture a quarter bent, and
  // the incoming one three quarters still to go, at a quarter opacity.
  const std::vector<PlannedLayer> layers = plan_frame(p, 4.75);
  ASSERT_EQ(layers.size(), 2u);
  EXPECT_NEAR(corner(*layers[0].morph, 2, 1).x, 0.5 - 0.025, 1e-6);
  EXPECT_NEAR(corner(*layers[1].morph, 2, 1).x, 0.5 + 0.075, 1e-6);
  EXPECT_NEAR(layers[1].alpha, 0.25, 1e-9);
}

TEST_F(MorphCutTest, UsesEachFramesOwnField) {
  const core::Project p = jump_cut();
  std::vector<track::FlowField> frames(10, moving(0.0));
  frames[7] = moving(0.2);
  analysed(p, std::move(frames));
  // 5.25 is in the eighth frame of the window, 4.5 + 0.7 to 4.5 + 0.8.
  EXPECT_NEAR(corner(*plan_frame(p, 5.25)[1].morph, 2, 1).x, 0.5 + 0.25 * 0.2, 1e-6);
  EXPECT_NEAR(corner(*plan_frame(p, 5.05)[1].morph, 2, 1).x, 0.5, 1e-6);
}

TEST_F(MorphCutTest, KeepsTheMeshOnTheFrame) {
  const core::Project p = jump_cut();
  analysed(p, std::vector<track::FlowField>(10, moving(0.3)));
  const EffectPass pass = *plan_frame(p, 5.0)[1].morph;
  for (int i = 0; i <= 4; ++i) {
    EXPECT_LE(corner(pass, i, 0).x, 1.0f);
    EXPECT_GE(corner(pass, i, 0).x, 0.0f);
  }
}

TEST_F(MorphCutTest, IsAnalysedAgainWhenEitherSideIsTrimmedOrItIsLengthened) {
  const core::Project p = jump_cut();
  const std::string key = morph_cut_of(p, "a")->key;

  core::Project trimmed = p;
  trimmed.sequence().tracks[0].clips[1].source_in = 12.5;
  EXPECT_NE(morph_cut_of(trimmed, "a")->key, key);

  core::Project longer = p;
  longer.sequence().tracks[0].clips[0].transition_out->duration = 1.5;
  EXPECT_NE(morph_cut_of(longer, "a")->key, key);

  // And the same cut twice is analysed once.
  EXPECT_EQ(morph_cut_of(jump_cut(), "a")->key, key);
}

TEST_F(MorphCutTest, AClipWithoutOneHasNone) {
  EXPECT_FALSE(morph_cut_of(jump_cut(), "b").has_value());
  core::Project dissolve = jump_cut();
  dissolve.sequence().tracks[0].clips[0].transition_out->kind = core::TransitionKind::Dissolve;
  EXPECT_TRUE(morph_cuts(dissolve).empty());
}

}  // namespace
}  // namespace cutline::render
