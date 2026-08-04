#include "cutline/core/serialize.hpp"

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
  p.canvas_w = 3840;
  p.canvas_h = 2160;
  p.fps = 60.0;
  p.master_gain = 0.7;

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
  title.is_text = true;
  TextSpec spec;
  spec.content = "Hello";
  spec.align = TextAlign::Right;
  spec.background = "#000000";
  spec.stroke_color = "#ffffff";
  spec.stroke_width = 3.0;
  spec.shadow = true;
  title.text = spec;

  Media matte;
  matte.id = "m3";
  matte.is_color = true;
  matte.color = "#112233";
  matte.gradient = MatteGradient{.color2 = "#445566", .angle = 45.0};

  p.media = {footage, title, matte};

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

  p.tracks = {v, a};
  p.markers = {Marker{.id = "k1", .time = 3.0, .label = "cue", .color = "#ff0000"}};
  p.in_point = 1.5;
  p.out_point = 8.25;
  return p;
}

TEST(Serialize, RoundTripsEveryField) {
  const Project original = rich_project();
  const auto loaded = from_json(to_json(original));

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project, original);
  EXPECT_TRUE(loaded->warnings.empty());
}

TEST(Serialize, RoundTripsAnEmptyProject) {
  const Project original = empty_project();
  const auto loaded = from_json(to_json(original));

  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->project, original);
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
  EXPECT_EQ(p.canvas_w, 1920);
  EXPECT_DOUBLE_EQ(p.fps, 30.0);
  EXPECT_DOUBLE_EQ(p.master_gain, 1.0);

  const Clip& c = p.tracks[0].clips[0];
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
  const Transform& t = loaded->project.tracks[0].clips[0].transform;
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
  EXPECT_FALSE(loaded->project.tracks[0].clips[0].effects[0].mask.active());
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
  p.tracks[0].clips = {c};

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
  second.canvas_w = 1280;

  EXPECT_FALSE(history.can_undo());
  history.push(first);
  EXPECT_TRUE(history.can_undo());

  const std::optional<Project> undone = history.undo(second);
  ASSERT_TRUE(undone.has_value());
  EXPECT_EQ(undone->canvas_w, 1920);
  EXPECT_TRUE(history.can_redo());

  const std::optional<Project> redone = history.redo(*undone);
  ASSERT_TRUE(redone.has_value());
  EXPECT_EQ(redone->canvas_w, 1280);
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
  p.tracks = {std::move(t)};

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

}  // namespace
}  // namespace cutline::core
