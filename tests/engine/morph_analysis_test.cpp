/// Morph Cut's looking through the renderer: one take made here — a grey wall
/// and a skin-toned patch drifting across it — cut into twice with a second
/// taken out, so the patch jumps at the cut. The field has to find the jump
/// on the patch and nothing on the wall. The looking itself is tested on its
/// own in the track tests; this is the join between it and the two sides'
/// pictures, handles and held frames included.

#include "cutline/engine/morph_analysis.hpp"

#include "cutline/core/nesting.hpp"
#include "cutline/core/sequences.hpp"
#include "cutline/render/morph_cut.hpp"
#include "cutline/render/plan.hpp"
#include "cutline/render/warp_stabilizer.hpp"

#include <gtest/gtest.h>

namespace cutline::engine {
namespace {

using core::Clip;
using core::Media;
using core::Project;

std::shared_ptr<gpu::Device> shared_device() {
  static std::shared_ptr<gpu::Device> device = [] {
    auto created = gpu::Device::create({.allow_software = true});
    return created ? *created : nullptr;
  }();
  return device;
}

[[nodiscard]] Media matte(const std::string& id, const std::string& color) {
  Media m;
  m.id = id;
  m.is_color = true;
  m.color = color;
  return m;
}

/// A 320 by 180 take, eight seconds long, the patch drifting from 0.2 of the
/// way across to 0.8: 0.075 of the frame a second. Cut at 3 and resumed at 4,
/// so either side of the cut it is 0.075 apart.
[[nodiscard]] Project jump_cut(double morph = 0.5) {
  Project p;
  p.sequence().canvas_w = 320;
  p.sequence().canvas_h = 180;
  p.sequence().fps = 10.0;
  p.media = {matte("wall", "#707070"), matte("skin", "#e0ac8c")};
  p = core::add_sequence(std::move(p), "Take");
  core::Sequence& inner = p.sequences[1];
  inner.canvas_w = 320;
  inner.canvas_h = 180;
  inner.fps = 10.0;
  inner.tracks.insert(inner.tracks.begin(), core::Track{.id = "v2", .kind = core::TrackKind::Video});
  Clip wall{.id = "wall", .media_id = "wall", .source_out = 8.0};
  Clip patch{.id = "patch", .media_id = "skin", .source_out = 8.0};
  patch.transform.scale_x = 0.3;
  patch.transform.scale_y = 0.5;
  patch.keyframes[core::anim_prop_index(core::AnimProp::X)] = {core::Keyframe{.t = 0.0, .v = 0.2},
                                                               core::Keyframe{.t = 8.0, .v = 0.8}};
  inner.tracks[0].clips = {patch};
  inner.tracks[1].clips = {wall};
  Media nest;
  nest.id = "nest";
  nest.sequence_id = inner.id;
  p.media.push_back(nest);
  p = core::sync_nested_media(std::move(p));

  Clip before{.id = "before", .media_id = "nest", .source_in = 1.0, .source_out = 3.0};
  before.transition_out = core::Transition{.kind = core::TransitionKind::MorphCut, .duration = morph};
  Clip after{.id = "after", .media_id = "nest", .source_in = 4.0, .source_out = 6.0, .start = 2.0};
  p.sequence().tracks = {
      core::Track{.id = "v1", .kind = core::TrackKind::Video, .clips = {before, after}}};
  return p;
}

}  // namespace

TEST(MorphThroughTheRenderer, FindsTheJumpOnTheSubjectAndNotOnTheWall) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  const Project p = jump_cut();
  const auto analysis = analyse_morph_cut(shared_device(), p, "before");
  ASSERT_TRUE(analysis.has_value()) << analysis.error();
  ASSERT_EQ(analysis->frames.size(), 5u);
  // Either side of the cut the patch is a second, 0.075 of the frame, apart,
  // whichever frame of the window is looked at: both sides run on through
  // their handles together.
  for (const track::FlowField& field : analysis->frames) {
    EXPECT_NEAR(field.at(0.0, 0.0).x, 0.0, 0.01);
    EXPECT_NEAR(field.at(1.0, 1.0).x, 0.0, 0.01);
  }
  // Where the patch is at the middle of the window, on the outgoing side:
  // the source there is 3.0, so 0.2 + 0.075 * 3.
  const track::FlowField& middle = analysis->frames[2];
  EXPECT_NEAR(middle.at(0.2 + 0.075 * 3.0, 0.5).x, 0.075, 0.03);
  EXPECT_NEAR(middle.at(0.2 + 0.075 * 3.0, 0.5).y, 0.0, 0.02);
}

TEST(MorphThroughTheRenderer, IsSavedUnderTheNameTheRendererLooksFor) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  const Project p = jump_cut();
  const auto folder = std::filesystem::temp_directory_path() / "cutline_morph_engine_test";
  std::filesystem::create_directories(folder);
  render::set_analysis_folder(folder);
  const auto analysis = analyse_morph_cut(shared_device(), p, "before");
  ASSERT_TRUE(analysis.has_value());
  const std::string key = render::morph_cut_of(p, "before")->key;
  ASSERT_TRUE(track::save_morph(*analysis, render::morph_analysis_path(key)));
  const auto layers = render::plan_frame(p, 2.0);
  render::set_analysis_folder({});
  std::filesystem::remove_all(folder);
  ASSERT_EQ(layers.size(), 2u);
  EXPECT_TRUE(layers[0].morph.has_value());
  EXPECT_TRUE(layers[1].morph.has_value());
}

TEST(MorphThroughTheRenderer, CanBeStopped) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  const auto analysis = analyse_morph_cut(shared_device(), jump_cut(), "before",
                                          [](int done, int) { return done < 2; });
  EXPECT_FALSE(analysis.has_value());
}

TEST(MorphThroughTheRenderer, AClipWithoutOneIsRefused) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  EXPECT_FALSE(analyse_morph_cut(shared_device(), jump_cut(), "after").has_value());
}

}  // namespace cutline::engine
