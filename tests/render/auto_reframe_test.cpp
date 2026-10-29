/// The Auto Reframe effect's geometry: a wide shot in a vertical sequence
/// filled rather than fitted, the window put over the analysed subject but
/// never past the picture's edge, and Premiere's three adjustments on top.

#include "cutline/render/auto_reframe.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/render/plan.hpp"
#include "cutline/render/warp_stabilizer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <process.h>
#include <string>

namespace cutline::render {
namespace {

/// A 1920 by 1080 shot filling a 1080 by 1920 sequence: 3413 wide, 1920 tall.
constexpr double kFilledWidth = 1920.0 * 1920.0 / 1080.0;

[[nodiscard]] core::Project vertical(std::map<std::string, double> params = {}) {
  core::Project p = core::empty_project(1, 0);
  p.sequence().canvas_w = 1080;
  p.sequence().canvas_h = 1920;
  core::Media m;
  m.id = "m";
  m.path = "D:/no/such/footage.mp4";
  m.duration = 10.0;
  m.has_video = true;
  m.width = 1920;
  m.height = 1080;
  p.media = {m};
  core::Clip c{.id = "c", .media_id = "m", .kind = core::TrackKind::Video, .source_out = 4.0};
  c.effects.push_back(core::ClipEffect{.type = "autoreframe", .params = std::move(params)});
  p.sequence().tracks[0].clips.push_back(c);
  return p;
}

[[nodiscard]] core::LayerBox box_of(const core::Project& p, double t = 1.0) {
  const std::vector<PlannedLayer> layers = plan_frame(p, t);
  EXPECT_EQ(layers.size(), 1u);
  return layers.empty() ? core::LayerBox{} : layers.front().box;
}

/// Each test's analyses in a folder of its own.
class AutoReframeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    folder_ = std::filesystem::temp_directory_path() /
              ("cutline_reframe_" + std::to_string(_getpid()) + "_" +
               ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(folder_);
    set_analysis_folder(folder_);
  }
  void TearDown() override {
    set_analysis_folder({});
    std::error_code ignored;
    std::filesystem::remove_all(folder_, ignored);
  }

  /// An analysis that found the subject at `x` through the whole clip.
  void analysed_at(const core::Project& p, double x) {
    track::ReframeAnalysis analysis{.fps = 10.0,
                                    .window_width = 1080.0 / kFilledWidth,
                                    .window_height = 1.0,
                                    .subjects = std::vector<track::Point>(40, {x, 0.5})};
    const std::string key = reframe_key(p, p.sequence().tracks[0].clips[0]);
    ASSERT_FALSE(key.empty());
    ASSERT_TRUE(track::save_reframe(analysis, reframe_analysis_path(key)));
  }

  std::filesystem::path folder_;
};

TEST_F(AutoReframeTest, FillsTheNewShapeRatherThanFittingInsideIt) {
  const core::LayerBox box = box_of(vertical());
  EXPECT_NEAR(box.width, kFilledWidth, 0.5);
  EXPECT_NEAR(box.height, 1920.0, 0.5);
  EXPECT_NEAR(box.center_x, 540.0, 1e-6) << "the middle, until it has been analysed";
}

TEST_F(AutoReframeTest, PutsTheAnalysedSubjectInTheMiddleOfTheNewFrame) {
  const core::Project p = vertical();
  analysed_at(p, 0.7);
  const core::LayerBox box = box_of(p);
  const double left = box.center_x - box.width / 2.0;
  EXPECT_NEAR(left + 0.7 * box.width, 540.0, 0.5);
}

TEST_F(AutoReframeTest, NeverShowsPastThePicturesEdge) {
  const core::Project p = vertical();
  analysed_at(p, 0.99);
  const core::LayerBox box = box_of(p);
  EXPECT_NEAR(box.center_x + box.width / 2.0, 1080.0, 0.5);
}

TEST_F(AutoReframeTest, OffsetScaleAndRotationAdjustWhatWasFound) {
  const core::LayerBox plain = box_of(vertical());
  const core::LayerBox adjusted =
      box_of(vertical({{"offset_x", 100.0}, {"offset_y", -50.0}, {"scale", 150.0},
                       {"rotation", 10.0}}));
  EXPECT_NEAR(adjusted.width, plain.width * 1.5, 0.5);
  EXPECT_NEAR(adjusted.center_x, plain.center_x + 100.0, 1e-6);
  EXPECT_NEAR(adjusted.center_y, plain.center_y - 50.0, 1e-6);
  EXPECT_NEAR(adjusted.rotation_deg, 10.0, 1e-9);
}

TEST_F(AutoReframeTest, AWindowOfTheSequencesShapeIsWhatIsLookedFor) {
  const core::Project p = vertical();
  const ReframeWindow window = reframe_window(p, p.sequence().tracks[0].clips[0]);
  EXPECT_NEAR(window.width, 1080.0 / kFilledWidth, 1e-9);
  EXPECT_DOUBLE_EQ(window.height, 1.0);
}

TEST_F(AutoReframeTest, ASwitchedOffEffectLeavesTheClipFitted) {
  core::Project p = vertical();
  p.sequence().tracks[0].clips[0].effects[0].enabled = false;
  EXPECT_NEAR(box_of(p).width, 1080.0, 0.5);
}

}  // namespace
}  // namespace cutline::render
