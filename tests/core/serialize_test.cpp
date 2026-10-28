#include "cutline/core/serialize.hpp"

#include "cutline/core/graphic.hpp"
#include "cutline/core/history.hpp"
#include "cutline/core/id.hpp"
#include "cutline/core/properties.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::core {
namespace {

/// A project exercising every part of the model that has to survive a round
/// trip: generated media, animation, effects, transitions, and markers.
Project rich_project() {
  Project p;
  p.sequence().canvas_w = 3840;
  p.sequence().canvas_h = 2160;
  p.sequence().fps = 60.0;
  p.sequence().master_gain = 0.7;

  Media footage;
  footage.id = "m1";
  footage.path = "";  // left empty so loading reports no missing-file warning
  footage.name = "clip.mkv";
  footage.duration = 20.0;
  footage.has_video = true;
  footage.audio_stream_count = 2;
  footage.width = 3840;
  footage.height = 2160;
  footage.fps = 59.94;

  Media title;
  title.id = "m2";
  title.name = "Title";
  title.is_graphic = true;
  {
    GraphicLayer words;
    words.id = "words";
    words.text = "Hello\nthere";
    words.box_width = 600.0;
    words.style.font_family = "Georgia";
    words.style.underline = true;
    words.style.all_caps = true;
    words.style.tracking = 50.0;
    words.style.leading = 4.0;
    words.style.align = TextAlign::Right;
    words.style.strokes = {GraphicStroke{.color = "#ff0000", .width = 3.0},
                           GraphicStroke{.color = "#00ff00", .width = 2.0}};
    words.style.background = GraphicBackground{.color = "#000000", .opacity = 0.5};
    words.style.shadow = GraphicShadow{.angle = 90.0, .distance = 12.0};
    words.master_style = "Lower third";
    words.keyframes["x"] = {{.t = 0.0, .v = 0.2}, {.t = 1.0, .v = 0.5, .e = Interp::Ease}};
    words.reveal_in = GraphicReveal{.kind = RevealKind::Typewriter, .duration = 1.5};
    GraphicLayer bar;
    bar.id = "bar";
    bar.name = "Shape 01";
    bar.kind = GraphicKind::Rectangle;
    bar.width = 640.0;
    bar.height = 80.0;
    bar.corner_radius = 8.0;
    bar.style.fill_enabled = false;
    bar.visible = false;
    bar.pin = GraphicPin{.to = "words", .left = true, .right = true, .left_offset = -20.0,
                         .right_offset = 20.0};
    bar.reveal_out = GraphicReveal{.kind = RevealKind::Wipe, .duration = 0.25};
    GraphicLayer logo;
    logo.id = "logo";
    logo.kind = GraphicKind::Image;
    logo.path = "C:/logo.png";
    Graphic graphic;
    graphic.layers = {words, bar, logo};
    graphic.scroll = GraphicScroll{.mode = ScrollMode::Roll, .end_off = false, .preroll = 1.0};
    graphic.intro = 0.5;
    graphic.outro = 0.75;
    graphic.length = 5.0;
    title.graphic = graphic;
  }
  TextSpec spec;
  spec.content = "Hello";
  spec.align = TextAlign::Right;
  spec.background = "#000000";
  spec.stroke_color = "#ffffff";
  spec.stroke_width = 3.0;
  spec.shadow = true;
  (void)spec;

  Media matte;
  matte.id = "m3";
  matte.is_color = true;
  matte.color = "#112233";
  matte.gradient = MatteGradient{.color2 = "#445566", .angle = 45.0};

  p.media = {footage, title, matte};
  p.graphic_styles = {GraphicMasterStyle{.name = "Lower third", .style = GraphicStyle{}}};

  Clip c;
  c.id = "c1";
  c.media_id = "m1";
  c.kind = TrackKind::Video;
  c.audio_stream = 1;
  c.source_in = 2.0;
  c.source_out = 12.0;
  c.start = 1.0;
  c.group_id = "g1";
  c.gain = 0.75;
  c.gain_keyframes = {{.t = 0.0, .v = 0.2, .e = Interp::Ease}, {.t = 4.0, .v = 1.0}};
  c.pan = -0.25;
  c.opacity = 0.9;
  c.fade_in = 0.5;
  c.fade_out = 0.25;
  c.transform = {.x = 0.4,
                 .y = 0.6,
                 .scale_x = 1.5,
                 .scale_y = 2.0,
                 .rotation = 30.0,
                 .anchor_x = 0.25,
                 .anchor_y = 0.75};
  c.speed = 1.5;
  c.reverse = true;
  c.transition_out = Transition{.kind = TransitionKind::Slide, .duration = 1.25};
  c.blend = BlendMode::Overlay;
  c.disabled = true;
  c.keyframes[anim_prop_index(AnimProp::X)] = {{.t = 0.0, .v = 0.0, .e = Interp::Hold},
                                               {.t = 2.0, .v = 1.0}};
  c.keyframes[anim_prop_index(AnimProp::Opacity)] = {{.t = 1.0, .v = 0.5}};
  c.keyframes[anim_prop_index(AnimProp::AnchorY)] = {{.t = 0.0, .v = 0.5}, {.t = 3.0, .v = 0.0}};
  c.keyframes[anim_prop_index(AnimProp::Pan)] = {{.t = 0.0, .v = -1.0}, {.t = 2.0, .v = 1.0}};

  ClipEffect blur;
  blur.type = "blur";
  blur.enabled = false;
  blur.params = {{"amount", 4.5}};
  blur.colors = {{"key", "#00d000"}};
  blur.curves["rgb_master"] = {{.x = 0.0, .y = 0.1}, {.x = 0.5, .y = 0.6}, {.x = 1.0, .y = 1.0}};
  blur.files = {{"look", "C:/Looks/Warm.cube"}};
  blur.keyframes["amount"] = {{.t = 0.0, .v = 0.0}, {.t = 1.0, .v = 9.0, .e = Interp::Ease}};
  blur.mask = Mask{.shape = MaskShape::Ellipse,
                   .x = 0.3,
                   .y = 0.4,
                   .width = 0.2,
                   .height = 0.1,
                   .rotation = 15.0,
                   .feather = 0.05,
                   .opacity = 0.75,
                   .inverted = true};
  c.effects = {blur};

  AudioClipEffect eq;
  eq.type = "equalizer";
  eq.params = {{"freq", 1000.0}, {"gain", -3.0}};
  eq.keyframes["freq"] = {{.t = 0.0, .v = 200.0}, {.t = 2.0, .v = 8000.0, .e = Interp::Ease}};
  c.audio_effects = {eq};

  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.label = "Overlay";
  v.hidden = true;
  v.height = 84.0;
  v.clips = {c};

  Track a;
  a.id = "a1";
  a.kind = TrackKind::Audio;
  a.muted = true;
  a.solo = true;
  a.locked = true;

  p.sequence().tracks = {v, a};
  p.sequence().markers = {Marker{.id = "k1", .time = 3.0, .label = "cue", .color = "#ff0000"}};
  p.sequence().in_point = 1.5;
  p.sequence().out_point = 8.25;
  return p;
}

TEST(Serialize, RoundTripsEveryField) {
  const Project original = rich_project();
  const auto loaded = from_json(to_json(original));

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project, original);
  EXPECT_TRUE(loaded->warnings.empty());
}

TEST(Serialize, ATitleWrittenBeforeGraphicsReadsAsAGraphicOfOneTextLayer) {
  const std::string old = R"({"version": 1, "project": {"media": [{"id": "t", "name": "Hi",
      "is_text": true, "has_video": true, "text": {"content": "Hi", "font_size": 64,
      "color": "#ff0000", "bold": false, "stroke_color": "#000000", "stroke_width": 2,
      "shadow": true}}]}})";
  const auto loaded = from_json(old);
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_EQ(loaded->project.media.size(), 1u);
  const Media& media = loaded->project.media.front();
  EXPECT_TRUE(media.is_graphic);
  ASSERT_TRUE(media.graphic.has_value());

  TextSpec spec;
  spec.content = "Hi";
  spec.font_size = 64.0;
  spec.color = "#ff0000";
  spec.bold = false;
  spec.stroke_color = "#000000";
  spec.stroke_width = 2.0;
  spec.shadow = true;
  EXPECT_EQ(*media.graphic, graphic_from_text(spec));
}

TEST(Serialize, ANewProjectWritesGraphicsNotTitles) {
  const std::string text = to_json(rich_project());
  EXPECT_NE(text.find("\"is_graphic\""), std::string::npos);
  EXPECT_EQ(text.find("\"is_text\""), std::string::npos);
}

TEST(Serialize, AGraphicSurvivesOnItsOwn) {
  const Graphic graphic = *rich_project().media[1].graphic;
  const auto back = graphic_from_json(graphic_to_json(graphic));
  ASSERT_TRUE(back.has_value()) << back.error();
  EXPECT_EQ(*back, graphic);
}

TEST(Serialize, RoundTripsAnEmptyProject) {
  const Project original = empty_project();
  const auto loaded = from_json(to_json(original));

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project, original);
}

TEST(Serialize, ASequenceKeepsItsColourManagement) {
  Project original = empty_project();
  original.sequence().colour = WorkingColour::Rec2100Hlg;
  original.sequence().graphics_white = 300.0;
  original.sequence().tone_map = false;

  const auto loaded = from_json(to_json(original));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project.sequence().colour, WorkingColour::Rec2100Hlg);
  EXPECT_EQ(loaded->project.sequence().graphics_white, 300.0);
  EXPECT_FALSE(loaded->project.sequence().tone_map);
}

// An SDR sequence at the defaults is written exactly as before there was colour
// management, so a project file does not change for anybody not using it.
TEST(Serialize, ASourceKeepsItsColourAndItsOverride) {
  Project original = empty_project();
  Media m;
  m.id = "m";
  m.path = "D:/rushes/phone.mov";
  m.bit_depth = 10;
  m.colour_space = WorkingColour::Rec709;
  m.colour_override = WorkingColour::Rec2100Hlg;
  original.media.push_back(m);

  const auto loaded = from_json(to_json(original));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_EQ(loaded->project.media.size(), 1u);
  EXPECT_EQ(loaded->project.media[0].bit_depth, 10);
  EXPECT_EQ(loaded->project.media[0].colour_space, WorkingColour::Rec709);
  EXPECT_EQ(loaded->project.media[0].colour_override, WorkingColour::Rec2100Hlg);
}

TEST(Serialize, AnSdrSequenceWritesNothingAboutColour) {
  const std::string text = to_json(empty_project());
  EXPECT_EQ(text.find("colour"), std::string::npos);
  EXPECT_EQ(text.find("graphics_white"), std::string::npos);
  EXPECT_EQ(text.find("tone_map"), std::string::npos);
}

TEST(Serialize, WritesTheSchemaVersion) {
  const std::string text = to_json(empty_project());
  EXPECT_NE(text.find("\"version\""), std::string::npos);
  EXPECT_NE(text.find("\"project\""), std::string::npos);
}

// Anything absent takes its default, which is how a file written before a field
// existed still opens.
TEST(Serialize, AbsentFieldsFallBackToDefaults) {
  const auto loaded = from_json(R"({
    "version": 1,
    "project": {
      "tracks": [{"id": "v1", "kind": "video", "clips": [{"id": "c1", "media_id": "m1"}]}]
    }
  })");

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  const Project& p = loaded->project;
  EXPECT_EQ(p.sequence().canvas_w, 1920);
  EXPECT_DOUBLE_EQ(p.sequence().fps, 30.0);
  EXPECT_DOUBLE_EQ(p.sequence().master_gain, 1.0);

  const Clip& c = p.sequence().tracks[0].clips[0];
  EXPECT_DOUBLE_EQ(c.gain, 1.0);
  EXPECT_DOUBLE_EQ(c.pan, 0.0) << "a file written before there was a panner is centred";
  EXPECT_DOUBLE_EQ(c.opacity, 1.0);
  EXPECT_DOUBLE_EQ(c.speed, 1.0);
  EXPECT_EQ(c.blend, BlendMode::Normal);
  EXPECT_FALSE(c.group_id.has_value());
  EXPECT_EQ(c.transform, Transform{});
}

TEST(Serialize, ATransformWrittenBeforeThereWasAnAnchorIsCentred) {
  // Every project saved by an earlier build has a transform with five fields in
  // it. Read back with the anchor at the middle of the layer, those files mean
  // exactly what they meant when they were written.
  const auto loaded = from_json(R"({
    "version": 1,
    "project": {
      "tracks": [{"id": "v1", "kind": "video", "clips": [
        {"id": "c1", "media_id": "m1",
         "transform": {"x": 0.25, "y": 0.5, "scale_x": 1.0, "scale_y": 1.0, "rotation": 45.0}}
      ]}]
    }
  })");

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  const Transform& t = loaded->project.sequence().tracks[0].clips[0].transform;
  EXPECT_DOUBLE_EQ(t.x, 0.25);
  EXPECT_DOUBLE_EQ(t.rotation, 45.0);
  EXPECT_DOUBLE_EQ(t.anchor_x, 0.5);
  EXPECT_DOUBLE_EQ(t.anchor_y, 0.5);
}

TEST(Serialize, AnEffectWrittenBeforeThereWereMasksAppliesEverywhere) {
  const auto loaded = from_json(R"({
    "version": 1,
    "project": {
      "tracks": [{"id": "v1", "kind": "video", "clips": [
        {"id": "c1", "media_id": "m1", "effects": [{"type": "blur", "params": {"amount": 4}}]}
      ]}]
    }
  })");

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_FALSE(loaded->project.sequence().tracks[0].clips[0].effects[0].mask.active());
}

TEST(Serialize, RejectsMalformedInput) {
  EXPECT_FALSE(from_json("not json at all").has_value());
  EXPECT_FALSE(from_json("[1, 2, 3]").has_value());
  EXPECT_FALSE(from_json(R"({"project": {}})").has_value());          // no version
  EXPECT_FALSE(from_json(R"({"version": 1})").has_value());           // no project
}

// A file from a future build is refused with an explanation rather than being
// partly misread.
TEST(Serialize, RefusesANewerSchema) {
  const auto loaded = from_json(R"({"version": 9999, "project": {}})");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_NE(loaded.error().find("newer version"), std::string::npos);
}

TEST(Serialize, ASourcesMarksSurviveTheFile) {
  // The reason they live on the media at all: a decision about which part of a
  // file to use should still be there when the project is opened again.
  Project p = empty_project();
  Media m;
  m.id = "m1";
  m.duration = 30.0;
  m.in_point = 4.0;
  m.out_point = 9.0;
  p.media = {m};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_EQ(loaded->project.media.size(), 1u);
  EXPECT_DOUBLE_EQ(*loaded->project.media[0].in_point, 4.0);
  EXPECT_DOUBLE_EQ(*loaded->project.media[0].out_point, 9.0);
}

TEST(Serialize, AnUnmarkedSourceStaysUnmarkedRatherThanGainingZeroes) {
  // Written only when set, so an older project does not come back marked from
  // the start of the file to the start of the file.
  Project p = empty_project();
  Media m;
  m.id = "m1";
  m.duration = 30.0;
  p.media = {m};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_FALSE(loaded->project.media[0].in_point.has_value());
  EXPECT_FALSE(loaded->project.media[0].out_point.has_value());
}

TEST(Serialize, AProxyAndTheSwitchForItSurviveTheFile) {
  Project p = empty_project();
  p.use_proxies = true;
  Media m;
  m.id = "m1";
  m.path = "D:/footage/wide.mp4";
  m.proxy_path = "D:/proxies/wide.mp4";
  p.media = {m};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(loaded->project.use_proxies);
  EXPECT_EQ(loaded->project.media[0].proxy_path, "D:/proxies/wide.mp4");
}

TEST(Serialize, SafeAreasSurviveTheFileAndArePremieresWhenNotSaid) {
  Project p = empty_project();
  p = set_safe_areas(std::move(p), SafeAreas{.title_width = 15.0, .title_height = 12.0,
                                             .action_width = 6.0, .action_height = 70.0});
  // Held to half: past it the two sides would meet.
  EXPECT_DOUBLE_EQ(p.safe_areas.action_height, 50.0);
  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project.safe_areas, p.safe_areas);

  const auto plain = from_json(to_json(empty_project()));
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->project.safe_areas, SafeAreas{});
  EXPECT_DOUBLE_EQ(plain->project.safe_areas.title_width, 20.0);
}

TEST(Serialize, AProjectWrittenBeforeProxiesReadsAsHavingNone) {
  // The default has to be off, or an older project would come back cutting
  // against small copies that do not exist.
  const auto loaded = from_json(to_json(empty_project()));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_FALSE(loaded->project.use_proxies);
}

TEST(Serialize, BinsAndWhatIsFiledInThemSurviveTheFile) {
  Project p = empty_project();
  p.bins = {Bin{.id = "bin_1", .name = "Day 1"},
            Bin{.id = "bin_2", .name = "Cameras", .parent = "bin_1"}};
  Media m;
  m.id = "m1";
  m.path = "D:/footage/wide.mp4";
  m.bin = "bin_2";
  p.media = {m};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  ASSERT_EQ(loaded->project.bins.size(), 2u);
  EXPECT_EQ(loaded->project.bins[1].name, "Cameras");
  EXPECT_EQ(loaded->project.bins[1].parent, "bin_1");
  EXPECT_TRUE(loaded->project.bins[0].parent.empty());
  EXPECT_EQ(loaded->project.media[0].bin, "bin_2");
}

TEST(Serialize, ClipMarkersSurviveTheFile) {
  Project p = empty_project();
  Media m;
  m.id = "m1";
  m.path = "D:/footage/wide.mp4";
  m.duration = 20.0;
  m.markers = {Marker{.id = "k1", .time = 2.5, .label = "Clap", .color = "#ff0000",
                      .duration = 1.0, .comment = "slate"}};
  p.media = {m};
  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project.media[0].markers, m.markers);
}

TEST(Serialize, TheVideoPreviewsScratchDiskTravelsWithTheProject) {
  Project p = empty_project();
  p.video_previews = ScratchDisk{.where = ScratchDisk::Where::Folder, .folder = "E:/Previews"};
  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project.video_previews, p.video_previews);
  EXPECT_EQ(to_json(empty_project()).find("video_previews"), std::string::npos)
      << "a project that never chose is written as it always was";
}

TEST(Serialize, AProjectWrittenBeforeBinsReadsAsOneFlatPool) {
  Media m;
  m.id = "m1";
  Project p = empty_project();
  p.media = {m};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(loaded->project.bins.empty());
  EXPECT_TRUE(loaded->project.media[0].bin.empty());
}

TEST(Serialize, ABinIdFromAFileIsNotHandedOutAgain) {
  // The same hazard as clip ids: the counter starts at zero every run, so a
  // project holding bin_4 would otherwise get a second bin_4 from the next
  // "New Bin" — and every operation that finds a bin by id would hit both.
  Project p = empty_project();
  p.bins = {Bin{.id = "bin_9", .name = "Day 1"}};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_NE(new_id("bin"), "bin_9");
}

TEST(Serialize, HowTimecodeIsCountedTravelsWithTheCut) {
  Project p = empty_project();
  p.sequence().fps = 30000.0 / 1001.0;
  p.sequence().drop_frame = false;

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_FALSE(loaded->project.sequence().drop_frame)
      << "a sequence handed to somebody else has to number its frames the same way there";
}

TEST(Serialize, AProjectWrittenBeforeDropFrameCountsItTheBroadcastWay) {
  // On by default, because the rates it applies to are broadcast rates and
  // drop-frame is what broadcast means by timecode. A project from before this
  // existed was being shown wall-clock numbers that skipped, so there is no
  // older behaviour worth preserving.
  const auto loaded = from_json(R"({"version": 1, "project": {"fps": 29.97}})");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(loaded->project.sequence().drop_frame);
}

TEST(Serialize, ReportsMissingMediaWithoutDroppingIt) {
  Project p = empty_project();
  Media m;
  m.id = "m1";
  m.path = "Z:/definitely/not/here.mkv";
  p.media = {m};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project.media.size(), 1u);  // kept, so it can be relinked
  ASSERT_EQ(loaded->warnings.size(), 1u);
  EXPECT_NE(loaded->warnings[0].find("not found"), std::string::npos);
}

// Enum spellings are part of the format, so they are asserted rather than
// assumed from enumerator order.
TEST(Serialize, WritesEnumsAsStableNames) {
  Project p = empty_project(1, 0);
  Clip c;
  c.id = "c1";
  c.blend = BlendMode::Difference;
  c.transition_out = Transition{.kind = TransitionKind::DipBlack, .duration = 1.0};
  c.keyframes[anim_prop_index(AnimProp::ScaleX)] = {{.t = 0.0, .v = 1.0, .e = Interp::Ease}};
  p.sequence().tracks[0].clips = {c};

  const std::string text = to_json(p);
  EXPECT_NE(text.find("\"difference\""), std::string::npos);
  EXPECT_NE(text.find("\"dip-black\""), std::string::npos);
  EXPECT_NE(text.find("\"scale_x\""), std::string::npos);
  EXPECT_NE(text.find("\"ease\""), std::string::npos);
  EXPECT_NE(text.find("\"video\""), std::string::npos);
}

// ----------------------------------------------------------------- history --

TEST(History, UndoAndRedoWalkTheStack) {
  History history;
  const Project first = empty_project(1, 0);
  Project second = first;
  second.sequence().canvas_w = 1280;

  EXPECT_FALSE(history.can_undo());
  history.push(first);
  EXPECT_TRUE(history.can_undo());

  const std::optional<Project> undone = history.undo(second);
  ASSERT_TRUE(undone.has_value());
  EXPECT_EQ(undone->sequence().canvas_w, 1920);
  EXPECT_TRUE(history.can_redo());

  const std::optional<Project> redone = history.redo(*undone);
  ASSERT_TRUE(redone.has_value());
  EXPECT_EQ(redone->sequence().canvas_w, 1280);
}

TEST(History, RefusesToStepPastTheEnds) {
  History history;
  const Project p = empty_project();
  EXPECT_FALSE(history.undo(p).has_value());
  EXPECT_FALSE(history.redo(p).has_value());
}

// Editing after undoing abandons the future that was undone.
TEST(History, ANewEditClearsRedo) {
  History history;
  const Project p = empty_project();
  history.push(p);
  const std::optional<Project> undone = history.undo(p);
  ASSERT_TRUE(undone.has_value());
  ASSERT_TRUE(history.can_redo());

  history.push(*undone);
  EXPECT_FALSE(history.can_redo());
}

TEST(History, DiscardsTheOldestBeyondTheLimit) {
  History history(2);
  const Project p = empty_project();
  history.push(p);
  history.push(p);
  history.push(p);
  EXPECT_EQ(history.undo_depth(), 2u);
}

// The preference is read after the session exists, so the limit has to be
// settable rather than fixed when the history is made.
TEST(History, ALowerLimitDiscardsTheOldestStraightAway) {
  History history(10);
  const Project p = empty_project();
  for (int i = 0; i < 6; ++i) history.push(p);
  ASSERT_EQ(history.undo_depth(), 6u);

  history.set_limit(2);
  EXPECT_EQ(history.undo_depth(), 2u) << "not at the next edit — now";
  EXPECT_EQ(history.limit(), 2u);
}

// The most recent edits are the ones worth keeping, so trimming takes from the
// far end. Checked by value rather than by count, because a trim that kept the
// wrong end would pass a count.
TEST(History, TrimmingKeepsTheMostRecentEdits) {
  History history(10);
  for (int i = 0; i < 5; ++i) {
    Project p = empty_project();
    p.sequence().canvas_w = 100 + i;
    history.push(p);
  }

  history.set_limit(2);
  const std::optional<Project> last = history.undo(empty_project());
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->sequence().canvas_w, 104) << "the newest snapshot pushed";
}

TEST(History, RaisingTheLimitKeepsWhatIsThere) {
  History history(3);
  const Project p = empty_project();
  for (int i = 0; i < 3; ++i) history.push(p);

  history.set_limit(50);
  EXPECT_EQ(history.undo_depth(), 3u);
}

// A future somebody is stepping through is not what "keep fewer past edits"
// asks for, and the redo stack is bounded by the undo stack it came from.
TEST(History, LoweringTheLimitLeavesRedoAlone) {
  History history(10);
  const Project p = empty_project();
  for (int i = 0; i < 4; ++i) history.push(p);
  for (int i = 0; i < 3; ++i) (void)history.undo(p);
  ASSERT_EQ(history.redo_depth(), 3u);

  history.set_limit(1);
  EXPECT_EQ(history.redo_depth(), 3u);
}

// A snapshot shares each track that is the same in the one beside it, matched
// by sequence and track id. Matched wrongly, an undo puts back somebody else's
// track; so every kind of change that moves tracks about is made here, and
// every state has to come back exactly, in both directions.
TEST(History, EveryStateComesBackWhateverMovedTheTracks) {
  std::vector<Project> states;
  Project p = empty_project(2, 2);
  p.sequences.push_back(empty_project(1, 1).sequences[0]);
  p.sequences.back().id = "second";
  states.push_back(p);

  const auto edit = [&](auto change) {
    change(p);
    states.push_back(p);
  };
  edit([](Project& q) { q.sequences[0].tracks[0].label = "picture"; });
  edit([](Project& q) {
    Track inserted;
    inserted.id = "inserted";
    inserted.label = "new";
    q.sequences[0].tracks.insert(q.sequences[0].tracks.begin(), inserted);
  });
  edit([](Project& q) { std::swap(q.sequences[0].tracks[1], q.sequences[0].tracks[2]); });
  edit([](Project& q) { q.sequences[1].tracks[0].muted = true; });
  edit([](Project& q) { q.sequences[0].tracks.pop_back(); });
  edit([](Project& q) { std::swap(q.sequences[0], q.sequences[1]); });
  edit([](Project& q) { q.sequences[1].tracks[0].label = "again"; });

  History history;
  for (std::size_t i = 0; i + 1 < states.size(); ++i) history.push(states[i]);

  Project current = states.back();
  for (std::size_t i = states.size() - 1; i-- > 0;) {
    auto back = history.undo(current);
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(*back, states[i]) << "undoing to state " << i;
    current = *back;
  }
  for (std::size_t i = 1; i < states.size(); ++i) {
    auto forward = history.redo(current);
    ASSERT_TRUE(forward.has_value());
    ASSERT_EQ(*forward, states[i]) << "redoing to state " << i;
    current = *forward;
  }
}

// Opening a project used to leave the id counter at zero, so the next split
// handed out a name the file already used — and every lookup by id then found
// two clips where it meant one.
TEST(Ids, LoadingAProjectClaimsTheNamesItAlreadyUses) {
  reset_ids();

  Project p;
  Clip c;
  c.id = "clip_9";
  c.group_id = "grp_12";
  Track t{.id = "v1", .kind = TrackKind::Video};
  t.clips = {std::move(c)};
  p.sequence().tracks = {std::move(t)};

  reset_ids();  // as if the application had just started
  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value());

  EXPECT_EQ(new_id("clip"), "clip_13") << "past the highest number in the file";
}

TEST(Ids, NoteIgnoresWhatItCannotHaveMinted) {
  reset_ids();
  note_id("v1");
  note_id("no-underscore");
  note_id("clip_");
  note_id("clip_notanumber");
  EXPECT_EQ(new_id("clip"), "clip_1");
}

TEST(Ids, TheCounterNeverGoesBackwards) {
  reset_ids();
  note_id("clip_50");
  note_id("clip_2");
  EXPECT_EQ(new_id("clip"), "clip_51");
}

TEST(History, ClearDropsBothStacks) {
  History history;
  const Project p = empty_project();
  history.push(p);
  const std::optional<Project> undone = history.undo(p);
  ASSERT_TRUE(undone.has_value());

  history.clear();
  EXPECT_FALSE(history.can_undo());
  EXPECT_FALSE(history.can_redo());
}

// ------------------------------------------------------- a drawn mask path --

TEST(Serialize, APathKeepsItsHandles) {
  Project p;
  p.sequence().canvas_w = 1920;
  p.sequence().canvas_h = 1080;

  ClipEffect blur;
  blur.type = "blur";
  blur.mask = Mask{.shape = MaskShape::Path,
                   .points = {MaskPoint{.x = -0.2, .y = -0.2, .out_x = 0.1, .out_y = -0.05},
                              MaskPoint{.x = 0.2, .y = -0.2, .in_x = -0.1, .in_y = -0.05},
                              MaskPoint{.x = 0.0, .y = 0.2}}};

  Clip c{.id = "c1", .media_id = "m1", .kind = TrackKind::Video, .source_out = 1.0};
  c.effects = {blur};
  p.sequence().tracks = {Track{.id = "v1", .kind = TrackKind::Video, .clips = {c}}};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project, p);
}

// Every path written before the pen existed has no handles at all. Reading one
// back as a polygon of sharp corners is what keeps those projects looking the
// way they were left.
TEST(Serialize, APathWrittenBeforeHandlesExistedReadsAsCorners) {
  const std::string older = R"({
    "version": 1,
    "project": {
      "canvas_w": 1920, "canvas_h": 1080,
      "tracks": [{"id": "v1", "kind": "video", "clips": [{
        "id": "c1", "media_id": "m1", "kind": "video", "source_out": 1.0,
        "effects": [{"type": "blur", "mask": {
          "shape": "path",
          "points": [{"x": -0.2, "y": -0.2}, {"x": 0.2, "y": -0.2}, {"x": 0.0, "y": 0.2}]
        }}]
      }]}]
    }
  })";

  const auto loaded = from_json(older);
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  const Mask& mask = loaded->project.sequence().tracks[0].clips[0].effects[0].mask;
  ASSERT_EQ(mask.points.size(), 3u);
  for (const MaskPoint& point : mask.points) EXPECT_TRUE(point.sharp());
}

// And a mask that never had a curve on it does not start writing four zeroes
// per point to say so.
TEST(Serialize, SharpCornersCostNothingToWriteDown) {
  Project p;
  ClipEffect blur;
  blur.type = "blur";
  blur.mask = Mask{.shape = MaskShape::Path,
                   .points = {MaskPoint{.x = -0.2, .y = -0.2}, MaskPoint{.x = 0.2, .y = -0.2},
                              MaskPoint{.x = 0.0, .y = 0.2}}};
  Clip c{.id = "c1", .media_id = "m1", .kind = TrackKind::Video, .source_out = 1.0};
  c.effects = {blur};
  p.sequence().tracks = {Track{.id = "v1", .kind = TrackKind::Video, .clips = {c}}};

  const std::string written = to_json(p);
  EXPECT_EQ(written.find("out_x"), std::string::npos)
      << "a sharp corner wrote handles it does not have";
}

TEST(Serialize, AMaskKeepsItsPathKeyframesAndTrackingMethod) {
  Project p;
  ClipEffect blur;
  blur.type = "blur";
  blur.mask = Mask{.shape = MaskShape::Path,
                   .points = {MaskPoint{.x = -0.2}, MaskPoint{.x = 0.2}, MaskPoint{.y = 0.2}},
                   .path_keys = {MaskPathKey{.t = 0.5, .points = {MaskPoint{.x = -0.1},
                                                                  MaskPoint{.x = 0.1},
                                                                  MaskPoint{.y = 0.3}}},
                                 MaskPathKey{.t = 1.5, .points = {MaskPoint{.x = -0.3},
                                                                  MaskPoint{.x = 0.3},
                                                                  MaskPoint{.y = 0.1}}}},
                   .tracking = MaskTracking::PositionRotation};
  Clip c{.id = "c1", .media_id = "m1", .kind = TrackKind::Video, .source_out = 1.0};
  c.effects = {blur};
  p.sequence().tracks = {Track{.id = "v1", .kind = TrackKind::Video, .clips = {c}}};

  const auto loaded = from_json(to_json(p));
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project, p);
}

}  // namespace
}  // namespace cutline::core
