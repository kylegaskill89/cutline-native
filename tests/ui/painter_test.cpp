/// How a surface is assembled, tested by watching it be drawn.
///
/// `RecordingPainter` turns "does a pressed XP button invert its bevel" into an
/// ordinary assertion. The ordering checks matter most: an inner shadow drawn
/// before the fill is painted over, and a border drawn before the bevel is half
/// covered by it — both survive a glance at a screenshot.

#include "cutline/ui/painter.hpp"

#include "cutline/ui/recording_painter.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace cutline::ui {
namespace {

using Kind = DrawCall::Kind;

constexpr Rect kBounds{10.0, 20.0, 120.0, 30.0};

[[nodiscard]] SurfaceStyle plain() {
  SurfaceStyle style;
  style.fill = Fill::solid(parse_color("#404040"));
  style.text = parse_color("#ffffff");
  return style;
}

// -------------------------------------------------------------------- rect --

TEST(Rect, ReportsItsEdges) {
  EXPECT_DOUBLE_EQ(kBounds.right(), 130.0);
  EXPECT_DOUBLE_EQ(kBounds.bottom(), 50.0);
  EXPECT_FALSE(kBounds.empty());
}

TEST(Rect, ContainsIsHalfOpen) {
  // Half-open, so adjacent rectangles do not both claim the pixel between them.
  EXPECT_TRUE(kBounds.contains(10.0, 20.0));
  EXPECT_FALSE(kBounds.contains(130.0, 50.0));
  EXPECT_TRUE(kBounds.contains(129.9, 49.9));
  EXPECT_FALSE(kBounds.contains(9.9, 20.0));
}

TEST(Rect, InsetShrinksOnEverySide) {
  const Rect inner = kBounds.inset(5.0);
  EXPECT_DOUBLE_EQ(inner.x, 15.0);
  EXPECT_DOUBLE_EQ(inner.y, 25.0);
  EXPECT_DOUBLE_EQ(inner.width, 110.0);
  EXPECT_DOUBLE_EQ(inner.height, 20.0);
}

TEST(Rect, InsetNeverInvertsTheRectangle) {
  // A border wider than the control it is on would otherwise produce negative
  // dimensions, which every drawing API handles differently and none well.
  const Rect crushed = kBounds.inset(1000.0);
  EXPECT_GE(crushed.width, 0.0);
  EXPECT_GE(crushed.height, 0.0);
}

TEST(Rect, AZeroSizedRectIsEmpty) {
  EXPECT_TRUE((Rect{0.0, 0.0, 0.0, 10.0}).empty());
  EXPECT_TRUE((Rect{0.0, 0.0, 10.0, 0.0}).empty());
}

// ------------------------------------------------------------- the basics --

TEST(PaintSurface, APlainFillIsOneCall) {
  RecordingPainter painter;
  paint_surface(painter, kBounds, plain());

  ASSERT_EQ(painter.calls().size(), 1u);
  EXPECT_EQ(painter.calls()[0].kind, Kind::Fill);
  EXPECT_EQ(painter.calls()[0].bounds, kBounds);
}

TEST(PaintSurface, AnEmptyRectangleDrawsNothing) {
  RecordingPainter painter;
  paint_surface(painter, Rect{5.0, 5.0, 0.0, 20.0}, plain());
  EXPECT_TRUE(painter.calls().empty());
}

TEST(PaintSurface, ABorderIsStrokedWhenItHasWidthAndColour) {
  SurfaceStyle style = plain();
  style.border = parse_color("#ff0000");
  style.border_width = 2.0;

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);

  const DrawCall* stroke = painter.first(Kind::Stroke);
  ASSERT_NE(stroke, nullptr);
  EXPECT_EQ(stroke->color, parse_color("#ff0000"));
  EXPECT_DOUBLE_EQ(stroke->width, 2.0);
}

TEST(PaintSurface, AZeroWidthOrTransparentBorderIsSkipped) {
  // Both are how a theme says "no border", and drawing either would cost a pass
  // over the shape for nothing.
  SurfaceStyle no_width = plain();
  no_width.border = parse_color("#ff0000");
  no_width.border_width = 0.0;

  SurfaceStyle no_alpha = plain();
  no_alpha.border = parse_color("#ff000000");
  no_alpha.border_width = 2.0;

  for (const SurfaceStyle& style : {no_width, no_alpha}) {
    RecordingPainter painter;
    paint_surface(painter, kBounds, style);
    EXPECT_EQ(painter.count(Kind::Stroke), 0u);
  }
}

TEST(PaintSurface, TheCornerRadiusReachesEveryShapedCall) {
  SurfaceStyle style = plain();
  style.corner_radius = 6.0;
  style.border = parse_color("#ff0000");
  style.border_width = 1.0;

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);

  for (const DrawCall& call : painter.calls()) {
    if (call.kind == Kind::Fill || call.kind == Kind::Stroke) {
      EXPECT_DOUBLE_EQ(call.corner_radius, 6.0) << to_string(call.kind);
    }
  }
}

// ------------------------------------------------------------------ order --

TEST(PaintSurface, TheBorderGoesOnTopOfTheBevel) {
  // Drawn the other way round, the bevel covers half the border and the control
  // looks like it has an uneven outline.
  SurfaceStyle style = plain();
  style.bevel = Bevel{.width = 1.0, .light = parse_color("#ffffff"),
                      .dark = parse_color("#808080")};
  style.border = parse_color("#000000");
  style.border_width = 1.0;

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);

  EXPECT_LT(painter.index_of(Kind::Bevel), painter.index_of(Kind::Stroke));
}

TEST(PaintSurface, TheBevelGoesOnTopOfTheFill) {
  SurfaceStyle style = plain();
  style.bevel = Bevel{.width = 1.0, .light = parse_color("#ffffff"),
                      .dark = parse_color("#808080")};

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);
  EXPECT_LT(painter.index_of(Kind::Fill), painter.index_of(Kind::Bevel));
}

TEST(PaintSurface, AnOuterShadowGoesBehindTheFill) {
  SurfaceStyle style = plain();
  style.shadow = Shadow{.offset_x = 0.0, .offset_y = 2.0, .blur = 6.0,
                        .color = parse_color("#00000080"), .inner = false};

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);
  EXPECT_LT(painter.index_of(Kind::Shadow), painter.index_of(Kind::Fill));
}

TEST(PaintSurface, AnInnerShadowGoesOverTheFillAndIsClipped) {
  // Drawn before the fill it would simply be painted over; drawn unclipped it
  // would spill outside the control.
  SurfaceStyle style = plain();
  style.corner_radius = 4.0;
  style.shadow = Shadow{.offset_x = 0.0, .offset_y = 1.0, .blur = 4.0,
                        .color = parse_color("#00000080"), .inner = true};

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);

  EXPECT_GT(painter.index_of(Kind::Shadow), painter.index_of(Kind::Fill));
  EXPECT_LT(painter.index_of(Kind::PushClip), painter.index_of(Kind::Shadow));
  EXPECT_GT(painter.index_of(Kind::PopClip), painter.index_of(Kind::Shadow));
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(PaintSurface, EveryClipIsPopped) {
  // A leaked clip silently swallows everything drawn afterwards, which looks
  // like a missing widget rather than a painting bug.
  for (const bool inner : {false, true}) {
    SurfaceStyle style = plain();
    style.shadow = Shadow{.blur = 4.0, .color = parse_color("#00000080"), .inner = inner};

    RecordingPainter painter;
    paint_surface(painter, kBounds, style);
    EXPECT_TRUE(painter.clips_balanced()) << "inner = " << inner;
  }
}

// ------------------------------------------------------------------ glass --

TEST(PaintSurface, GlassBlursTheBackdropBeforeTinting) {
  // The blur reads what is already on the surface, so it has to happen before
  // anything is drawn over it.
  SurfaceStyle style;
  style.fill = Fill::glass(parse_color("#ffffff40"), 18.0);

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);

  ASSERT_EQ(painter.count(Kind::BackdropBlur), 1u);
  EXPECT_LT(painter.index_of(Kind::BackdropBlur), painter.index_of(Kind::Fill));
  EXPECT_DOUBLE_EQ(painter.first(Kind::BackdropBlur)->width, 18.0);
}

TEST(PaintSurface, GlassWithNoBlurSkipsThePass) {
  // Reading back the surface is the expensive primitive; a zero radius is a
  // translucent fill and should cost like one.
  SurfaceStyle style;
  style.fill = Fill::glass(parse_color("#ffffff40"), 0.0);

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);
  EXPECT_EQ(painter.count(Kind::BackdropBlur), 0u);
  EXPECT_EQ(painter.count(Kind::Fill), 1u);
}

TEST(PaintSurface, ASolidFillNeverBlurs) {
  RecordingPainter painter;
  paint_surface(painter, kBounds, plain());
  EXPECT_EQ(painter.count(Kind::BackdropBlur), 0u);
}

// ------------------------------------------------- the real themes drawing --

TEST(PaintSurface, AnXpButtonDrawsItsBevelRaisedAndPressedInset) {
  // The chrome claim, followed all the way through to the drawing: pressing
  // does not merely darken the fill, it swaps which edges are light.
  const Theme& xp = *built_in_theme("xp");

  RecordingPainter normal;
  paint_surface(normal, kBounds, xp.style(Part::Button, State::Normal));
  const DrawCall* raised = normal.first(Kind::Bevel);
  ASSERT_NE(raised, nullptr) << "an XP button with no bevel is not an XP button";
  ASSERT_TRUE(raised->bevel.has_value());
  EXPECT_FALSE(raised->bevel->inset);

  RecordingPainter pressed;
  paint_surface(pressed, kBounds, xp.style(Part::Button, State::Pressed));
  const DrawCall* sunken = pressed.first(Kind::Bevel);
  ASSERT_NE(sunken, nullptr);
  ASSERT_TRUE(sunken->bevel.has_value());
  EXPECT_TRUE(sunken->bevel->inset);
}

TEST(PaintSurface, AnAeroPanelBlursWhatIsBehindIt) {
  RecordingPainter painter;
  paint_surface(painter, kBounds, built_in_theme("aero")->style(Part::Panel));
  EXPECT_EQ(painter.count(Kind::BackdropBlur), 1u);
}

TEST(PaintSurface, AFlatButtonDrawsNeitherBevelNorBlur) {
  RecordingPainter painter;
  paint_surface(painter, kBounds, built_in_theme("flat")->style(Part::Button));
  EXPECT_EQ(painter.count(Kind::Bevel), 0u);
  EXPECT_EQ(painter.count(Kind::BackdropBlur), 0u);
  EXPECT_EQ(painter.count(Kind::Fill), 1u);
  EXPECT_EQ(painter.count(Kind::Stroke), 1u);
}

TEST(PaintSurface, EveryBuiltInPartPaintsSomethingAndBalancesItsClips) {
  // A part that draws nothing at all is a widget that will be invisible, and a
  // leaked clip takes the rest of the frame with it.
  for (const Theme& theme : built_in_themes()) {
    for (int i = 0; i <= static_cast<int>(Part::ScrollThumb); ++i) {
      const auto part = static_cast<Part>(i);
      RecordingPainter painter;
      paint_surface(painter, kBounds, theme.style(part));

      EXPECT_FALSE(painter.calls().empty())
          << theme.id << " / " << to_string(part) << " draws nothing";
      EXPECT_TRUE(painter.clips_balanced()) << theme.id << " / " << to_string(part);
    }
  }
}

// ------------------------------------------------------------------- text --

TEST(TextRuns, TakeTheirColourAndGlowFromTheStyle) {
  SurfaceStyle style = plain();
  style.text = parse_color("#12ab34");
  style.text_glow = 3.0;

  const TextRun run = text_run(kBounds, "Export", style, 13.0, TextAlign::Center, true);
  EXPECT_EQ(run.color, parse_color("#12ab34"));
  EXPECT_DOUBLE_EQ(run.glow, 3.0);
  EXPECT_DOUBLE_EQ(run.size, 13.0);
  EXPECT_EQ(run.align, TextAlign::Center);
  EXPECT_TRUE(run.bold);
  EXPECT_EQ(run.text, "Export");
}

TEST(TextRuns, AeroLabelsGlowAndFlatLabelsDoNot) {
  const TextRun aero =
      text_run(kBounds, "Title", built_in_theme("aero")->style(Part::TitleBar), 13.0);
  const TextRun flat =
      text_run(kBounds, "Title", built_in_theme("flat")->style(Part::TitleBar), 13.0);

  EXPECT_GT(aero.glow, 0.0) << "dark text on glass needs a halo to stay readable";
  EXPECT_DOUBLE_EQ(flat.glow, 0.0);
}

TEST(RecordedText, IsCapturedWithItsRun) {
  RecordingPainter painter;
  painter.text(text_run(kBounds, "Hello", plain(), 12.0));

  ASSERT_EQ(painter.count(Kind::Text), 1u);
  const DrawCall* call = painter.first(Kind::Text);
  ASSERT_TRUE(call->run.has_value());
  EXPECT_EQ(call->run->text, "Hello");
}

// -------------------------------------------------------------- recording --

TEST(RecordingPainterBasics, ClearForgetsEverything) {
  RecordingPainter painter;
  paint_surface(painter, kBounds, plain());
  ASSERT_FALSE(painter.calls().empty());
  painter.clear();
  EXPECT_TRUE(painter.calls().empty());
}

TEST(RecordingPainterBasics, AnUnbalancedClipIsReported) {
  RecordingPainter painter;
  painter.push_clip(kBounds, 0.0);
  EXPECT_FALSE(painter.clips_balanced());

  painter.pop_clip();
  EXPECT_TRUE(painter.clips_balanced());

  painter.pop_clip();  // one too many
  EXPECT_FALSE(painter.clips_balanced());
}

TEST(RecordingPainterBasics, IndexOfIsNegativeWhenAKindIsAbsent) {
  RecordingPainter painter;
  paint_surface(painter, kBounds, plain());
  EXPECT_EQ(painter.index_of(Kind::Bevel), -1);
  EXPECT_EQ(painter.first(Kind::Bevel), nullptr);
}

TEST(RecordingPainterBasics, KindsComeBackInOrder) {
  SurfaceStyle style = plain();
  style.bevel = Bevel{.width = 1.0};
  style.border = parse_color("#000000");
  style.border_width = 1.0;

  RecordingPainter painter;
  paint_surface(painter, kBounds, style);

  const std::vector<Kind> expected{Kind::Fill, Kind::Bevel, Kind::Stroke};
  EXPECT_EQ(painter.kinds(), expected);
}

}  // namespace
}  // namespace cutline::ui
