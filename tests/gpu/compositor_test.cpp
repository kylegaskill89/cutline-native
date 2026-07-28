/// Compositor tests, run by rendering and reading the pixels back.
///
/// These are the "golden image" tests, but they assert on *properties* of the
/// result rather than on a stored PNG: that a matte fills the frame with the
/// colour it was given, that opacity lands on the arithmetic midpoint, that a
/// rotation puts the corners where trigonometry says. A stored reference image
/// would break on every driver that rounds differently, and would say nothing
/// about why.
///
/// They run on WARP when there is no GPU, which is what lets them run in CI.

#include "cutline/gpu/compositor.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

namespace cutline::gpu {
namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;

/// One device for the whole binary. Creating a Direct3D device per test is
/// slow enough on WARP to dominate the run.
std::shared_ptr<Device> shared_device() {
  static std::shared_ptr<Device> device = [] {
    auto created = Device::create({.allow_software = true});
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

[[nodiscard]] Rgba pixel_at(const Image& image, int x, int y) {
  const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4;
  return {image.pixels[index], image.pixels[index + 1], image.pixels[index + 2],
          image.pixels[index + 3]};
}

/// The compositor works in linear light but reads back sRGB-encoded, so an
/// expected linear value has to be encoded before comparing.
[[nodiscard]] int encode_srgb(double linear) {
  const double encoded =
      linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
  return static_cast<int>(std::lround(encoded * 255.0));
}

class CompositorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    device_ = shared_device();
    if (!device_) GTEST_SKIP() << "no Direct3D 12 device available";

    auto created = Compositor::create(device_, kWidth, kHeight);
    ASSERT_TRUE(created.has_value()) << created.error();
    compositor_ = std::move(*created);
  }

  /// Composites and reads back in one step, which is what every test wants.
  [[nodiscard]] Image render(std::span<const Layer> layers) {
    auto composed = compositor_->compose(layers);
    EXPECT_TRUE(composed.has_value()) << (composed ? "" : composed.error());

    auto image = compositor_->read_back();
    EXPECT_TRUE(image.has_value()) << (image ? "" : image.error());
    return image ? *image : Image{};
  }

  /// A layer covering the whole canvas in a single linear colour.
  [[nodiscard]] static Layer fill(Color color, BlendMode blend = BlendMode::Normal,
                                  float opacity = 1.0f) {
    Layer layer;
    layer.color = color;
    layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
    layer.blend = blend;
    layer.opacity = opacity;
    return layer;
  }

  std::shared_ptr<Device> device_;
  std::unique_ptr<Compositor> compositor_;
};

// ------------------------------------------------------------------- basics --

TEST_F(CompositorTest, AnEmptyStackComposesTransparentBlack) {
  const Image image = render({});
  ASSERT_FALSE(image.empty());
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight / 2), (Rgba{0, 0, 0, 0}));
}

TEST_F(CompositorTest, ReadbackHasTheCanvasShape) {
  const Image image = render({});
  EXPECT_EQ(image.width, kWidth);
  EXPECT_EQ(image.height, kHeight);
  EXPECT_EQ(image.pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4);
}

TEST_F(CompositorTest, ASolidLayerFillsTheCanvasWithItsColour) {
  // Mid grey in sRGB is 0.5 encoded, not 0.5 linear; going through
  // Color::from_srgb and back must land where it started.
  const Layer layer = fill(Color::from_srgb(0.5f, 0.5f, 0.5f));
  const Image image = render({&layer, 1});

  const Rgba centre = pixel_at(image, kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 128, 1);
  EXPECT_NEAR(centre.g, 128, 1);
  EXPECT_NEAR(centre.b, 128, 1);
  EXPECT_EQ(centre.a, 255);
}

TEST_F(CompositorTest, TheColourIsUniformAcrossTheWholeFrame) {
  const Layer layer = fill(Color::from_srgb(0.25f, 0.5f, 0.75f));
  const Image image = render({&layer, 1});

  const Rgba corner = pixel_at(image, 1, 1);
  EXPECT_EQ(pixel_at(image, kWidth - 2, 1), corner);
  EXPECT_EQ(pixel_at(image, 1, kHeight - 2), corner);
  EXPECT_EQ(pixel_at(image, kWidth - 2, kHeight - 2), corner);
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight / 2), corner);
}

// -------------------------------------------------------------- draw order --

TEST_F(CompositorTest, LaterLayersDrawOverEarlierOnes) {
  const Layer bottom = fill({1.0f, 0.0f, 0.0f, 1.0f});
  const Layer top = fill({0.0f, 1.0f, 0.0f, 1.0f});
  const Layer layers[] = {bottom, top};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.r, 0);
  EXPECT_EQ(centre.g, 255);
}

TEST_F(CompositorTest, AFullyTransparentLayerLeavesWhatIsBeneathAlone) {
  const Layer bottom = fill({1.0f, 0.0f, 0.0f, 1.0f});
  const Layer top = fill({0.0f, 1.0f, 0.0f, 1.0f}, BlendMode::Normal, 0.0f);
  const Layer layers[] = {bottom, top};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.r, 255);
  EXPECT_EQ(centre.g, 0);
}

TEST_F(CompositorTest, HalfOpacityLandsOnTheLinearMidpoint) {
  // The whole reason for the float pipeline. Blending black and white at 50%
  // in *linear* light gives 0.5 linear, which encodes to about 188 — not the
  // 128 a gamma-encoded canvas would produce.
  const Layer bottom = fill({0.0f, 0.0f, 0.0f, 1.0f});
  const Layer top = fill({1.0f, 1.0f, 1.0f, 1.0f}, BlendMode::Normal, 0.5f);
  const Layer layers[] = {bottom, top};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, encode_srgb(0.5), 2);
  EXPECT_GT(centre.r, 150) << "blending appears to be happening in encoded space";
}

// ------------------------------------------------------------ blend modes --

TEST_F(CompositorTest, MultiplyDarkensTowardsTheProduct) {
  const Layer bottom = fill({0.5f, 0.5f, 0.5f, 1.0f});
  const Layer top = fill({0.5f, 0.5f, 0.5f, 1.0f}, BlendMode::Multiply);
  const Layer layers[] = {bottom, top};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, encode_srgb(0.25), 2);
}

TEST_F(CompositorTest, ScreenBrightensTowardsTheComplementProduct) {
  const Layer bottom = fill({0.5f, 0.5f, 0.5f, 1.0f});
  const Layer top = fill({0.5f, 0.5f, 0.5f, 1.0f}, BlendMode::Screen);
  const Layer layers[] = {bottom, top};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, encode_srgb(0.75), 2);
}

TEST_F(CompositorTest, DifferenceOfALayerWithItselfIsBlack) {
  const Layer bottom = fill({0.6f, 0.3f, 0.9f, 1.0f});
  const Layer top = fill({0.6f, 0.3f, 0.9f, 1.0f}, BlendMode::Difference);
  const Layer layers[] = {bottom, top};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_LE(centre.r, 2);
  EXPECT_LE(centre.g, 2);
  EXPECT_LE(centre.b, 2);
}

TEST_F(CompositorTest, DarkenAndLightenPickPerChannel) {
  const Layer bottom = fill({0.8f, 0.2f, 0.5f, 1.0f});
  const Layer darken = fill({0.2f, 0.8f, 0.5f, 1.0f}, BlendMode::Darken);
  const Layer darken_layers[] = {bottom, darken};

  const Rgba dark = pixel_at(render(darken_layers), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(dark.r, encode_srgb(0.2), 2);
  EXPECT_NEAR(dark.g, encode_srgb(0.2), 2);

  const Layer lighten = fill({0.2f, 0.8f, 0.5f, 1.0f}, BlendMode::Lighten);
  const Layer lighten_layers[] = {bottom, lighten};

  const Rgba light = pixel_at(render(lighten_layers), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(light.r, encode_srgb(0.8), 2);
  EXPECT_NEAR(light.g, encode_srgb(0.8), 2);
}

TEST_F(CompositorTest, AddAccumulatesRatherThanReplacing) {
  const Layer bottom = fill({0.25f, 0.0f, 0.0f, 1.0f});
  const Layer top = fill({0.25f, 0.0f, 0.0f, 1.0f}, BlendMode::Add);
  const Layer layers[] = {bottom, top};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, encode_srgb(0.5), 2);
}

TEST_F(CompositorTest, ABlendModeOnlyAffectsWhereItsQuadCovers) {
  // The backdrop copy the non-separable modes need must not leak outside the
  // layer's rectangle.
  const Layer bottom = fill({1.0f, 0.0f, 0.0f, 1.0f});

  Layer top;
  top.color = {1.0f, 1.0f, 1.0f, 1.0f};
  top.blend = BlendMode::Multiply;
  // A quarter-size quad in the centre.
  top.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth * 0.5f, kHeight * 0.5f, 0.0f};
  const Layer layers[] = {bottom, top};

  const Image image = render(layers);
  EXPECT_EQ(pixel_at(image, 2, 2).r, 255) << "the corner should be untouched";
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight / 2).r, 255)
      << "multiplying by white should leave the centre unchanged";
}

// ---------------------------------------------------------------- geometry --

TEST_F(CompositorTest, AQuadCoversOnlyItsOwnRectangle) {
  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  // The left half of the canvas.
  layer.quad = {kWidth * 0.25f, kHeight * 0.5f, kWidth * 0.5f, kHeight, 0.0f};
  const Image image = render({&layer, 1});

  EXPECT_EQ(pixel_at(image, kWidth / 4, kHeight / 2).a, 255) << "inside the quad";
  EXPECT_EQ(pixel_at(image, kWidth * 3 / 4, kHeight / 2).a, 0) << "outside the quad";
}

TEST_F(CompositorTest, PositionMovesTheQuadRatherThanTheContent) {
  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  // A small square in the top-left quadrant.
  layer.quad = {kWidth * 0.25f, kHeight * 0.25f, kWidth * 0.25f, kHeight * 0.25f, 0.0f};
  const Image image = render({&layer, 1});

  EXPECT_EQ(pixel_at(image, kWidth / 4, kHeight / 4).a, 255);
  EXPECT_EQ(pixel_at(image, kWidth * 3 / 4, kHeight * 3 / 4).a, 0);
}

TEST_F(CompositorTest, RotationTurnsClockwise) {
  // A wide, short bar rotated 90 degrees becomes tall and narrow. Checking the
  // shape rather than a specific pixel keeps this independent of rasterisation
  // rounding.
  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight * 0.25f, 90.0f};
  const Image image = render({&layer, 1});

  // Now vertical: covered above and below the centre, clear to the left and
  // right.
  EXPECT_EQ(pixel_at(image, kWidth / 2, 2).a, 255);
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight - 3).a, 255);
  EXPECT_EQ(pixel_at(image, 2, kHeight / 2).a, 0);
  EXPECT_EQ(pixel_at(image, kWidth - 3, kHeight / 2).a, 0);
}

TEST_F(CompositorTest, AZeroSizedQuadDrawsNothing) {
  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, 0.0f, 0.0f, 0.0f};

  const Image image = render({&layer, 1});
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight / 2).a, 0);
}

TEST_F(CompositorTest, AQuadLargerThanTheCanvasIsClipped) {
  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth * 4.0f, kHeight * 4.0f, 0.0f};

  const Image image = render({&layer, 1});
  EXPECT_EQ(pixel_at(image, 0, 0).a, 255);
  EXPECT_EQ(pixel_at(image, kWidth - 1, kHeight - 1).a, 255);
}

// ------------------------------------------------------------------ resize --

TEST_F(CompositorTest, ResizeChangesTheReadbackShape) {
  ASSERT_TRUE(compositor_->resize(32, 16).has_value());
  EXPECT_EQ(compositor_->width(), 32);
  EXPECT_EQ(compositor_->height(), 16);

  const Layer layer = fill({1.0f, 1.0f, 1.0f, 1.0f});
  auto composed = compositor_->compose({&layer, 1});
  ASSERT_TRUE(composed.has_value()) << composed.error();

  auto image = compositor_->read_back();
  ASSERT_TRUE(image.has_value()) << image.error();
  EXPECT_EQ(image->width, 32);
  EXPECT_EQ(image->height, 16);
  EXPECT_EQ(image->pixels.size(), 32u * 16u * 4u);
}

// ----------------------------------------------------------------- effects --
//
// Effects run on coded values, not linear ones, because that is where FFmpeg's
// filters are defined. These tests state the coded-space answer explicitly, so
// moving the maths into linear light would fail them rather than pass quietly.

/// A mid-grey fill, which is where most of the colour maths is easiest to
/// reason about: coded 0.5 is neither clipped nor near a curve's knee.
[[nodiscard]] Layer grey_fill(LayerEffects effects) {
  Layer layer;
  layer.color = Color::from_srgb(0.5f, 0.5f, 0.5f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.effects = effects;
  return layer;
}

TEST_F(CompositorTest, NeutralEffectsLeaveTheLayerAlone) {
  const Layer plain = grey_fill({});
  const Rgba centre = pixel_at(render({&plain, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 128, 1);
  EXPECT_NEAR(centre.g, 128, 1);
  EXPECT_NEAR(centre.b, 128, 1);
}

TEST_F(CompositorTest, BrightnessOffsetsCodedLumaNotLinearLight) {
  // eq=brightness adds to luma in coded space: 0.5 + 0.25 = 0.75 coded, which
  // is 191 out of 255. In linear light the same offset would land near 226.
  LayerEffects effects;
  effects.brightness = 0.25f;

  const Layer layer = grey_fill(effects);
  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 191, 2);
}

TEST_F(CompositorTest, ContrastPivotsAboutTheCodedMidpoint) {
  // Mid grey is the pivot, so any contrast leaves it where it is.
  LayerEffects effects;
  effects.contrast = 2.0f;

  const Layer layer = grey_fill(effects);
  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 128, 2);
}

TEST_F(CompositorTest, ContrastPushesOffMidpointValuesApart) {
  LayerEffects effects;
  effects.contrast = 2.0f;

  Layer layer;
  layer.color = Color::from_srgb(0.75f, 0.75f, 0.75f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.effects = effects;

  // (0.75 - 0.5) * 2 + 0.5 = 1.0 coded.
  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_GE(centre.r, 253);
}

TEST_F(CompositorTest, ZeroSaturationLeavesGreyRatherThanBlack) {
  LayerEffects effects;
  effects.saturation = 0.0f;

  Layer layer;
  layer.color = Color::from_srgb(0.9f, 0.2f, 0.2f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.effects = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, centre.g, 2) << "desaturated pixels should be neutral";
  EXPECT_NEAR(centre.g, centre.b, 2);
  EXPECT_GT(centre.r, 0) << "desaturation should preserve luma, not crush it";
}

TEST_F(CompositorTest, SaturationBoostPushesAColourFurtherFromGrey) {
  Layer plain;
  plain.color = Color::from_srgb(0.6f, 0.45f, 0.45f);
  plain.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  LayerEffects effects;
  effects.saturation = 2.0f;
  Layer boosted = plain;
  boosted.effects = effects;

  const Rgba before = pixel_at(render({&plain, 1}), kWidth / 2, kHeight / 2);
  const Rgba after = pixel_at(render({&boosted, 1}), kWidth / 2, kHeight / 2);
  EXPECT_GT(after.r - after.g, before.r - before.g);
}

TEST_F(CompositorTest, InvertFlipsCodedValues) {
  // Coded 0.5 inverts to coded 0.5, so a mid grey is its own inverse. Doing
  // this in linear light instead would move it noticeably.
  LayerEffects effects;
  effects.invert = true;

  const Layer layer = grey_fill(effects);
  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 128, 2);
}

TEST_F(CompositorTest, InvertTurnsWhiteToBlack) {
  LayerEffects effects;
  effects.invert = true;

  Layer layer;
  layer.color = Color::from_srgb(1.0f, 1.0f, 1.0f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.effects = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_LE(centre.r, 2);
}

TEST_F(CompositorTest, HueRotationMovesColourWithoutKillingIt) {
  Layer layer;
  layer.color = Color::from_srgb(0.9f, 0.1f, 0.1f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  LayerEffects effects;
  effects.hue_degrees = 120.0f;
  layer.effects = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_LT(centre.r, 200) << "red should no longer dominate after a 120 degree rotation";
  EXPECT_GT(centre.r + centre.g + centre.b, 30) << "rotation should not crush the pixel";
}

TEST_F(CompositorTest, AFullTurnOfHueIsTheIdentity) {
  Layer plain;
  plain.color = Color::from_srgb(0.8f, 0.3f, 0.5f);
  plain.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  Layer turned = plain;
  LayerEffects effects;
  effects.hue_degrees = 360.0f;
  turned.effects = effects;

  const Rgba before = pixel_at(render({&plain, 1}), kWidth / 2, kHeight / 2);
  const Rgba after = pixel_at(render({&turned, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(after.r, before.r, 2);
  EXPECT_NEAR(after.g, before.g, 2);
  EXPECT_NEAR(after.b, before.b, 2);
}

// -------------------------------------------------------------------- crop --

TEST_F(CompositorTest, CropCutsEdgesToTransparentAndKeepsTheSize) {
  LayerEffects effects;
  effects.crop_left = 0.25f;
  effects.crop_right = 0.25f;

  const Layer layer = grey_fill(effects);
  const Image image = render({&layer, 1});

  EXPECT_EQ(pixel_at(image, 2, kHeight / 2).a, 0) << "the left edge should be cut";
  EXPECT_EQ(pixel_at(image, kWidth - 3, kHeight / 2).a, 0) << "the right edge should be cut";
  EXPECT_EQ(pixel_at(image, kWidth / 2, kHeight / 2).a, 255) << "the middle should survive";
  EXPECT_EQ(pixel_at(image, kWidth / 2, 2).a, 255) << "the uncropped axis is untouched";
}

TEST_F(CompositorTest, CropDoesNotMoveOrScaleWhatIsKept) {
  // The kept region stays exactly where it was, which is what distinguishes a
  // crop from a scale.
  Layer plain = grey_fill({});
  LayerEffects effects;
  effects.crop_top = 0.25f;
  Layer cropped = grey_fill(effects);

  const Rgba uncropped = pixel_at(render({&plain, 1}), kWidth / 2, kHeight / 2);
  const Rgba after = pixel_at(render({&cropped, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(after.r, uncropped.r);
}

TEST_F(CompositorTest, CroppedEdgesRevealTheLayerBeneath) {
  Layer bottom;
  bottom.color = {1.0f, 0.0f, 0.0f, 1.0f};
  bottom.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  LayerEffects effects;
  effects.crop_left = 0.5f;
  Layer top;
  top.color = {0.0f, 1.0f, 0.0f, 1.0f};
  top.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  top.effects = effects;

  const Layer layers[] = {bottom, top};
  const Image image = render(layers);

  EXPECT_EQ(pixel_at(image, 2, kHeight / 2).r, 255) << "cropped away, so red shows through";
  EXPECT_EQ(pixel_at(image, kWidth - 3, kHeight / 2).g, 255) << "kept, so green covers";
}

// ---------------------------------------------------------------- vignette --

TEST_F(CompositorTest, VignetteDarkensTheCornersAndSparesTheCentre) {
  LayerEffects effects;
  effects.vignette = 1.2f;  // radians, near the maximum

  const Layer layer = grey_fill(effects);
  const Image image = render({&layer, 1});

  const Rgba centre = pixel_at(image, kWidth / 2, kHeight / 2);
  const Rgba corner = pixel_at(image, 1, 1);
  EXPECT_NEAR(centre.r, 128, 2) << "the centre should be untouched";
  EXPECT_LT(corner.r, centre.r / 2) << "the corner should be strongly darkened";
}

TEST_F(CompositorTest, AZeroVignetteChangesNothing) {
  const Layer plain = grey_fill({});
  LayerEffects effects;
  effects.vignette = 0.0f;
  const Layer explicitly_off = grey_fill(effects);

  const Rgba before = pixel_at(render({&plain, 1}), 1, 1);
  const Rgba after = pixel_at(render({&explicitly_off, 1}), 1, 1);
  EXPECT_EQ(after.r, before.r);
}

// -------------------------------------------------------------- chroma key --

TEST_F(CompositorTest, ChromaKeyRemovesTheKeyColour) {
  LayerEffects effects;
  effects.chroma_key = true;
  effects.chroma_similarity = 0.3f;
  effects.chroma_blend = 0.1f;

  Layer layer;
  // Exactly the default key colour, so it must key out completely.
  layer.color = Color::from_srgb(0.0f, 208.0f / 255.0f, 0.0f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.effects = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.a, 0);
}

TEST_F(CompositorTest, ChromaKeyLeavesUnrelatedColoursAlone) {
  LayerEffects effects;
  effects.chroma_key = true;

  Layer layer;
  layer.color = Color::from_srgb(0.9f, 0.1f, 0.1f);  // red, far from the key
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.effects = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.a, 255);
}

TEST_F(CompositorTest, ChromaKeyRevealsWhatIsBehindIt) {
  Layer bottom;
  bottom.color = {1.0f, 0.0f, 0.0f, 1.0f};
  bottom.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  LayerEffects effects;
  effects.chroma_key = true;
  Layer green;
  green.color = Color::from_srgb(0.0f, 208.0f / 255.0f, 0.0f);
  green.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  green.effects = effects;

  const Layer layers[] = {bottom, green};
  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.r, 255) << "the keyed layer should be gone, showing red";
}

TEST_F(CompositorTest, KeyingIsOffUnlessAskedFor) {
  LayerEffects effects;
  effects.chroma_key = false;

  Layer layer;
  layer.color = Color::from_srgb(0.0f, 208.0f / 255.0f, 0.0f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.effects = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.a, 255) << "green should survive when keying is not enabled";
}

// -------------------------------------------------------- adjustment layers --

/// An adjustment layer covering the whole canvas, which is what one placed with
/// a default transform amounts to.
[[nodiscard]] Layer adjustment(LayerEffects effects, float strength = 1.0f) {
  Layer layer;
  layer.adjustment = true;
  layer.effects = effects;
  layer.opacity = strength;
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  return layer;
}

TEST_F(CompositorTest, AnAdjustmentLayerAffectsWhatIsBeneathIt) {
  LayerEffects effects;
  effects.invert = true;

  const Layer white = fill({1.0f, 1.0f, 1.0f, 1.0f});
  const Layer layers[] = {white, adjustment(effects)};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_LE(centre.r, 2) << "the white beneath should have been inverted to black";
}

TEST_F(CompositorTest, AnAdjustmentLayerDrawsNothingOfItsOwn) {
  // On an empty canvas it must leave transparent black rather than painting a
  // rectangle, because it has no content.
  const Layer only = adjustment({});
  const Rgba centre = pixel_at(render({&only, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.a, 0);
}

TEST_F(CompositorTest, AdjustmentOpacityIsStrengthNotTransparency) {
  // Half strength should land halfway between the original and the fully
  // adjusted result, not make the layer half see-through.
  LayerEffects effects;
  effects.invert = true;

  const Layer white = fill({1.0f, 1.0f, 1.0f, 1.0f});
  const Layer layers[] = {white, adjustment(effects, 0.5f)};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.r, 100) << "half an inversion should be mid grey, not black";
  EXPECT_LT(centre.r, 210);
  EXPECT_EQ(centre.a, 255) << "coverage beneath should be preserved";
}

TEST_F(CompositorTest, ANeutralAdjustmentChangesNothing) {
  const Layer grey = fill(Color::from_srgb(0.5f, 0.5f, 0.5f));
  const Layer with_adjustment[] = {grey, adjustment({})};

  const Rgba plain = pixel_at(render({&grey, 1}), kWidth / 2, kHeight / 2);
  const Rgba adjusted = pixel_at(render(with_adjustment), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(adjusted.r, plain.r, 1);
  EXPECT_NEAR(adjusted.g, plain.g, 1);
}

TEST_F(CompositorTest, AnAdjustmentReachesOnlyItsOwnQuad) {
  LayerEffects effects;
  effects.invert = true;

  Layer partial = adjustment(effects);
  // The left half only.
  partial.quad = {kWidth * 0.25f, kHeight * 0.5f, kWidth * 0.5f, kHeight, 0.0f};

  const Layer white = fill({1.0f, 1.0f, 1.0f, 1.0f});
  const Layer layers[] = {white, partial};
  const Image image = render(layers);

  EXPECT_LE(pixel_at(image, kWidth / 4, kHeight / 2).r, 2) << "inside: inverted";
  EXPECT_GE(pixel_at(image, kWidth * 3 / 4, kHeight / 2).r, 253) << "outside: untouched";
}

TEST_F(CompositorTest, AnAdjustmentStacksOnEverythingBelowNotJustTheLastLayer) {
  // Two layers underneath, the upper one covering half. Both halves must come
  // out adjusted.
  LayerEffects effects;
  effects.invert = true;

  const Layer red = fill({1.0f, 0.0f, 0.0f, 1.0f});

  Layer green;
  green.color = {0.0f, 1.0f, 0.0f, 1.0f};
  green.quad = {kWidth * 0.25f, kHeight * 0.5f, kWidth * 0.5f, kHeight, 0.0f};

  const Layer layers[] = {red, green, adjustment(effects)};
  const Image image = render(layers);

  // Inverted green is magenta; inverted red is cyan.
  const Rgba left = pixel_at(image, kWidth / 4, kHeight / 2);
  EXPECT_GE(left.r, 253);
  EXPECT_LE(left.g, 2);

  const Rgba right = pixel_at(image, kWidth * 3 / 4, kHeight / 2);
  EXPECT_LE(right.r, 2);
  EXPECT_GE(right.g, 253);
}

TEST_F(CompositorTest, LayersAboveAnAdjustmentAreNotAffectedByIt) {
  LayerEffects effects;
  effects.invert = true;

  const Layer white = fill({1.0f, 1.0f, 1.0f, 1.0f});
  const Layer above = fill({0.0f, 1.0f, 0.0f, 1.0f});
  const Layer layers[] = {white, adjustment(effects), above};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_GE(centre.g, 253) << "the layer drawn after the adjustment should be untouched";
  EXPECT_LE(centre.r, 2);
}

TEST_F(CompositorTest, ManyLayersGrowTheDescriptorHeapWithoutLosingAny) {
  // The heap grows in steps; composing past a step boundary must still draw
  // every layer, including the last one on top.
  std::vector<Layer> layers;
  for (int i = 0; i < 24; ++i) {
    layers.push_back(fill({0.0f, 0.0f, 0.0f, 1.0f}));
  }
  layers.back().color = {0.0f, 1.0f, 0.0f, 1.0f};

  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.g, 255);
}

}  // namespace
}  // namespace cutline::gpu
