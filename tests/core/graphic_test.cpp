/// Placing a graphic's layers: where each one is at a moment, after its
/// keyframes, its pins, its entrance and exit, and a roll or crawl.

#include "cutline/core/graphic.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::core {
namespace {

constexpr double kW = 1920.0;
constexpr double kH = 1080.0;
constexpr double kLength = 10.0;

/// A 200 by 100 shape centred at a point given in pixels.
[[nodiscard]] GraphicLayer shape(std::string id, double x = kW / 2.0, double y = kH / 2.0) {
  GraphicLayer layer;
  layer.id = std::move(id);
  layer.kind = GraphicKind::Rectangle;
  layer.width = 200.0;
  layer.height = 100.0;
  layer.x = x / kW;
  layer.y = y / kH;
  return layer;
}

[[nodiscard]] std::vector<PlacedLayer> at(const Graphic& graphic, double t,
                                          double length = kLength) {
  return place_graphic(graphic, t, length, kW, kH, measure_without_fonts);
}

[[nodiscard]] Graphic of(std::vector<GraphicLayer> layers) {
  Graphic graphic;
  graphic.layers = std::move(layers);
  return graphic;
}

TEST(PlaceGraphic, AShapeSitsWhereItsAnchorIsPut) {
  const auto placed = at(of({shape("a", 300.0, 200.0)}), 1.0);
  ASSERT_EQ(placed.size(), 1u);
  EXPECT_DOUBLE_EQ(placed[0].content.left, 200.0);
  EXPECT_DOUBLE_EQ(placed[0].content.top, 150.0);
  EXPECT_DOUBLE_EQ(placed[0].content.width, 200.0);
  // The quad is the drawing, margins and all, round the same centre.
  EXPECT_DOUBLE_EQ(placed[0].box.center_x, 300.0);
  EXPECT_DOUBLE_EQ(placed[0].box.center_y, 200.0);
  EXPECT_GT(placed[0].box.width, 200.0);
}

TEST(PlaceGraphic, TheBottomLayerComesFirstBecauseItIsDrawnFirst) {
  // Held: what is placed points back at its layers.
  const Graphic graphic = of({shape("top"), shape("bottom")});
  const auto placed = at(graphic, 1.0);
  ASSERT_EQ(placed.size(), 2u);
  EXPECT_EQ(placed[0].layer->id, "bottom");
  EXPECT_EQ(placed[1].layer->id, "top");
}

TEST(PlaceGraphic, AHiddenLayerIsNotPlaced) {
  GraphicLayer hidden = shape("hidden");
  hidden.visible = false;
  EXPECT_EQ(at(of({hidden, shape("shown")}), 1.0).size(), 1u);
}

TEST(PlaceGraphic, ScaleAndRotationHappenAboutTheAnchor) {
  GraphicLayer layer = shape("a", 500.0, 500.0);
  layer.anchor_x = 0.0;
  layer.anchor_y = 0.0;
  layer.scale = 2.0;
  layer.rotation = 90.0;
  const auto placed = at(of({layer}), 1.0);
  // The anchor -- the top left -- stays at 500, 500; the shape doubles and
  // turns about it, so its centre is now below the anchor rather than right.
  EXPECT_DOUBLE_EQ(placed[0].content.left, 500.0);
  EXPECT_DOUBLE_EQ(placed[0].content.width, 400.0);
  EXPECT_NEAR(placed[0].box.center_x, 500.0 - 100.0, 1e-9);
  EXPECT_NEAR(placed[0].box.center_y, 500.0 + 200.0, 1e-9);
  EXPECT_DOUBLE_EQ(placed[0].box.rotation_deg, 90.0);
}

TEST(PlaceGraphic, KeyframesMoveALayer) {
  GraphicLayer layer = shape("a");
  layer.keyframes["x"] = {{.t = 0.0, .v = 0.25}, {.t = 2.0, .v = 0.75}};
  layer.keyframes["opacity"] = {{.t = 0.0, .v = 0.0}, {.t = 2.0, .v = 1.0}};
  const auto placed = at(of({layer}), 1.0);
  EXPECT_DOUBLE_EQ(placed[0].box.center_x, kW / 2.0);
  EXPECT_DOUBLE_EQ(placed[0].opacity, 0.5);
}

// ------------------------------------------------------------------ reveals --

TEST(Reveals, AFadeInRisesOverItsDuration) {
  GraphicLayer layer = shape("a");
  layer.reveal_in = GraphicReveal{.kind = RevealKind::Fade, .duration = 1.0};
  EXPECT_DOUBLE_EQ(at(of({layer}), 0.0)[0].opacity, 0.0);
  EXPECT_GT(at(of({layer}), 0.5)[0].opacity, 0.2);
  EXPECT_DOUBLE_EQ(at(of({layer}), 1.0)[0].opacity, 1.0);
}

TEST(Reveals, AFadeOutFallsToNothingAtTheEnd) {
  GraphicLayer layer = shape("a");
  layer.reveal_out = GraphicReveal{.kind = RevealKind::Fade, .duration = 1.0};
  EXPECT_DOUBLE_EQ(at(of({layer}), kLength - 2.0)[0].opacity, 1.0);
  EXPECT_DOUBLE_EQ(at(of({layer}), kLength)[0].opacity, 0.0);
}

TEST(Reveals, ASlideStartsWhollyOutsideTheFrame) {
  GraphicLayer left = shape("a");
  left.reveal_in = GraphicReveal{.kind = RevealKind::SlideLeft, .duration = 1.0};
  EXPECT_LE(at(of({left}), 0.0)[0].content.right(), 0.0);
  EXPECT_DOUBLE_EQ(at(of({left}), 1.0)[0].content.left, kW / 2.0 - 100.0);

  GraphicLayer up = shape("b");
  up.reveal_in = GraphicReveal{.kind = RevealKind::SlideUp, .duration = 1.0};
  EXPECT_GE(at(of({up}), 0.0)[0].content.top, kH);
}

TEST(Reveals, AScaleGrowsFromNothingAboutTheAnchor) {
  GraphicLayer layer = shape("a");
  layer.reveal_in = GraphicReveal{.kind = RevealKind::Scale, .duration = 1.0};
  EXPECT_DOUBLE_EQ(at(of({layer}), 0.0)[0].content.width, 0.0);
  EXPECT_DOUBLE_EQ(at(of({layer}), 0.0)[0].box.center_x, kW / 2.0);
}

TEST(Reveals, AWipeUncoversFromTheLeft) {
  GraphicLayer layer = shape("a");
  layer.reveal_in = GraphicReveal{.kind = RevealKind::Wipe, .duration = 1.0};
  EXPECT_DOUBLE_EQ(at(of({layer}), 0.0)[0].wipe, 0.0);
  EXPECT_DOUBLE_EQ(at(of({layer}), 2.0)[0].wipe, 1.0);
}

TEST(Reveals, ATypewriterCountsCharactersNotBytes) {
  GraphicLayer layer;
  layer.id = "t";
  layer.text = "caf\xC3\xA9s";  // five characters, six bytes
  layer.reveal_in = GraphicReveal{.kind = RevealKind::Typewriter, .duration = 1.0};
  EXPECT_EQ(at(of({layer}), 0.0)[0].characters, 0u);
  EXPECT_EQ(at(of({layer}), 0.6)[0].characters, 3u);
  EXPECT_EQ(at(of({layer}), 2.0)[0].characters, kAllCharacters);
}

// ------------------------------------------------------------- timing --

TEST(GraphicTime, TheIntroAndOutroKeepTheirTimingWhenTheClipIsTrimmed) {
  Graphic graphic;
  graphic.intro = 1.0;
  graphic.outro = 1.0;
  graphic.length = 5.0;
  // Made five seconds long and now ten: the first and last seconds are as
  // they were, and the middle three stretch over eight.
  EXPECT_DOUBLE_EQ(graphic_time(graphic, 0.5, 10.0), 0.5);
  EXPECT_DOUBLE_EQ(graphic_time(graphic, 9.5, 10.0), 4.5);
  EXPECT_DOUBLE_EQ(graphic_time(graphic, 5.0, 10.0), 1.0 + 4.0 * 3.0 / 8.0);
}

TEST(GraphicTime, WithNothingProtectedTimeIsTheClipsOwn) {
  EXPECT_DOUBLE_EQ(graphic_time(Graphic{}, 3.0, 10.0), 3.0);
}

// ------------------------------------------------------------ roll and crawl --

TEST(Scroll, ARollTravelsFromBelowTheFrameToAboveIt) {
  Graphic graphic = of({shape("a")});
  graphic.scroll.mode = ScrollMode::Roll;
  EXPECT_DOUBLE_EQ(at(graphic, 0.0)[0].content.top, kH);
  EXPECT_DOUBLE_EQ(at(graphic, kLength)[0].content.bottom(), 0.0);
}

TEST(Scroll, ACrawlTravelsRightToLeft) {
  Graphic graphic = of({shape("a")});
  graphic.scroll.mode = ScrollMode::Crawl;
  EXPECT_DOUBLE_EQ(at(graphic, 0.0)[0].content.left, kW);
  EXPECT_DOUBLE_EQ(at(graphic, kLength)[0].content.right(), 0.0);
}

TEST(Scroll, PrerollHoldsBeforeItMoves) {
  Graphic graphic = of({shape("a")});
  graphic.scroll.mode = ScrollMode::Roll;
  graphic.scroll.preroll = 2.0;
  EXPECT_DOUBLE_EQ(at(graphic, 1.0)[0].content.top, kH);
}

TEST(Scroll, EasingStillArrivesOnTime) {
  Graphic graphic = of({shape("a")});
  graphic.scroll.mode = ScrollMode::Roll;
  graphic.scroll.ease_in = 2.0;
  graphic.scroll.ease_out = 2.0;
  EXPECT_NEAR(at(graphic, kLength)[0].content.bottom(), 0.0, 1e-6);
  // And moves less in its first second than steady movement would.
  const double steady = (kH + 100.0) / kLength;
  EXPECT_LT(kH - at(graphic, 1.0)[0].content.top, steady);
}

TEST(Scroll, EndingOnScreenStopsWithTheBottomInFrame) {
  Graphic graphic = of({shape("a")});
  graphic.scroll.mode = ScrollMode::Roll;
  graphic.scroll.end_off = false;
  EXPECT_DOUBLE_EQ(at(graphic, kLength)[0].content.bottom(), kH);
}

// ------------------------------------------------------------------ helpers --

TEST(GraphicHelpers, CharactersAreCountedAndCutWhole) {
  EXPECT_EQ(character_count("caf\xC3\xA9"), 4u);
  EXPECT_EQ(first_characters("caf\xC3\xA9s", 4), "caf\xC3\xA9");
  EXPECT_EQ(first_characters("abc", 10), "abc");
}

TEST(GraphicHelpers, AllCapsIsShownInCapitals) {
  GraphicLayer layer;
  layer.text = "Name";
  layer.style.all_caps = true;
  EXPECT_EQ(displayed_text(layer), "NAME");
}

TEST(GraphicHelpers, AnOldTitleBecomesOneCentredTextLayer) {
  TextSpec spec;
  spec.content = "Hello";
  spec.color = "#ff0000";
  spec.stroke_color = "#000000";
  spec.stroke_width = 3.0;
  const Graphic graphic = graphic_from_text(spec);
  ASSERT_EQ(graphic.layers.size(), 1u);
  const GraphicLayer& layer = graphic.layers.front();
  EXPECT_EQ(layer.text, "Hello");
  EXPECT_EQ(layer.style.fill, "#ff0000");
  ASSERT_EQ(layer.style.strokes.size(), 1u);
  EXPECT_DOUBLE_EQ(layer.style.strokes.front().width, 3.0);
  EXPECT_DOUBLE_EQ(layer.x, 0.5);
  EXPECT_DOUBLE_EQ(layer.y, 0.5);
}

TEST(GraphicHelpers, EveryMarginReachesPastStrokesAndShadow) {
  GraphicLayer plain = shape("a");
  GraphicLayer decorated = plain;
  decorated.style.strokes = {GraphicStroke{.width = 5.0}};
  decorated.style.shadow = GraphicShadow{.distance = 10.0, .size = 2.0, .blur = 4.0};
  EXPECT_GE(drawing_margin(decorated), drawing_margin(plain) + 5.0 + 10.0 + 2.0 + 8.0);
}

}  // namespace
}  // namespace cutline::core
