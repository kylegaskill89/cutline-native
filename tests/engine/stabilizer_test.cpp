/// Warp Stabilizer through the renderer: a shot that shakes, analysed, and
/// drawn steadier than it was. The stabiliser's own arithmetic is tested on
/// its own in the track tests; this is the join between it, the analysis, the
/// cache of analyses and the pass.

#include "cutline/engine/frame_renderer.hpp"
#include "cutline/engine/stabilizer_analysis.hpp"
#include "cutline/render/warp_stabilizer.hpp"

#include "cutline/core/nesting.hpp"
#include "cutline/core/sequences.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <format>
#include <process.h>
#include <random>

namespace cutline::engine {
namespace {

using core::Clip;
using core::Media;
using core::Project;

constexpr int kWidth = 320;
constexpr int kHeight = 180;
constexpr int kFrames = 48;

std::shared_ptr<gpu::Device> shared_device() {
  static std::shared_ptr<gpu::Device> device = [] {
    auto created = gpu::Device::create({.allow_software = true});
    return created ? *created : nullptr;
  }();
  return device;
}

/// Squares of many sizes and shades scattered over the frame.
[[nodiscard]] Media scatter() {
  Media m;
  m.id = "g";
  m.is_graphic = true;
  core::Graphic graphic;
  std::mt19937 random(5);
  std::uniform_real_distribution<double> place(-0.1, 1.1);
  std::uniform_real_distribution<double> size(6.0, 22.0);
  std::uniform_int_distribution<int> shade(40, 255);
  for (int i = 0; i < 90; ++i) {
    core::GraphicLayer layer;
    layer.id = "s" + std::to_string(i);
    layer.kind = core::GraphicKind::Rectangle;
    layer.width = size(random);
    layer.height = size(random);
    layer.x = place(random);
    layer.y = place(random);
    layer.style.fill = std::format("#{:02x}{:02x}{:02x}", shade(random), shade(random),
                                   shade(random));
    graphic.layers.push_back(std::move(layer));
  }
  m.graphic = std::move(graphic);
  return m;
}

/// The squares in a nest, shaken a few pixels every frame, with Warp
/// Stabilizer on the clip that holds them.
[[nodiscard]] Project shaking() {
  Project p;
  p.sequence().canvas_w = kWidth;
  p.sequence().canvas_h = kHeight;
  p.sequence().fps = 24.0;
  p.media = {scatter()};
  p = core::add_sequence(std::move(p), "Inner");
  core::Sequence& inner = p.sequences[1];
  inner.canvas_w = kWidth;
  inner.canvas_h = kHeight;
  inner.fps = 24.0;
  Clip squares;
  squares.id = "squares";
  squares.media_id = "g";
  squares.source_out = 5.0;
  std::mt19937 random(9);
  std::normal_distribution<double> shake(0.0, 4.0);
  auto& xs = squares.keyframes[static_cast<std::size_t>(core::AnimProp::X)];
  auto& ys = squares.keyframes[static_cast<std::size_t>(core::AnimProp::Y)];
  for (int i = 0; i <= kFrames; ++i) {
    xs.push_back(core::Keyframe{.t = i / 24.0, .v = 0.5 + shake(random) / kWidth});
    ys.push_back(core::Keyframe{.t = i / 24.0, .v = 0.5 + shake(random) / kHeight});
  }
  inner.tracks[0].clips = {squares};

  Media nest;
  nest.id = "nest";
  nest.sequence_id = inner.id;
  p.media.push_back(nest);
  p = core::sync_nested_media(std::move(p));

  Clip outer;
  outer.id = "outer";
  outer.media_id = "nest";
  outer.source_out = kFrames / 24.0;
  outer.effects = {core::ClipEffect{.type = "warpstabilizer"}};
  core::Track track;
  track.id = "v1";
  track.kind = core::TrackKind::Video;
  track.clips = {outer};
  p.sequence().tracks = {track};
  return p;
}

class StabilizerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
    folder_ = std::filesystem::temp_directory_path() /
              ("cutline_analysis_" + std::to_string(_getpid()));
    std::filesystem::create_directories(folder_);
    render::set_analysis_folder(folder_);
  }
  void TearDown() override {
    render::set_analysis_folder({});
    std::error_code error;
    std::filesystem::remove_all(folder_, error);
  }

  /// How much the middle of the picture changes from frame to frame, on
  /// average: low when it holds still.
  [[nodiscard]] double flicker(const Project& p) {
    auto renderer = FrameRenderer::create(shared_device(), kWidth, kHeight);
    EXPECT_TRUE(renderer.has_value());
    std::vector<std::uint8_t> before;
    double total = 0.0;
    int counted = 0;
    for (int i = 4; i < kFrames - 4; ++i) {
      EXPECT_TRUE((*renderer)->render(p, i / 24.0).has_value());
      const auto image = (*renderer)->read_back();
      EXPECT_TRUE(image.has_value());
      if (!before.empty()) {
        double sum = 0.0;
        int n = 0;
        for (int y = kHeight / 4; y < kHeight * 3 / 4; ++y) {
          for (int x = kWidth / 4; x < kWidth * 3 / 4; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            sum += std::abs(static_cast<int>(image->pixels[at]) - before[at]);
            ++n;
          }
        }
        total += sum / n;
        ++counted;
      }
      before = image->pixels;
    }
    return total / counted;
  }

  std::filesystem::path folder_;
};

TEST_F(StabilizerTest, BeforeItIsAnalysedTheClipIsDrawnAsItIs) {
  const Project p = shaking();
  const Clip& clip = p.sequence().tracks[0].clips[0];
  EXPECT_FALSE(render::has_analysis(render::analysis_key(p, clip, clip.effects[0])));
  EXPECT_TRUE(render::plan_effect_passes(clip, 1.0, 1.0 / 24.0, &p).empty());
}

TEST_F(StabilizerTest, AnAnalysedShakingShotIsDrawnSteadier) {
  Project p = shaking();
  const Clip& clip = p.sequence().tracks[0].clips[0];
  const auto analysis = analyse_for_stabilizer(shared_device(), p, "outer", 0);
  ASSERT_TRUE(analysis.has_value()) << analysis.error();
  EXPECT_EQ(analysis->frames.size(), static_cast<std::size_t>(kFrames));
  const std::string key = render::analysis_key(p, clip, clip.effects[0]);
  ASSERT_TRUE(track::save_analysis(*analysis, render::analysis_path(key)));
  ASSERT_TRUE(render::has_analysis(key));

  const double steadied = flicker(p);
  p.sequence().tracks[0].clips[0].effects[0].enabled = false;
  const double shaken = flicker(p);
  EXPECT_LT(steadied, shaken * 0.5) << steadied << " against " << shaken;
}

TEST_F(StabilizerTest, TrimmingTheClipNamesADifferentAnalysis) {
  Project p = shaking();
  const Clip& clip = p.sequence().tracks[0].clips[0];
  const std::string before = render::analysis_key(p, clip, clip.effects[0]);
  p.sequence().tracks[0].clips[0].source_in = 0.5;
  const Clip& trimmed = p.sequence().tracks[0].clips[0];
  EXPECT_NE(render::analysis_key(p, trimmed, trimmed.effects[0]), before);
  // Settings that do not change what is analysed leave the name alone.
  p.sequence().tracks[0].clips[0].effects[0].params["smoothness"] = 80.0;
  EXPECT_EQ(render::analysis_key(p, trimmed, p.sequence().tracks[0].clips[0].effects[0]),
            render::analysis_key(p, trimmed, trimmed.effects[0]));
}

}  // namespace
}  // namespace cutline::engine
