/// Auto Reframe's looking through the renderer: a wide shot made here — a
/// grey wall and a small skin-toned patch walking across it — reframed into
/// a vertical sequence, and the window found where the patch is as it moves.
/// The looking itself is tested on its own in the track tests; this is the
/// join between it, the clip's pictures and the window's shape.

#include "cutline/engine/reframe_analysis.hpp"

#include "cutline/core/nesting.hpp"
#include "cutline/core/sequences.hpp"
#include "cutline/render/auto_reframe.hpp"

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

/// A 320 by 180 nest, two seconds long: the patch walks from 0.8 of the way
/// across to 0.6. Its clip on V1 of a 360 by 640 sequence, with Auto Reframe.
[[nodiscard]] Project walking() {
  Project p;
  p.sequence().canvas_w = 360;
  p.sequence().canvas_h = 640;
  p.sequence().fps = 24.0;
  p.media = {matte("wall", "#707070"), matte("skin", "#e0ac8c")};
  p = core::add_sequence(std::move(p), "Wide");
  core::Sequence& inner = p.sequences[1];
  inner.canvas_w = 320;
  inner.canvas_h = 180;
  inner.fps = 24.0;
  inner.tracks.insert(inner.tracks.begin(), core::Track{.id = "v2", .kind = core::TrackKind::Video});
  Clip wall{.id = "wall", .media_id = "wall", .source_out = 2.0};
  Clip patch{.id = "patch", .media_id = "skin", .source_out = 2.0};
  patch.transform.scale_x = 0.12;
  patch.transform.scale_y = 0.3;
  patch.keyframes[core::anim_prop_index(core::AnimProp::X)] = {core::Keyframe{.t = 0.0, .v = 0.8},
                                                               core::Keyframe{.t = 2.0, .v = 0.6}};
  inner.tracks[0].clips = {patch};
  inner.tracks[1].clips = {wall};
  Media nest;
  nest.id = "nest";
  nest.sequence_id = inner.id;
  p.media.push_back(nest);
  p = core::sync_nested_media(std::move(p));

  Clip outer{.id = "outer", .media_id = "nest", .source_out = 2.0};
  outer.effects.push_back(core::ClipEffect{.type = "autoreframe"});
  p.sequence().tracks = {core::Track{.id = "v1", .kind = core::TrackKind::Video, .clips = {outer}}};
  return p;
}

}  // namespace

TEST(ReframeThroughTheRenderer, FindsTheWindowWhereTheSubjectIsAsItMoves) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  const Project p = walking();
  const auto analysis = analyse_for_reframe(shared_device(), p, "outer");
  ASSERT_TRUE(analysis.has_value()) << analysis.error();
  ASSERT_EQ(analysis->subjects.size(), 20u);
  EXPECT_NEAR(analysis->window_width, (360.0 / 640.0) / (320.0 / 180.0), 1e-6);
  EXPECT_NEAR(analysis->subjects.front().x, 0.8, 0.05);
  EXPECT_NEAR(analysis->subjects.back().x, 0.6 + 0.2 / 20.0, 0.05);
  EXPECT_TRUE(analysis->cuts.empty());
}

TEST(ReframeThroughTheRenderer, CanBeStopped) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  const auto analysis =
      analyse_for_reframe(shared_device(), walking(), "outer", [](int done, int) { return done < 3; });
  EXPECT_FALSE(analysis.has_value());
}

}  // namespace cutline::engine

