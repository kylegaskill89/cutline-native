/// Scene Edit Detection through the renderer: a clip of a nest that was cut
/// together from three colours, with the cuts found where they are on the
/// timeline. The looking itself is tested on its own in the track tests; this
/// is the join between it, the clip's pictures and the timeline's time.

#include "cutline/engine/scene_detection.hpp"

#include "cutline/core/nesting.hpp"
#include "cutline/core/sequences.hpp"

#include <gtest/gtest.h>

#include <cmath>

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

/// Red, blue and green for a second each, nested, and the clip of the nest
/// from half a second in to two and a half, placed at four seconds.
[[nodiscard]] Project edited_before_it_arrived() {
  Project p;
  p.sequence().canvas_w = 160;
  p.sequence().canvas_h = 90;
  p.sequence().fps = 24.0;
  p.media = {matte("red", "#d02020"), matte("blue", "#2030d0"), matte("green", "#20b040")};
  p = core::add_sequence(std::move(p), "Edited");
  core::Sequence& inner = p.sequences[1];
  inner.canvas_w = 160;
  inner.canvas_h = 90;
  inner.fps = 24.0;
  const char* order[] = {"red", "blue", "green"};
  for (int i = 0; i < 3; ++i) {
    Clip shot;
    shot.id = order[i];
    shot.media_id = order[i];
    shot.start = i;
    shot.source_out = 1.0;
    inner.tracks[0].clips.push_back(shot);
  }
  Media nest;
  nest.id = "nest";
  nest.sequence_id = inner.id;
  p.media.push_back(nest);
  p = core::sync_nested_media(std::move(p));

  Clip outer;
  outer.id = "outer";
  outer.media_id = "nest";
  outer.start = 4.0;
  outer.source_in = 0.5;
  outer.source_out = 2.5;
  core::Track track;
  track.id = "v1";
  track.kind = core::TrackKind::Video;
  track.clips = {outer};
  p.sequence().tracks = {track};
  return p;
}

}  // namespace

TEST(SceneDetectionThroughTheRenderer, FindsTheCutsWhereTheyAreOnTheTimeline) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  int last_total = 0;
  const auto cuts = detect_scene_edits(shared_device(), edited_before_it_arrived(), "outer",
                                       [&](int, int total) {
                                         last_total = total;
                                         return true;
                                       });
  ASSERT_TRUE(cuts.has_value()) << cuts.error();
  ASSERT_EQ(cuts->size(), 2u);
  EXPECT_NEAR((*cuts)[0], 4.5, 0.5 / 24.0);
  EXPECT_NEAR((*cuts)[1], 5.5, 0.5 / 24.0);
  EXPECT_EQ(last_total, 48);
}

TEST(SceneDetectionThroughTheRenderer, CanBeStopped) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  const auto cuts = detect_scene_edits(shared_device(), edited_before_it_arrived(), "outer",
                                       [](int done, int) { return done < 5; });
  EXPECT_FALSE(cuts.has_value());
}

TEST(SceneDetectionThroughTheRenderer, RefusesAClipWithNoPicture) {
  if (!shared_device()) GTEST_SKIP() << "no Direct3D 12 device available";
  Project p = edited_before_it_arrived();
  p.sequence().tracks[0].kind = core::TrackKind::Audio;
  p.sequence().tracks[0].clips[0].kind = core::TrackKind::Audio;
  EXPECT_FALSE(detect_scene_edits(shared_device(), p, "outer").has_value());
  EXPECT_FALSE(detect_scene_edits(shared_device(), p, "nothing").has_value());
}

}  // namespace cutline::engine
