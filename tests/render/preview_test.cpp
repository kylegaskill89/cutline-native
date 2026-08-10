/// Rendering a sequence at less than its own size.
///
/// The claim being tested is one sentence — the same picture, smaller — and
/// the way it breaks is always the same: something measured in pixels that did
/// not get scaled with the canvas. So most of these compare a layer's box on a
/// full canvas against the same layer's box on a reduced one, and insist the
/// two agree once the ratio is taken out.

#include "cutline/render/preview.hpp"

#include "cutline/core/layout.hpp"
#include "cutline/render/effect_catalog.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::render {
namespace {

[[nodiscard]] core::Project sequence() {
  core::Project p;
  p.sequence().canvas_w = 1920;
  p.sequence().canvas_h = 1080;

  core::Media footage;
  footage.id = "m1";
  footage.name = "shot.mp4";
  footage.duration = 10.0;
  footage.has_video = true;
  footage.width = 1280;
  footage.height = 720;
  p.media.push_back(footage);

  core::Track track{.id = "v1", .kind = core::TrackKind::Video};
  track.clips.push_back(core::Clip{.id = "c1",
                                   .media_id = "m1",
                                   .kind = core::TrackKind::Video,
                                   .source_in = 0.0,
                                   .source_out = 4.0});
  p.sequence().tracks.push_back(std::move(track));
  return p;
}

[[nodiscard]] const core::Media* media_of(const core::Project& p) { return &p.media.front(); }
[[nodiscard]] const core::Clip& clip_of(const core::Project& p) {
  return p.sequence().tracks.front().clips.front();
}

TEST(ScaledCanvas, AFactorOfOneChangesNothingAtAll) {
  const core::Project p = sequence();
  EXPECT_EQ(scaled_canvas(p, 1.0), p);
}

TEST(ScaledCanvas, ANonsenseFactorIsRefusedRatherThanApplied) {
  const core::Project p = sequence();
  EXPECT_EQ(scaled_canvas(p, 0.0), p);
  EXPECT_EQ(scaled_canvas(p, -0.5), p);
  // A one-pixel preview is not a preview.
  EXPECT_EQ(scaled_canvas(p, 0.001), p);
}

TEST(ScaledCanvas, TheCanvasShrinksAndKeepsItsShape) {
  const core::Project half = scaled_canvas(sequence(), 0.5);
  EXPECT_EQ(half.sequence().canvas_w, 960);
  EXPECT_EQ(half.sequence().canvas_h, 540);
}

// The whole promise, stated as arithmetic: every layer lands in the same place,
// measured as a fraction of the canvas it is on.
TEST(ScaledCanvas, ALayerIsInTheSamePlaceRelativeToTheFrame) {
  core::Project full = sequence();
  full.sequence().tracks[0].clips[0].transform = {
      .x = 0.3, .y = 0.7, .scale_x = 0.8, .scale_y = 0.6, .rotation = 15.0};
  const core::Project quarter = scaled_canvas(full, 0.25);

  const core::LayerBox big =
      core::layer_box(clip_of(full), media_of(full), full.sequence().canvas_w, full.sequence().canvas_h, 0.0);
  const core::LayerBox small = core::layer_box(clip_of(quarter), media_of(quarter),
                                               quarter.sequence().canvas_w, quarter.sequence().canvas_h, 0.0);

  EXPECT_NEAR(small.center_x * 4.0, big.center_x, 1e-9);
  EXPECT_NEAR(small.center_y * 4.0, big.center_y, 1e-9);
  EXPECT_NEAR(small.width * 4.0, big.width, 1e-9);
  EXPECT_NEAR(small.height * 4.0, big.height, 1e-9);
  EXPECT_DOUBLE_EQ(small.rotation_deg, big.rotation_deg);
}

// A title is laid out in canvas pixels, so leaving its size alone makes a
// caption twice as big on a half-size preview. This is the one anybody would
// see immediately.
TEST(ScaledCanvas, ATitleShrinksWithTheCanvasItIsSetIn) {
  core::Project p = sequence();
  core::Media title;
  title.id = "t1";
  title.is_text = true;
  title.has_video = true;
  title.text = core::TextSpec{.content = "Hello", .font_size = 96.0, .stroke_width = 4.0};
  p.media.push_back(title);

  const core::Project half = scaled_canvas(p, 0.5);
  EXPECT_DOUBLE_EQ(half.media[1].text->font_size, 48.0);
  EXPECT_DOUBLE_EQ(half.media[1].text->stroke_width, 2.0);
  // And the footage's own dimensions are left alone: they describe the source,
  // not the canvas, and `natural_size` fits them to whatever it is given.
  EXPECT_EQ(half.media[0].width, 1280);
}

TEST(ScaledCanvas, ABlurRadiusShrinksBecauseItIsMeasuredInPixels) {
  core::Project p = sequence();
  p.sequence().tracks[0].clips[0].effects.push_back(
      core::ClipEffect{.type = "blur", .params = {{"amount", 20.0}}});

  const core::Project half = scaled_canvas(p, 0.5);
  EXPECT_DOUBLE_EQ(clip_of(half).effects[0].params.at("amount"), 10.0);
}

// An animated parameter ignores its stored value, so scaling only one of them
// leaves whichever is actually in use unscaled half the time.
TEST(ScaledCanvas, AnAnimatedBlurShrinksItsKeyframesToo) {
  core::Project p = sequence();
  core::ClipEffect blur{.type = "blur", .params = {{"amount", 20.0}}};
  blur.keyframes["amount"] = {core::Keyframe{.t = 0.0, .v = 0.0},
                              core::Keyframe{.t = 2.0, .v = 40.0}};
  p.sequence().tracks[0].clips[0].effects.push_back(std::move(blur));

  const core::Project half = scaled_canvas(p, 0.5);
  const auto& frames = clip_of(half).effects[0].keyframes.at("amount");
  EXPECT_DOUBLE_EQ(frames[0].v, 0.0);
  EXPECT_DOUBLE_EQ(frames[1].v, 20.0);
}

// Fractions are already independent of the canvas, so touching them would be
// the bug. A crop of ten per cent is ten per cent at any size.
TEST(ScaledCanvas, AnEffectMeasuredInFractionsIsLeftAlone) {
  core::Project p = sequence();
  p.sequence().tracks[0].clips[0].effects.push_back(
      core::ClipEffect{.type = "crop", .params = {{"left", 10.0}, {"top", 25.0}}});

  const core::Project half = scaled_canvas(p, 0.5);
  EXPECT_DOUBLE_EQ(clip_of(half).effects[0].params.at("left"), 10.0);
  EXPECT_DOUBLE_EQ(clip_of(half).effects[0].params.at("top"), 25.0);
}

// The canvas rounds, and everything else is scaled by what the canvas actually
// became rather than by what was asked for — otherwise a title on an odd-sized
// sequence drifts a fraction of a pixel out of step with the frame around it.
TEST(ScaledCanvas, EverythingIsScaledByWhatTheCanvasBecame) {
  core::Project p = sequence();
  p.sequence().canvas_w = 1919;
  p.sequence().canvas_h = 1079;

  core::Media title;
  title.id = "t1";
  title.is_text = true;
  title.text = core::TextSpec{.font_size = 100.0};
  p.media.push_back(title);

  const core::Project half = scaled_canvas(p, 0.5);
  EXPECT_EQ(half.sequence().canvas_w, 960);
  EXPECT_DOUBLE_EQ(half.media[1].text->font_size, 100.0 * 960.0 / 1919.0);
}

TEST(ScaledCanvas, NothingIsLostFromTheProjectOnTheWayThrough) {
  const core::Project p = sequence();
  const core::Project half = scaled_canvas(p, 0.5);

  EXPECT_EQ(half.media.size(), p.media.size());
  EXPECT_EQ(half.sequence().tracks.size(), p.sequence().tracks.size());
  EXPECT_EQ(half.sequence().tracks[0].clips.size(), p.sequence().tracks[0].clips.size());
  EXPECT_EQ(clip_of(half).id, clip_of(p).id);
}

// The header for `preview.hpp` says a new parameter measured in pixels belongs
// in `scaled_canvas` too, and that there is no way for a test to find one that
// is missing — a fraction and a pixel count are both just doubles. There is
// now, because the catalogue says which parameters are lengths: that is what
// puts "px" after the number on the slider.
//
// Two had already been missed when this was written. A directional blur's
// amount and a sharpen's radius are both pixel distances handed to the shader
// exactly as a Gaussian blur's sigma is, and neither was scaled — so at half
// preview quality both were drawn twice as wide as the export would draw them.
TEST(ScaledCanvas, EveryPixelParameterInTheCatalogueIsScaled) {
  for (const EffectSpec& spec : effect_catalog()) {
    for (const EffectParamSpec& param : spec.params) {
      if (param.suffix != "px") continue;

      core::Project p = sequence();
      core::ClipEffect effect{.type = std::string(spec.type)};
      effect.params[std::string(param.key)] = 20.0;
      effect.keyframes[std::string(param.key)] = {core::Keyframe{.t = 0.0, .v = 40.0}};
      p.sequence().tracks[0].clips[0].effects.push_back(std::move(effect));

      const core::Project half = scaled_canvas(p, 0.5);
      const core::ClipEffect& scaled = clip_of(half).effects[0];
      EXPECT_DOUBLE_EQ(scaled.params.at(std::string(param.key)), 10.0)
          << spec.type << "." << param.key << " is in pixels and was not scaled";
      EXPECT_DOUBLE_EQ(scaled.keyframes.at(std::string(param.key))[0].v, 20.0)
          << spec.type << "." << param.key << " keyframes were not scaled";
    }
  }
}

// And the other half of the rule: anything *not* declared in pixels must be
// left exactly as it was, or a percentage would shrink along with the canvas.
TEST(ScaledCanvas, NothingElseInTheCatalogueIsTouched) {
  for (const EffectSpec& spec : effect_catalog()) {
    for (const EffectParamSpec& param : spec.params) {
      if (param.suffix == "px") continue;

      core::Project p = sequence();
      core::ClipEffect effect{.type = std::string(spec.type)};
      effect.params[std::string(param.key)] = 20.0;
      p.sequence().tracks[0].clips[0].effects.push_back(std::move(effect));

      const core::Project half = scaled_canvas(p, 0.5);
      EXPECT_DOUBLE_EQ(clip_of(half).effects[0].params.at(std::string(param.key)), 20.0)
          << spec.type << "." << param.key << " is not a length and should not have moved";
    }
  }
}

}  // namespace
}  // namespace cutline::render
