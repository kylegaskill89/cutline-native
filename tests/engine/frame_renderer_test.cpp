/// End-to-end tests: a project goes in, pixels come out.
///
/// Most of these use generated media only, so they exercise the whole chain —
/// plan, geometry, effects, compositing, readback — without needing a video
/// file, and therefore run in CI. The ones that do need footage skip unless
/// CUTLINE_TEST_MEDIA_DIR points at it.

#include "cutline/engine/frame_renderer.hpp"

#include "cutline/core/graphic.hpp"
#include "cutline/core/interpret.hpp"
#include "cutline/core/captions.hpp"
#include "cutline/core/nesting.hpp"
#include "cutline/core/sequences.hpp"
#include "cutline/core/model.hpp"
#include "cutline/media/encoder.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

#include <process.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <system_error>
#include <vector>

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
  p.sequence().canvas_w = kWidth;
  p.sequence().canvas_h = kHeight;
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
  p.sequence().canvas_w = 32;
  p.sequence().canvas_h = 16;

  const gpu::Image image = render(p, 0.0);
  EXPECT_EQ(image.width, 32);
  EXPECT_EQ(image.height, 16);
}

TEST_F(FrameRendererTest, AColourMatteFillsTheFrame) {
  Project p = canvas_project();
  p.media = {matte("m", "#ff0000")};
  p.sequence().tracks = {video_track("v1", {clip("c", "m", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 2.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.r, 250);
  EXPECT_LT(centre.g, 5);
  EXPECT_EQ(centre.a, 255);
}

TEST_F(FrameRendererTest, NothingDrawsOutsideTheClipsSpan) {
  Project p = canvas_project();
  p.media = {matte("m", "#ff0000")};
  p.sequence().tracks = {video_track("v1", {clip("c", "m", 0.0, 5.0)})};

  EXPECT_EQ(pixel_at(render(p, 6.0), kWidth / 2, kHeight / 2).a, 0);
}

TEST_F(FrameRendererTest, AMalformedColourFallsBackRatherThanVanishing) {
  Project p = canvas_project();
  p.media = {matte("m", "not a colour")};
  p.sequence().tracks = {video_track("v1", {clip("c", "m", 0.0, 5.0)})};

  EXPECT_EQ(pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2).a, 255);
}

// ------------------------------------------------------------------- nests --
//
// A nest is a compositing boundary: its clips are composited together and the
// result is what the outer clip's effects run on. Nothing but pixels can say
// whether that happened — the arithmetic is right in every layer while the
// picture is still wrong if the boundary is not there.

TEST_F(FrameRendererTest, ANestDrawsTheSequenceInsideIt) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000")};

  // A second sequence holding the matte, and a pool entry standing for it.
  p = core::add_sequence(std::move(p), "Inner");
  p.sequences[1].canvas_w = kWidth;
  p.sequences[1].canvas_h = kHeight;
  p.sequences[1].tracks[0].clips = {clip("inner", "red", 0.0, 5.0)};

  core::Media entry;
  entry.id = "nest";
  entry.sequence_id = p.sequences[1].id;
  p.media.push_back(entry);
  p = core::sync_nested_media(std::move(p));

  p.sequence().tracks = {video_track("v1", {clip("outer", "nest", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.r, 250) << "the sequence inside the nest never composited";
  EXPECT_LT(centre.g, 5);
  EXPECT_EQ(centre.a, 255);
}

TEST_F(FrameRendererTest, AMulticamDrawsOnlyTheCameraItShows) {
  // Camera 1 is red on V1, camera 2 blue on V2 above it. As a nest the blue
  // would cover the red; as a multicam each piece shows its own camera, and a
  // dissolve between two of them shows both at once.
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"), matte("blue", "#0000ff")};
  p = core::add_sequence(std::move(p), "Shoot", 2, 0);
  core::Sequence& shoot = p.sequences[1];
  shoot.canvas_w = kWidth;
  shoot.canvas_h = kHeight;
  shoot.multicam = true;
  shoot.tracks[0].clips = {clip("two", "blue", 0.0, 10.0)};
  shoot.tracks[1].clips = {clip("one", "red", 0.0, 10.0)};

  core::Media entry;
  entry.id = "shoot";
  entry.sequence_id = shoot.id;
  p.media.push_back(entry);
  p = core::sync_nested_media(std::move(p));

  Clip first = clip("first", "shoot", 0.0, 4.0);
  first.transition_out = core::Transition{.kind = core::TransitionKind::Dissolve, .duration = 2.0};
  Clip second = clip("second", "shoot", 4.0, 4.0);
  second.source_in = 4.0;
  second.source_out = 8.0;
  second.camera = 1;
  p.sequence().tracks = {video_track("v1", {first, second})};

  const Rgba one = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(one.r, 250) << "the first camera is red";
  EXPECT_LT(one.b, 5);
  const Rgba two = pixel_at(render(p, 6.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(two.b, 250) << "the second camera is blue";
  EXPECT_LT(two.r, 5);
  const Rgba both = pixel_at(render(p, 4.5), kWidth / 2, kHeight / 2);
  EXPECT_GT(both.r, 10) << "the outgoing camera should still show";
  EXPECT_GT(both.b, 10) << "the incoming camera should already show";

  // Switched off, it is a nest again, and the top track covers the rest.
  p.sequence().tracks[0].clips[0].multicam_off = true;
  const Rgba nest = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(nest.b, 250);
  EXPECT_LT(nest.r, 5);
}

TEST_F(FrameRendererTest, ANestsEffectAppliesToTheWholePictureAtOnce) {
  // The reason nesting exists. Two clips side by side under one invert: nested,
  // the invert runs on what they composited to. Splicing the inner layers into
  // the outer list would invert each of them separately — which for two mattes
  // happens to look the same, so the test is that the *nest* inverted at all.
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000")};

  p = core::add_sequence(std::move(p), "Inner");
  p.sequences[1].canvas_w = kWidth;
  p.sequences[1].canvas_h = kHeight;
  p.sequences[1].tracks[0].clips = {clip("inner", "red", 0.0, 5.0)};

  core::Media entry;
  entry.id = "nest";
  entry.sequence_id = p.sequences[1].id;
  p.media.push_back(entry);
  p = core::sync_nested_media(std::move(p));

  Clip outer = clip("outer", "nest", 0.0, 5.0);
  outer.effects.push_back(
      core::ClipEffect{.type = "invert", .enabled = true, .params = {{"on", 1.0}}});
  p.sequence().tracks = {video_track("v1", {outer})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  // Red inverted is cyan.
  EXPECT_LT(centre.r, 40) << "the effect never reached the nested picture";
  EXPECT_GT(centre.g, 200);
  EXPECT_GT(centre.b, 200);
}

TEST_F(FrameRendererTest, ANestOfANestStillDraws) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000")};

  p = core::add_sequence(std::move(p), "Deep");
  p.sequences[1].canvas_w = kWidth;
  p.sequences[1].canvas_h = kHeight;
  p.sequences[1].tracks[0].clips = {clip("deep", "red", 0.0, 5.0)};
  core::Media inner_entry;
  inner_entry.id = "nest1";
  inner_entry.sequence_id = p.sequences[1].id;
  p.media.push_back(inner_entry);

  p = core::add_sequence(std::move(p), "Middle");
  p.sequences[2].canvas_w = kWidth;
  p.sequences[2].canvas_h = kHeight;
  p.sequences[2].tracks[0].clips = {clip("mid", "nest1", 0.0, 5.0)};
  core::Media outer_entry;
  outer_entry.id = "nest2";
  outer_entry.sequence_id = p.sequences[2].id;
  p.media.push_back(outer_entry);

  p = core::sync_nested_media(std::move(p));
  p.sequence().tracks = {video_track("v1", {clip("outer", "nest2", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.r, 250) << "a nest two deep drew nothing";
}

TEST_F(FrameRendererTest, ANestPointingAtNothingDrawsNothingRatherThanFailing) {
  Project p = canvas_project();
  core::Media entry;
  entry.id = "nest";
  entry.sequence_id = "no-such-sequence";
  entry.has_video = true;
  entry.duration = 5.0;
  p.media = {entry};
  p.sequence().tracks = {video_track("v1", {clip("outer", "nest", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  ASSERT_FALSE(image.empty());
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight / 2).a, 0);
}

// ------------------------------------------------------------------ titles --

/// A title as it was written before graphics, turned into one as a project
/// file from then is: what these tests checked then, they check of that now.
Media titled(std::string id, core::TextSpec spec) {
  Media m;
  m.id = std::move(id);
  m.is_graphic = true;
  m.graphic = core::graphic_from_text(spec);
  return m;
}

/// A title that fills a good part of a 64-pixel canvas.
[[nodiscard]] core::TextSpec big_text(std::string content = "AB") {
  core::TextSpec spec;
  spec.content = std::move(content);
  spec.font_size = 32.0;
  spec.color = "#ffffff";
  return spec;
}

/// How many pixels the render put anything into.
[[nodiscard]] int covered(const gpu::Image& image) {
  int count = 0;
  for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
    if (image.pixels[i] > 8) ++count;
  }
  return count;
}

/// The tests that need glyphs on the screen.
///
/// A fixture of their own rather than a skip repeated six times, because what
/// they have in common is a *capability* and not a condition — and because the
/// grouping is what says which tests are about titles and which are about a
/// renderer that happens to have one on it.
///
/// Under the `media` preset there is no Skia and therefore no text layer, so a
/// title composites as nothing at all. That is deliberate; these five had been
/// failing CI for weeks because of it, which is the one reason a red test is
/// not a defect. A sixth was passing, which was worse — "a title is transparent
/// around its glyphs" is true of a title that drew no glyphs.
class TitleRendererTest : public FrameRendererTest {
 protected:
  void SetUp() override {
    FrameRendererTest::SetUp();
    if (IsSkipped()) return;
    if (!can_draw_text()) GTEST_SKIP() << "this build has no text layer";
  }
};

TEST_F(TitleRendererTest, ATitleDrawsItsText) {
  Project p = canvas_project();
  p.media = {titled("t", big_text())};
  p.sequence().tracks = {video_track("v1", {clip("c", "t", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  ASSERT_FALSE(image.empty());

  const int drawn = covered(image);
  EXPECT_GT(drawn, 0) << "the title drew nothing at all";
  // And it is text rather than a filled rectangle: glyphs leave most of their
  // own box empty, which is what tells a drawn title from a placeholder.
  EXPECT_LT(drawn, kWidth * kHeight * 3 / 4) << "that looks like a solid block";
}

TEST_F(TitleRendererTest, ACaptionIsDrawnAtItsFootOverEverything) {
  Project p = canvas_project();
  p.media = {matte("blue", "#0000ff")};
  p.sequence().tracks = {video_track("v1", {clip("c", "blue", 0.0, 5.0)})};
  std::string track;
  p = core::add_caption_track(std::move(p), &track);
  p = core::add_caption(std::move(p), track, 0.5, "Hi");
  core::CaptionStyle style;
  style.look.font_size = 16.0;
  style.look.background.reset();
  style.y = 0.95;
  p = core::set_caption_style(std::move(p), track, style);

  // White over blue, near the foot, and nowhere near the top.
  const auto whiter = [](const gpu::Image& image, int from_row, int to_row) {
    int count = 0;
    for (int y = from_row; y < to_row; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        const Rgba px = pixel_at(image, x, y);
        if (px.r > 128 && px.g > 128) ++count;
      }
    }
    return count;
  };
  const gpu::Image shown = render(p, 1.0);
  EXPECT_GT(whiter(shown, kHeight / 2, kHeight), 0) << "the caption drew nothing";
  EXPECT_EQ(whiter(shown, 0, kHeight / 4), 0) << "the caption stands on its foot";
  EXPECT_EQ(whiter(render(p, 4.0), 0, kHeight), 0) << "after its end there is none";

  renderer_->set_draw_captions(false);
  EXPECT_EQ(whiter(render(p, 1.0), 0, kHeight), 0) << "switched off, it is not drawn";
}

TEST_F(TitleRendererTest, ATitleIsTransparentAroundItsGlyphs) {
  Project p = canvas_project();
  // A single narrow letter in the middle: the corners of the canvas cannot be
  // part of it.
  p.media = {titled("t", big_text("l"))};
  p.sequence().tracks = {video_track("v1", {clip("c", "t", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  EXPECT_EQ(pixel_at(image, 1, 1).a, 0);
  EXPECT_EQ(pixel_at(image, kWidth - 2, kHeight - 2).a, 0);
}

TEST_F(TitleRendererTest, ATitleWithABackgroundCoversItsBox) {
  core::TextSpec spec = big_text();
  spec.background = "#0000ff";

  Project p = canvas_project();
  p.media = {titled("t", spec)};
  p.sequence().tracks = {video_track("v1", {clip("c", "t", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  const Rgba centre = pixel_at(image, kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.a, 255);
}

TEST_F(TitleRendererTest, ATitleIsSizedToItsTextRatherThanToTheCanvas) {
  // Two titles of very different lengths at the same font size cover very
  // different areas. If the quad were the canvas either way, the shorter one
  // would simply be stretched and the coverage would match.
  Project narrow = canvas_project();
  narrow.media = {titled("t", big_text("l"))};
  narrow.sequence().tracks = {video_track("v1", {clip("c", "t", 0.0, 5.0)})};

  Project wide = canvas_project();
  wide.media = {titled("t", big_text("MMMM"))};
  wide.sequence().tracks = {video_track("v1", {clip("c", "t", 0.0, 5.0)})};

  const int thin = covered(render(narrow, 1.0));
  const int fat = covered(render(wide, 1.0));
  EXPECT_GT(fat, thin * 2) << "both titles covered about the same area";
}

TEST_F(TitleRendererTest, AnEditedTitleIsRedrawnRatherThanCached) {
  Project p = canvas_project();
  p.media = {titled("t", big_text("l"))};
  p.sequence().tracks = {video_track("v1", {clip("c", "t", 0.0, 5.0)})};
  const int before = covered(render(p, 1.0));

  // Same media id, different text: a cache keyed on the id alone would show
  // the old title forever.
  p.media = {titled("t", big_text("MMMM"))};
  const int after = covered(render(p, 1.0));

  EXPECT_GT(after, before * 2);
}

TEST_F(TitleRendererTest, ATitleTakesTheEffectsOnItsClip) {
  Project p = canvas_project();
  core::TextSpec spec = big_text();
  spec.background = "#ff0000";
  p.media = {titled("t", spec)};

  Clip c = clip("c", "t", 0.0, 5.0);
  c.effects = {core::ClipEffect{.type = "grayscale", .params = {{"amount", 100.0}}}};
  p.sequence().tracks = {video_track("v1", {c})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.r, centre.g) << "a title should be as effectable as a video clip";
  EXPECT_EQ(centre.g, centre.b);
  EXPECT_EQ(centre.a, 255);
}

// ---------------------------------------------------------------- graphics --

/// A shape of a colour, `size` pixels square, centred at a point in pixels.
[[nodiscard]] core::GraphicLayer square(std::string id, std::string colour, double x, double y,
                                        double size = 24.0) {
  core::GraphicLayer layer;
  layer.id = std::move(id);
  layer.kind = core::GraphicKind::Rectangle;
  layer.width = size;
  layer.height = size;
  layer.x = x / kWidth;
  layer.y = y / kHeight;
  layer.style.fill = std::move(colour);
  return layer;
}

[[nodiscard]] Media graphic(std::string id, std::vector<core::GraphicLayer> layers) {
  Media m;
  m.id = std::move(id);
  m.is_graphic = true;
  m.graphic = core::Graphic{.layers = std::move(layers)};
  return m;
}

TEST_F(TitleRendererTest, AGraphicDrawsEachLayerWhereItIsPut) {
  Project p = canvas_project();
  p.media = {graphic("g", {square("red", "#ff0000", 16.0, 16.0),
                           square("blue", "#0000ff", 48.0, 48.0)})};
  p.sequence().tracks = {video_track("v1", {clip("c", "g", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  const Rgba red = pixel_at(image, 16, 16);
  const Rgba blue = pixel_at(image, 48, 48);
  EXPECT_GT(red.r, 240);
  EXPECT_LT(red.b, 15);
  EXPECT_GT(blue.b, 240);
  EXPECT_LT(blue.r, 15);
  EXPECT_EQ(pixel_at(image, 48, 16).a, 0) << "nothing was put there";
}

TEST_F(TitleRendererTest, AShapeIsTheColourItWasGivenNotWhite) {
  // A new shape is a mid grey. Found looking white on screen, which was the
  // HDR display's capture clipping it; this is what says the render itself
  // is right.
  Project p = canvas_project();
  p.media = {graphic("g", {square("grey", "#a0a0a0", 32.0, 32.0, 40.0)})};
  p.sequence().tracks = {video_track("v1", {clip("c", "g", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), 32, 32);
  EXPECT_NEAR(centre.r, 0xa0, 3);
  EXPECT_NEAR(centre.g, 0xa0, 3);
  EXPECT_NEAR(centre.b, 0xa0, 3);
}

// ------------------------------------------- drop shadow and track matte --

/// A graphic of one white square, 20 pixels across, in the middle of the frame.
[[nodiscard]] Project square_in_the_middle(core::ClipEffect effect) {
  Project p = canvas_project();
  p.media = {graphic("g", {square("sq", "#ffffff", 32.0, 32.0, 20.0)})};
  Clip c = clip("c", "g", 0.0, 5.0);
  c.effects = {std::move(effect)};
  p.sequence().tracks = {video_track("v1", {c})};
  return p;
}

[[nodiscard]] core::ClipEffect drop_shadow(bool alone = false, std::string colour = "#000000") {
  return core::ClipEffect{.type = "dropshadow",
                          .params = {{"opacity", 100.0},
                                     {"direction", 135.0},
                                     {"distance", 10.0},
                                     {"softness", 0.0},
                                     {"shadow_only", alone ? 1.0 : 0.0}},
                          .colors = {{"color", std::move(colour)}}};
}

TEST_F(TitleRendererTest, ADropShadowFallsDownAndRightBehindTheLayer) {
  const gpu::Image image = render(square_in_the_middle(drop_shadow()), 1.0);
  // The square itself is still white, on top.
  const Rgba middle = pixel_at(image, 32, 32);
  EXPECT_GT(middle.r, 240);
  EXPECT_EQ(middle.a, 255);
  // Beyond its lower right corner, the shadow: opaque and black. Ten pixels
  // at 135 degrees is seven each way, so the shadow runs to 49 and this is
  // clear of its softened edge.
  const Rgba shadow = pixel_at(image, 44, 44);
  EXPECT_EQ(shadow.a, 255);
  EXPECT_LT(shadow.r, 15);
  // And nothing up and to the left of it.
  EXPECT_EQ(pixel_at(image, 18, 18).a, 0);
}

TEST_F(TitleRendererTest, AShadowTakesTheColourItIsGiven) {
  const Rgba shadow = pixel_at(render(square_in_the_middle(drop_shadow(false, "#ff0000")), 1.0),
                               44, 44);
  EXPECT_GT(shadow.r, 240);
  EXPECT_LT(shadow.g, 15);
}

TEST_F(TitleRendererTest, ShadowOnlyLeavesTheLayerOut) {
  const gpu::Image image = render(square_in_the_middle(drop_shadow(true)), 1.0);
  // Where the square was and the shadow is not, nothing.
  EXPECT_EQ(pixel_at(image, 24, 24).a, 0);
  // Where they overlap, the shadow, not the white.
  EXPECT_LT(pixel_at(image, 38, 38).r, 15);
  EXPECT_EQ(pixel_at(image, 38, 38).a, 255);
}

/// Red on V1 through a matte of a white square on V2, which is hidden: the
/// way a track matte is usually set up.
[[nodiscard]] Project red_through_a_matte(bool luma, bool reverse,
                                          std::string matte_colour = "#ffffff") {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"),
             graphic("m", {square("sq", std::move(matte_colour), 32.0, 32.0, 20.0)})};
  Clip keyed = clip("keyed", "red", 0.0, 5.0);
  keyed.effects = {core::ClipEffect{.type = "trackmatte",
                                    .params = {{"track", 2.0},
                                               {"luma", luma ? 1.0 : 0.0},
                                               {"reverse", reverse ? 1.0 : 0.0}}}};
  Track above = video_track("v2", {clip("matte", "m", 0.0, 5.0)});
  above.hidden = true;
  p.sequence().tracks = {above, video_track("v1", {keyed})};
  return p;
}

TEST_F(TitleRendererTest, ATrackMatteShowsTheLayerOnlyWhereTheMatteIs) {
  const gpu::Image image = render(red_through_a_matte(false, false), 1.0);
  const Rgba inside = pixel_at(image, 32, 32);
  EXPECT_GT(inside.r, 240);
  EXPECT_EQ(inside.a, 255);
  EXPECT_EQ(pixel_at(image, 4, 4).a, 0) << "outside the matte";
}

TEST_F(TitleRendererTest, AReversedMatteShowsTheLayerEverywhereElse) {
  const gpu::Image image = render(red_through_a_matte(false, true), 1.0);
  EXPECT_EQ(pixel_at(image, 32, 32).a, 0);
  EXPECT_GT(pixel_at(image, 4, 4).r, 240);
}

TEST_F(TitleRendererTest, ALumaMatteGoesByBrightnessNotOpacity) {
  // A black square is opaque but dark: by alpha it lets everything through,
  // by luma nothing.
  EXPECT_GT(pixel_at(render(red_through_a_matte(false, false, "#000000"), 1.0), 32, 32).a, 240);
  EXPECT_EQ(pixel_at(render(red_through_a_matte(true, false, "#000000"), 1.0), 32, 32).a, 0);
}

TEST_F(TitleRendererTest, TheTopLayerOfAGraphicIsDrawnOverTheOthers) {
  Project p = canvas_project();
  // Top first, as the panel lists them.
  p.media = {graphic("g", {square("top", "#00ff00", 32.0, 32.0),
                           square("under", "#ff0000", 32.0, 32.0, 40.0)})};
  p.sequence().tracks = {video_track("v1", {clip("c", "g", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), 32, 32);
  EXPECT_GT(centre.g, 240);
  EXPECT_LT(centre.r, 15);
}

TEST_F(TitleRendererTest, AClipsOpacityAppliesToTheWholeGraphicNotEachLayer) {
  // Two layers overlapping, and the clip at half opacity. Drawn layer by
  // layer, the overlap would be three quarters opaque; as one picture, as
  // Premiere draws a graphic, it is half like everything else.
  Project p = canvas_project();
  p.media = {graphic("g", {square("a", "#ffffff", 32.0, 32.0, 40.0),
                           square("b", "#ffffff", 32.0, 32.0, 40.0)}),
             matte("black", "#000000")};
  Clip over = clip("c", "g", 0.0, 5.0);
  over.opacity = 0.5;
  p.sequence().tracks = {video_track("v2", {over}),
                         video_track("v1", {clip("bg", "black", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), 32, 32);
  // Half of white over black, in the coded values the read-back gives: well
  // short of the 220 or so that three quarters would come to.
  EXPECT_LT(centre.r, 200);
  EXPECT_GT(centre.r, 150);
}

TEST_F(TitleRendererTest, AWipeUncoversALayerFromTheLeft) {
  core::GraphicLayer wide = square("w", "#ffffff", 32.0, 32.0, 48.0);
  wide.reveal_in = core::GraphicReveal{.kind = core::RevealKind::Wipe, .duration = 2.0};
  Project p = canvas_project();
  p.media = {graphic("g", {wide})};
  p.sequence().tracks = {video_track("v1", {clip("c", "g", 0.0, 5.0)})};

  // Half way through the wipe: the left of the square shows, the right not yet.
  const gpu::Image image = render(p, 1.0);
  EXPECT_GT(pixel_at(image, 14, 32).a, 200);
  EXPECT_EQ(pixel_at(image, 50, 32).a, 0);
}

TEST_F(TitleRendererTest, AnAnimatedLayerMovesBetweenFrames) {
  core::GraphicLayer moving = square("m", "#ffffff", 16.0, 32.0, 12.0);
  moving.keyframes["x"] = {{.t = 0.0, .v = 16.0 / kWidth}, {.t = 2.0, .v = 48.0 / kWidth}};
  Project p = canvas_project();
  p.media = {graphic("g", {moving})};
  p.sequence().tracks = {video_track("v1", {clip("c", "g", 0.0, 5.0)})};

  EXPECT_GT(pixel_at(render(p, 0.0), 16, 32).a, 200);
  const gpu::Image later = render(p, 2.0);
  EXPECT_EQ(pixel_at(later, 16, 32).a, 0);
  EXPECT_GT(pixel_at(later, 48, 32).a, 200);
}

TEST_F(FrameRendererTest, ATitleWithNoTextIsSkippedRatherThanFailingTheFrame) {
  Project p = canvas_project();
  p.media = {titled("t", big_text(""))};
  p.sequence().tracks = {video_track("v1", {clip("c", "t", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  ASSERT_FALSE(image.empty());
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight / 2).a, 0);
}

// -------------------------------------------------------------- draw order --

TEST_F(FrameRendererTest, TheTopTrackDrawsOverTheBottomOne) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"), matte("green", "#00ff00")};
  // Stored top-first.
  p.sequence().tracks = {video_track("v2", {clip("top", "green", 0.0, 5.0)}),
              video_track("v1", {clip("bottom", "red", 0.0, 5.0)})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.g, 250);
  EXPECT_LT(centre.r, 5);
}

TEST_F(FrameRendererTest, AHiddenTrackDoesNotRender) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"), matte("green", "#00ff00")};
  p.sequence().tracks = {video_track("v2", {clip("top", "green", 0.0, 5.0)}),
              video_track("v1", {clip("bottom", "red", 0.0, 5.0)})};
  p.sequence().tracks[0].hidden = true;

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
  p.sequence().tracks = {video_track("v1", {c})};

  const gpu::Image image = render(p, 1.0);
  EXPECT_EQ(pixel_at(image, kWidth / 4, kHeight / 4).a, 255);
  EXPECT_EQ(pixel_at(image, kWidth * 3 / 4, kHeight * 3 / 4).a, 0);
}

TEST_F(FrameRendererTest, ASmallerCanvasIsTheSamePictureSmaller) {
  // What exporting at a lower resolution rests on: every transform in the model
  // is a fraction of the canvas rather than a pixel count, so a smaller canvas
  // scales the composition instead of cropping it.
  Project p = canvas_project();
  p.sequence().canvas_w = kWidth / 2;
  p.sequence().canvas_h = kHeight / 2;
  p.media = {matte("m", "#ffffff")};

  Clip c = clip("c", "m", 0.0, 5.0);
  c.transform = core::Transform{.x = 0.25, .y = 0.25, .scale_x = 0.5, .scale_y = 0.5};
  p.sequence().tracks = {video_track("v1", {c})};

  const gpu::Image image = render(p, 1.0);
  ASSERT_EQ(image.width, kWidth / 2);

  // The same square in the same corner, at half the size.
  EXPECT_EQ(pixel_at(image, image.width / 4, image.height / 4).a, 255);
  EXPECT_EQ(pixel_at(image, image.width * 3 / 4, image.height * 3 / 4).a, 0);
}

TEST_F(FrameRendererTest, OpacityCarriesThroughToThePixels) {
  Project p = canvas_project();
  p.media = {matte("red", "#ff0000"), matte("white", "#ffffff")};

  Clip top = clip("top", "white", 0.0, 5.0);
  top.opacity = 0.5;
  p.sequence().tracks = {video_track("v2", {top}),
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
  p.sequence().tracks = {video_track("v1", {c})};

  const Rgba centre = pixel_at(render(p, 1.0), kWidth / 2, kHeight / 2);
  EXPECT_LT(centre.r, 5) << "an inverted white matte should be black";
}

// A free-drawn mask reached the card with an empty path for as long as the
// feature existed. `to_gpu_pass` builds the mask from a brace list of its ten
// numbers, and the corners are a vector that the list cannot carry — so the
// shader had nothing to test a pixel against and applied the effect everywhere.
//
// It was invisible from the editor: the outline drew, the handles dragged, the
// numbers all round-tripped. Only the picture was wrong, and only for the one
// shape whose definition is not a number.
TEST_F(FrameRendererTest, AFreeDrawnMaskKeepsTheEffectInsideIt) {
  Project p = canvas_project();
  p.media = {matte("m", "#ffffff")};

  // A diamond about the centre, well clear of the corners of the frame.
  core::ClipEffect invert{.type = "invert", .enabled = true, .params = {{"on", 1.0}}};
  invert.mask = core::Mask{.shape = core::MaskShape::Path,
                           .points = {core::MaskPoint{.x = 0.0, .y = -0.3},
                                      core::MaskPoint{.x = 0.3, .y = 0.0},
                                      core::MaskPoint{.x = 0.0, .y = 0.3},
                                      core::MaskPoint{.x = -0.3, .y = 0.0}}};

  Clip c = clip("c", "m", 0.0, 5.0);
  c.effects = {invert};
  p.sequence().tracks = {video_track("v1", {c})};

  const gpu::Image image = render(p, 1.0);
  ASSERT_FALSE(image.empty());

  EXPECT_LT(pixel_at(image, kWidth / 2, kHeight / 2).r, 5)
      << "inside the path the white matte should have been inverted to black";
  EXPECT_GT(pixel_at(image, 2, 2).r, 250)
      << "outside the path the matte should have been left alone";
}

// And the handles are what make it a pen rather than a polygon: the same four
// points, bowed outwards, have to cover more of the frame than the diamond they
// started as.
TEST_F(FrameRendererTest, BendingAPathsEdgesChangesWhatItCovers) {
  const auto coverage = [this](bool curved) {
    Project p = canvas_project();
    p.media = {matte("m", "#ffffff")};

    // The circle constant: handles of this length turn four cubics into a
    // circle, so the curved run should cover about pi/2 of the diamond.
    const double r = 0.3;
    const double k = curved ? 0.5523 * r : 0.0;
    core::ClipEffect invert{.type = "invert", .enabled = true, .params = {{"on", 1.0}}};
    invert.mask = core::Mask{
        .shape = core::MaskShape::Path,
        .points = {core::MaskPoint{.x = 0.0, .y = -r, .in_x = -k, .out_x = k},
                   core::MaskPoint{.x = r, .y = 0.0, .in_y = -k, .out_y = k},
                   core::MaskPoint{.x = 0.0, .y = r, .in_x = k, .out_x = -k},
                   core::MaskPoint{.x = -r, .y = 0.0, .in_y = k, .out_y = -k}}};

    Clip c = clip("c", "m", 0.0, 5.0);
    c.effects = {invert};
    p.sequence().tracks = {video_track("v1", {c})};

    const gpu::Image image = render(p, 1.0);
    int inside = 0;
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        if (pixel_at(image, x, y).r < 128) ++inside;
      }
    }
    return inside;
  };

  const int diamond = coverage(false);
  const int disc = coverage(true);
  ASSERT_GT(diamond, 0) << "the straight-edged path masked nothing at all";
  EXPECT_GT(disc, diamond) << "bowing the edges out should enclose more, not less";
  // A circle against its inscribed diamond is pi/2. Loose, because sixty-four
  // pixels across is a coarse place to measure an area.
  EXPECT_NEAR(static_cast<double>(disc) / diamond, std::numbers::pi / 2.0, 0.15);
}

TEST_F(FrameRendererTest, EffectKeyframesResolveAtTheRenderedTime) {
  Project p = canvas_project();
  p.media = {matte("m", "#808080")};

  Clip c = clip("c", "m", 0.0, 4.0);
  core::ClipEffect brightness{.type = "brightness", .enabled = true, .params = {{"amount", 0.0}}};
  brightness.keyframes["amount"] = {{.t = 0.0, .v = -100.0}, {.t = 4.0, .v = 100.0}};
  c.effects = {brightness};
  p.sequence().tracks = {video_track("v1", {c})};

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

  p.sequence().tracks = {video_track("v2", {layer}),
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
  p.sequence().tracks = {video_track("v1", {a, b})};

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
  p.sequence().tracks = {video_track("v2", {clip("orphan", "gone", 0.0, 5.0)}),
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
  p.sequence().tracks = {video_track("v2", {clip("bad", "broken", 0.0, 5.0)}),
              video_track("v1", {clip("base", "red", 0.0, 5.0)})};

  const gpu::Image image = render(p, 1.0);
  EXPECT_GT(pixel_at(image, kWidth / 2, kHeight / 2).r, 250);
  EXPECT_FALSE(renderer_->missing_media().empty());
}

TEST_F(FrameRendererTest, MissingMediaIsClearedBetweenFrames) {
  Project good = canvas_project();
  good.media = {matte("red", "#ff0000")};
  good.sequence().tracks = {video_track("v1", {clip("base", "red", 0.0, 5.0)})};

  Project bad = good;
  bad.sequence().tracks.insert(bad.sequence().tracks.begin(), video_track("v2", {clip("orphan", "gone", 0.0, 5.0)}));

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
    p.sequence().tracks = {video_track("v1", {clip("c", "v", 0.0, 10.0)})};
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

TEST_F(FootageTest, ConformingTheFootageChangesWhichFrameIsOnScreen) {
  // The evidence that Interpret Footage reaches the picture at all, and there
  // is no way to get it except by rendering: every layer below this one can be
  // right about the arithmetic while the renderer still asks the decoder for a
  // time in the wrong units.
  //
  // Four seconds in, unconformed, is four seconds of file. Conformed from 60 to
  // 24 the same moment is 1.6 seconds of file, which on real footage is a
  // different picture — and it is *the picture 1.6 seconds in*, which the
  // second half checks by rendering that moment plainly and comparing.
  Project plain = with_video();
  plain.media[0].fps = 60.0;

  const gpu::Image at_four = render(plain, 4.0);
  ASSERT_FALSE(at_four.empty());

  Project conformed = core::interpret_media(plain, "v", 24.0);
  ASSERT_TRUE(core::is_conformed(conformed.media[0]));
  const gpu::Image conformed_at_four = render(conformed, 4.0);
  ASSERT_FALSE(conformed_at_four.empty());

  EXPECT_NE(conformed_at_four.pixels, at_four.pixels)
      << "the conform never reached the decoder";

  // And it is showing the right one: 4 x 0.4 is 1.6 seconds into the file.
  const gpu::Image at_one_six = render(plain, 1.6);
  ASSERT_FALSE(at_one_six.empty());
  EXPECT_EQ(conformed_at_four.pixels, at_one_six.pixels)
      << "it moved, but not to the frame the conform names";
}

TEST_F(FootageTest, AnUnconformedSourceIsUntouched) {
  // The guard on the multiply: a factor of one has to be exactly one, or every
  // source in every project moves by a rounding error.
  Project plain = with_video();
  plain.media[0].fps = 60.0;

  Project cleared = core::interpret_media(plain, "v", 24.0);
  cleared = core::interpret_media(std::move(cleared), "v", std::nullopt);

  EXPECT_EQ(render(cleared, 4.0).pixels, render(plain, 4.0).pixels);
}

// A rough cut: pieces kept from scattered parts of the source, which is what is
// left after somebody throws the rest away. Every boundary is a jump to a
// different part of the file, and so a seek and a group of pictures.
//
// The spans are sized against the reference clip, which is a few seconds long:
// a stride wide enough that crossing a boundary is further than decoding
// forwards would have covered, and clips long enough for the lookahead to have
// somewhere to run.
[[nodiscard]] Project rough_cut(const std::string& path) {
  Project p = canvas_project();

  Media m;
  m.id = "v";
  m.path = path;
  m.has_video = true;
  m.duration = 60.0;
  m.width = 3840;
  m.height = 2160;
  p.media = {m};

  constexpr double kEach = 1.0;
  constexpr double kStride = 2.4;  // so each cut travels 1.4 s of source

  Track track;
  track.id = "v1";
  track.kind = TrackKind::Video;
  for (int i = 0; i < 3; ++i) {
    Clip c;
    c.id = "c" + std::to_string(i);
    c.media_id = "v";
    c.source_in = 0.2 + static_cast<double>(i) * kStride;
    c.source_out = c.source_in + kEach;
    c.start = static_cast<double>(i) * kEach;
    track.clips.push_back(c);
  }
  p.sequence().tracks = {std::move(track)};
  return p;
}

TEST_F(FootageTest, CrossingACutTakesARunDecodedBeforeItGotThere) {
  // The stall this exists to remove. A cut to another part of the source is a
  // seek plus a whole group of pictures — measured at 96 to 287 ms on a 4K
  // capture, on the render thread, at every boundary. Prepared half a second
  // ahead instead, out of turns that have room to spare.
  //
  // Counted rather than timed. What has to be true is that the run was *ready*;
  // a stopwatch would make this a test about whichever machine ran it.
  const Project p = rough_cut(path_);
  constexpr double kFps = 60.0;

  for (int i = 0; i < 3 * 60; ++i) {
    ASSERT_TRUE(renderer_->render(p, i / kFps).has_value());
  }

  const auto stats = renderer_->decode_stats();
  ASSERT_GT(stats.frames_decoded_ahead, 0)
      << "nothing was decoded ahead at all, so the premise is wrong";
  // Both cuts after the first clip, each crossed on a run already in hand.
  EXPECT_GE(stats.runs_taken_ahead, 2) << "the cuts were seeked through as they arrived";
}

TEST_F(FootageTest, CrossingACutBackwardsTakesARunDecodedBeforeItGotThere) {
  // Backwards, a cut lands on the *end* of the earlier clip, and that is as
  // likely as not the middle of a group of pictures. Prepared on the reader
  // while the clip after it is still being played out, the far side is in hand
  // when the playhead arrives, and the only seek is the one that puts the
  // playhead at the end to begin with.
  const Project p = rough_cut(path_);
  constexpr double kFps = 60.0;

  for (int i = 3 * 60 - 1; i >= 0; --i) {
    ASSERT_TRUE(renderer_->render(p, i / kFps).has_value());
  }

  const auto stats = renderer_->decode_stats();
  ASSERT_GT(stats.frames_decoded_ahead, 0)
      << "nothing was decoded ahead at all, so the premise is wrong";
  EXPECT_LE(stats.backward_seeks, 1) << "the cuts were seeked through as they arrived";
}

TEST_F(FootageTest, PlayingStraightThroughStillDecodesNothingAhead) {
  // The other side of it: a sequence with no jump in it must not pay for a
  // prefetch nothing is going to want. Looking half a second ahead of the
  // playhead and finding the same clip is the ordinary case, and it has to cost
  // nothing at all.
  const Project p = with_video();
  constexpr double kFps = 60.0;

  for (int i = 0; i < 3 * 60; ++i) {
    ASSERT_TRUE(renderer_->render(p, i / kFps).has_value());
  }

  const auto stats = renderer_->decode_stats();
  EXPECT_EQ(stats.frames_decoded_ahead, 0) << "it prepared for a cut that is not there";
  EXPECT_LE(stats.seeks, 1);
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
  inverted.sequence().tracks[0].clips[0].effects = {
      core::ClipEffect{.type = "invert", .enabled = true, .params = {{"on", 1.0}}}};

  const gpu::Image before = render(plain, 1.0);
  renderer_->release_sources();
  const gpu::Image after = render(inverted, 1.0);

  const Rgba a = pixel_at(before, kWidth / 2, kHeight / 2);
  const Rgba b = pixel_at(after, kWidth / 2, kHeight / 2);
  // Inversion is its own opposite, so the two should sit either side of mid.
  EXPECT_NEAR(a.r + b.r, 255, 12);
}

// -------------------------------------------------- timestamps on a coarse grid --
//
// A container's time base decides how finely a frame can be stamped, and
// Matroska's is a whole millisecond. So a 60 fps capture — which is what comes
// out of a screen recorder — is stamped 0, 17, 33, 50, 67, 83 ms, while
// playback asks for 0, 16.667, 33.333, 50, 66.667, 83.333. Nothing here is
// variable frame rate and nothing has gone wrong with the file; the two grids
// simply do not line up.
//
// This is written with a file of its own rather than against the reference
// footage, because the property being tested belongs to the *container* and an
// mp4's finer time base hides it entirely.

/// A flat frame whose brightness is `20 + index * 8`, far enough apart to
/// survive 4:2:0 and be told apart after compositing.
[[nodiscard]] std::vector<std::uint8_t> stepped_frame(int index, int width, int height) {
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
  const auto value = static_cast<std::uint8_t>(20 + index * 8);
  for (std::size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = value;
    rgba[i + 1] = value;
    rgba[i + 2] = value;
    rgba[i + 3] = 255;
  }
  return rgba;
}

class CoarseTimestampTest : public FrameRendererTest {
 protected:
  static constexpr int kFrames = 24;
  static constexpr double kFps = 60.0;
  static constexpr int kSourceWidth = 160;
  static constexpr int kSourceHeight = 120;

  void SetUp() override {
    FrameRendererTest::SetUp();
    if (IsSkipped()) return;

    // Matroska for its millisecond time base, and the process id because
    // `ctest -j` runs the suites as separate processes.
    path_ = std::filesystem::temp_directory_path() /
            ("cutline_coarse_" + std::to_string(_getpid()) + ".mkv");

    media::VideoEncodeSettings settings;
    settings.width = kSourceWidth;
    settings.height = kSourceHeight;
    settings.fps = kFps;
    // Software, because the hardware encoders are not on every machine and this
    // is about timestamps rather than about any one encoder.
    settings.preference = media::EncoderPreference::Software;

    auto writer = media::MediaWriter::create(path_.string(), settings);
    if (!writer.has_value()) GTEST_SKIP() << "no encoder available: " << writer.error();
    for (int i = 0; i < kFrames; ++i) {
      const auto frame = stepped_frame(i, kSourceWidth, kSourceHeight);
      ASSERT_TRUE((*writer)->write_frame(frame).has_value());
    }
    ASSERT_TRUE((*writer)->finish().has_value());
  }

  void TearDown() override {
    renderer_.reset();
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] Project with_source() const {
    Project p = canvas_project();

    Media m;
    m.id = "v";
    m.path = path_.generic_string();
    m.has_video = true;
    m.duration = kFrames / kFps;
    m.width = kSourceWidth;
    m.height = kSourceHeight;

    p.media = {m};
    p.sequence().tracks = {video_track("v1", {clip("c", "v", 0.0, kFrames / kFps)})};
    return p;
  }

  std::filesystem::path path_;
};

TEST_F(CoarseTimestampTest, EveryFrameOfTheGridIsADifferentPicture) {
  const Project p = with_source();

  // The last two are left off: the final frame of a stream has nothing after it
  // to be distinguished from, and the clip's own end is a boundary.
  constexpr int kAsked = kFrames - 2;
  std::vector<int> shown;
  shown.reserve(kAsked);
  for (int i = 0; i < kAsked; ++i) {
    const gpu::Image image = render(p, i / kFps);
    ASSERT_FALSE(image.empty());
    shown.push_back(pixel_at(image, kWidth / 2, kHeight / 2).r);
  }

  // Asking for each frame in turn must show each frame in turn. Against a
  // tolerance of a tenth of a millisecond the frame stamped 33 did not answer a
  // request for 33.333, so the renderer decoded past it to 50 — skipping that
  // frame and then answering the *next* request with the 50 it had already
  // shown. One request in three, which on a 4K60 capture measured as playback
  // at 38 of its 60 frames with the decoder keeping up the whole time.
  const auto distinct = static_cast<int>(
      std::set<int>(shown.begin(), shown.end()).size());
  EXPECT_GE(distinct, kAsked - 1)
      << "asked for " << kAsked << " consecutive frames and got " << distinct
      << " different pictures";

  // And in the right order: a picture that goes backwards would keep the count
  // up while showing the wrong frame.
  for (std::size_t i = 1; i < shown.size(); ++i) {
    EXPECT_LE(shown[i - 1], shown[i]) << "the picture went backwards at frame " << i;
  }
}

TEST_F(CoarseTimestampTest, BackwardsShowsTheSameFrameAsForwards) {
  const Project p = with_source();
  constexpr int kAsked = kFrames - 2;

  std::vector<int> forwards(kAsked);
  for (int i = 0; i < kAsked; ++i) {
    forwards[i] = pixel_at(render(p, i / kFps), kWidth / 2, kHeight / 2).r;
  }

  // Walking back over what was just played is answered from the run of frames
  // the decoder kept, not by decoding — and that lookup had its own idea of
  // "the frame at this time". A tenth of a millisecond, against the half frame
  // the forward loop allows, so on Matroska's millisecond grid the request at
  // 16.667 ms took the frame stamped 17 forwards and the one stamped 0 back.
  std::vector<int> backwards(kAsked);
  for (int i = kAsked - 1; i >= 0; --i) {
    backwards[i] = pixel_at(render(p, i / kFps), kWidth / 2, kHeight / 2).r;
  }

  for (int i = 0; i < kAsked; ++i) {
    EXPECT_EQ(forwards[i], backwards[i]) << "the two directions disagree about frame " << i;
  }
}


// --------------------------------------------------------------- ten bits --

[[nodiscard]] std::string fixture(const char* name) {
  return (std::filesystem::path(CUTLINE_MEDIA_FIXTURES) / name).generic_string();
}

/// A 64x32 committed file, filling a canvas its own size.
[[nodiscard]] Project showing(const char* name) {
  Project p = canvas_project();
  p.sequence().canvas_h = 32;
  Media m;
  m.id = "f";
  m.path = fixture(name);
  m.has_video = true;
  m.duration = 0.25;
  m.width = 64;
  m.height = 32;
  p.media = {m};
  p.sequence().tracks = {video_track("v1", {clip("c", "f", 0.0, 0.25)})};
  return p;
}

/// The 10-bit luma code at a point of what was rendered, read back as P010.
[[nodiscard]] int luma_code_at(FrameRenderer& renderer, int x, int y) {
  const auto slot = renderer.compositor().begin_yuv_read_back(gpu::YuvLayout::P010);
  if (!slot) return -1;
  int code = -1;
  const auto done = renderer.compositor().finish_yuv_read_back(*slot, [&](const gpu::YuvPlanes& planes) {
    std::uint16_t value = 0;
    std::memcpy(&value, planes.luma + static_cast<std::size_t>(y) * planes.luma_stride + x * 2, 2);
    code = value >> 6;
  });
  return done ? code : -1;
}

class DeepRendererTest : public FrameRendererTest {
 protected:
  void SetUp() override {
    device_ = shared_device();
    if (!device_) GTEST_SKIP() << "no Direct3D 12 device available";
    auto created = FrameRenderer::create(device_, 64, 32);
    ASSERT_TRUE(created.has_value()) << created.error();
    renderer_ = std::move(*created);
  }
};

// Code 514 has no 8-bit equivalent: through eight bits it comes back 512 or
// 516. Read back at ten, it is only 514 if all ten bits went in and came out.
TEST_F(DeepRendererTest, TenBitFootageKeepsItsTenBits) {
  const Project p = showing("grey10.mkv");
  ASSERT_TRUE(renderer_->render(p, 0.05).has_value());
  EXPECT_TRUE(renderer_->missing_media().empty());
  EXPECT_NEAR(luma_code_at(*renderer_, 32, 16), 514, 1);
}


// ------------------------------------------------------- colour management --
//
// Every source is brought into the sequence's working space as it is read.
// The fixtures carry exact codes, so each expectation below is worked out from
// the standards rather than from what the renderer happened to produce.

/// The 10-bit studio code for a level, the way the fixtures were made.
[[nodiscard]] int studio_code(double level) { return static_cast<int>(std::lround(64.0 + 876.0 * level)); }

/// ST 2084 for a light level in nits.
[[nodiscard]] double pq_of(double nits) {
  const double m1 = 2610.0 / 16384.0;
  const double m2 = 128.0 * 2523.0 / 4096.0;
  const double c1 = 3424.0 / 4096.0;
  const double c2 = 32.0 * 2413.0 / 4096.0;
  const double c3 = 32.0 * 2392.0 / 4096.0;
  const double p = std::pow(nits / 10000.0, m1);
  return std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

[[nodiscard]] Project in_colour(Project p, core::WorkingColour colour, double white = 203.0,
                                bool tone_map = true) {
  p.sequence().colour = colour;
  p.sequence().graphics_white = white;
  p.sequence().tone_map = tone_map;
  return p;
}

[[nodiscard]] int luma_code_in(FrameRenderer& renderer, gpu::WorkingColour output, int x, int y) {
  const auto slot = renderer.compositor().begin_yuv_read_back(gpu::YuvLayout::P010, output);
  if (!slot) return -1;
  int code = -1;
  const auto done =
      renderer.compositor().finish_yuv_read_back(*slot, [&](const gpu::YuvPlanes& planes) {
        std::uint16_t value = 0;
        std::memcpy(&value, planes.luma + static_cast<std::size_t>(y) * planes.luma_stride + x * 2,
                    2);
        code = value >> 6;
      });
  return done ? code : -1;
}

// The two patches are 203 and 1000 nits, and nothing about a PQ sequence
// written out as PQ should move them.
TEST_F(DeepRendererTest, PqFootageInAPqSequenceComesOutAsItWentIn) {
  ASSERT_TRUE(renderer_->render(in_colour(showing("pq.mkv"), core::WorkingColour::Rec2100Pq), 0.05)
                  .has_value());
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 10, 16), 573, 1);
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 54, 16), 723, 1);
}

// HLG's reference white, 75% signal, is 203 nits on a 1,000-nit display once
// the system gamma is applied -- which is the PQ code for 203 nits.
TEST_F(DeepRendererTest, HlgReferenceWhiteLandsOnTheSameLightInAPqSequence) {
  ASSERT_TRUE(renderer_->render(in_colour(showing("hlg.mkv"), core::WorkingColour::Rec2100Pq), 0.05)
                  .has_value());
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 10, 16),
              studio_code(pq_of(203.0)), 2);
}

// SDR in an HDR sequence: its white is graphics white, so a grey is that grey's
// share of 203 nits.
TEST_F(DeepRendererTest, SdrFootageInAPqSequenceIsScaledToGraphicsWhite) {
  ASSERT_TRUE(renderer_->render(in_colour(showing("grey10.mkv"), core::WorkingColour::Rec2100Pq), 0.05)
                  .has_value());
  const double coded = (514.0 - 64.0) / 876.0;
  const double linear =
      coded <= 0.04045 ? coded / 12.92 : std::pow((coded + 0.055) / 1.055, 2.4);
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 32, 16),
              studio_code(pq_of(linear * 203.0)), 2);
}

TEST_F(DeepRendererTest, GraphicsWhiteDecidesHowBrightAWhiteMatteIs) {
  Project p = canvas_project();
  p.sequence().canvas_h = 32;
  p.media = {matte("w", "#ffffff")};
  p.sequence().tracks = {video_track("v1", {clip("c", "w", 0.0, 1.0)})};

  ASSERT_TRUE(renderer_->render(in_colour(p, core::WorkingColour::Rec2100Pq), 0.5).has_value());
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 32, 16),
              studio_code(pq_of(203.0)), 1);

  ASSERT_TRUE(renderer_->render(in_colour(p, core::WorkingColour::Rec2100Pq, 100.0), 0.5).has_value());
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 32, 16),
              studio_code(pq_of(100.0)), 1);
}

// HDR in an SDR sequence. Tone mapped, 1,000 nits is brought to white and 203
// nits sits below it, so the two are still told apart; clipped, both are white.
TEST_F(DeepRendererTest, HdrFootageInAnSdrSequenceIsToneMappedUnlessToldNotTo) {
  ASSERT_TRUE(renderer_->render(in_colour(showing("pq.mkv"), core::WorkingColour::Rec709), 0.05)
                  .has_value());
  auto image = renderer_->read_back();
  ASSERT_TRUE(image.has_value()) << image.error();
  const Rgba mapped_low = pixel_at(*image, 10, 16);
  const Rgba mapped_high = pixel_at(*image, 54, 16);
  EXPECT_GE(mapped_high.g, 252);
  EXPECT_LT(mapped_low.g, mapped_high.g - 8) << "203 nits was not kept below the highlight";
  EXPECT_GT(mapped_low.g, 200) << "203 nits was taken far below SDR white";
  EXPECT_NEAR(mapped_low.r, mapped_low.b, 2) << "a neutral grey came out tinted";

  ASSERT_TRUE(renderer_->render(in_colour(showing("pq.mkv"), core::WorkingColour::Rec709, 203.0, false), 0.05)
                  .has_value());
  image = renderer_->read_back();
  ASSERT_TRUE(image.has_value()) << image.error();
  EXPECT_GE(pixel_at(*image, 10, 16).g, 252);
  EXPECT_GE(pixel_at(*image, 54, 16).g, 252);
}

// A PQ sequence written to an SDR file is tone mapped on the way, the same as
// on an SDR display.
TEST_F(DeepRendererTest, AnHdrSequenceCanBeWrittenOutAsSdr) {
  ASSERT_TRUE(renderer_->render(in_colour(showing("pq.mkv"), core::WorkingColour::Rec2100Pq), 0.05)
                  .has_value());
  const int low = luma_code_in(*renderer_, gpu::WorkingColour::Rec709, 10, 16);
  const int high = luma_code_in(*renderer_, gpu::WorkingColour::Rec709, 54, 16);
  EXPECT_NEAR(high, 940, 2);
  EXPECT_LT(low, high - 30);
}


// ------------------------------------------------------------- HDR display --
//
// What an HDR display is sent: scRGB, where 1.0 is 80 nits.

[[nodiscard]] float scrgb_green_at(const std::vector<float>& pixels, int x, int y) {
  return pixels[(static_cast<std::size_t>(y) * 64 + x) * 4 + 1];
}

// An HDR sequence goes to an HDR display at its real brightness: 203 nits is
// 203 nits, and 1,000 is 1,000, whatever the desktop's SDR white is.
TEST_F(DeepRendererTest, AnHdrDisplayIsSentAPqSequenceAtItsRealBrightness) {
  renderer_->compositor().set_hdr_display(240.0f);
  ASSERT_TRUE(renderer_->render(in_colour(showing("pq.mkv"), core::WorkingColour::Rec2100Pq), 0.05)
                  .has_value());
  const auto pixels = renderer_->compositor().read_back_hdr();
  ASSERT_TRUE(pixels.has_value()) << pixels.error();
  EXPECT_NEAR(scrgb_green_at(*pixels, 10, 16), 203.0f / 80.0f, 0.05f);
  EXPECT_NEAR(scrgb_green_at(*pixels, 54, 16), 1000.0f / 80.0f, 0.2f);
}

// An SDR sequence goes at the desktop's SDR white, so its white matches the
// white of the interface around the monitor.
TEST_F(DeepRendererTest, AnSdrSequenceOnAnHdrDisplaySitsAtTheDesktopsWhite) {
  renderer_->compositor().set_hdr_display(240.0f);
  ASSERT_TRUE(renderer_->render(showing("grey10.mkv"), 0.05).has_value());
  const auto pixels = renderer_->compositor().read_back_hdr();
  ASSERT_TRUE(pixels.has_value()) << pixels.error();
  const double coded = (514.0 - 64.0) / 876.0;
  const double linear = std::pow((coded + 0.055) / 1.055, 2.4);
  EXPECT_NEAR(scrgb_green_at(*pixels, 32, 16), linear * 240.0 / 80.0, 0.01);
}

TEST_F(DeepRendererTest, NoHdrDisplayIsMadeUnlessAskedFor) {
  ASSERT_TRUE(renderer_->render(showing("grey10.mkv"), 0.05).has_value());
  const auto shown = renderer_->compositor().display_texture();
  ASSERT_TRUE(shown.has_value()) << shown.error();
  EXPECT_EQ(shown->hdr_resource, nullptr);
  EXPECT_FALSE(renderer_->compositor().read_back_hdr().has_value());
}


// Premiere's Color Space Override. The grey clip is tagged SDR; read as PQ in a
// PQ sequence its code is the light it means and comes back as itself, where
// read as tagged it is mapped from SDR white and comes back as something else.
TEST_F(DeepRendererTest, AnOverrideReadsTheValuesAsItSays) {
  Project p = in_colour(showing("grey10.mkv"), core::WorkingColour::Rec2100Pq);
  ASSERT_TRUE(renderer_->render(p, 0.05).has_value());
  const int as_tagged = luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 32, 16);

  p.media[0].colour_override = core::WorkingColour::Rec2100Pq;
  ASSERT_TRUE(renderer_->render(p, 0.05).has_value());
  const int overridden = luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 32, 16);

  EXPECT_NEAR(overridden, 514, 1);
  EXPECT_GT(std::abs(as_tagged - 514), 20) << "the override made no difference";
}

// What a scope of an HDR sequence measures: the signal, in its own code
// values, not the tone-mapped picture an SDR display is shown -- where 203 and
// 1,000 nits would be 241 and white.
TEST_F(DeepRendererTest, AnHdrSequencesSignalIsItsOwnCodeValues) {
  ASSERT_TRUE(renderer_->render(in_colour(showing("pq.mkv"), core::WorkingColour::Rec2100Pq), 0.05)
                  .has_value());
  const auto signal = renderer_->read_back(gpu::WorkingColour::Rec2100Pq);
  ASSERT_TRUE(signal.has_value()) << signal.error();
  EXPECT_NEAR(pixel_at(*signal, 10, 16).g, pq_of(203.0) * 255.0, 2.0);
  EXPECT_NEAR(pixel_at(*signal, 54, 16).g, pq_of(1000.0) * 255.0, 2.0);
}

/// The PQ patches inside a PQ sequence, nested whole in an outer sequence of
/// `outer` colour.
[[nodiscard]] Project pq_nested_in(core::WorkingColour outer) {
  Project p = in_colour(showing("pq.mkv"), outer);
  p = core::add_sequence(std::move(p), "Inner");
  core::Sequence& inner = p.sequences.back();
  inner.canvas_w = 64;
  inner.canvas_h = 32;
  inner.colour = core::WorkingColour::Rec2100Pq;
  inner.tracks = p.sequences[0].tracks;

  core::Media entry;
  entry.id = "nest";
  entry.sequence_id = inner.id;
  p.media.push_back(entry);
  p = core::sync_nested_media(std::move(p));
  p.sequences[0].tracks = {video_track("v1", {clip("outer", "nest", 0.0, 0.25)})};
  return p;
}

// A nest used to be rendered to eight bits of SDR whatever was in it, so an
// HDR sequence nested in another lost everything above SDR white.
TEST_F(DeepRendererTest, AnHdrNestKeepsItsHighlights) {
  ASSERT_TRUE(renderer_->render(pq_nested_in(core::WorkingColour::Rec2100Pq), 0.05).has_value());
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 10, 16), 573, 2);
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 54, 16), 723, 2);
}

// And in an SDR sequence it is tone mapped as any HDR source is, rather than
// clipped: 1,000 nits at white, 203 below it.
TEST_F(DeepRendererTest, AnHdrNestInAnSdrSequenceIsToneMapped) {
  ASSERT_TRUE(renderer_->render(pq_nested_in(core::WorkingColour::Rec709), 0.05).has_value());
  const auto image = renderer_->read_back();
  ASSERT_TRUE(image.has_value()) << image.error();
  EXPECT_GE(pixel_at(*image, 54, 16).g, 252);
  EXPECT_LT(pixel_at(*image, 10, 16).g, pixel_at(*image, 54, 16).g - 8);
}

// ----------------------------------------------------------------- lumetri --
//
// Each expectation is worked out from the grade's own maths on a flat matte:
// sRGB coding in a Rec.709 sequence, and the PQ code of the light in an HDR one.

/// A matte of one colour, filling the frame, graded.
[[nodiscard]] Project graded(std::string colour, core::ClipEffect grade) {
  Project p = canvas_project();
  p.media = {matte("m", std::move(colour))};
  Clip c = clip("c", "m", 0.0, 5.0);
  c.effects = {std::move(grade)};
  p.sequence().tracks = {video_track("v1", {c})};
  return p;
}

[[nodiscard]] core::ClipEffect lumetri(std::map<std::string, double> params) {
  return core::ClipEffect{.type = "lumetri", .params = std::move(params)};
}

/// A .cube that swaps red and blue, written where a test can point at it.
[[nodiscard]] std::filesystem::path swap_lut() {
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("cutline_swap_" + std::to_string(_getpid()) + ".cube");
  std::ofstream(path) << "LUT_3D_SIZE 2\n0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n";
  return path;
}

TEST_F(FrameRendererTest, LumetriExposureIsAStopOfLight) {
  // Grey 128 is 0.2159 in light; twice that is coded as 175.6.
  const Rgba grey = pixel_at(render(graded("#808080", lumetri({{"exposure", 1.0}})), 1.0), 32, 32);
  EXPECT_NEAR(grey.r, 176, 2);
  EXPECT_NEAR(grey.g, 176, 2);
  EXPECT_NEAR(grey.b, 176, 2);
}

TEST_F(FrameRendererTest, LumetriSaturationAtNoughtLeavesTheLuma) {
  const Rgba red = pixel_at(render(graded("#ff0000", lumetri({{"saturation", 0.0}})), 1.0), 32, 32);
  EXPECT_NEAR(red.r, 54, 2);
  EXPECT_NEAR(red.g, 54, 2);
  EXPECT_NEAR(red.b, 54, 2);
}

TEST_F(FrameRendererTest, LumetriTemperatureWarmsAGrey) {
  const Rgba grey =
      pixel_at(render(graded("#808080", lumetri({{"temperature", 100.0}})), 1.0), 32, 32);
  EXPECT_GT(grey.r, grey.g + 10);
  EXPECT_GT(grey.g, grey.b + 10);
}

TEST_F(FrameRendererTest, LumetriMasterCurveMovesTheMiddle) {
  core::ClipEffect grade = lumetri({});
  grade.curves["rgb_master"] = {{.x = 0.5, .y = 0.75}};
  const Rgba grey = pixel_at(render(graded("#808080", grade), 1.0), 32, 32);
  EXPECT_NEAR(grey.r, 192, 2);
  EXPECT_NEAR(grey.b, 192, 2);
}

TEST_F(FrameRendererTest, LumetriHueVsSaturationTakesTheColourOutOfOneHueOnly) {
  core::ClipEffect grade = lumetri({});
  // Down to nothing at red, back to no change by yellow and magenta.
  grade.curves["hue_sat"] = {{.x = 0.0, .y = 0.0}, {.x = 0.15, .y = 0.5}, {.x = 0.85, .y = 0.5}};
  const Rgba red = pixel_at(render(graded("#ff0000", grade), 1.0), 32, 32);
  EXPECT_NEAR(red.r, red.g, 3);
  const Rgba green = pixel_at(render(graded("#00ff00", grade), 1.0), 32, 32);
  EXPECT_GT(green.g, 250);
  EXPECT_LT(green.r, 5);
}

TEST_F(FrameRendererTest, LumetriInputLutIsApplied) {
  const std::filesystem::path path = swap_lut();
  core::ClipEffect grade = lumetri({});
  grade.files["input_lut"] = path.string();
  const Rgba red = pixel_at(render(graded("#ff0000", grade), 1.0), 32, 32);
  std::filesystem::remove(path);
  EXPECT_LT(red.r, 3);
  EXPECT_GT(red.b, 252);
}

TEST_F(FrameRendererTest, LumetriLookIntensityMixesTheLookIn) {
  const std::filesystem::path path = swap_lut();
  core::ClipEffect grade = lumetri({{"intensity", 50.0}});
  grade.files["look"] = path.string();
  const Rgba red = pixel_at(render(graded("#ff0000", grade), 1.0), 32, 32);
  std::filesystem::remove(path);
  EXPECT_NEAR(red.r, 128, 2);
  EXPECT_NEAR(red.b, 128, 2);
  EXPECT_LT(red.g, 3);
}

TEST_F(FrameRendererTest, LumetriWheelsLiftAndGain) {
  // Gain: half as much again. Lift: a quarter of the way to white.
  EXPECT_NEAR(pixel_at(render(graded("#808080", lumetri({{"highlights_luma", 100.0}})), 1.0),
                       32, 32).r, 192, 2);
  EXPECT_NEAR(pixel_at(render(graded("#808080", lumetri({{"shadows_luma", 100.0}})), 1.0),
                       32, 32).r, 160, 2);
}

TEST_F(FrameRendererTest, LumetriSecondaryCorrectsOnlyWhatItKeys) {
  const auto reds = lumetri({{"key_hue_lo", 330.0}, {"key_hue_hi", 30.0}, {"sec_saturation", 0.0}});
  const Rgba keyed = pixel_at(render(graded("#ff0000", reds), 1.0), 32, 32);
  EXPECT_NEAR(keyed.r, 54, 2);
  EXPECT_NEAR(keyed.g, 54, 2);

  const auto greens =
      lumetri({{"key_hue_lo", 90.0}, {"key_hue_hi", 150.0}, {"sec_saturation", 0.0}});
  const Rgba left = pixel_at(render(graded("#ff0000", greens), 1.0), 32, 32);
  EXPECT_GT(left.r, 252);
  EXPECT_LT(left.g, 3);
}

TEST_F(FrameRendererTest, LumetriShowMaskGreysWhatIsNotKeyed) {
  const auto greens = lumetri({{"key_hue_lo", 90.0}, {"key_hue_hi", 150.0}, {"show_mask", 1.0}});
  const Rgba grey = pixel_at(render(graded("#ff0000", greens), 1.0), 32, 32);
  EXPECT_NEAR(grey.r, 128, 2);
  EXPECT_NEAR(grey.g, 128, 2);
}

TEST_F(FrameRendererTest, LumetriVignetteDarkensTheCornersAndNotTheMiddle) {
  const gpu::Image image = render(graded("#808080", lumetri({{"vig_amount", -5.0}})), 1.0);
  EXPECT_NEAR(pixel_at(image, 32, 32).r, 128, 2);
  EXPECT_LT(pixel_at(image, 0, 0).r, 10);
  const int edge = pixel_at(image, 0, 32).r;
  EXPECT_LT(edge, 120);
  EXPECT_GT(edge, 10);
}

TEST_F(FrameRendererTest, LumetriCreativeSharpenStillGradesEitherSideOfIt) {
  core::ClipEffect grade = lumetri({{"exposure", 1.0}, {"sharpen", 50.0}});
  grade.curves["rgb_master"] = {{.x = 0.0, .y = 0.0}, {.x = 1.0, .y = 1.0}};
  grade.curves["rgb_red"] = {{.x = 0.0, .y = 0.0}, {.x = 0.5, .y = 0.5}, {.x = 1.0, .y = 1.0}};
  // Flat, so sharpening finds no edge: exposure before it, the straight
  // curves after it, and the stop of light is all that shows.
  EXPECT_NEAR(pixel_at(render(graded("#808080", grade), 1.0), 32, 32).g, 176, 2);
}

// Graphics white is 203 nits in a PQ sequence; a stop up is 406, which only an
// HDR grade can reach without clipping.
TEST_F(FrameRendererTest, LumetriExposureInAPqSequenceDoublesTheLight) {
  ASSERT_TRUE(renderer_
                  ->render(in_colour(graded("#ffffff", lumetri({{"exposure", 1.0}})),
                                     core::WorkingColour::Rec2100Pq),
                           1.0)
                  .has_value());
  EXPECT_NEAR(luma_code_in(*renderer_, gpu::WorkingColour::Rec2100Pq, 32, 32),
              studio_code(pq_of(406.0)), 2);
}

// --------------------------------------------------------------- ultra key --

[[nodiscard]] core::ClipEffect ultra_key(std::map<std::string, double> params = {}) {
  return core::ClipEffect{.type = "ultrakey", .params = std::move(params)};
}

/// A layer keyed over red, which is what shows wherever the key clears it.
[[nodiscard]] Project keyed_over_red(std::string colour, core::ClipEffect key,
                                     std::optional<core::Transform> transform = {}) {
  Project p = canvas_project();
  p.media = {matte("back", "#ff0000"), matte("front", std::move(colour))};
  Clip front = clip("f", "front", 0.0, 5.0);
  front.effects = {std::move(key)};
  if (transform.has_value()) front.transform = *transform;
  // Stored top-first.
  p.sequence().tracks = {video_track("v2", {front}),
                         video_track("v1", {clip("b", "back", 0.0, 5.0)})};
  return p;
}

TEST_F(FrameRendererTest, UltraKeyClearsTheKeyColour) {
  const Rgba seen = pixel_at(render(keyed_over_red("#00d000", ultra_key()), 1.0), 32, 32);
  EXPECT_GT(seen.r, 250);
  EXPECT_LT(seen.g, 5);
}

TEST_F(FrameRendererTest, UltraKeyClearsADarkerPartOfTheScreenToo) {
  const Rgba seen = pixel_at(render(keyed_over_red("#00a000", ultra_key()), 1.0), 32, 32);
  EXPECT_GT(seen.r, 240);
  EXPECT_LT(seen.g, 15);
}

TEST_F(FrameRendererTest, UltraKeyKeepsWhatIsNotTheScreen) {
  for (const char* colour : {"#808080", "#e0a080", "#2040c0"}) {
    const core::ClipEffect key = ultra_key();
    const Rgba seen = pixel_at(render(keyed_over_red(colour, key), 1.0), 32, 32);
    const Rgba plain = pixel_at(render(keyed_over_red(colour, ultra_key({{"desaturate", 0.0}})),
                                       1.0), 32, 32);
    EXPECT_EQ(seen.r, plain.r) << colour;
    // An effect this build does not know draws nothing: the colour as it is.
    const Rgba alone =
        pixel_at(render(graded(colour, core::ClipEffect{.type = "none"}), 1.0), 32, 32);
    EXPECT_NEAR(seen.r, alone.r, 2) << colour;
    EXPECT_NEAR(seen.g, alone.g, 2) << colour;
    EXPECT_NEAR(seen.b, alone.b, 2) << colour;
  }
}

TEST_F(FrameRendererTest, UltraKeyTakesTheSpillOutOfAGreenishForeground) {
  // Shown as Color Channel, so the colour is seen whole whatever the matte.
  const Rgba spilled = pixel_at(
      render(keyed_over_red("#70a070", ultra_key({{"output", 2.0}, {"spill", 0.0}})), 1.0), 32,
      32);
  EXPECT_GT(spilled.g, spilled.r + 30);
  const Rgba cleaned =
      pixel_at(render(keyed_over_red("#70a070", ultra_key({{"output", 2.0}})), 1.0), 32, 32);
  EXPECT_NEAR(cleaned.g, cleaned.r, 6);
  EXPECT_NEAR(cleaned.g, cleaned.b, 6);
}

TEST_F(FrameRendererTest, UltraKeyTakesTheSpillOutOfAnEdgeButLeavesAYellow) {
  // Skin and screen half and half, as the soft edge of a subject is.
  const Rgba edge =
      pixel_at(render(keyed_over_red("#64a466", ultra_key({{"output", 2.0}})), 1.0), 32, 32);
  EXPECT_LE(edge.g, edge.r + 8);
  const Rgba yellow =
      pixel_at(render(keyed_over_red("#f0e020", ultra_key({{"output", 2.0}})), 1.0), 32, 32);
  EXPECT_GT(yellow.g, 200);
  EXPECT_GT(yellow.r, 220);
}

TEST_F(FrameRendererTest, UltraKeyAlphaChannelShowsTheMatteInGrey) {
  const Rgba screen =
      pixel_at(render(keyed_over_red("#00d000", ultra_key({{"output", 1.0}})), 1.0), 32, 32);
  EXPECT_LT(screen.r, 5);
  EXPECT_LT(screen.g, 5);
  const Rgba kept =
      pixel_at(render(keyed_over_red("#808080", ultra_key({{"output", 1.0}})), 1.0), 32, 32);
  EXPECT_GT(kept.r, 250);
  EXPECT_GT(kept.b, 250);
}

TEST_F(FrameRendererTest, UltraKeyChokeClearsMoreOfAnEdge) {
  // Green to grey across the frame: the key clears the left, and choke takes
  // the edge of what is kept further right.
  const auto cleared = [this](double choke) {
    Project p = keyed_over_red("#00d000", ultra_key({{"choke", choke}}));
    p.media[1].gradient = core::MatteGradient{.color2 = "#808080", .angle = 0.0};
    const gpu::Image image = render(p, 1.0);
    int red = 0;
    for (int x = 0; x < kWidth; ++x) {
      const Rgba seen = pixel_at(image, x, 32);
      if (seen.r > 200 && seen.g < 60) ++red;
    }
    return red;
  };
  const int plain = cleared(0.0);
  EXPECT_GT(plain, 4);
  EXPECT_LT(plain, kWidth - 4);
  EXPECT_GE(cleared(100.0), plain + 3);
}

TEST_F(FrameRendererTest, UltraKeySoftenSpreadsTheMattePastAHardEdge) {
  // A grey square in the middle, its edge hard until softened.
  const core::Transform half{.scale_x = 0.5, .scale_y = 0.5};
  const auto just_outside = [&](double soften) {
    return pixel_at(render(keyed_over_red("#808080", ultra_key({{"soften", soften}}), half), 1.0),
                    15, 32);
  };
  EXPECT_GT(just_outside(0.0).r, 250);
  const Rgba softened = just_outside(100.0);
  EXPECT_LT(softened.r, 245);
  EXPECT_GT(softened.g, 10);
}

TEST_F(FrameRendererTest, UltraKeyLuminanceDarkensWhatStays) {
  const Rgba seen =
      pixel_at(render(keyed_over_red("#808080", ultra_key({{"luminance", 50.0}})), 1.0), 32, 32);
  EXPECT_NEAR(seen.r, 64, 3);
  EXPECT_NEAR(seen.b, 64, 3);
}

TEST_F(FrameRendererTest, UltraKeyClearsAScreenInDeepShade) {
  // Under half the key's brightness, as a vignetted corner of a screen is.
  const Rgba seen = pixel_at(render(keyed_over_red("#005a00", ultra_key()), 1.0), 32, 32);
  EXPECT_GT(seen.r, 240);
  EXPECT_LT(seen.g, 15);
}

TEST_F(FrameRendererTest, UltraKeyKeepsADarkSubject) {
  for (const char* colour : {"#202020", "#3a2a20", "#20283a"}) {
    const Rgba seen = pixel_at(render(keyed_over_red(colour, ultra_key()), 1.0), 32, 32);
    EXPECT_LT(seen.r, 70) << colour;
  }
}
// --------------------------------------------------------------- transform --

[[nodiscard]] core::ClipEffect transform_fx(std::map<std::string, double> params) {
  return core::ClipEffect{.type = "transform", .params = std::move(params)};
}

/// Black to white across the frame, transformed, over blue.
[[nodiscard]] Project transformed(core::ClipEffect effect) {
  Project p = canvas_project();
  p.media = {matte("back", "#0000ff"), matte("ramp", "#000000")};
  p.media[1].gradient = core::MatteGradient{.color2 = "#ffffff", .angle = 0.0};
  Clip front = clip("f", "ramp", 0.0, 5.0);
  front.effects = {std::move(effect)};
  // Stored top-first.
  p.sequence().tracks = {video_track("v2", {front}),
                         video_track("v1", {clip("b", "back", 0.0, 5.0)})};
  return p;
}

TEST_F(FrameRendererTest, TransformScaleShrinksTheLayerWithinItsFrame) {
  const gpu::Image image = render(transformed(transform_fx({{"scale_height", 50.0}})), 1.0);
  // Beyond the half-size picture, the frame is empty and the blue shows.
  EXPECT_GT(pixel_at(image, 4, 32).b, 250);
  EXPECT_LT(pixel_at(image, 32, 32).b, 200);
  EXPECT_GT(pixel_at(image, 4, 4).b, 250);
}

TEST_F(FrameRendererTest, TransformRotationTurnsTheLayer) {
  const gpu::Image plain = render(transformed(transform_fx({})), 1.0);
  const gpu::Image turned = render(transformed(transform_fx({{"rotation", 180.0}})), 1.0);
  // The ramp runs the other way: the left is now the light end.
  EXPECT_LT(pixel_at(plain, 6, 32).r, pixel_at(plain, 58, 32).r);
  EXPECT_NEAR(pixel_at(turned, 6, 32).r, pixel_at(plain, 57, 32).r, 6);
  EXPECT_NEAR(pixel_at(turned, 58, 32).r, pixel_at(plain, 5, 32).r, 6);
}

TEST_F(FrameRendererTest, TransformPositionMovesTheLayerAndCutsItAtTheFrame) {
  const gpu::Image image = render(transformed(transform_fx({{"position_x", 0.75}})), 1.0);
  // Moved a quarter right: the left quarter is empty, and what passed the
  // right edge is gone rather than drawn past it.
  EXPECT_GT(pixel_at(image, 8, 32).b, 250);
  EXPECT_LT(pixel_at(image, 40, 32).r, 140);
}

TEST_F(FrameRendererTest, TransformOpacityIsTheLayersOwn) {
  Project p = transformed(transform_fx({{"opacity", 50.0}}));
  p.media[1].gradient.reset();
  p.media[1].color = "#ff0000";
  const Rgba seen = pixel_at(render(p, 1.0), 32, 32);
  EXPECT_GT(seen.r, 100);
  EXPECT_GT(seen.b, 100);
}

TEST_F(FrameRendererTest, TransformBicubicKeepsAFlatColourFlat) {
  Project p = transformed(transform_fx({{"rotation", 30.0}, {"sampling", 1.0}}));
  p.media[1].gradient.reset();
  p.media[1].color = "#808080";
  const Rgba seen = pixel_at(render(p, 1.0), 32, 32);
  EXPECT_NEAR(seen.r, 128, 2);
  EXPECT_NEAR(seen.b, 128, 2);
}

TEST_F(FrameRendererTest, TransformShutterBlursWhatMoves) {
  // A red layer sliding right a whole frame width a second, so its left edge
  // smears across a 360 degree shutter and stays hard without one.
  const auto edge = [this](double shutter) {
    core::ClipEffect effect = transform_fx(
        {{"composition_shutter", shutter > 0.0 ? 0.0 : 1.0}, {"shutter_angle", shutter}});
    effect.keyframes["position_x"] = {core::Keyframe{.t = 0.0, .v = 0.5},
                                      core::Keyframe{.t = 1.0, .v = 1.5}};
    Project p = transformed(std::move(effect));
    p.media[1].gradient.reset();
    p.media[1].color = "#ff0000";
    p.sequence().fps = 4.0;
    const gpu::Image image = render(p, 0.5);
    // How many pixels along the row are neither the layer nor the blue.
    int between = 0;
    for (int x = 0; x < kWidth; ++x) {
      const Rgba seen = pixel_at(image, x, 32);
      if (seen.r > 20 && seen.r < 235) ++between;
    }
    return between;
  };
  EXPECT_LE(edge(0.0), 2);
  EXPECT_GE(edge(360.0), 8);
}

}  // namespace
}  // namespace cutline::engine
