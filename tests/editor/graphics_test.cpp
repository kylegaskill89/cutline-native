/// Making and editing graphics: the one source an editor creates rather than
/// imports, and the layers inside it.

#include "cutline/editor/graphics.hpp"

#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::editor {
namespace {

using core::Graphic;
using core::GraphicKind;
using core::GraphicLayer;
using core::Project;
using core::Track;
using core::TrackKind;

constexpr double kCanvasW = 1920.0;
constexpr double kCanvasH = 1080.0;

[[nodiscard]] Project with_tracks() {
  Project project;
  project.sequence().fps = 30.0;
  project.sequence().tracks = {Track{.id = "v2", .kind = TrackKind::Video},
                    Track{.id = "v1", .kind = TrackKind::Video},
                    Track{.id = "a1", .kind = TrackKind::Audio}};
  return project;
}

[[nodiscard]] Graphic saying(std::string content) {
  Graphic graphic;
  GraphicLayer layer = text_layer(std::move(content));
  layer.id = "layer1";
  graphic.layers.push_back(layer);
  return graphic;
}

/// A shape `width` wide placed with its left edge at `left` pixels.
[[nodiscard]] GraphicLayer bar(std::string id, double left, double width) {
  GraphicLayer layer = shape_layer(GraphicKind::Rectangle);
  layer.id = std::move(id);
  layer.width = width;
  layer.height = 50.0;
  layer.anchor_x = 0.0;
  layer.x = left / kCanvasW;
  return layer;
}

[[nodiscard]] const core::PlacedLayer& placed(const std::vector<core::PlacedLayer>& all,
                                              std::string_view id) {
  for (const core::PlacedLayer& at : all) {
    if (at.layer->id == id) return at;
  }
  static const core::PlacedLayer none;
  ADD_FAILURE() << "no layer " << id;
  return none;
}

[[nodiscard]] std::vector<core::PlacedLayer> place(const Graphic& graphic) {
  return core::place_graphic(graphic, 0.0, 5.0, kCanvasW, kCanvasH, core::measure_without_fonts);
}

// ---------------------------------------------------------------- graphics --

TEST(Graphics, ANewGraphicIsGeneratedMediaWithItsLayers) {
  std::string id;
  const Project p = add_graphic(with_tracks(), saying("Chapter One"), &id);

  ASSERT_EQ(p.media.size(), 1u);
  EXPECT_FALSE(id.empty());
  EXPECT_EQ(p.media.front().id, id);
  EXPECT_TRUE(p.media.front().is_graphic);
  EXPECT_TRUE(p.media.front().path.empty()) << "a graphic is not a file";
  ASSERT_TRUE(p.media.front().graphic.has_value());
  EXPECT_EQ(p.media.front().graphic->layers.front().text, "Chapter One");
}

TEST(Graphics, AGraphicIsNamedAfterWhatItSays) {
  const Project p = add_graphic(with_tracks(), saying("Chapter One"));
  EXPECT_EQ(p.media.front().name, "Chapter One");
}

TEST(Graphics, AMultiLineTitleIsNamedAfterItsFirstLine) {
  const Project p = add_graphic(with_tracks(), saying("Cutline\nnative"));
  EXPECT_EQ(p.media.front().name, "Cutline");
}

TEST(Graphics, AGraphicWithNoWordsStillHasAName) {
  const Project p = add_graphic(with_tracks(), saying(""));
  EXPECT_EQ(p.media.front().name, "Graphic") << "a nameless entry cannot be found in a list";
}

TEST(Graphics, TwoSayingTheSameThingAreTwoGraphics) {
  // Unlike a file, which is deduplicated against the pool: there is no identity
  // here beyond the words, and two cards reading "Chapter One" are two cards.
  Project p = add_graphic(with_tracks(), saying("Chapter One"));
  p = add_graphic(std::move(p), saying("Chapter One"));

  ASSERT_EQ(p.media.size(), 2u);
  EXPECT_NE(p.media[0].id, p.media[1].id);
}

TEST(Graphics, AGraphicHasALengthSoItCanBePlaced) {
  const Project p = add_graphic(with_tracks(), saying("Hello"));
  EXPECT_GT(p.media.front().duration, 0.0);
}

TEST(Graphics, PlacingPutsItOnATrackAndReportsTheClip) {
  std::string clip_id;
  const Project p = add_graphic_at(with_tracks(), saying("Hello"), 2.0, {}, &clip_id);

  ASSERT_FALSE(clip_id.empty()) << "a caller has just made this and wants to select it";
  const core::Clip* clip = core::find_clip(p, clip_id);
  ASSERT_NE(clip, nullptr);
  EXPECT_DOUBLE_EQ(clip->start, 2.0);
  EXPECT_EQ(clip->kind, TrackKind::Video);
  EXPECT_GT(core::clip_duration(*clip), 0.0);
}

TEST(Graphics, PlacingHonoursTheTrackItIsGiven) {
  std::string clip_id;
  const Project p = add_graphic_at(with_tracks(), saying("Hello"), 0.0, "v1", &clip_id);

  const auto& lower = p.sequence().tracks[1];
  ASSERT_EQ(lower.id, "v1");
  ASSERT_EQ(lower.clips.size(), 1u);
  EXPECT_EQ(lower.clips.front().id, clip_id);
}

TEST(Graphics, TheGraphicComesBackForAMediaAndForAClip) {
  std::string clip_id;
  const Project p = add_graphic_at(with_tracks(), saying("Hello"), 0.0, {}, &clip_id);

  const core::Clip* clip = core::find_clip(p, clip_id);
  ASSERT_NE(clip, nullptr);
  const Graphic* by_media = graphic_of(p, clip->media_id);
  ASSERT_NE(by_media, nullptr);
  EXPECT_EQ(by_media->layers.front().text, "Hello");
  EXPECT_EQ(clip_graphic(p, clip_id), by_media) << "both routes should reach the same graphic";
}

TEST(Graphics, AnythingThatIsNotAGraphicHasNone) {
  Project p = with_tracks();
  p.media = {core::Media{.id = "m1", .path = "wide.mp4", .has_video = true}};

  EXPECT_EQ(graphic_of(p, "m1"), nullptr);
  EXPECT_EQ(graphic_of(p, "nope"), nullptr);
  EXPECT_EQ(clip_graphic(p, "nope"), nullptr);
}

TEST(Graphics, EditingReplacesTheLayersAndTheName) {
  std::string id;
  Project p = add_graphic(with_tracks(), saying("First"), &id);
  p = set_graphic(std::move(p), id, saying("Second"));

  ASSERT_NE(graphic_of(p, id), nullptr);
  EXPECT_EQ(graphic_of(p, id)->layers.front().text, "Second");
  EXPECT_EQ(p.media.front().name, "Second") << "the name follows the words";
}

TEST(Graphics, EditingKeepsANameSomebodyChose) {
  std::string id;
  Project p = add_graphic(with_tracks(), saying("First"), &id);
  p.media.front().name = "Opening card";

  p = set_graphic(std::move(p), id, saying("Second"));
  EXPECT_EQ(p.media.front().name, "Opening card")
      << "a name that was not the text must survive an edit to the text";
}

TEST(Graphics, SettingWhatIsAlreadyThereChangesNothing) {
  // Which is what lets the session skip the undo entry.
  std::string id;
  const Project before = add_graphic(with_tracks(), saying("Same"), &id);
  EXPECT_EQ(set_graphic(before, id, saying("Same")), before);
}

TEST(Graphics, EditingSomethingThatIsNotAGraphicChangesNothing) {
  Project p = with_tracks();
  p.media = {core::Media{.id = "m1", .path = "wide.mp4", .has_video = true}};

  EXPECT_EQ(set_graphic(p, "m1", saying("Hello")), p);
  EXPECT_EQ(set_graphic(p, "nope", saying("Hello")), p);
}

TEST(Graphics, ANewTitleSaysSomething) {
  // An empty card is indistinguishable from a broken one.
  const Graphic graphic = default_title();
  ASSERT_EQ(graphic.layers.size(), 1u);
  EXPECT_FALSE(graphic.layers.front().text.empty());
  EXPECT_GT(graphic.layers.front().style.font_size, 0.0);
}

// ------------------------------------------------------------------ layers --

TEST(Layers, ANewLayerGoesOnTopWithAnIdOfItsOwn) {
  std::string id;
  const Graphic graphic = add_layer(saying("Words"), shape_layer(GraphicKind::Rectangle), &id);
  ASSERT_EQ(graphic.layers.size(), 2u);
  EXPECT_EQ(graphic.layers.front().id, id);
  EXPECT_NE(id, "layer1");
}

TEST(Layers, ShapesAreNamedAsPremiereNamesThem) {
  Graphic graphic = add_layer(Graphic{}, shape_layer(GraphicKind::Rectangle));
  graphic = add_layer(std::move(graphic), shape_layer(GraphicKind::Ellipse));
  EXPECT_EQ(graphic.layers[1].name, "Shape 01");
  EXPECT_EQ(graphic.layers[0].name, "Shape 02");
}

TEST(Layers, AnImageIsNamedAfterItsFile) {
  const Graphic graphic = add_layer(Graphic{}, image_layer("C:\\art\\logo.png"));
  EXPECT_EQ(graphic.layers.front().name, "logo.png");
}

TEST(Layers, ATextLayerIsListedByItsWords) {
  EXPECT_EQ(layer_label(text_layer("Jane Smith\nDirector")), "Jane Smith");
  GraphicLayer named = text_layer("x");
  named.name = "Caption";
  EXPECT_EQ(layer_label(named), "Caption");
}

TEST(Layers, RemovingOneLetsGoOfThePinsToIt) {
  Graphic graphic = saying("Name");
  GraphicLayer under = bar("bar", 100.0, 300.0);
  under.pin = core::GraphicPin{.to = "layer1", .left = true};
  graphic.layers.push_back(under);

  graphic = remove_layer(std::move(graphic), "layer1");
  ASSERT_EQ(graphic.layers.size(), 1u);
  EXPECT_FALSE(graphic.layers.front().pin.any());
}

TEST(Layers, ADuplicateSitsJustAboveTheOriginal) {
  Graphic graphic = saying("Name");
  graphic.layers.push_back(bar("bar", 0.0, 100.0));
  std::string copy;
  graphic = duplicate_layer(std::move(graphic), "bar", &copy);
  ASSERT_EQ(graphic.layers.size(), 3u);
  EXPECT_EQ(graphic.layers[1].id, copy);
  EXPECT_EQ(graphic.layers[2].id, "bar");
}

TEST(Layers, MovingReordersAndKeepsEverythingElse) {
  Graphic graphic = saying("Name");
  graphic.layers.push_back(bar("a", 0.0, 10.0));
  graphic.layers.push_back(bar("b", 0.0, 10.0));
  graphic = move_layer(std::move(graphic), "b", 0);
  EXPECT_EQ(graphic.layers[0].id, "b");
  EXPECT_EQ(graphic.layers[1].id, "layer1");
  EXPECT_EQ(graphic.layers[2].id, "a");
}

// --------------------------------------------------------------- keyframes --

TEST(Layers, SettingAnAnimatedValueKeysItThere) {
  Graphic graphic = saying("Move");
  graphic = set_layer_keyframe(std::move(graphic), "layer1", "x", 0.0, 0.2);
  graphic = set_layer_value(std::move(graphic), "layer1", "x", 2.0, 0.8);
  const GraphicLayer& layer = graphic.layers.front();
  ASSERT_EQ(layer.keyframes.at("x").size(), 2u);
  EXPECT_DOUBLE_EQ(core::layer_value(layer, "x", 1.0), 0.5);
}

TEST(Layers, SettingAStillValueChangesIt) {
  const Graphic graphic = set_layer_value(saying("Still"), "layer1", "opacity", 1.0, 0.25);
  EXPECT_DOUBLE_EQ(graphic.layers.front().opacity, 0.25);
  EXPECT_TRUE(graphic.layers.front().keyframes.empty());
}

TEST(Layers, ClearingKeyframesLeavesTheValueWhereItWas) {
  Graphic graphic = saying("Move");
  graphic = set_layer_keyframe(std::move(graphic), "layer1", "x", 0.0, 0.2);
  graphic = set_layer_keyframe(std::move(graphic), "layer1", "x", 2.0, 0.8);
  graphic = clear_layer_keyframes(std::move(graphic), "layer1", "x", 1.0);
  EXPECT_TRUE(graphic.layers.front().keyframes.empty());
  EXPECT_DOUBLE_EQ(graphic.layers.front().x, 0.5);
}

// ------------------------------------------------------- responsive design --

TEST(Pins, APinnedLayerFollowsTheLayerItIsPinnedTo) {
  // A bar whose right edge is pinned to the right edge of a shape: when the
  // shape grows by 100 pixels, the bar's right edge goes with it.
  Graphic graphic;
  graphic.layers = {bar("name", 100.0, 300.0), bar("plate", 80.0, 340.0)};
  auto pinned = pin_layer(graphic, "plate", "name", PinEdges{.left = true, .right = true},
                          kCanvasW, kCanvasH, core::measure_without_fonts);
  ASSERT_TRUE(pinned.has_value()) << pinned.error();

  Graphic grown = *pinned;
  grown.layers[0].width = 400.0;
  const auto before = place(*pinned);
  const auto after = place(grown);
  EXPECT_NEAR(placed(before, "plate").content.right(), 420.0, 1e-6);
  EXPECT_NEAR(placed(after, "plate").content.right(), 520.0, 1e-6);
  EXPECT_NEAR(placed(after, "plate").content.left, 80.0, 1e-6) << "the left edge stayed";
  EXPECT_NEAR(placed(after, "plate").width, 440.0, 1e-6) << "a shape stretches between pins";
}

TEST(Pins, ALayerPinnedToTheFrameKeepsItsDistanceFromTheEdge) {
  Graphic graphic;
  graphic.layers = {bar("corner", 1700.0, 100.0)};
  auto pinned = pin_layer(graphic, "corner", "", PinEdges{.right = true}, kCanvasW, kCanvasH,
                          core::measure_without_fonts);
  ASSERT_TRUE(pinned.has_value()) << pinned.error();
  // On a wider frame, the same distance from its right-hand edge.
  const auto wide = core::place_graphic(*pinned, 0.0, 5.0, 2560.0, kCanvasH,
                                        core::measure_without_fonts);
  EXPECT_NEAR(wide.front().content.right(), 2560.0 - 120.0, 1e-6);
}

TEST(Pins, ALoopIsRefused) {
  Graphic graphic;
  graphic.layers = {bar("a", 0.0, 10.0), bar("b", 0.0, 10.0)};
  auto first = pin_layer(graphic, "a", "b", PinEdges{.left = true}, kCanvasW, kCanvasH,
                         core::measure_without_fonts);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(pin_layer(*first, "b", "a", PinEdges{.left = true}, kCanvasW, kCanvasH,
                         core::measure_without_fonts)
                   .has_value());
  EXPECT_FALSE(pin_layer(graphic, "a", "a", PinEdges{.left = true}, kCanvasW, kCanvasH,
                         core::measure_without_fonts)
                   .has_value());
}

// ---------------------------------------------------------------- alignment --

TEST(Alignment, OneLayerLinesUpWithTheFrame) {
  Graphic graphic;
  graphic.layers = {bar("a", 300.0, 200.0)};
  graphic = align_layers(std::move(graphic), {"a"}, Align::Right, kCanvasW, kCanvasH,
                         core::measure_without_fonts);
  EXPECT_NEAR(place(graphic).front().content.right(), kCanvasW, 1e-6);
}

TEST(Alignment, SeveralLineUpWithEachOther) {
  Graphic graphic;
  graphic.layers = {bar("a", 300.0, 200.0), bar("b", 100.0, 50.0)};
  graphic = align_layers(std::move(graphic), {"a", "b"}, Align::Left, kCanvasW, kCanvasH,
                         core::measure_without_fonts);
  const auto all = place(graphic);
  EXPECT_NEAR(placed(all, "a").content.left, 100.0, 1e-6);
  EXPECT_NEAR(placed(all, "b").content.left, 100.0, 1e-6);
}

TEST(Alignment, AnAnimatedLayerMovesItsWholePath) {
  Graphic graphic;
  graphic.layers = {bar("a", 300.0, 200.0)};
  graphic = set_layer_keyframe(std::move(graphic), "a", "x", 0.0, 300.0 / kCanvasW);
  graphic = set_layer_keyframe(std::move(graphic), "a", "x", 1.0, 500.0 / kCanvasW);
  graphic = align_layers(std::move(graphic), {"a"}, Align::Left, kCanvasW, kCanvasH,
                         core::measure_without_fonts);
  const std::vector<core::Keyframe>& path = graphic.layers.front().keyframes.at("x");
  EXPECT_NEAR(path[0].v * kCanvasW, 0.0, 1e-6);
  EXPECT_NEAR(path[1].v * kCanvasW, 200.0, 1e-6) << "it still travels 200 pixels";
}

// ------------------------------------------------------------ master styles --

TEST(MasterStyles, ChangingOneChangesEveryLayerFollowingIt) {
  std::string first;
  std::string second;
  Project p = add_graphic(with_tracks(), saying("One"), &first);
  p = add_graphic(std::move(p), saying("Two"), &second);
  p = create_master_style(std::move(p), first, "layer1", "Caption");
  ASSERT_EQ(p.graphic_styles.size(), 1u);
  const auto style = p.graphic_styles.front();
  p = set_graphic(p, second, apply_master_style(*graphic_of(p, second), "layer1", style));

  core::GraphicMasterStyle changed = style;
  changed.style.fill = "#ffcc00";
  p = set_master_style(std::move(p), changed);
  EXPECT_EQ(graphic_of(p, first)->layers.front().style.fill, "#ffcc00");
  EXPECT_EQ(graphic_of(p, second)->layers.front().style.fill, "#ffcc00");
}

TEST(MasterStyles, ALayerChangedByHandNoLongerMatches) {
  std::string id;
  Project p = add_graphic(with_tracks(), saying("One"), &id);
  p = create_master_style(std::move(p), id, "layer1", "Caption");
  EXPECT_TRUE(follows_master(p, graphic_of(p, id)->layers.front()));

  GraphicLayer changed = graphic_of(p, id)->layers.front();
  changed.style.italic = true;
  EXPECT_FALSE(follows_master(p, changed));
}

}  // namespace
}  // namespace cutline::editor
