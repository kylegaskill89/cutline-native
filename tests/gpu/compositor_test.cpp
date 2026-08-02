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

// Only for the resource-state constant the texture handover reports. The
// compositor's own headers keep Direct3D out of sight; a test asserting on the
// state may as well name it rather than write the number.
#include <d3d12.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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

/// The passes below are packed the way `render/effect_passes.hpp` packs them,
/// which is the one place that decides. Named here so the tests read as what
/// they mean rather than as four floats in an order nobody can check.
[[nodiscard]] EffectPass colour(float brightness = 0.0f, float contrast = 1.0f,
                                float saturation = 1.0f, float hue_radians = 0.0f) {
  return EffectPass{PassKind::Color, {brightness, contrast, saturation, hue_radians}};
}

[[nodiscard]] EffectPass inverted() { return EffectPass{PassKind::Invert, {}}; }

[[nodiscard]] EffectPass vignetted(float radians) {
  return EffectPass{PassKind::Vignette, {radians}};
}

[[nodiscard]] EffectPass cropped(float left, float top, float right, float bottom) {
  return EffectPass{PassKind::Crop, {left, top, right, bottom}};
}

[[nodiscard]] EffectPass keyed(Color key, float similarity = 0.3f, float blend = 0.1f) {
  return EffectPass{PassKind::ChromaKey, {key.r, key.g, key.b, similarity, blend}};
}

[[nodiscard]] EffectPass blurred_by(float sigma) {
  return EffectPass{PassKind::Blur, {sigma}};
}

/// The registry green, coded rather than linear: a key colour is a hex string.
[[nodiscard]] Color key_green() { return Color{0.0f, 208.0f / 255.0f, 0.0f, 1.0f}; }

/// A mid-grey fill, which is where most of the colour maths is easiest to
/// reason about: coded 0.5 is neither clipped nor near a curve's knee.
[[nodiscard]] Layer grey_fill(std::span<const EffectPass> passes) {
  Layer layer;
  layer.color = Color::from_srgb(0.5f, 0.5f, 0.5f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = passes;
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
  const std::vector<EffectPass> effects{colour(0.25f)};
  const Layer layer = grey_fill(effects);
  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 191, 2);
}

TEST_F(CompositorTest, ContrastPivotsAboutTheCodedMidpoint) {
  // Mid grey is the pivot, so any contrast leaves it where it is.
  const std::vector<EffectPass> effects{colour(0.0f, 2.0f)};
  const Layer layer = grey_fill(effects);
  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 128, 2);
}

TEST_F(CompositorTest, ContrastPushesOffMidpointValuesApart) {
  const std::vector<EffectPass> effects{colour(0.0f, 2.0f)};

  Layer layer;
  layer.color = Color::from_srgb(0.75f, 0.75f, 0.75f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  // (0.75 - 0.5) * 2 + 0.5 = 1.0 coded.
  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_GE(centre.r, 253);
}

TEST_F(CompositorTest, ZeroSaturationLeavesGreyRatherThanBlack) {
  const std::vector<EffectPass> effects{colour(0.0f, 1.0f, 0.0f)};

  Layer layer;
  layer.color = Color::from_srgb(0.9f, 0.2f, 0.2f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, centre.g, 2) << "desaturated pixels should be neutral";
  EXPECT_NEAR(centre.g, centre.b, 2);
  EXPECT_GT(centre.r, 0) << "desaturation should preserve luma, not crush it";
}

TEST_F(CompositorTest, SaturationBoostPushesAColourFurtherFromGrey) {
  Layer plain;
  plain.color = Color::from_srgb(0.6f, 0.45f, 0.45f);
  plain.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  const std::vector<EffectPass> effects{colour(0.0f, 1.0f, 2.0f)};
  Layer boosted = plain;
  boosted.passes = effects;

  const Rgba before = pixel_at(render({&plain, 1}), kWidth / 2, kHeight / 2);
  const Rgba after = pixel_at(render({&boosted, 1}), kWidth / 2, kHeight / 2);
  EXPECT_GT(after.r - after.g, before.r - before.g);
}

TEST_F(CompositorTest, InvertFlipsCodedValues) {
  // Coded 0.5 inverts to coded 0.5, so a mid grey is its own inverse. Doing
  // this in linear light instead would move it noticeably.
  const std::vector<EffectPass> effects{inverted()};

  const Layer layer = grey_fill(effects);
  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 128, 2);
}

TEST_F(CompositorTest, InvertTurnsWhiteToBlack) {
  const std::vector<EffectPass> effects{inverted()};

  Layer layer;
  layer.color = Color::from_srgb(1.0f, 1.0f, 1.0f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_LE(centre.r, 2);
}

TEST_F(CompositorTest, HueRotationMovesColourWithoutKillingIt) {
  Layer layer;
  layer.color = Color::from_srgb(0.9f, 0.1f, 0.1f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  const std::vector<EffectPass> effects{colour(0.0f, 1.0f, 1.0f, 2.0943951f)};
  layer.passes = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_LT(centre.r, 200) << "red should no longer dominate after a 120 degree rotation";
  EXPECT_GT(centre.r + centre.g + centre.b, 30) << "rotation should not crush the pixel";
}

TEST_F(CompositorTest, AFullTurnOfHueIsTheIdentity) {
  Layer plain;
  plain.color = Color::from_srgb(0.8f, 0.3f, 0.5f);
  plain.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  Layer turned = plain;
  const std::vector<EffectPass> effects{colour(0.0f, 1.0f, 1.0f, 6.2831853f)};
  turned.passes = effects;

  const Rgba before = pixel_at(render({&plain, 1}), kWidth / 2, kHeight / 2);
  const Rgba after = pixel_at(render({&turned, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(after.r, before.r, 2);
  EXPECT_NEAR(after.g, before.g, 2);
  EXPECT_NEAR(after.b, before.b, 2);
}

// -------------------------------------------------------------------- crop --

TEST_F(CompositorTest, CropCutsEdgesToTransparentAndKeepsTheSize) {
  const std::vector<EffectPass> effects{cropped(0.25f, 0.0f, 0.25f, 0.0f)};

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
  const std::vector<EffectPass> effects{cropped(0.0f, 0.25f, 0.0f, 0.0f)};
  Layer cropped = grey_fill(effects);

  const Rgba uncropped = pixel_at(render({&plain, 1}), kWidth / 2, kHeight / 2);
  const Rgba after = pixel_at(render({&cropped, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(after.r, uncropped.r);
}

TEST_F(CompositorTest, CroppedEdgesRevealTheLayerBeneath) {
  Layer bottom;
  bottom.color = {1.0f, 0.0f, 0.0f, 1.0f};
  bottom.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  const std::vector<EffectPass> effects{cropped(0.5f, 0.0f, 0.0f, 0.0f)};
  Layer top;
  top.color = {0.0f, 1.0f, 0.0f, 1.0f};
  top.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  top.passes = effects;

  const Layer layers[] = {bottom, top};
  const Image image = render(layers);

  EXPECT_EQ(pixel_at(image, 2, kHeight / 2).r, 255) << "cropped away, so red shows through";
  EXPECT_EQ(pixel_at(image, kWidth - 3, kHeight / 2).g, 255) << "kept, so green covers";
}

// ---------------------------------------------------------------- vignette --

TEST_F(CompositorTest, VignetteDarkensTheCornersAndSparesTheCentre) {
  const std::vector<EffectPass> effects{vignetted(1.2f)};  // radians, near the maximum

  const Layer layer = grey_fill(effects);
  const Image image = render({&layer, 1});

  const Rgba centre = pixel_at(image, kWidth / 2, kHeight / 2);
  const Rgba corner = pixel_at(image, 1, 1);
  EXPECT_NEAR(centre.r, 128, 2) << "the centre should be untouched";
  EXPECT_LT(corner.r, centre.r / 2) << "the corner should be strongly darkened";
}

TEST_F(CompositorTest, AZeroVignetteChangesNothing) {
  const Layer plain = grey_fill({});
  const std::vector<EffectPass> effects{vignetted(0.0f)};
  const Layer explicitly_off = grey_fill(effects);

  const Rgba before = pixel_at(render({&plain, 1}), 1, 1);
  const Rgba after = pixel_at(render({&explicitly_off, 1}), 1, 1);
  EXPECT_EQ(after.r, before.r);
}

// -------------------------------------------------------------- chroma key --

TEST_F(CompositorTest, ChromaKeyRemovesTheKeyColour) {
  const std::vector<EffectPass> effects{keyed(key_green(), 0.3f, 0.1f)};

  Layer layer;
  // Exactly the default key colour, so it must key out completely.
  layer.color = Color::from_srgb(0.0f, 208.0f / 255.0f, 0.0f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.a, 0);
}

TEST_F(CompositorTest, ChromaKeyLeavesUnrelatedColoursAlone) {
  const std::vector<EffectPass> effects{keyed(key_green())};

  Layer layer;
  layer.color = Color::from_srgb(0.9f, 0.1f, 0.1f);  // red, far from the key
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.a, 255);
}

TEST_F(CompositorTest, ChromaKeyRevealsWhatIsBehindIt) {
  Layer bottom;
  bottom.color = {1.0f, 0.0f, 0.0f, 1.0f};
  bottom.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  const std::vector<EffectPass> effects{keyed(key_green())};
  Layer green;
  green.color = Color::from_srgb(0.0f, 208.0f / 255.0f, 0.0f);
  green.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  green.passes = effects;

  const Layer layers[] = {bottom, green};
  const Rgba centre = pixel_at(render(layers), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.r, 255) << "the keyed layer should be gone, showing red";
}

TEST_F(CompositorTest, KeyingIsOffUnlessAskedFor) {
  const std::vector<EffectPass> effects;

  Layer layer;
  layer.color = Color::from_srgb(0.0f, 208.0f / 255.0f, 0.0f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_EQ(centre.a, 255) << "green should survive when keying is not enabled";
}

// ---------------------------------------------------------------- gradients --

/// A full-canvas matte running from `from` to `to` at `angle`.
[[nodiscard]] Layer gradient_fill(Color from, Color to, float angle) {
  Layer layer;
  layer.color = from;
  layer.gradient = true;
  layer.gradient_color = to;
  layer.gradient_angle_deg = angle;
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  return layer;
}

TEST_F(CompositorTest, AGradientRunsLeftToRightAtZeroDegrees) {
  const Layer layer =
      gradient_fill(Color::from_srgb(0.0f, 0.0f, 0.0f), Color::from_srgb(1.0f, 1.0f, 1.0f), 0.0f);
  const Image image = render({&layer, 1});

  const int left = pixel_at(image, 0, kHeight / 2).r;
  const int middle = pixel_at(image, kWidth / 2, kHeight / 2).r;
  const int right = pixel_at(image, kWidth - 1, kHeight / 2).r;

  EXPECT_LT(left, 20);
  EXPECT_GT(right, 235);
  EXPECT_GT(middle, left);
  EXPECT_LT(middle, right);
}

TEST_F(CompositorTest, AGradientIsFlatAlongTheAxisItDoesNotRunOn) {
  const Layer layer =
      gradient_fill(Color::from_srgb(0.0f, 0.0f, 0.0f), Color::from_srgb(1.0f, 1.0f, 1.0f), 0.0f);
  const Image image = render({&layer, 1});

  // Left to right means every column is a constant colour.
  EXPECT_EQ(pixel_at(image, kWidth / 4, 1).r, pixel_at(image, kWidth / 4, kHeight - 2).r);
}

TEST_F(CompositorTest, NinetyDegreesRunsTopToBottom) {
  const Layer layer =
      gradient_fill(Color::from_srgb(0.0f, 0.0f, 0.0f), Color::from_srgb(1.0f, 1.0f, 1.0f), 90.0f);
  const Image image = render({&layer, 1});

  EXPECT_LT(pixel_at(image, kWidth / 2, 0).r, 20);
  EXPECT_GT(pixel_at(image, kWidth / 2, kHeight - 1).r, 235);
  EXPECT_EQ(pixel_at(image, 1, kHeight / 4).r, pixel_at(image, kWidth - 2, kHeight / 4).r);
}

TEST_F(CompositorTest, TheGradientSpansTheWholeQuadOnADiagonal) {
  // The half-extent is the rect projected onto the gradient direction, so a
  // diagonal still reaches both stops at opposite corners rather than
  // saturating early or leaving flat bands.
  const Layer layer =
      gradient_fill(Color::from_srgb(0.0f, 0.0f, 0.0f), Color::from_srgb(1.0f, 1.0f, 1.0f), 45.0f);
  const Image image = render({&layer, 1});

  EXPECT_LT(pixel_at(image, 0, 0).r, 20) << "the near corner should reach the first stop";
  EXPECT_GT(pixel_at(image, kWidth - 1, kHeight - 1).r, 235)
      << "the far corner should reach the second";
}

TEST_F(CompositorTest, TheMidpointInterpolatesInCodedSpace) {
  // Black to white through a canvas gradient is coded 0.5 at the middle, which
  // reads as 128. Interpolating in linear light would put it near 188.
  const Layer layer =
      gradient_fill(Color::from_srgb(0.0f, 0.0f, 0.0f), Color::from_srgb(1.0f, 1.0f, 1.0f), 0.0f);
  const Image image = render({&layer, 1});

  EXPECT_NEAR(pixel_at(image, kWidth / 2, kHeight / 2).r, 128, 4);
}

TEST_F(CompositorTest, AMatteWithoutAGradientIsStillFlat) {
  const Layer layer = fill(Color::from_srgb(0.25f, 0.5f, 0.75f));
  const Image image = render({&layer, 1});
  EXPECT_EQ(pixel_at(image, 0, 0), pixel_at(image, kWidth - 1, kHeight - 1));
}

TEST_F(CompositorTest, EffectsApplyOnTopOfAGradient) {
  Layer layer =
      gradient_fill(Color::from_srgb(0.0f, 0.0f, 0.0f), Color::from_srgb(1.0f, 1.0f, 1.0f), 0.0f);
  const std::vector<EffectPass> effects{inverted()};
  layer.passes = effects;

  const Image image = render({&layer, 1});
  EXPECT_GT(pixel_at(image, 0, kHeight / 2).r, 235) << "the dark end should now be light";
  EXPECT_LT(pixel_at(image, kWidth - 1, kHeight / 2).r, 20);
}

// -------------------------------------------------------------------- mask --
//
// A mask belongs to one pass, and the shader applies it the same way whatever
// the pass is: work out the effect, work out the coverage, mix between the two.
// So these test one effect and trust the rest, which is the point of doing it
// in one place.

/// An ellipse in the middle of the layer, a quarter of it across.
[[nodiscard]] PassMask ellipse(float radius = 0.25f) {
  PassMask mask;
  mask.shape = 1.0f;
  mask.width = radius;
  mask.height = radius;
  return mask;
}

[[nodiscard]] PassMask rectangle(float half_width, float half_height) {
  PassMask mask;
  mask.shape = 2.0f;
  mask.width = half_width;
  mask.height = half_height;
  return mask;
}

TEST_F(CompositorTest, AMaskedEffectAppliesInsideAndNotOutside) {
  EffectPass pass = inverted();
  pass.mask = ellipse();
  const std::vector<EffectPass> effects{pass};

  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Image image = render({&layer, 1});
  EXPECT_LE(pixel_at(image, kWidth / 2, kHeight / 2).r, 4) << "inverted in the middle";
  EXPECT_GE(pixel_at(image, 2, 2).r, 250) << "and left alone in the corner";
}

TEST_F(CompositorTest, AnInvertedMaskSwapsWhichSideIsAffected) {
  EffectPass pass = inverted();
  pass.mask = ellipse();
  pass.mask.inverted = 1.0f;
  const std::vector<EffectPass> effects{pass};

  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Image image = render({&layer, 1});
  EXPECT_GE(pixel_at(image, kWidth / 2, kHeight / 2).r, 250) << "the middle is spared";
  EXPECT_LE(pixel_at(image, 2, 2).r, 4) << "and the corner is not";
}

TEST_F(CompositorTest, AMaskOpacityIsTheEffectsStrengthNotTheLayers) {
  // Half a mask is half an inversion, not a half-transparent layer. The
  // distinction matters: a mask that thinned the picture would show whatever is
  // beneath it, and there is nothing beneath this one.
  EffectPass pass = inverted();
  pass.mask = ellipse();
  pass.mask.opacity = 0.5f;
  const std::vector<EffectPass> effects{pass};

  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(centre.r, 128, 6) << "halfway between white and its inverse";
  EXPECT_EQ(centre.a, 255) << "and still fully opaque";
}

TEST_F(CompositorTest, AFeatheredMaskRampsAcrossItsEdge) {
  EffectPass pass = inverted();
  pass.mask = ellipse(0.3f);
  pass.mask.feather = 0.15f;
  const std::vector<EffectPass> effects{pass};

  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Image image = render({&layer, 1});
  // Straight out from the centre: fully inverted, then partly, then untouched.
  const int inside = pixel_at(image, kWidth / 2, kHeight / 2).r;
  const int edge = pixel_at(image, kWidth / 2 + static_cast<int>(kWidth * 0.3f), kHeight / 2).r;
  const int outside = pixel_at(image, kWidth - 2, kHeight / 2).r;

  EXPECT_LE(inside, 4);
  EXPECT_GT(edge, inside);
  EXPECT_LT(edge, outside);
  EXPECT_GE(outside, 250);
}

TEST_F(CompositorTest, AHardMaskHasNoRamp) {
  EffectPass pass = inverted();
  pass.mask = ellipse(0.3f);
  const std::vector<EffectPass> effects{pass};

  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Image image = render({&layer, 1});
  // A pixel either side of where the edge falls, with a couple to spare for the
  // sampling grid: one fully inverted, one untouched, nothing in between.
  EXPECT_LE(pixel_at(image, kWidth / 2 + static_cast<int>(kWidth * 0.28f), kHeight / 2).r, 4);
  EXPECT_GE(pixel_at(image, kWidth / 2 + static_cast<int>(kWidth * 0.32f), kHeight / 2).r, 250);
}

TEST_F(CompositorTest, ARectangleMaskHasCornersAnEllipseDoesNot) {
  // The same half-extents either way. A corner of the box is inside the
  // rectangle and outside the ellipse, which is the whole difference between
  // the two shapes and the only thing worth asserting about it.
  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  EffectPass boxed = inverted();
  boxed.mask = rectangle(0.3f, 0.3f);
  const std::vector<EffectPass> box{boxed};

  EffectPass rounded = inverted();
  rounded.mask = ellipse(0.3f);
  const std::vector<EffectPass> round{rounded};

  // Just inside the rectangle's corner, comfortably outside the ellipse.
  const int x = kWidth / 2 + static_cast<int>(kWidth * 0.26f);
  const int y = kHeight / 2 + static_cast<int>(kHeight * 0.26f);

  layer.passes = box;
  EXPECT_LE(pixel_at(render({&layer, 1}), x, y).r, 4) << "the corner is inside a rectangle";
  layer.passes = round;
  EXPECT_GE(pixel_at(render({&layer, 1}), x, y).r, 250) << "and outside an ellipse";
}

TEST_F(CompositorTest, TwoEffectsCanBeMaskedDifferently) {
  // The thing the flat effect struct could not express at all: a mask belongs
  // to one effect rather than to the clip.
  EffectPass left = inverted();
  left.mask = rectangle(0.25f, 0.5f);
  left.mask.x = 0.25f;

  EffectPass right = colour(0.0f, 1.0f, 0.0f);  // drained of colour
  right.mask = rectangle(0.25f, 0.5f);
  right.mask.x = 0.75f;

  const std::vector<EffectPass> effects{left, right};

  Layer layer;
  layer.color = Color::from_srgb(0.8f, 0.2f, 0.2f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Image image = render({&layer, 1});
  const Rgba inverted_side = pixel_at(image, kWidth / 4, kHeight / 2);
  const Rgba drained_side = pixel_at(image, kWidth * 3 / 4, kHeight / 2);

  EXPECT_LT(inverted_side.r, inverted_side.b) << "the left was inverted";
  EXPECT_NEAR(drained_side.r, drained_side.g, 3) << "the right was drained";
  EXPECT_NEAR(drained_side.g, drained_side.b, 3);
}

TEST_F(CompositorTest, AMaskTurnsWithItsOwnRotation) {
  // A tall thin rectangle laid on its side reaches across rather than up.
  EffectPass pass = inverted();
  pass.mask = rectangle(0.1f, 0.45f);
  pass.mask.cos_rotation = 0.0f;  // a quarter turn
  pass.mask.sin_rotation = 1.0f;
  const std::vector<EffectPass> effects{pass};

  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Image image = render({&layer, 1});
  EXPECT_LE(pixel_at(image, 4, kHeight / 2).r, 4) << "it reaches to the sides";
  EXPECT_GE(pixel_at(image, kWidth / 2, 4).r, 250) << "and not to the top";
}

TEST_F(CompositorTest, AnUnmaskedPassStillAppliesEverywhere) {
  const std::vector<EffectPass> effects{inverted()};

  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Image image = render({&layer, 1});
  EXPECT_LE(pixel_at(image, kWidth / 2, kHeight / 2).r, 4);
  EXPECT_LE(pixel_at(image, 2, 2).r, 4);
}

// -------------------------------------------------------------------- blur --

TEST_F(CompositorTest, BlurSoftensTheLayersOwnEdge) {
  // A half-covering white quad has a vertical edge down the middle. Blurring
  // must turn that step into a ramp.
  //
  // The ramp is *inside* the quad, not across it. A pass runs over the layer in
  // its own space, so a blur has the layer to work with and nothing else — it
  // softens the layer's edge inwards rather than growing the layer outwards.
  // Premiere spreads past the edge instead, which needs a margin around the
  // scratch that this does not have yet.
  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.25f, kHeight * 0.5f, kWidth * 0.5f, kHeight, 0.0f};

  const Image sharp = render({&layer, 1});
  EXPECT_EQ(pixel_at(sharp, kWidth / 2 - 2, kHeight / 2).a, 255);
  EXPECT_EQ(pixel_at(sharp, kWidth / 2 + 2, kHeight / 2).a, 0);

  const std::vector<EffectPass> effects{blurred_by(4.0f)};
  layer.passes = effects;
  const Image soft = render({&layer, 1});

  const int just_inside = pixel_at(soft, kWidth / 2 - 2, kHeight / 2).a;
  EXPECT_LT(just_inside, 255) << "the edge should have become a ramp";
  EXPECT_GT(just_inside, 0) << "and not have vanished";
  EXPECT_EQ(pixel_at(soft, kWidth / 4, kHeight / 2).a, 255) << "well inside is untouched";
}

TEST_F(CompositorTest, TheSameEffectsInADifferentOrderGiveADifferentPicture) {
  // The claim the whole restructuring rests on, checked against pixels rather
  // than against a plan. Under the flat resolver these two stacks produced one
  // identical struct and one identical frame.
  const std::vector<EffectPass> dark_first{colour(-0.3f), colour(0.0f, 3.0f)};
  const std::vector<EffectPass> contrast_first{colour(0.0f, 3.0f), colour(-0.3f)};

  Layer layer;
  layer.color = Color::from_srgb(0.6f, 0.6f, 0.6f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  layer.passes = dark_first;
  const int one = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2).r;
  layer.passes = contrast_first;
  const int other = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2).r;

  EXPECT_NE(one, other) << "both came out " << one;
}

TEST_F(CompositorTest, AStackFarLongerThanTheOldBudgetStillDraws) {
  // Eight effects on one layer, which the flat struct could not have carried:
  // every one of them needed its own permanent field in sixty-four DWORDs of
  // root constants, and twenty-eight were already spoken for. Passes share one
  // block because only one runs at a time, so the length of a stack costs
  // nothing but draws.
  const std::vector<EffectPass> many{
      colour(0.05f),        colour(0.0f, 1.1f), colour(0.0f, 1.0f, 1.2f),
      inverted(),           inverted(),         vignetted(0.2f),
      blurred_by(1.0f),     colour(0.0f, 0.95f)};

  Layer layer;
  layer.color = Color::from_srgb(0.5f, 0.5f, 0.5f);
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = many;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_GT(centre.a, 250) << "the layer survived its own stack";
  EXPECT_GT(centre.r, 100);
  EXPECT_LT(centre.r, 200);
}

TEST_F(CompositorTest, AZeroSigmaLeavesTheLayerUntouched) {
  Layer sharp;
  sharp.color = {1.0f, 1.0f, 1.0f, 1.0f};
  sharp.quad = {kWidth * 0.25f, kHeight * 0.5f, kWidth * 0.5f, kHeight, 0.0f};

  Layer explicitly_zero = sharp;
  const std::vector<EffectPass> none{blurred_by(0.0f)};
  explicitly_zero.passes = none;

  const Image a = render({&sharp, 1});
  const Image b = render({&explicitly_zero, 1});
  EXPECT_EQ(pixel_at(b, kWidth / 2 + 2, kHeight / 2).a,
            pixel_at(a, kWidth / 2 + 2, kHeight / 2).a);
}

TEST_F(CompositorTest, BlurPreservesAFlatFieldRatherThanDarkeningIt) {
  // The kernel must be normalised: blurring a uniform colour has to leave it
  // exactly where it was, and an unnormalised one shows up here first.
  const std::vector<EffectPass> effects{blurred_by(6.0f)};

  const Layer plain = grey_fill({});
  const Layer blurred = grey_fill(effects);

  const Rgba before = pixel_at(render({&plain, 1}), kWidth / 2, kHeight / 2);
  const Rgba after = pixel_at(render({&blurred, 1}), kWidth / 2, kHeight / 2);
  EXPECT_NEAR(after.r, before.r, 2);
  EXPECT_NEAR(after.a, before.a, 2);
}

TEST_F(CompositorTest, PassesRunInTheOrderTheyAreListed) {
  // Invert then blur is a blurred black layer; blur then invert would be an
  // inverted blur of white. Under the flat resolver the two were the same
  // thing, because the order was fixed and the stack was only a set.
  const std::vector<EffectPass> effects{inverted(), blurred_by(3.0f)};

  Layer layer;
  layer.color = {1.0f, 1.0f, 1.0f, 1.0f};
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  layer.passes = effects;

  const Rgba centre = pixel_at(render({&layer, 1}), kWidth / 2, kHeight / 2);
  EXPECT_LE(centre.r, 4) << "the inversion should have happened before the blur";
}

TEST_F(CompositorTest, ABlurredLayerStillComposesOverWhatIsBeneath) {
  const Layer red = fill({1.0f, 0.0f, 0.0f, 1.0f});

  Layer green;
  green.color = {0.0f, 1.0f, 0.0f, 1.0f};
  green.quad = {kWidth * 0.25f, kHeight * 0.5f, kWidth * 0.5f, kHeight, 0.0f};
  const std::vector<EffectPass> soft{blurred_by(3.0f)};
  green.passes = soft;

  const Layer layers[] = {red, green};
  const Image image = render(layers);

  EXPECT_GE(pixel_at(image, 2, kHeight / 2).g, 200) << "well inside the blurred quad";
  EXPECT_GE(pixel_at(image, kWidth - 3, kHeight / 2).r, 250) << "well outside it";
}

TEST_F(CompositorTest, BlurAndSharpLayersCanBeMixedInOneCompose) {
  // The blur path swaps render targets mid-compose; the layers around it must
  // still land on the scene.
  Layer blurred;
  blurred.color = {0.0f, 0.0f, 1.0f, 1.0f};
  blurred.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  const std::vector<EffectPass> soft{blurred_by(5.0f)};
  blurred.passes = soft;

  Layer sharp;
  sharp.color = {0.0f, 1.0f, 0.0f, 1.0f};
  sharp.quad = {kWidth * 0.25f, kHeight * 0.5f, kWidth * 0.5f, kHeight, 0.0f};

  const Layer layers[] = {blurred, sharp};
  const Image image = render(layers);

  EXPECT_GE(pixel_at(image, kWidth / 4, kHeight / 2).g, 250) << "the sharp layer drew";
  EXPECT_GE(pixel_at(image, kWidth * 3 / 4, kHeight / 2).b, 200) << "the blurred layer drew";
}

// -------------------------------------------------------- adjustment layers --

/// An adjustment layer covering the whole canvas, which is what one placed with
/// a default transform amounts to.
[[nodiscard]] Layer adjustment(std::span<const EffectPass> passes, float strength = 1.0f) {
  Layer layer;
  layer.adjustment = true;
  layer.passes = passes;
  layer.opacity = strength;
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  return layer;
}

TEST_F(CompositorTest, AnAdjustmentLayerAffectsWhatIsBeneathIt) {
  const std::vector<EffectPass> effects{inverted()};

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
  const std::vector<EffectPass> effects{inverted()};

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
  const std::vector<EffectPass> effects{inverted()};

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
  const std::vector<EffectPass> effects{inverted()};

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
  const std::vector<EffectPass> effects{inverted()};

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

// ------------------------------------------------- handing the frame across --

TEST_F(CompositorTest, TheDisplayTextureDescribesTheCanvas) {
  ASSERT_TRUE(compositor_->compose({}).has_value());

  const auto scene = compositor_->display_texture();
  ASSERT_TRUE(scene.has_value()) << scene.error();
  EXPECT_FALSE(scene->empty());
  EXPECT_EQ(scene->width, kWidth);
  EXPECT_EQ(scene->height, kHeight);
  // Whoever samples this is told the state, and being wrong about it is not
  // the sort of mistake a picture shows.
  EXPECT_EQ(scene->state, static_cast<unsigned>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
  // Plain, not _SRGB. Sampling through an _SRGB view would decode back to
  // linear on the way out and the preview would come out washed out.
  EXPECT_EQ(scene->format, static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
}

TEST_F(CompositorTest, TheSameFrameComesBackEitherWay) {
  // The one failure that would go unnoticed: the texture path leaving the
  // display target in a state the readback path does not expect, so that asking
  // for the pixels after asking for the texture gives something else.
  Layer layer = fill({0.0f, 1.0f, 0.0f, 1.0f});

  ASSERT_TRUE(compositor_->compose({&layer, 1}).has_value());
  const auto before = compositor_->read_back();
  ASSERT_TRUE(before.has_value()) << before.error();

  ASSERT_TRUE(compositor_->display_texture().has_value());

  const auto after = compositor_->read_back();
  ASSERT_TRUE(after.has_value()) << after.error();
  EXPECT_EQ(after->pixels, before->pixels)
      << "handing the texture over changed what the pixels read back as";
}

TEST_F(CompositorTest, TheTextureCanBeAskedForRepeatedly) {
  ASSERT_TRUE(compositor_->compose({}).has_value());

  const auto first = compositor_->display_texture();
  ASSERT_TRUE(first.has_value()) << first.error();
  const auto second = compositor_->display_texture();
  ASSERT_TRUE(second.has_value()) << second.error();

  // The same target every time, so a caller may keep the handle across frames
  // and simply find newer pixels in it.
  EXPECT_EQ(first->resource, second->resource);
}

// ---------------------------------------------------------- rasterised RGBA --

/// A 2x2 patch of premultiplied sRGB RGBA, which is what a rasteriser hands
/// over: opaque red on the left, fully transparent on the right.
struct RgbaPatch {
  static constexpr int kSize = 2;
  std::array<std::uint8_t, kSize * kSize * 4> pixels{
      255, 0, 0, 255, 0, 0, 0, 0,  //
      255, 0, 0, 255, 0, 0, 0, 0,
  };

  [[nodiscard]] FrameView view() const {
    FrameView frame;
    frame.width = kSize;
    frame.height = kSize;
    frame.layout = PixelLayout::Rgba8;
    frame.full_range = true;
    frame.planes[0] = PlaneView{.data = pixels.data(), .stride = kSize * 4};
    return frame;
  }
};

TEST_F(CompositorTest, ARasterisedLayerKeepsItsColourAndItsHoles) {
  const RgbaPatch patch;
  const FrameView frame = patch.view();

  Layer layer;
  layer.frame = &frame;
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};

  const std::array<Layer, 1> layers{layer};
  const Image image = render(layers);
  ASSERT_FALSE(image.empty());

  // Well inside the opaque half, since a bilinear sampler mixes the two columns
  // along the seam between them.
  const Rgba solid = pixel_at(image, 4, kHeight / 2);
  EXPECT_GT(solid.r, 250);
  EXPECT_LT(solid.g, 5);
  EXPECT_EQ(solid.a, 255);

  // And the transparent half stays transparent rather than turning black: an
  // alpha of zero has to survive the trip through the effects path.
  EXPECT_EQ(pixel_at(image, kWidth - 4, kHeight / 2).a, 0);
}

TEST_F(CompositorTest, ARasterisedLayerTakesTheEffectsAppliedToIt) {
  const RgbaPatch patch;
  const FrameView frame = patch.view();

  Layer layer;
  layer.frame = &frame;
  layer.quad = {kWidth * 0.5f, kHeight * 0.5f, kWidth, kHeight, 0.0f};
  // A title is a source like any other: the effect stack has to reach it the
  // same way it reaches a video frame.
  const std::vector<EffectPass> effects{colour(0.0f, 1.0f, 0.0f)};
  layer.passes = effects;

  const std::array<Layer, 1> layers{layer};
  const Rgba grey = pixel_at(render(layers), 4, kHeight / 2);
  EXPECT_EQ(grey.r, grey.g);
  EXPECT_EQ(grey.g, grey.b);
  EXPECT_GT(grey.a, 250) << "desaturating must not touch the alpha";
}

TEST_F(CompositorTest, ResizingGivesATextureOfTheNewSize) {
  ASSERT_TRUE(compositor_->compose({}).has_value());
  ASSERT_TRUE(compositor_->display_texture().has_value());

  ASSERT_TRUE(compositor_->resize(kWidth * 2, kHeight * 2).has_value());
  ASSERT_TRUE(compositor_->compose({}).has_value());

  const auto after = compositor_->display_texture();
  ASSERT_TRUE(after.has_value()) << after.error();
  EXPECT_EQ(after->width, kWidth * 2);
  EXPECT_EQ(after->height, kHeight * 2);

  // Deliberately no assertion that the handle changed. A resize frees the old
  // target and Direct3D is free to put the new one at the same address — which
  // it does often enough that asserting otherwise fails only when the tests run
  // in a particular order. That reuse is exactly why the contract says a
  // texture is good until the next call and no longer: a stale handle does not
  // become invalid in any way a comparison could catch. It just quietly refers
  // to something else.
}

}  // namespace
}  // namespace cutline::gpu
