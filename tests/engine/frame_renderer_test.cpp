/// End-to-end tests: a project goes in, pixels come out.
///
/// Most of these use generated media only, so they exercise the whole chain —
/// plan, geometry, effects, compositing, readback — without needing a video
/// file, and therefore run in CI. The ones that do need footage skip unless
/// CUTLINE_TEST_MEDIA_DIR points at it.

#include "cutline/engine/frame_renderer.hpp"

#include "cutline/core/model.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

namespace cutline::engine {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

constexpr int kWidth = 64;
constexpr int kHeight = 64;

std::shared_ptr<gpu::Device> shared_device() {
  static std::shared_ptr<gpu::Device> device = [] {
    auto created = gpu::Device::create({.allow_software = true});
    return created ? *created : nullptr;
  }();
  return device;
}

struct Rgba {
  int r = 0;
  int g = 0;
  int b = 0;
  int a = 0;

  friend bool operator==(const Rgba&, const Rgba&) = default;
};

[[nodiscard]] Rgba pixel_at(const gpu::Image& image, int x, int y) {
  const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4;
  return {image.pixels[index], image.pixels[index + 1], image.pixels[index + 2],
          image.pixels[index + 3]};
}

Media matte(std::string id, std::string color) {
  Media m;
  m.id = std::move(id);
  m.is_color = true;
  m.color = std::move(color);
  return m;
}

Clip clip(std::string id, std::string media_id, double start, double length) {
  Clip c;
  c.id = std::move(id);
  c.media_id = std::move(media_id);
  c.start = start;
  c.source_in = 0.0;
  c.source_out = length;
  return c;
}

Track video_track(std::string id, std::vector<Clip> clips) {
  Track t;
  t.id = std::move(id);
  t.kind = TrackKind::Video;
  t.clips = std::move(clips);
  return t;
}

Project canvas_project() {
  Project p;
  p.canvas_w = kWidth;
  p.canvas_h = kHeight;
  return p;
}

class FrameRendererTest : public ::testing::Test {
 protected:
  void SetUp() override {
    device_ = shared_device();
    if (!device_) GTEST_SKIP() << "no Direct3D 12 device available";

    auto created = FrameRenderer::create(device_, kWidth, kHeight);
    ASSERT_TRUE(created.has_value()) << created.error();
    renderer_ = std::move(*created);
  }

  /// Renders without reading back, for the cases that only care about the side
  /// effect: advancing a decoder, or populating the missing-media report.
  void render_only(const Project& project, double t) {
    auto ok = renderer_->render(project, t);
    EXPECT_TRUE(ok.has_value()) << (ok ? "" : ok.error());
  }

  [[nodiscard]] gpu::Image render(const Project& project, double t) {
    auto ok = renderer_->render(project, t);
    EXPECT_TRUE(ok.has_value()) << (ok ? "" : ok.error());

    auto image = renderer_->read_back();
    EXPECT_TRUE(image.has_value()) << (image ? "" : image.error());
    return image ? *image : gpu::Image{};
  }

  std::shared_ptr<gpu::Device> device_;
  std::unique_ptr<FrameRenderer> renderer_;
};

// ------------------------------------------------------------------ basics --

TEST_F(FrameRendererTest, AnEmptyProjectRendersNothing) {
  const gpu::Image image = render(canvas_project(), 0.0);
  ASSERT_FALSE(image.empty());
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight / 2).a, 0);
}

TEST_F(FrameRendererTest, TheCanvasSizeComesFromTheProject) {
  Project p = canvas_project();
  p.canvas_w = 32;
  p.canvas_h = 16;

  const gpu::Image image = render(p, 0.0);
  EXPECT_EQ(image.width, 32);
  EXPECT_EQ(image.height, 16);
}

TEST_F(FrameRendererTest, AColourMatteFillsTheFrame) {
  Project p = canvas_project();
  p.media = {matte("m", "#ff0000")};
  p.tracks = {video_track("v1", {clip("c", "m", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 2.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.r, 250);
  EXPECT_LT(centre.g, 5);
  EXPECT_EQ(centre.a, 255);
}

TEST_F(FrameRendererTest, NothingDrawsOutsideTheClipsSpan) {
  Project p = canvas_project();
  p.media = {matte("m", "#ff0000")};
  p.tracks = {video_track("v1", {clip("c", "m", 0.0, 5.0)})};

  EXPECT_EQ(pixel_at(render(p, 6.0), kWidth / 2, kHeight / 2).a, 0);
}

TEST_F(FrameRendererTest, AMalformedColourFallsBackRatherThanVanishing) {
  Project p = canvas_project();
  p.media = {matte("m", "not a colour")};
  p.tracks = {video_track("v1", {clip("c", "m", 0.0, 5.0)})};

  EXPECT_EQ(pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2).a, 255);
}

// -------------------------------------------------------------- draw order --

TEST_F(FrameRendererTest, TheTopTrackDrawsOverTheBottomOne) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"), matte("green", "#00ff00")};
  // Stored top-first.
  p.tracks = {video_track("v2", {clip("top", "green", 0.0, 5.0)}),
              video_track("v1", {clip("bottom", "red", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.g, 250);
  EXPECT_LT(centre.r, 5);
}

TEST_F(FrameRendererTest, AHiddenTrackDoesNotRender) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"), matte("green", "#00ff00")};
  p.tracks = {video_track("v2", {clip("top", "green", 0.0, 5.0)}),
              video_track("v1", {clip("bottom", "red", 0.0, 5.0)})};
  p.tracks[0].hidden = true;

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.r, 250) << "the red beneath should show through";
}

// --------------------------------------------------------------- transform --

TEST_F(FrameRendererTest, TheTransformPlacesTheClipOnTheCanvas) {
  Project p = canvas_project();
  p.media = {matte("m", "#ffffff")};

  Clip c = clip("c", "m", 0.0, 5.0);
  // A quarter-size square in the top-left quadrant.
  c.transform = core::Transform{.x = 0.25, .y = 0.25, .scale_x = 0.5, .scale_y = 0.5};
  p.tracks = {video_track("v1", {c})};

  const gpu::Image image = render(p, 1.0);
  EXPECT_EQ(pixel_at(image, kWidth / 4, kHeight / 4).a, 255);
  EXPECT_EQ(pixel_at(image, kWidth * 3 / 4, kHeight * 3 / 4).a, 0);
}

TEST_F(FrameRendererTest, OpacityCarriesThroughToThePixels) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"), matte("white", "#ffffff")};

  Clip top = clip("top", "white", 0.0, 5.0);
  top.opacity = 0.5;
  p.tracks = {video_track("v2", {top}),
              video_track("v1", {clip("bottom", "red", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.g, 20) << "white at half opacity should lighten the red";
  EXPECT_LT(centre.g, 250);
}

// ----------------------------------------------------------------- effects --

TEST_F(FrameRendererTest, AClipsEffectStackReachesTheShader) {
  Project p = canvas_project();
  p.media = {matte("m", "#ffffff")};

  Clip c = clip("c", "m", 0.0, 5.0);
  c.effects = {core::ClipEffect{.type = "invert", .enabled = true, .params = {{"on", 1.0}}}};
  p.tracks = {video_track("v1", {c})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_LT(centre.r, 5) << "an inverted white matte should be black";
}

TEST_F(FrameRendererTest, EffectKeyframesResolveAtTheRenderedTime) {
  Project p = canvas_project();
  p.media = {matte("m", "#808080")};

  Clip c = clip("c", "m", 0.0, 4.0);
  core::ClipEffect brightness{.type = "brightness", .enabled = true, .params = {{"amount", 0.0}}};
  brightness.keyframes["amount"] = {{.t = 0.0, .v = -100.0}, {.t = 4.0, .v = 100.0}};
  c.effects = {brightness};
  p.tracks = {video_track("v1", {c})};

  // Sampled just inside the clip's end: the span is half-open, so at exactly
  // 4.0 there is nothing to render.
  const int dark = pixel_at(render(p, 0.0), kWidth / 2, kHeight / 2).r;
  const int light = pixel_at(render(p, 3.9), kWidth / 2, kHeight / 2).r;
  EXPECT_LT(dark, light) << "the animated brightness should rise over the clip";
}

TEST_F(FrameRendererTest, AnAdjustmentLayerAffectsTheTracksBelow) {
  Project p = canvas_project();

  Media adjustment;
  adjustment.id = "adj";
  adjustment.is_adjustment = true;
  p.media = {matte("white", "#ffffff"), adjustment};

  Clip layer = clip("adj", "adj", 0.0, 5.0);
  layer.effects = {core::ClipEffect{.type = "invert", .enabled = true, .params = {{"on", 1.0}}}};

  p.tracks = {video_track("v2", {layer}),
              video_track("v1", {clip("base", "white", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_LT(centre.r, 5) << "the white beneath should have been inverted";
}

// -------------------------------------------------------------- transitions --

TEST_F(FrameRendererTest, ADissolveMixesTheTwoClips) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"), matte("blue", "#0000ff")};

  Clip a = clip("a", "red", 0.0, 4.0);
  a.transition_out = core::Transition{.kind = core::TransitionKind::Dissolve, .duration = 2.0};
  Clip b = clip("b", "blue", 4.0, 4.0);
  p.tracks = {video_track("v1", {a, b})};

  // The incoming clip has no head handle -- its source_in is zero -- so the
  // overlap is borrowed entirely from the outgoing clip's tail and runs
  // [4, 5]. At exactly 4.0 the fade-in has not started, so the midpoint is
  // where both are genuinely contributing.
  const Rgba centre = pixel_at(render(p, 4.5), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.r, 10) << "the outgoing clip should still show";
  EXPECT_GT(centre.b, 10) << "the incoming clip should already show";
}

// ------------------------------------------------------------ missing media --

TEST_F(FrameRendererTest, AClipWithNoMediaIsReportedRatherThanFailingTheFrame) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000")};
  p.tracks = {video_track("v2", {clip("orphan", "gone", 0.0, 5.0)}),
              video_track("v1", {clip("base", "red", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  EXPECT_GT(pixel_at(image, kWidth / 2, kHeight / 2).r, 250)
      << "the rest of the frame should still render";
  EXPECT_FALSE(renderer_->missing_media().empty());
}

TEST_F(FrameRendererTest, AnUnreadableFileIsReportedRatherThanFailingTheFrame) {
  Project p = canvas_project();

  Media broken;
  broken.id = "broken";
  broken.path = "d:/no/such/file.mp4";
  broken.has_video = true;
  broken.duration = 10.0;

  p.media = {matte("red", "#ff0000"), broken};
  p.tracks = {video_track("v2", {clip("bad", "broken", 0.0, 5.0)}),
              video_track("v1", {clip("base", "red", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  EXPECT_GT(pixel_at(image, kWidth / 2, kHeight / 2).r, 250);
  EXPECT_FALSE(renderer_->missing_media().empty());
}

TEST_F(FrameRendererTest, MissingMediaIsClearedBetweenFrames) {
  Project good = canvas_project();
  good.media = {matte("red", "#ff0000")};
  good.tracks = {video_track("v1", {clip("base", "red", 0.0, 5.0)})};

  Project bad = good;
  bad.tracks.insert(bad.tracks.begin(), video_track("v2", {clip("orphan", "gone", 0.0, 5.0)}));

  render_only(bad, 1.0);
  EXPECT_FALSE(renderer_->missing_media().empty());

  render_only(good, 1.0);
  EXPECT_TRUE(renderer_->missing_media().empty()) << "a stale report would mislead the UI";
}

// ----------------------------------------------------------- real footage --

/// The reference clip, if the environment points at it.
[[nodiscard]] std::string reference_clip() {
  const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
  if (dir == nullptr) return {};

  const std::filesystem::path path = std::filesystem::path(dir) / "Boiler.mp4";
  return std::filesystem::exists(path) ? path.generic_string() : std::string{};
}

class FootageTest : public FrameRendererTest {
 protected:
  void SetUp() override {
    FrameRendererTest::SetUp();
    if (IsSkipped()) return;

    path_ = reference_clip();
    if (path_.empty()) {
      GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to a directory containing Boiler.mp4";
    }
  }

  [[nodiscard]] Project with_video() const {
    Project p = canvas_project();

    Media m;
    m.id = "v";
    m.path = path_;
    m.has_video = true;
    m.duration = 60.0;
    m.width = 3840;
    m.height = 2160;

    p.media = {m};
    p.tracks = {video_track("v1", {clip("c", "v", 0.0, 10.0)})};
    return p;
  }

  std::string path_;
};

TEST_F(FootageTest, RendersActualPictureContent) {
  const gpu::Image image = render(with_video(), 1.0);
  ASSERT_FALSE(image.empty());
  EXPECT_TRUE(renderer_->missing_media().empty());

  // A frame of real footage is not one flat colour. Comparing scattered pixels
  // catches a decoder that silently produced nothing.
  const Rgba a = pixel_at(image, kWidth / 4, kHeight / 4);
  const Rgba b = pixel_at(image, kWidth * 3 / 4, kHeight * 3 / 4);
  EXPECT_NE(a, b);
  EXPECT_EQ(a.a, 255);
}

// --------------------------------------------------------- decode economy --
//
// A preview that seeks per frame and one that decodes through look identical
// from outside and cost very differently: a seek is roughly seventeen times a
// sequential decode, because it re-decodes from a keyframe.

TEST_F(FootageTest, PlayingForwardsDecodesEachFrameOnce) {
  const Project p = with_video();
  constexpr double kFps = 60.0;
  constexpr int kFrames = 90;

  for (int i = 0; i < kFrames; ++i) {
    ASSERT_TRUE(renderer_->render(p, i / kFps).has_value());
  }

  const auto stats = renderer_->decode_stats();
  // One seek to position the source at the start, and none after it.
  EXPECT_LE(stats.seeks, 1);
  // About one source frame per output frame, with slack for the decoder
  // settling onto the first.
  EXPECT_LT(stats.frames_decoded, kFrames + 10);
}

TEST_F(FootageTest, AStepSmallerThanAFrameDoesNotSeek) {
  // The decoder stops at the first frame reaching the request, so it sits up to
  // one frame *ahead* of what was asked for. A later request closer than that
  // overshoot used to read as a move backwards and cost a whole GOP: 17 seeks
  // and 1827 decoded frames over six seconds of playback, against 384 once it
  // was tolerated.
  const Project p = with_video();

  ASSERT_TRUE(renderer_->render(p, 1.0).has_value());
  const auto after_first = renderer_->decode_stats();

  // Steps of a third of a frame, which a playhead driven by an audio clock
  // produces constantly.
  for (int i = 1; i <= 30; ++i) {
    ASSERT_TRUE(renderer_->render(p, 1.0 + i / 180.0).has_value());
  }

  const auto stats = renderer_->decode_stats();
  EXPECT_EQ(stats.seeks, after_first.seeks) << "a sub-frame step forced a seek";
  EXPECT_EQ(stats.backward_seeks, after_first.backward_seeks);
}

TEST_F(FootageTest, AGenuineJumpBackwardsStillSeeks) {
  // The tolerance must not swallow a real backwards move, or scrubbing would
  // show a stale frame.
  const Project p = with_video();
  ASSERT_TRUE(renderer_->render(p, 5.0).has_value());
  const auto before = renderer_->decode_stats();

  ASSERT_TRUE(renderer_->render(p, 1.0).has_value());
  EXPECT_GT(renderer_->decode_stats().backward_seeks, before.backward_seeks);
}

TEST_F(FootageTest, DifferentTimesGiveDifferentFrames) {
  const Project p = with_video();
  const gpu::Image early = render(p, 0.5);
  const gpu::Image late = render(p, 6.0);

  bool differs = false;
  for (std::size_t i = 0; i < early.pixels.size() && !differs; ++i) {
    differs = early.pixels[i] != late.pixels[i];
  }
  EXPECT_TRUE(differs) << "the playhead does not appear to be moving the source";
}

TEST_F(FootageTest, RenderingForwardsAndSeekingBackAgree) {
  // Playback decodes forwards; scrubbing seeks. Both must land on the same
  // picture, or preview and export would disagree about what a frame is.
  const Project p = with_video();

  render_only(p, 0.5);
  render_only(p, 1.0);
  const gpu::Image forwards = render(p, 2.0);

  renderer_->release_sources();
  const gpu::Image sought = render(p, 2.0);

  ASSERT_EQ(forwards.pixels.size(), sought.pixels.size());
  std::size_t differing = 0;
  for (std::size_t i = 0; i < forwards.pixels.size(); ++i) {
    if (forwards.pixels[i] != sought.pixels[i]) ++differing;
  }
  EXPECT_EQ(differing, 0u) << differing << " of " << forwards.pixels.size()
                           << " bytes differ between decoding forwards and seeking";
}

TEST_F(FootageTest, AnEffectAppliesToRealFootage) {
  Project plain = with_video();

  Project inverted = plain;
  inverted.tracks[0].clips[0].effects = {
      core::ClipEffect{.type = "invert", .enabled = true, .params = {{"on", 1.0}}}};

  const gpu::Image before = render(plain, 1.0);
  renderer_->release_sources();
  const gpu::Image after = render(inverted, 1.0);

  const Rgba a = pixel_at(before, kWidth / 2, kHeight / 2);
  const Rgba b = pixel_at(after, kWidth / 2, kHeight / 2);
  // Inversion is its own opposite, so the two should sit either side of mid.
  EXPECT_NEAR(a.r + b.r, 255, 12);
}

}  // namespace
}  // namespace cutline::engine
