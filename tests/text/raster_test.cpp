/// Graphic layers, checked by looking at the pixels.
///
/// Glyph shapes are the font's business and not worth asserting on — what
/// matters here is that the image is the size the layout said it would be, that
/// what should be transparent is, and that the premultiplied contract the
/// compositor relies on actually holds.

#include "cutline/text/raster.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace cutline::text {
namespace {

using core::GraphicKind;
using core::GraphicLayer;

[[nodiscard]] GraphicLayer title(std::string content = "Hello") {
  GraphicLayer layer;
  layer.text = std::move(content);
  layer.style.font_size = 48.0;
  return layer;
}

[[nodiscard]] GraphicLayer box(double width, double height) {
  GraphicLayer layer;
  layer.kind = GraphicKind::Rectangle;
  layer.width = width;
  layer.height = height;
  layer.style.fill = "#ff0000";
  return layer;
}

/// How many pixels are not fully transparent.
[[nodiscard]] std::size_t drawn(const Raster& raster) {
  std::size_t count = 0;
  for (std::size_t i = 3; i < raster.pixels.size(); i += 4) {
    if (raster.pixels[i] > 0) ++count;
  }
  return count;
}

[[nodiscard]] std::uint8_t channel_at(const Raster& raster, int x, int y, int channel) {
  const std::size_t index = (static_cast<std::size_t>(y) * raster.width + x) * 4 + channel;
  return index < raster.pixels.size() ? raster.pixels[index] : 0;
}

[[nodiscard]] std::uint8_t alpha_at(const Raster& raster, int x, int y) {
  return channel_at(raster, x, y, 3);
}

/// The centre of mass of the drawn pixels, as a fraction of the size. What
/// tells left-aligned text from right-aligned without reading glyphs.
[[nodiscard]] double horizontal_balance(const Raster& raster) {
  double weighted = 0.0;
  double total = 0.0;
  for (int y = 0; y < raster.height; ++y) {
    for (int x = 0; x < raster.width; ++x) {
      const double a = alpha_at(raster, x, y);
      weighted += a * x;
      total += a;
    }
  }
  return total > 0.0 ? weighted / total / raster.width : 0.5;
}

[[nodiscard]] double vertical_balance(const Raster& raster) {
  double weighted = 0.0;
  double total = 0.0;
  for (int y = 0; y < raster.height; ++y) {
    for (int x = 0; x < raster.width; ++x) {
      const double a = alpha_at(raster, x, y);
      weighted += a * y;
      total += a;
    }
  }
  return total > 0.0 ? weighted / total / raster.height : 0.5;
}

// --------------------------------------------------------------------- text --

TEST(Rasterise, DrawsSomething) {
  const auto raster = rasterise(title());
  ASSERT_TRUE(raster.has_value()) << raster.error();
  EXPECT_GT(raster->width, 0);
  EXPECT_GT(raster->height, 0);
  EXPECT_GT(drawn(*raster), 0u) << "every pixel came out transparent";
  EXPECT_EQ(raster->pixels.size(),
            static_cast<std::size_t>(raster->width) * raster->height * 4);
}

TEST(Rasterise, IsTallEnoughForItsFontSize) {
  const auto raster = rasterise(title());
  ASSERT_TRUE(raster.has_value()) << raster.error();
  // Ascent plus descent is most of the font size, whatever the face.
  EXPECT_GE(raster->height, 40);
}

TEST(Rasterise, TheBackgroundIsTransparentUnlessAskedFor) {
  const auto raster = rasterise(title());
  ASSERT_TRUE(raster.has_value()) << raster.error();
  // The corners are margin, which nothing draws into.
  EXPECT_EQ(alpha_at(*raster, 0, 0), 0);
  EXPECT_EQ(alpha_at(*raster, raster->width - 1, raster->height - 1), 0);
}

TEST(Rasterise, ABackgroundBoxReachesPastTheTextByItsPadding) {
  GraphicLayer layer = title();
  layer.style.background =
      core::GraphicBackground{.color = "#000000", .opacity = 1.0, .padding = 10.0};

  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  const core::LayerExtent extent = measure(layer);
  // Inside the padding, just outside the text, it is solid.
  EXPECT_EQ(alpha_at(*raster, static_cast<int>(extent.left) - 5, raster->height / 2), 255);
  // And the margin past the padding is still clear.
  EXPECT_EQ(alpha_at(*raster, 0, 0), 0);
}

TEST(Rasterise, ABackgroundsOpacityIsItsAlpha) {
  GraphicLayer layer = title();
  layer.style.background =
      core::GraphicBackground{.color = "#000000", .opacity = 0.5, .padding = 10.0};
  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  const core::LayerExtent extent = measure(layer);
  EXPECT_NEAR(alpha_at(*raster, static_cast<int>(extent.left) - 5, raster->height / 2), 128, 2);
}

TEST(Rasterise, TwoLinesAreTallerThanOne) {
  const auto one = rasterise(title("Hello"));
  const auto two = rasterise(title("Hello\nagain"));
  ASSERT_TRUE(one.has_value());
  ASSERT_TRUE(two.has_value());
  EXPECT_GT(two->height, one->height);
}

TEST(Rasterise, LeadingSpacesTheLinesFurtherApart) {
  GraphicLayer tight = title("Hello\nagain");
  GraphicLayer loose = tight;
  loose.style.leading = 40.0;
  EXPECT_NEAR(measure(loose).content.height - measure(tight).content.height, 40.0, 0.5);
}

TEST(Rasterise, ALongerLineIsWider) {
  const auto few = rasterise(title("i"));
  const auto many = rasterise(title("wwwwwwwwww"));
  ASSERT_TRUE(few.has_value());
  ASSERT_TRUE(many.has_value());
  EXPECT_GT(many->width, few->width);
}

TEST(Rasterise, TrackingSpreadsTheLetters) {
  GraphicLayer plain = title("wwwww");
  GraphicLayer spread = plain;
  // Two hundred thousandths of 48 pixels, four gaps.
  spread.style.tracking = 200.0;
  EXPECT_NEAR(measure(spread).content.width - measure(plain).content.width, 4 * 9.6, 1.0);
}

TEST(Rasterise, AParagraphBoxWrapsAtSpaces) {
  GraphicLayer point = title("one two three four five six");
  GraphicLayer paragraph = point;
  paragraph.box_width = 200.0;
  const core::LayerExtent wrapped = measure(paragraph);
  EXPECT_DOUBLE_EQ(wrapped.content.width, 200.0) << "the box is the text's width";
  EXPECT_GT(wrapped.content.height, measure(point).content.height * 2.0)
      << "the words did not wrap onto more lines";
}

TEST(Rasterise, AllCapsIsDrawnInCapitals) {
  GraphicLayer lower = title("xxxx");
  GraphicLayer upper = lower;
  upper.style.all_caps = true;
  EXPECT_GT(measure(upper).content.width, measure(lower).content.width);
}

TEST(Rasterise, AlignmentDecidesWhereShortLinesSit) {
  // A long line sets the width; a short one is placed within it.
  const auto aligned = [](core::TextAlign align) {
    GraphicLayer layer = title("wwwwwwwwww\ni");
    layer.style.align = align;
    return rasterise(layer);
  };
  const auto centred = aligned(core::TextAlign::Center);
  const auto left = aligned(core::TextAlign::Left);
  const auto right = aligned(core::TextAlign::Right);
  ASSERT_TRUE(centred.has_value());
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());

  EXPECT_LT(horizontal_balance(*left), horizontal_balance(*centred));
  EXPECT_GT(horizontal_balance(*right), horizontal_balance(*centred));
}

TEST(Rasterise, ATypewriterDrawsTheFirstLettersWhereTheWholeTextPutsThem) {
  const GraphicLayer layer = title("wwwwwwwwww");
  const auto whole = rasterise(layer);
  const auto part = rasterise(layer, 3);
  ASSERT_TRUE(whole.has_value());
  ASSERT_TRUE(part.has_value());
  // The same picture size, so the letters already typed do not move.
  EXPECT_EQ(part->width, whole->width);
  EXPECT_EQ(part->height, whole->height);
  EXPECT_LT(drawn(*part), drawn(*whole));
  EXPECT_LT(horizontal_balance(*part), 0.4) << "the first letters are on the left";
}

TEST(Rasterise, ThePixelsArePremultiplied) {
  // The compositor divides the alpha back out, and a channel brighter than the
  // alpha it was multiplied by would come back over 1 and clip.
  GraphicLayer layer = title();
  layer.style.fill = "#ffffff";

  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();

  for (std::size_t i = 0; i + 3 < raster->pixels.size(); i += 4) {
    const std::uint8_t a = raster->pixels[i + 3];
    ASSERT_LE(raster->pixels[i + 0], a) << "red exceeds alpha at byte " << i;
    ASSERT_LE(raster->pixels[i + 1], a) << "green exceeds alpha at byte " << i;
    ASSERT_LE(raster->pixels[i + 2], a) << "blue exceeds alpha at byte " << i;
  }
}

TEST(Rasterise, TheColourIsTheOneAsked) {
  GraphicLayer layer = title();
  layer.style.fill = "#ff0000";

  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();

  // The most opaque pixel is inside a glyph, and there the fill is undiluted.
  std::size_t best = 0;
  for (std::size_t i = 3; i < raster->pixels.size(); i += 4) {
    if (raster->pixels[i] > raster->pixels[best + 3]) best = i - 3;
  }
  EXPECT_GT(raster->pixels[best + 0], 200) << "red should dominate";
  EXPECT_LT(raster->pixels[best + 1], 40);
  EXPECT_LT(raster->pixels[best + 2], 40);
}

TEST(Rasterise, AMalformedColourFallsBackRatherThanVanishing) {
  GraphicLayer layer = title();
  layer.style.fill = "not a colour";

  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  EXPECT_GT(drawn(*raster), 0u);
}

TEST(Rasterise, AnUnknownFontIsSubstitutedRatherThanRefused) {
  // A project written on another machine can name anything. Every other
  // application substitutes; refusing to draw the title would be worse.
  GraphicLayer layer = title();
  layer.style.font_family = "Definitely Not An Installed Font, sans-serif";

  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  EXPECT_GT(drawn(*raster), 0u);
}

TEST(Rasterise, AStrokeAndAShadowFitInsideTheImage) {
  GraphicLayer layer = title();
  layer.style.strokes = {core::GraphicStroke{.color = "#000000", .width = 6.0}};
  layer.style.shadow = core::GraphicShadow{};

  const auto plain = rasterise(title());
  const auto decorated = rasterise(layer);
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(decorated.has_value()) << decorated.error();

  // Room was made for both rather than letting them run off the edge.
  EXPECT_GT(decorated->width, plain->width);
  EXPECT_GT(decorated->height, plain->height);
  EXPECT_EQ(alpha_at(*decorated, 0, 0), 0) << "the margin should still be clear";
}

TEST(Rasterise, NoTextIsRefusedRatherThanDrawnEmpty) {
  EXPECT_FALSE(rasterise(title("")).has_value());

  GraphicLayer sizeless = title();
  sizeless.style.font_size = 0.0;
  EXPECT_FALSE(rasterise(sizeless).has_value());
}

// ------------------------------------------------------------------- shapes --

TEST(Rasterise, ARectangleFillsItsBoxInsideItsMargin) {
  const GraphicLayer layer = box(100.0, 40.0);
  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  const core::LayerExtent extent = measure(layer);
  EXPECT_DOUBLE_EQ(extent.content.width, 100.0);
  const int cx = static_cast<int>(extent.left + 50.0);
  const int cy = static_cast<int>(extent.top + 20.0);
  EXPECT_EQ(alpha_at(*raster, cx, cy), 255);
  EXPECT_GT(channel_at(*raster, cx, cy, 0), 250);
  EXPECT_EQ(alpha_at(*raster, 0, 0), 0);
}

TEST(Rasterise, AnEllipseLeavesItsCornersEmpty) {
  GraphicLayer layer = box(100.0, 100.0);
  layer.kind = GraphicKind::Ellipse;
  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  const core::LayerExtent extent = measure(layer);
  EXPECT_EQ(alpha_at(*raster, static_cast<int>(extent.left) + 3, static_cast<int>(extent.top) + 3), 0);
  EXPECT_EQ(alpha_at(*raster, static_cast<int>(extent.left + 50), static_cast<int>(extent.top + 50)),
            255);
}

TEST(Rasterise, RoundedCornersLeaveTheCornerEmpty) {
  GraphicLayer square = box(100.0, 100.0);
  GraphicLayer rounded = square;
  rounded.corner_radius = 30.0;
  const auto a = rasterise(square);
  const auto b = rasterise(rounded);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const core::LayerExtent extent = measure(square);
  const int x = static_cast<int>(extent.left) + 2;
  const int y = static_cast<int>(extent.top) + 2;
  EXPECT_EQ(alpha_at(*a, x, y), 255);
  EXPECT_EQ(alpha_at(*b, x, y), 0);
}

TEST(Rasterise, StrokesStackOutwardsTheFirstInnermost) {
  GraphicLayer layer = box(100.0, 100.0);
  layer.style.strokes = {core::GraphicStroke{.color = "#00ff00", .width = 6.0},
                         core::GraphicStroke{.color = "#0000ff", .width = 6.0}};
  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  const core::LayerExtent extent = measure(layer);
  const int y = static_cast<int>(extent.top + 50.0);
  // Three pixels outside the edge is the green stroke; nine is the blue one.
  const int green = static_cast<int>(extent.left) - 3;
  const int blue = static_cast<int>(extent.left) - 9;
  EXPECT_GT(channel_at(*raster, green, y, 1), 200);
  EXPECT_LT(channel_at(*raster, green, y, 2), 50);
  EXPECT_GT(channel_at(*raster, blue, y, 2), 200);
  EXPECT_LT(channel_at(*raster, blue, y, 1), 50);
}

TEST(Rasterise, AShadowFallsAwayFromTheLight) {
  // The light at 135 degrees is up and to the left, so the shadow is below
  // and to the right of the shape.
  GraphicLayer layer = box(60.0, 60.0);
  layer.style.fill_enabled = false;
  layer.style.shadow = core::GraphicShadow{.opacity = 1.0, .angle = 135.0, .distance = 20.0,
                                           .blur = 0.0};
  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  EXPECT_GT(horizontal_balance(*raster), 0.55);
  EXPECT_GT(vertical_balance(*raster), 0.55);
}

TEST(Rasterise, AShapeWithNoFillDrawsOnlyItsStroke) {
  GraphicLayer layer = box(100.0, 100.0);
  layer.style.fill_enabled = false;
  layer.style.strokes = {core::GraphicStroke{.width = 4.0}};
  const auto raster = rasterise(layer);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  const core::LayerExtent extent = measure(layer);
  EXPECT_EQ(alpha_at(*raster, static_cast<int>(extent.left + 50), static_cast<int>(extent.top + 50)),
            0);
}

TEST(Rasterise, AMissingPictureIsSaidRatherThanDrawnBlank) {
  GraphicLayer layer;
  layer.kind = GraphicKind::Image;
  layer.path = "C:/definitely/not/here.png";
  EXPECT_FALSE(rasterise(layer).has_value());
}

// ------------------------------------------------------------------ measure --

TEST(Measure, AgreesWithWhatIsDrawn) {
  // The placement sizes a layer's quad from `measure` and the compositor fills
  // it with what `rasterise` produced. If the two disagree it is stretched.
  for (const GraphicLayer& layer : {title("Hello\nagain"), box(120.0, 30.0)}) {
    const core::Size size = measure(layer).drawn();
    const auto raster = rasterise(layer);
    ASSERT_TRUE(raster.has_value()) << raster.error();
    EXPECT_NEAR(size.width, raster->width, 1.0);
    EXPECT_NEAR(size.height, raster->height, 1.0);
  }
}

TEST(Measure, NothingMeasuresToNothing) {
  EXPECT_EQ(measure(title("")).content, core::Size{});
}

TEST(Measure, ABiggerFontMeasuresBigger) {
  GraphicLayer small = title();
  small.style.font_size = 24.0;
  GraphicLayer large = title();
  large.style.font_size = 96.0;

  EXPECT_GT(measure(large).content.width, measure(small).content.width);
  EXPECT_GT(measure(large).content.height, measure(small).content.height);
}

TEST(Fonts, TheMachinesFamiliesAreListedOnceEachInOrder) {
  const std::vector<std::string> families = font_families();
  ASSERT_FALSE(families.empty());
  EXPECT_TRUE(std::ranges::is_sorted(families));
  EXPECT_EQ(std::ranges::adjacent_find(families), families.end());
}

}  // namespace
}  // namespace cutline::text
