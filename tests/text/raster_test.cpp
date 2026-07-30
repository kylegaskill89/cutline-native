/// Titles, checked by looking at the pixels.
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

[[nodiscard]] core::TextSpec title(std::string content = "Hello") {
  core::TextSpec spec;
  spec.content = std::move(content);
  spec.font_size = 48.0;
  return spec;
}

/// How many pixels are not fully transparent.
[[nodiscard]] std::size_t drawn(const Raster& raster) {
  std::size_t count = 0;
  for (std::size_t i = 3; i < raster.pixels.size(); i += 4) {
    if (raster.pixels[i] > 0) ++count;
  }
  return count;
}

[[nodiscard]] std::uint8_t alpha_at(const Raster& raster, int x, int y) {
  const std::size_t index = (static_cast<std::size_t>(y) * raster.width + x) * 4 + 3;
  return index < raster.pixels.size() ? raster.pixels[index] : 0;
}

/// The centre of mass of the drawn pixels along x, as a fraction of the width.
/// What tells left-aligned text from right-aligned without reading glyphs.
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

TEST(Rasterise, ABackgroundFillsTheWholeImage) {
  core::TextSpec spec = title();
  spec.background = "#000000";

  const auto raster = rasterise(spec);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  EXPECT_EQ(alpha_at(*raster, 0, 0), 255);
  EXPECT_EQ(alpha_at(*raster, raster->width - 1, raster->height - 1), 255);
}

TEST(Rasterise, TwoLinesAreTallerThanOne) {
  const auto one = rasterise(title("Hello"));
  const auto two = rasterise(title("Hello\nagain"));
  ASSERT_TRUE(one.has_value());
  ASSERT_TRUE(two.has_value());
  EXPECT_GT(two->height, one->height);
}

TEST(Rasterise, ALongerLineIsWider) {
  const auto few = rasterise(title("i"));
  const auto many = rasterise(title("wwwwwwwwww"));
  ASSERT_TRUE(few.has_value());
  ASSERT_TRUE(many.has_value());
  EXPECT_GT(many->width, few->width);
}

TEST(Rasterise, AlignmentDecidesWhereShortLinesSit) {
  // A long line sets the width; a short one is placed within it.
  const auto centred = [] {
    core::TextSpec spec = title("wwwwwwwwww\ni");
    spec.align = core::TextAlign::Center;
    return rasterise(spec);
  }();
  const auto left = [] {
    core::TextSpec spec = title("wwwwwwwwww\ni");
    spec.align = core::TextAlign::Left;
    return rasterise(spec);
  }();
  const auto right = [] {
    core::TextSpec spec = title("wwwwwwwwww\ni");
    spec.align = core::TextAlign::Right;
    return rasterise(spec);
  }();

  ASSERT_TRUE(centred.has_value());
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());

  EXPECT_LT(horizontal_balance(*left), horizontal_balance(*centred));
  EXPECT_GT(horizontal_balance(*right), horizontal_balance(*centred));
}

TEST(Rasterise, ThePixelsArePremultiplied) {
  // The compositor divides the alpha back out, and a channel brighter than the
  // alpha it was multiplied by would come back over 1 and clip.
  core::TextSpec spec = title();
  spec.color = "#ffffff";

  const auto raster = rasterise(spec);
  ASSERT_TRUE(raster.has_value()) << raster.error();

  for (std::size_t i = 0; i + 3 < raster->pixels.size(); i += 4) {
    const std::uint8_t a = raster->pixels[i + 3];
    ASSERT_LE(raster->pixels[i + 0], a) << "red exceeds alpha at byte " << i;
    ASSERT_LE(raster->pixels[i + 1], a) << "green exceeds alpha at byte " << i;
    ASSERT_LE(raster->pixels[i + 2], a) << "blue exceeds alpha at byte " << i;
  }
}

TEST(Rasterise, TheColourIsTheOneAsked) {
  core::TextSpec spec = title();
  spec.color = "#ff0000";

  const auto raster = rasterise(spec);
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
  core::TextSpec spec = title();
  spec.color = "not a colour";

  const auto raster = rasterise(spec);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  EXPECT_GT(drawn(*raster), 0u);
}

TEST(Rasterise, AnUnknownFontIsSubstitutedRatherThanRefused) {
  // A project written on another machine can name anything. Every other
  // application substitutes; refusing to draw the title would be worse.
  core::TextSpec spec = title();
  spec.font_family = "Definitely Not An Installed Font, sans-serif";

  const auto raster = rasterise(spec);
  ASSERT_TRUE(raster.has_value()) << raster.error();
  EXPECT_GT(drawn(*raster), 0u);
}

TEST(Rasterise, AStrokeAndAShadowFitInsideTheImage) {
  core::TextSpec spec = title();
  spec.stroke_color = "#000000";
  spec.stroke_width = 6.0;
  spec.shadow = true;

  const auto plain = rasterise(title());
  const auto decorated = rasterise(spec);
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(decorated.has_value()) << decorated.error();

  // Room was made for both rather than letting them run off the edge.
  EXPECT_GT(decorated->width, plain->width);
  EXPECT_GT(decorated->height, plain->height);
  EXPECT_EQ(alpha_at(*decorated, 0, 0), 0) << "the margin should still be clear";
}

TEST(Rasterise, NoTextIsRefusedRatherThanDrawnEmpty) {
  core::TextSpec spec = title("");
  EXPECT_FALSE(rasterise(spec).has_value());

  core::TextSpec sizeless = title();
  sizeless.font_size = 0.0;
  EXPECT_FALSE(rasterise(sizeless).has_value());
}

TEST(Measure, AgreesWithWhatIsDrawn) {
  // The layout sizes a title's quad from `measure` and the compositor fills it
  // with what `rasterise` produced. If the two disagree the text is stretched.
  const core::TextSpec spec = title("Hello\nagain");
  const Size size = measure(spec);
  const auto raster = rasterise(spec);
  ASSERT_TRUE(raster.has_value()) << raster.error();

  EXPECT_NEAR(size.width, raster->width, 1.0);
  EXPECT_NEAR(size.height, raster->height, 1.0);
}

TEST(Measure, NothingMeasuresToNothing) {
  EXPECT_EQ(measure(title("")), Size{});
}

TEST(Measure, ABiggerFontMeasuresBigger) {
  core::TextSpec small = title();
  small.font_size = 24.0;
  core::TextSpec large = title();
  large.font_size = 96.0;

  EXPECT_GT(measure(large).width, measure(small).width);
  EXPECT_GT(measure(large).height, measure(small).height);
}

}  // namespace
}  // namespace cutline::text
