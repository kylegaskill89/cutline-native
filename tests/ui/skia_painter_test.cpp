/// The Skia backend, checked against the pixels it actually produces.
///
/// `RecordingPainter` proves the right calls are made in the right order; it
/// cannot say whether a gradient runs the right way up or whether a raised
/// bevel puts its light edge on top. These render to a CPU raster surface — no
/// GPU, no window — and read the result back.
///
/// The assertions are about relationships between pixels rather than exact
/// values: a gradient's top being lighter than its bottom is what the theme
/// means, where a specific byte would break on any antialiasing change.

#include "cutline/ui/skia_painter.hpp"

#include "cutline/ui/theme.hpp"

#include <gtest/gtest.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <memory>

namespace cutline::ui {
namespace {

constexpr int kWidth = 200;
constexpr int kHeight = 100;

/// A raster surface and a painter over it.
class Canvas {
 public:
  Canvas() {
    surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(kWidth, kHeight));
    if (surface_ != nullptr) {
      surface_->getCanvas()->clear(SK_ColorBLACK);
      painter_ = SkiaPainter::create(surface_->getCanvas());
    }
  }

  [[nodiscard]] bool ready() const { return surface_ != nullptr && painter_ != nullptr; }
  [[nodiscard]] SkiaPainter& painter() const { return *painter_; }

  /// The pixel at (x, y) as a theme colour, so assertions read in the same
  /// terms the themes are written in.
  [[nodiscard]] Color at(int x, int y) const {
    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return {};
    const SkColor4f c = pixmap.getColor4f(x, y);
    return Color{c.fR, c.fG, c.fB, c.fA};
  }

  /// Perceived lightness, for comparing one part of a shape to another.
  [[nodiscard]] double luma(int x, int y) const {
    const Color c = at(x, y);
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
  }

 private:
  sk_sp<SkSurface> surface_;
  std::unique_ptr<SkiaPainter> painter_;
};

constexpr Rect kBox{20.0, 20.0, 160.0, 60.0};

class SkiaPainterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!canvas_.ready()) GTEST_SKIP() << "could not create a raster surface";
  }
  Canvas canvas_;
};

// -------------------------------------------------------------------- fill --

TEST_F(SkiaPainterTest, ASolidFillPaintsItsColour) {
  canvas_.painter().fill(kBox, 0.0, Fill::solid(parse_color("#ff8000")));

  const Color centre = canvas_.at(100, 50);
  EXPECT_NEAR(centre.r, 1.0f, 0.02f);
  EXPECT_NEAR(centre.g, 0.5f, 0.02f);
  EXPECT_NEAR(centre.b, 0.0f, 0.02f);
}

TEST_F(SkiaPainterTest, AFillStaysInsideItsBounds) {
  canvas_.painter().fill(kBox, 0.0, Fill::solid(parse_color("#ffffff")));

  // Just outside the box on every side is still the black it was cleared to.
  EXPECT_LT(canvas_.luma(100, 18), 0.05);
  EXPECT_LT(canvas_.luma(100, 82), 0.05);
  EXPECT_LT(canvas_.luma(18, 50), 0.05);
  EXPECT_LT(canvas_.luma(182, 50), 0.05);
}

TEST_F(SkiaPainterTest, AVerticalGradientRunsTopToBottom) {
  // Zero degrees has to mean vertical: nearly all chrome uses it, and a
  // gradient that came out sideways would be wrong in every theme at once.
  canvas_.painter().fill(kBox, 0.0,
                         Fill::gradient({{0.0f, parse_color("#ffffff")},
                                         {1.0f, parse_color("#000000")}}));

  EXPECT_GT(canvas_.luma(100, 23), 0.8) << "the top should be the first stop";
  EXPECT_LT(canvas_.luma(100, 77), 0.2) << "the bottom should be the last stop";
  EXPECT_GT(canvas_.luma(100, 30), canvas_.luma(100, 70)) << "the ramp is upside down";
}

TEST_F(SkiaPainterTest, AGradientAtNinetyDegreesRunsAcross) {
  canvas_.painter().fill(kBox, 0.0,
                         Fill::gradient({{0.0f, parse_color("#ffffff")},
                                         {1.0f, parse_color("#000000")}},
                                        90.0));

  EXPECT_GT(canvas_.luma(25, 50), canvas_.luma(175, 50));
  // And it should not also be varying vertically.
  EXPECT_NEAR(canvas_.luma(100, 30), canvas_.luma(100, 70), 0.05);
}

TEST_F(SkiaPainterTest, AGradientStopIsHonouredWhereItSits) {
  // The XP gloss depends on a hard step partway down. If positions were ignored
  // and colours spread evenly, this would come out as a smooth ramp.
  canvas_.painter().fill(kBox, 0.0,
                         Fill::gradient({{0.00f, parse_color("#ffffff")},
                                         {0.49f, parse_color("#ffffff")},
                                         {0.51f, parse_color("#000000")},
                                         {1.00f, parse_color("#000000")}}));

  EXPECT_GT(canvas_.luma(100, 35), 0.9) << "above the step should still be white";
  EXPECT_LT(canvas_.luma(100, 65), 0.1) << "below the step should be black";
}

// ------------------------------------------------------------------ stroke --

TEST_F(SkiaPainterTest, ABorderIsDrawnInsideItsBounds) {
  // Skia centres strokes on the path, so without the inset a border would
  // spill outside and make every control a pixel larger than it was laid out.
  canvas_.painter().stroke(kBox, 0.0, parse_color("#ffffff"), 4.0);

  EXPECT_LT(canvas_.luma(100, 18), 0.05) << "the border spilled above its bounds";
  EXPECT_GT(canvas_.luma(100, 22), 0.5) << "the border is not on the inside edge";
  EXPECT_LT(canvas_.luma(100, 50), 0.05) << "a stroke should not fill the middle";
}

// ------------------------------------------------------------------- bevel --

TEST_F(SkiaPainterTest, ARaisedBevelIsLitFromAbove) {
  canvas_.painter().bevel(kBox, Bevel{.width = 3.0,
                                      .light = parse_color("#ffffff"),
                                      .dark = parse_color("#202020"),
                                      .inset = false});

  EXPECT_GT(canvas_.luma(100, 21), 0.8) << "the top edge should be the light one";
  EXPECT_LT(canvas_.luma(100, 79), 0.2) << "the bottom edge should be the dark one";
  EXPECT_GT(canvas_.luma(21, 50), 0.8) << "the left edge should be light";
}

TEST_F(SkiaPainterTest, AnInsetBevelSwapsTheEdges) {
  // The pixels behind the claim that pressing a control is not just darkening
  // it. Same colours, opposite sides.
  canvas_.painter().bevel(kBox, Bevel{.width = 3.0,
                                      .light = parse_color("#ffffff"),
                                      .dark = parse_color("#202020"),
                                      .inset = true});

  EXPECT_LT(canvas_.luma(100, 21), 0.2) << "an inset bevel is dark on top";
  EXPECT_GT(canvas_.luma(100, 79), 0.8) << "an inset bevel is light underneath";
}

// -------------------------------------------------------------------- text --

TEST_F(SkiaPainterTest, TextPutsPixelsDown) {
  canvas_.painter().text(TextRun{.bounds = kBox,
                                 .text = "Export",
                                 .color = parse_color("#ffffff"),
                                 .size = 24.0,
                                 .align = TextAlign::Center});

  int lit = 0;
  for (int y = 20; y < 80; ++y) {
    for (int x = 20; x < 180; ++x) {
      if (canvas_.luma(x, y) > 0.3) ++lit;
    }
  }
  EXPECT_GT(lit, 50) << "nothing was drawn, so there is probably no font";
}

TEST_F(SkiaPainterTest, EmptyTextDrawsNothing) {
  canvas_.painter().text(TextRun{.bounds = kBox, .text = "", .color = parse_color("#ffffff")});
  EXPECT_LT(canvas_.luma(100, 50), 0.05);
}

TEST_F(SkiaPainterTest, MeasuringGrowsWithTheStringAndTheSize) {
  const double small = canvas_.painter().measure("Export", 12.0, false);
  const double large = canvas_.painter().measure("Export", 24.0, false);
  const double longer = canvas_.painter().measure("Export Frame", 12.0, false);

  if (small <= 0.0) GTEST_SKIP() << "no system font available to measure with";
  EXPECT_GT(large, small);
  EXPECT_GT(longer, small);
  EXPECT_DOUBLE_EQ(canvas_.painter().measure("", 12.0, false), 0.0);
}

// ------------------------------------------------------------------- glass --

TEST_F(SkiaPainterTest, TheBackdropBlurSoftensWhatIsBehindIt) {
  // Vista's glass, and the one primitive that reads the surface rather than
  // writing to it. A hard edge underneath should come out gradual.
  canvas_.painter().fill(Rect{0.0, 0.0, 100.0, kHeight}, 0.0,
                         Fill::solid(parse_color("#ffffff")));

  const double before = canvas_.luma(104, 50);
  ASSERT_LT(before, 0.05) << "the right half should start black";

  canvas_.painter().backdrop_blur(Rect{0.0, 0.0, kWidth, kHeight}, 0.0, 24.0);

  const double after = canvas_.luma(104, 50);
  EXPECT_GT(after, before + 0.05) << "white did not bleed across the edge";
  EXPECT_LT(after, 0.95) << "the blur washed the whole area out";
}

// ------------------------------------------------- whole themes, end to end --

TEST_F(SkiaPainterTest, EveryBuiltInButtonDrawsSomething) {
  // Catches a theme whose style is structurally valid but paints nothing
  // visible — a fully transparent fill with no border, say.
  for (const Theme& theme : built_in_themes()) {
    Canvas canvas;
    ASSERT_TRUE(canvas.ready());
    paint_surface(canvas.painter(), kBox, theme.style(Part::Button));

    EXPECT_GT(canvas.at(100, 50).a, 0.0f) << theme.id << " painted nothing";
    EXPECT_GT(canvas.luma(100, 50), 0.02) << theme.id << " painted only black";
  }
}

TEST_F(SkiaPainterTest, AnXpButtonLooksDifferentPressed) {
  // The whole chain: theme to style to primitives to pixels.
  const Theme& xp = *built_in_theme("xp");

  Canvas normal;
  Canvas pressed;
  ASSERT_TRUE(normal.ready());
  ASSERT_TRUE(pressed.ready());

  paint_surface(normal.painter(), kBox, xp.style(Part::Button, State::Normal));
  paint_surface(pressed.painter(), kBox, xp.style(Part::Button, State::Pressed));

  // The top edge is light when raised and dark when pressed, which is the
  // bevel inverting rather than the fill merely darkening.
  EXPECT_GT(normal.luma(100, 22), pressed.luma(100, 22));
}

}  // namespace
}  // namespace cutline::ui
