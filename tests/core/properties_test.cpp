#include "cutline/core/properties.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/id.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cutline::core {
namespace {

using Ids = std::vector<std::string>;

Project one_clip_project() {
  Project p;
  Media m;
  m.id = "m1";
  m.duration = 20.0;
  m.has_video = true;
  p.media = {m};

  Clip c;
  c.id = "c1";
  c.media_id = "m1";
  c.source_in = 0.0;
  c.source_out = 10.0;

  Track v;
  v.id = "v1";
  v.kind = TrackKind::Video;
  v.clips = {c};
  p.tracks = {v};
  return p;
}

const Clip& only_clip(const Project& p) { return p.tracks[0].clips[0]; }

// -------------------------------------------------------- clip properties --

TEST(ClipProperties, EnableAndDisable) {
  Project p = one_clip_project();
  p = set_clips_enabled(std::move(p), Ids{"c1"}, false);
  EXPECT_TRUE(only_clip(p).disabled);
  p = set_clips_enabled(std::move(p), Ids{"c1"}, true);
  EXPECT_FALSE(only_clip(p).disabled);
}

TEST(ClipProperties, BlendMode) {
  Project p = one_clip_project();
  p = set_clip_blend(std::move(p), "c1", BlendMode::Screen);
  EXPECT_EQ(only_clip(p).blend, BlendMode::Screen);
  p = set_clip_blend(std::move(p), "c1", BlendMode::Normal);
  EXPECT_EQ(only_clip(p).blend, BlendMode::Normal);
}

TEST(ClipProperties, GainIsClamped) {
  Project p = one_clip_project();
  EXPECT_DOUBLE_EQ(set_clip_gain(p, "c1", -1.0).tracks[0].clips[0].gain, 0.0);
  EXPECT_DOUBLE_EQ(set_clip_gain(p, "c1", 99.0).tracks[0].clips[0].gain, kMaxGain);
  EXPECT_DOUBLE_EQ(set_clip_gain(p, "c1", 0.5).tracks[0].clips[0].gain, 0.5);
}

TEST(CanvasProperties, TheSequenceCanBeResized) {
  const Project p;
  const Project wide = set_canvas(p, 3840, 2160);
  EXPECT_EQ(wide.canvas_w, 3840);
  EXPECT_EQ(wide.canvas_h, 2160);
}

TEST(CanvasProperties, TheFrameRateCanBeChangedAndIsClamped) {
  const Project p;
  EXPECT_DOUBLE_EQ(set_fps(p, 59.94).fps, 59.94);
  EXPECT_DOUBLE_EQ(set_fps(p, 0.0).fps, kMinFps) << "zero would divide by nothing";
  EXPECT_DOUBLE_EQ(set_fps(p, -24.0).fps, kMinFps) << "and negative is reverse by another name";
  EXPECT_DOUBLE_EQ(set_fps(p, 100000.0).fps, kMaxFps);
}

TEST(CanvasProperties, ChangingTheFrameRateLeavesTheClipsWhereTheyAre) {
  // Every time in the model is in seconds, so the rate is how finely that
  // continuum is sampled. A cut stays where it was put.
  Project p;
  Clip c;
  c.id = "c1";
  c.start = 2.5;
  c.source_out = 4.0;
  Track t{.id = "v1", .kind = TrackKind::Video};
  t.clips = {std::move(c)};
  p.tracks = {std::move(t)};

  const Project faster = set_fps(p, 60.0);
  EXPECT_DOUBLE_EQ(faster.tracks[0].clips[0].start, 2.5);
  EXPECT_DOUBLE_EQ(faster.tracks[0].clips[0].source_out, 4.0);
}

TEST(CanvasProperties, ASizeOutsideTheRangeIsClampedRatherThanRefused) {
  const Project p;
  EXPECT_EQ(set_canvas(p, 0, 0).canvas_w, kMinCanvas);
  EXPECT_EQ(set_canvas(p, -100, -100).canvas_h, kMinCanvas);
  EXPECT_EQ(set_canvas(p, 999999, 999999).canvas_w, kMaxCanvas);
}

// Transforms are canvas fractions, so a resize is the same picture at a
// different size. Nothing about the clips changes, and a resize that quietly
// rewrote them would make the operation impossible to undo cleanly.
TEST(CanvasProperties, ResizingLeavesTheClipsAlone) {
  Project p = one_clip_project();
  p.tracks[0].clips[0].transform = {.x = 0.25, .y = 0.75, .scale_x = 0.5};

  const Project resized = set_canvas(p, 1080, 1920);
  EXPECT_EQ(resized.tracks, p.tracks);
}

TEST(MasterProperties, MasterGainIsClamped) {

  const Project p;
  EXPECT_DOUBLE_EQ(set_master_gain(p, -1.0).master_gain, 0.0);
  EXPECT_DOUBLE_EQ(set_master_gain(p, 99.0).master_gain, kMaxMasterGain);
  EXPECT_DOUBLE_EQ(set_master_gain(p, 0.5).master_gain, 0.5);
}

TEST(MasterProperties, AFreshProjectMixesAtUnity) {
  EXPECT_DOUBLE_EQ(Project{}.master_gain, 1.0);
}

TEST(ClipProperties, OpacityIsClamped) {
  const Project p = one_clip_project();
  EXPECT_DOUBLE_EQ(set_clip_opacity(p, "c1", -1.0).tracks[0].clips[0].opacity, 0.0);
  EXPECT_DOUBLE_EQ(set_clip_opacity(p, "c1", 5.0).tracks[0].clips[0].opacity, 1.0);
}

TEST(ClipProperties, TransformIsReplacedWholesale) {
  Project p = one_clip_project();
  Transform t = only_clip(p).transform;
  t.x = 0.25;
  t.rotation = 90.0;
  p = set_clip_transform(std::move(p), "c1", t);

  EXPECT_DOUBLE_EQ(only_clip(p).transform.x, 0.25);
  EXPECT_DOUBLE_EQ(only_clip(p).transform.rotation, 90.0);
  EXPECT_DOUBLE_EQ(only_clip(p).transform.y, 0.5);  // untouched default
}

// A zero-length transition is just a cut, so setting one clears it.
TEST(ClipProperties, TransitionSetAndClear) {
  Project p = one_clip_project();
  p = set_clip_transition(std::move(p), "c1",
                          Transition{.kind = TransitionKind::Push, .duration = 1.0});
  ASSERT_TRUE(only_clip(p).transition_out.has_value());
  EXPECT_EQ(only_clip(p).transition_out->kind, TransitionKind::Push);

  p = set_clip_transition(std::move(p), "c1",
                          Transition{.kind = TransitionKind::Push, .duration = 0.0});
  EXPECT_FALSE(only_clip(p).transition_out.has_value());

  p = set_clip_transition(std::move(p), "c1",
                          Transition{.kind = TransitionKind::Push, .duration = 1.0});
  p = set_clip_transition(std::move(p), "c1", std::nullopt);
  EXPECT_FALSE(only_clip(p).transition_out.has_value());
}

// ---------------------------------------------------------------- markers --

TEST(Markers, CarryANoteAndALength) {
  Project p;
  p = add_marker(std::move(p), 4.0, "cue");
  ASSERT_EQ(p.markers.size(), 1u);

  const std::string id = p.markers[0].id;
  p = set_marker(std::move(p), id, "reshoot", "the boom is in frame here", "#c07a92", 3.0);

  EXPECT_EQ(p.markers[0].label, "reshoot");
  EXPECT_EQ(p.markers[0].comment, "the boom is in frame here");
  EXPECT_EQ(p.markers[0].color, "#c07a92");
  EXPECT_DOUBLE_EQ(p.markers[0].duration, 3.0);
  EXPECT_DOUBLE_EQ(p.markers[0].time, 4.0) << "when it is is not what this edits";
}

// A marker that ended before it began would draw backwards and mean nothing.
TEST(Markers, ANegativeLengthBecomesAPoint) {
  Project p = add_marker(Project{}, 1.0);
  p = set_marker(std::move(p), p.markers[0].id, {}, {}, {}, -5.0);
  EXPECT_DOUBLE_EQ(p.markers[0].duration, 0.0);
}

TEST(Markers, SettingOneThatIsNotThereChangesNothing) {
  const Project p = add_marker(Project{}, 1.0);
  const Project after = set_marker(p, "nobody", "x", "y", "#fff", 2.0);
  EXPECT_EQ(after.markers, p.markers);
}

// ------------------------------------------------------ marks on a source --

namespace {

Project one_source_project() {
  Project p;
  Media m;
  m.id = "f1";
  m.duration = 30.0;
  m.has_video = true;
  p.media = {m};
  return p;
}

}  // namespace

TEST(SourceMarks, TheMarksAreKeptOnTheSourceRatherThanTheSequence) {
  // The point of keeping them here: they are a fact about the file, so the
  // sequence's own marks are untouched by marking a source.
  Project p = one_source_project();
  p = set_source_in_point(std::move(p), "f1", 4.0);
  p = set_source_out_point(std::move(p), "f1", 9.0);

  EXPECT_DOUBLE_EQ(*p.media[0].in_point, 4.0);
  EXPECT_DOUBLE_EQ(*p.media[0].out_point, 9.0);
  EXPECT_FALSE(p.in_point.has_value());
  EXPECT_FALSE(p.out_point.has_value());
}

TEST(SourceMarks, AMarkPastTheEndOfTheSourceLandsOnTheEnd) {
  // A sequence grows to hold what is put in it; a file does not.
  Project p = one_source_project();
  p = set_source_out_point(std::move(p), "f1", 500.0);
  EXPECT_DOUBLE_EQ(*p.media[0].out_point, 30.0);

  p = set_source_in_point(std::move(p), "f1", -5.0);
  EXPECT_DOUBLE_EQ(*p.media[0].in_point, 0.0);
}

TEST(SourceMarks, AnInPastTheOutClearsTheOut) {
  // The same rule as the sequence marks, so the pair can never be inverted and
  // nothing reading them has to check.
  Project p = one_source_project();
  p = set_source_out_point(std::move(p), "f1", 5.0);
  p = set_source_in_point(std::move(p), "f1", 8.0);

  EXPECT_DOUBLE_EQ(*p.media[0].in_point, 8.0);
  EXPECT_FALSE(p.media[0].out_point.has_value());
}

TEST(SourceMarks, AnOutBeforeTheInClearsTheIn) {
  Project p = one_source_project();
  p = set_source_in_point(std::move(p), "f1", 8.0);
  p = set_source_out_point(std::move(p), "f1", 5.0);

  EXPECT_DOUBLE_EQ(*p.media[0].out_point, 5.0);
  EXPECT_FALSE(p.media[0].in_point.has_value());
}

TEST(SourceMarks, NothingIsGivenNothingToClear) {
  Project p = one_source_project();
  p = set_source_in_point(std::move(p), "f1", 4.0);
  p = set_source_in_point(std::move(p), "f1", std::nullopt);
  EXPECT_FALSE(p.media[0].in_point.has_value());
}

TEST(SourceMarks, MarkingASourceThatIsNotThereChangesNothing) {
  const Project p = one_source_project();
  EXPECT_EQ(set_source_in_point(p, "nobody", 2.0).media, p.media);
  EXPECT_EQ(clear_source_marks(p, "nobody").media, p.media);
}

TEST(SourceMarks, ClearingTakesBothEnds) {
  Project p = one_source_project();
  p = set_source_in_point(std::move(p), "f1", 4.0);
  p = set_source_out_point(std::move(p), "f1", 9.0);
  p = clear_source_marks(std::move(p), "f1");

  EXPECT_FALSE(p.media[0].in_point.has_value());
  EXPECT_FALSE(p.media[0].out_point.has_value());
}

// --------------------------------------------------- matching the sequence --

namespace {

Media footage_media(int width, int height, double fps) {
  Media m;
  m.id = "f1";
  m.name = "capture.mkv";
  m.duration = 60.0;
  m.has_video = true;
  m.width = width;
  m.height = height;
  m.fps = fps;
  return m;
}

}  // namespace

TEST(MatchSequence, AnEmptySequenceTakesTheFootagesShapeAndRate) {
  Project p;
  p.media = {footage_media(3840, 2160, 60.0)};

  p = match_sequence_to(std::move(p), "f1");
  EXPECT_EQ(p.canvas_w, 3840);
  EXPECT_EQ(p.canvas_h, 2160);
  EXPECT_DOUBLE_EQ(p.fps, 60.0);
}

// Once anything has been placed, the shape is one somebody has been working to.
TEST(MatchSequence, ASequenceWithAnythingInItIsLeftAlone) {
  Project p = one_clip_project();
  p.media.push_back(footage_media(3840, 2160, 60.0));

  const Project after = match_sequence_to(p, "f1");
  EXPECT_EQ(after.canvas_w, p.canvas_w);
  EXPECT_DOUBLE_EQ(after.fps, p.fps);
}

// A title takes the canvas's shape rather than giving it one, and has no rate.
TEST(MatchSequence, GeneratedSourcesGiveTheSequenceNothing) {
  Project p;
  Media title;
  title.id = "t1";
  title.is_text = true;
  title.width = 100;
  title.height = 100;
  title.fps = 12.0;
  p.media = {title};

  const Project after = match_sequence_to(p, "t1");
  EXPECT_EQ(after.canvas_w, Project{}.canvas_w);
  EXPECT_DOUBLE_EQ(after.fps, Project{}.fps);
}

TEST(MatchSequence, FootageThatKnowsNeitherChangesNothing) {
  Project p;
  Media unknown;
  unknown.id = "u1";
  unknown.has_video = true;
  p.media = {unknown};

  const Project after = match_sequence_to(p, "u1");
  EXPECT_EQ(after.canvas_w, Project{}.canvas_w);
  EXPECT_DOUBLE_EQ(after.fps, Project{}.fps);
}

TEST(MatchSequence, AMediaThatIsNotThereChangesNothing) {
  const Project p;
  EXPECT_EQ(match_sequence_to(p, "nobody").canvas_w, p.canvas_w);
}

// The rates and sizes a file can claim are not always ones a sequence may have.
TEST(MatchSequence, AnAbsurdFormatIsClampedRatherThanTaken) {
  Project p;
  p.media = {footage_media(99999, 99999, 100000.0)};
  p = match_sequence_to(std::move(p), "f1");

  EXPECT_EQ(p.canvas_w, kMaxCanvas);
  EXPECT_DOUBLE_EQ(p.fps, kMaxFps);
}

// ------------------------------------------------------------------- fades --

TEST(Fades, AreClampedToTheClipLength) {
  Project p = one_clip_project();  // ten seconds long
  p = set_clip_fade(std::move(p), "c1", ClipEdge::In, 99.0);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_in, 10.0);

  p = set_clip_fade(std::move(p), "c1", ClipEdge::In, -5.0);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_in, 0.0);
}

// The two fades share the clip's length and may not overlap.
TEST(Fades, LeaveRoomForEachOther) {
  Project p = one_clip_project();
  p = set_clip_fade(std::move(p), "c1", ClipEdge::In, 7.0);
  p = set_clip_fade(std::move(p), "c1", ClipEdge::Out, 9.0);

  EXPECT_DOUBLE_EQ(only_clip(p).fade_in, 7.0);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_out, 3.0);
}

// ------------------------------------------------------------------ speed --

TEST(Speed, RetimesWithoutChangingSource) {
  Project p = one_clip_project();
  p = set_clip_speed(std::move(p), "c1", 2.0);

  EXPECT_DOUBLE_EQ(only_clip(p).speed, 2.0);
  EXPECT_DOUBLE_EQ(only_clip(p).source_out, 10.0);
  EXPECT_DOUBLE_EQ(clip_duration(only_clip(p)), 5.0);
}

TEST(Speed, IsClampedToTheAllowedRange) {
  const Project p = one_clip_project();
  EXPECT_DOUBLE_EQ(set_clip_speed(p, "c1", 0.0).tracks[0].clips[0].speed, kMinSpeed);
  EXPECT_DOUBLE_EQ(set_clip_speed(p, "c1", 1e6).tracks[0].clips[0].speed, kMaxSpeed);
}

TEST(Speed, SetsReverseOnlyWhenAsked) {
  Project p = one_clip_project();
  p = set_clip_speed(std::move(p), "c1", 2.0);
  EXPECT_FALSE(only_clip(p).reverse);

  p = set_clip_speed(std::move(p), "c1", 2.0, true);
  EXPECT_TRUE(only_clip(p).reverse);

  p = set_clip_speed(std::move(p), "c1", 3.0);  // no reverse argument
  EXPECT_TRUE(only_clip(p).reverse);            // so it is left alone
}

// Speeding a clip up shortens it, which can leave its fades longer than it is.
TEST(Speed, ReclampsFadesToTheNewDuration) {
  Project p = one_clip_project();
  p = set_clip_fade(std::move(p), "c1", ClipEdge::In, 6.0);
  p = set_clip_fade(std::move(p), "c1", ClipEdge::Out, 4.0);

  p = set_clip_speed(std::move(p), "c1", 4.0);  // ten seconds becomes two and a half

  EXPECT_DOUBLE_EQ(clip_duration(only_clip(p)), 2.5);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_in, 2.5);
  EXPECT_DOUBLE_EQ(only_clip(p).fade_out, 0.0);
}

// ------------------------------------------------------------ frame hold --

TEST(FrameHold, FreezesOnTheFrameUnderThePlayhead) {
  Project p = one_clip_project();  // source [0,10) at timeline 0
  p = set_clips_hold(std::move(p), Ids{"c1"}, 4.0);

  ASSERT_TRUE(only_clip(p).hold.has_value());
  EXPECT_DOUBLE_EQ(*only_clip(p).hold, 4.0);
  // And that is the frame however far into the clip you look.
  EXPECT_DOUBLE_EQ(source_time_at(only_clip(p), 1.0), 4.0);
  EXPECT_DOUBLE_EQ(source_time_at(only_clip(p), 9.0), 4.0);
}

TEST(FrameHold, ReleasingItPutsTheClipBackAsItWas) {
  Project p = one_clip_project();
  p = set_clips_hold(std::move(p), Ids{"c1"}, 4.0);
  p = set_clips_hold(std::move(p), Ids{"c1"}, std::nullopt);

  EXPECT_FALSE(only_clip(p).hold.has_value());
  EXPECT_DOUBLE_EQ(source_time_at(only_clip(p), 3.0), 3.0);
}

// A hold asked for outside the clip still freezes it on a frame it owns.
TEST(FrameHold, IsClampedIntoTheClip) {
  Project p = one_clip_project();
  p = set_clips_hold(std::move(p), Ids{"c1"}, 99.0);
  ASSERT_TRUE(only_clip(p).hold.has_value());
  EXPECT_DOUBLE_EQ(*only_clip(p).hold, 10.0) << "its own out point";
}

// Moving a hold asks where the clip *would* be playing, not where it is frozen.
TEST(FrameHold, CanBeMovedToAnotherFrame) {
  Project p = one_clip_project();
  p = set_clips_hold(std::move(p), Ids{"c1"}, 2.0);
  p = set_clips_hold(std::move(p), Ids{"c1"}, 7.0);
  EXPECT_DOUBLE_EQ(*only_clip(p).hold, 7.0);
}

// The picture only. A frozen frame over running sound is the effect.
TEST(FrameHold, LeavesAudioAlone) {
  Project p = one_clip_project();
  Clip sound;
  sound.id = "a1c";
  sound.media_id = "m1";
  sound.kind = TrackKind::Audio;
  sound.source_out = 10.0;
  Track a{.id = "a1", .kind = TrackKind::Audio};
  a.clips = {std::move(sound)};
  p.tracks.push_back(std::move(a));

  p = set_clips_hold(std::move(p), Ids{"c1", "a1c"}, 4.0);
  EXPECT_TRUE(only_clip(p).hold.has_value());
  EXPECT_FALSE(find_clip(p, "a1c")->hold.has_value());
}

// ------------------------------------------------- speed across a selection --

namespace {

// Two clips end to end on the picture track, a third on a second track that
// starts where the first one ends. Enough to see what a ripple moves.
Project three_clip_project() {
  Project p = one_clip_project();  // c1: ten seconds from zero on v1

  Clip second;
  second.id = "c2";
  second.media_id = "m1";
  second.start = 10.0;
  second.source_in = 0.0;
  second.source_out = 5.0;
  p.tracks[0].clips.push_back(second);

  Clip other;
  other.id = "c3";
  other.media_id = "m1";
  other.start = 10.0;
  other.source_in = 0.0;
  other.source_out = 4.0;

  Track v2;
  v2.id = "v2";
  v2.kind = TrackKind::Video;
  v2.clips = {other};
  p.tracks.push_back(v2);
  return p;
}

const Clip* clip(const Project& p, std::string_view id) { return find_clip(p, id); }

}  // namespace

TEST(SpeedAcrossSelection, LeavesNeighboursAloneWithoutRipple) {
  Project p = three_clip_project();
  p = set_clips_speed(std::move(p), Ids{"c1"}, 2.0, std::nullopt, false);

  EXPECT_DOUBLE_EQ(clip_duration(*clip(p, "c1")), 5.0);
  EXPECT_DOUBLE_EQ(clip(p, "c2")->start, 10.0);  // the gap stays open
  EXPECT_DOUBLE_EQ(clip(p, "c3")->start, 10.0);
}

TEST(SpeedAcrossSelection, RippleClosesTheGapASpeedUpLeaves) {
  Project p = three_clip_project();
  p = set_clips_speed(std::move(p), Ids{"c1"}, 2.0, std::nullopt, true);

  EXPECT_DOUBLE_EQ(clip(p, "c2")->start, 5.0);
  EXPECT_DOUBLE_EQ(clip(p, "c3")->start, 5.0);  // sync locked, so it comes too
}

TEST(SpeedAcrossSelection, RippleMakesRoomForASlowDown) {
  Project p = three_clip_project();
  p = set_clips_speed(std::move(p), Ids{"c1"}, 0.5, std::nullopt, true);

  EXPECT_DOUBLE_EQ(clip_duration(*clip(p, "c1")), 20.0);
  EXPECT_DOUBLE_EQ(clip(p, "c2")->start, 20.0);
}

// A pinned track sits still through an edit made somewhere else.
TEST(SpeedAcrossSelection, RippleSkipsATrackWithoutSyncLock) {
  Project p = three_clip_project();
  p.tracks[1].sync_locked = false;
  p = set_clips_speed(std::move(p), Ids{"c1"}, 2.0, std::nullopt, true);

  EXPECT_DOUBLE_EQ(clip(p, "c2")->start, 5.0);
  EXPECT_DOUBLE_EQ(clip(p, "c3")->start, 10.0);
}

// The linked pair is one edit, not two, so the sequence closes up once.
TEST(SpeedAcrossSelection, ALinkedPairShiftsTheSequenceOnce) {
  Project p = three_clip_project();
  p.tracks[0].clips[0].group_id = "g";
  p.tracks[1].clips[0].group_id = "g";
  p.tracks[1].clips[0].start = 0.0;
  p.tracks[1].clips[0].source_out = 10.0;  // the same length, and linked

  p = set_clips_speed(std::move(p), Ids{"c1"}, 2.0, std::nullopt, true);

  EXPECT_DOUBLE_EQ(clip_duration(*clip(p, "c3")), 5.0);  // the group retimed too
  EXPECT_DOUBLE_EQ(clip(p, "c2")->start, 5.0);           // and moved five, not ten
}

TEST(SpeedAcrossSelection, AnEmptySelectionChangesNothing) {
  const Project p = three_clip_project();
  const Project after = set_clips_speed(p, Ids{}, 2.0, std::nullopt, true);
  EXPECT_DOUBLE_EQ(clip_duration(*clip(after, "c1")), 10.0);
  EXPECT_DOUBLE_EQ(clip(after, "c2")->start, 10.0);
}

// ------------------------------------------------------- generated media --

TEST(GeneratedMedia, TextSpecOnlyAppliesToTitles) {
  Project p = one_clip_project();
  Media title;
  title.id = "t1";
  title.is_text = true;
  title.text = TextSpec{};
  p.media.push_back(title);

  TextSpec spec;
  spec.content = "Hello";
  spec.font_size = 42.0;
  p = set_text_spec(std::move(p), "t1", spec);
  EXPECT_EQ(p.media[1].text->content, "Hello");

  // The footage media is not a title, so it is untouched.
  const Project before = p;
  EXPECT_EQ(set_text_spec(before, "m1", spec), before);
}

TEST(GeneratedMedia, MatteColourAndGradient) {
  Project p = one_clip_project();
  Media matte;
  matte.id = "k1";
  matte.is_color = true;
  matte.color = kDefaultMatteColor;
  p.media.push_back(matte);

  p = set_matte_color(std::move(p), "k1", "#ff0000");
  EXPECT_EQ(p.media[1].color, "#ff0000");

  p = set_matte_gradient(std::move(p), "k1", MatteGradient{.color2 = "#0000ff", .angle = 90.0});
  ASSERT_TRUE(p.media[1].gradient.has_value());
  EXPECT_DOUBLE_EQ(p.media[1].gradient->angle, 90.0);

  p = set_matte_gradient(std::move(p), "k1", std::nullopt);
  EXPECT_FALSE(p.media[1].gradient.has_value());
}

TEST(GeneratedMedia, MatteSettersIgnoreOtherMedia) {
  const Project before = one_clip_project();
  EXPECT_EQ(set_matte_color(before, "m1", "#ff0000"), before);
  EXPECT_EQ(set_matte_gradient(before, "m1", MatteGradient{}), before);
}

// ------------------------------------------------------------------ tracks --

// Video tracks are stored top-first, so a new one goes to the front and becomes
// the topmost compositing layer.
TEST(Tracks, VideoTracksAreAddedOnTop) {
  Project p = one_clip_project();
  p = add_video_track(std::move(p));
  EXPECT_EQ(p.tracks[0].kind, TrackKind::Video);
  EXPECT_TRUE(p.tracks[0].clips.empty());
  EXPECT_EQ(p.tracks[1].id, "v1");
}

TEST(Tracks, AudioTracksAreAddedAtTheBottom) {
  Project p = one_clip_project();
  p = add_audio_track(std::move(p));
  EXPECT_EQ(p.tracks.back().kind, TrackKind::Audio);
}

TEST(Tracks, LabelIsTrimmedAndClearable) {
  Project p = one_clip_project();
  p = set_track_label(std::move(p), "v1", "  Overlay  ");
  EXPECT_EQ(p.tracks[0].label, "Overlay");

  p = set_track_label(std::move(p), "v1", "   ");
  EXPECT_EQ(p.tracks[0].label, "");
}

TEST(Tracks, PatchTouchesOnlyWhatItSets) {
  Project p = one_clip_project();
  p = update_track(std::move(p), "v1", TrackPropsPatch{.hidden = true});
  EXPECT_TRUE(p.tracks[0].hidden);
  EXPECT_FALSE(p.tracks[0].locked);

  p = update_track(std::move(p), "v1", TrackPropsPatch{.locked = true});
  EXPECT_TRUE(p.tracks[0].hidden);  // still set
  EXPECT_TRUE(p.tracks[0].locked);
}

TEST(Tracks, RemoveTakesItsClipsWithIt) {
  Project p = one_clip_project();
  p = remove_track(std::move(p), "v1");
  EXPECT_TRUE(p.tracks.empty());
  EXPECT_EQ(find_clip(p, "c1"), nullptr);
}

TEST(Tracks, UnknownTracksAreNoOps) {
  const Project before = one_clip_project();
  EXPECT_EQ(set_track_label(before, "nope", "x"), before);
  EXPECT_EQ(update_track(before, "nope", TrackPropsPatch{.muted = true}), before);
  EXPECT_EQ(remove_track(before, "nope"), before);
}

// ----------------------------------------------------------------- markers --

Project marked_project() {
  Project p = one_clip_project();
  p = add_marker(std::move(p), 5.0, "five");
  p = add_marker(std::move(p), 1.0, "one");
  p = add_marker(std::move(p), 3.0, "three");
  return p;
}

TEST(Markers, AreKeptInTimeOrder) {
  const Project p = marked_project();
  ASSERT_EQ(p.markers.size(), 3u);
  EXPECT_DOUBLE_EQ(p.markers[0].time, 1.0);
  EXPECT_DOUBLE_EQ(p.markers[1].time, 3.0);
  EXPECT_DOUBLE_EQ(p.markers[2].time, 5.0);
}

TEST(Markers, NearFindsTheClosestWithinTolerance) {
  const Project p = marked_project();
  ASSERT_NE(marker_near(p, 3.2, 0.5), nullptr);
  EXPECT_EQ(marker_near(p, 3.2, 0.5)->label, "three");
  EXPECT_EQ(marker_near(p, 3.2, 0.1), nullptr);
}

TEST(Markers, NavigateForwardsAndBackwards) {
  const Project p = marked_project();
  ASSERT_NE(next_marker(p, 1.0), nullptr);
  EXPECT_DOUBLE_EQ(next_marker(p, 1.0)->time, 3.0);
  EXPECT_EQ(next_marker(p, 5.0), nullptr);

  ASSERT_NE(previous_marker(p, 5.0), nullptr);
  EXPECT_DOUBLE_EQ(previous_marker(p, 5.0)->time, 3.0);
  EXPECT_EQ(previous_marker(p, 1.0), nullptr);
}

TEST(Markers, RemoveAndClear) {
  Project p = marked_project();
  const std::string id = p.markers[1].id;

  p = remove_marker(std::move(p), id);
  EXPECT_EQ(p.markers.size(), 2u);

  p = clear_markers(std::move(p));
  EXPECT_TRUE(p.markers.empty());
}

// --------------------------------------------------------------- factories --

TEST(EmptyProject, HasTheRequestedLanesAndNoMedia) {
  reset_ids();
  const Project p = empty_project();

  ASSERT_EQ(p.tracks.size(), 3u);
  EXPECT_EQ(p.tracks[0].kind, TrackKind::Video);
  EXPECT_EQ(p.tracks[1].kind, TrackKind::Audio);
  EXPECT_EQ(p.tracks[2].kind, TrackKind::Audio);
  EXPECT_TRUE(p.media.empty());
  EXPECT_EQ(p.canvas_w, 1920);
  EXPECT_EQ(p.canvas_h, 1080);
  EXPECT_DOUBLE_EQ(p.fps, 30.0);
}

TEST(EmptyProject, HonoursACustomTrackCount) {
  const Project p = empty_project(2, 1);
  ASSERT_EQ(p.tracks.size(), 3u);
  EXPECT_EQ(p.tracks[0].kind, TrackKind::Video);
  EXPECT_EQ(p.tracks[1].kind, TrackKind::Video);
  EXPECT_EQ(p.tracks[2].kind, TrackKind::Audio);
}

// ------------------------------------------------------------- in and out --

TEST(Marks, AnUnmarkedProjectIsTheWholeTimeline) {
  const Project p = one_clip_project();  // one clip, ten seconds
  EXPECT_FALSE(has_marks(p));
  EXPECT_EQ(marked_span(p), (MarkedSpan{.start = 0.0, .duration = 10.0}));
}

TEST(Marks, AnInAloneRunsToTheEnd) {
  const Project p = set_in_point(one_clip_project(), 4.0);
  EXPECT_TRUE(has_marks(p));
  EXPECT_EQ(marked_span(p), (MarkedSpan{.start = 4.0, .duration = 6.0}));
}

TEST(Marks, AnOutAloneRunsFromTheStart) {
  const Project p = set_out_point(one_clip_project(), 4.0);
  EXPECT_EQ(marked_span(p), (MarkedSpan{.start = 0.0, .duration = 4.0}));
}

TEST(Marks, BothTogetherAreTheSpanBetweenThem) {
  Project p = set_in_point(one_clip_project(), 2.0);
  p = set_out_point(std::move(p), 7.0);
  EXPECT_EQ(marked_span(p), (MarkedSpan{.start = 2.0, .duration = 5.0}));
}

TEST(Marks, AnInPastTheOutClearsTheOut) {
  // Rather than crossing it. Somebody marking an in past the out has moved on
  // to a different span, and dragging the out after it would silently keep a
  // boundary they had stopped caring about.
  Project p = set_out_point(one_clip_project(), 3.0);
  p = set_in_point(std::move(p), 6.0);

  EXPECT_FALSE(p.out_point.has_value());
  EXPECT_DOUBLE_EQ(*p.in_point, 6.0);
  EXPECT_EQ(marked_span(p), (MarkedSpan{.start = 6.0, .duration = 4.0}));
}

TEST(Marks, AnOutBeforeTheInClearsTheIn) {
  Project p = set_in_point(one_clip_project(), 6.0);
  p = set_out_point(std::move(p), 3.0);

  EXPECT_FALSE(p.in_point.has_value());
  EXPECT_DOUBLE_EQ(*p.out_point, 3.0);
}

TEST(Marks, TheTwoCanNeverBeInverted) {
  // The property the two rules above exist to guarantee: whatever order the
  // marks are set in, `marked_span` never has to cope with a backwards range.
  Project p = one_clip_project();
  for (const double at : {5.0, 1.0, 8.0, 3.0, 3.0, 9.0}) {
    p = set_in_point(std::move(p), at);
    EXPECT_GE(marked_span(p).duration, 0.0);
    p = set_out_point(std::move(p), 10.0 - at);
    EXPECT_GE(marked_span(p).duration, 0.0);
    if (p.in_point.has_value() && p.out_point.has_value()) {
      EXPECT_LT(*p.in_point, *p.out_point);
    }
  }
}

TEST(Marks, ClearingLeavesTheWholeTimeline) {
  Project p = set_in_point(one_clip_project(), 2.0);
  p = set_out_point(std::move(p), 7.0);
  p = clear_marks(std::move(p));

  EXPECT_FALSE(has_marks(p));
  EXPECT_EQ(marked_span(p), (MarkedSpan{.start = 0.0, .duration = 10.0}));
}

TEST(Marks, AreClampedToTheSequence) {
  // A mark left behind by a clip that has since been deleted must not ask the
  // exporter for frames that are not there.
  const Project negative = set_in_point(one_clip_project(), -5.0);
  EXPECT_DOUBLE_EQ(*negative.in_point, 0.0) << "negative times are clamped when set";

  Project stale = one_clip_project();
  stale.out_point = 900.0;  // as a file written before a clip was deleted might
  EXPECT_EQ(marked_span(stale), (MarkedSpan{.start = 0.0, .duration = 10.0}));
}

TEST(Marks, SettingNothingClearsOnlyThatOne) {
  Project p = set_in_point(one_clip_project(), 2.0);
  p = set_out_point(std::move(p), 7.0);

  p = set_in_point(std::move(p), std::nullopt);
  EXPECT_FALSE(p.in_point.has_value());
  EXPECT_TRUE(p.out_point.has_value()) << "the other one is not collateral";
}


// -------------------------------------------------------------- track mix --

TEST(TrackMix, SetsTheFaderAndThePanner) {
  Project p = empty_project();
  p.tracks.push_back(Track{.id = "a1", .kind = TrackKind::Audio});

  p = set_track_gain(std::move(p), "a1", 0.5);
  p = set_track_pan(std::move(p), "a1", -0.25);

  EXPECT_DOUBLE_EQ(p.tracks.back().gain, 0.5);
  EXPECT_DOUBLE_EQ(p.tracks.back().pan, -0.25);
}

TEST(TrackMix, IsClampedToWhatTheMixerWillHold) {
  Project p = empty_project();
  p.tracks.push_back(Track{.id = "a1", .kind = TrackKind::Audio});

  EXPECT_DOUBLE_EQ(set_track_gain(p, "a1", 99.0).tracks.back().gain, kMaxGain);
  EXPECT_DOUBLE_EQ(set_track_pan(p, "a1", 4.0).tracks.back().pan, 1.0);
  EXPECT_DOUBLE_EQ(set_track_pan(p, "a1", -4.0).tracks.back().pan, -1.0);
}

TEST(TrackMix, AVideoTrackHasNothingToMix) {
  const Project p = empty_project();
  EXPECT_EQ(set_track_gain(p, p.tracks.front().id, 0.5), p);
  EXPECT_EQ(set_track_pan(p, p.tracks.front().id, 0.5), p);
}

TEST(TrackMix, ATrackThatIsNotThereChangesNothing) {
  const Project p = empty_project();
  EXPECT_EQ(set_track_gain(p, "nope", 0.5), p);
  EXPECT_EQ(set_track_pan(p, "nope", 0.5), p);
}

// -------------------------------------------------------------- labels --

TEST(ClipLabel, ColoursEveryClipNamed) {
  // Labelling is done to a selection: a shot is usually several clips, and
  // colouring the picture and leaving the sound is not what anybody means.
  Project p = one_clip_project();
  Clip second = p.tracks[0].clips[0];
  second.id = "c2";
  second.start = 20.0;
  p.tracks[0].clips.push_back(second);

  p = set_clips_label(std::move(p), std::vector<std::string>{"c1", "c2"}, "#8f7bb8");

  EXPECT_EQ(find_clip(p, "c1")->label_color, "#8f7bb8");
  EXPECT_EQ(find_clip(p, "c2")->label_color, "#8f7bb8");
}

TEST(ClipLabel, AnEmptyColourPutsItBack) {
  Project p = one_clip_project();
  p = set_clips_label(std::move(p), std::vector<std::string>{"c1"}, "#8f7bb8");
  p = set_clips_label(std::move(p), std::vector<std::string>{"c1"}, "");
  EXPECT_TRUE(find_clip(p, "c1")->label_color.empty());
}

TEST(ClipLabel, ChangesNothingAboutWhatIsRendered) {
  // A label is for the person reading the timeline. A colour that also did
  // something would be a colour nobody dared use.
  const Project before = one_clip_project();
  Project after = set_clips_label(before, std::vector<std::string>{"c1"}, "#8f7bb8");

  const Clip& was = *find_clip(before, "c1");
  const Clip& now = *find_clip(after, "c1");
  EXPECT_DOUBLE_EQ(was.start, now.start);
  EXPECT_DOUBLE_EQ(was.opacity, now.opacity);
  EXPECT_EQ(was.disabled, now.disabled);
  EXPECT_EQ(was.effects.size(), now.effects.size());
}

// ------------------------------------------------------------- media labels --

TEST(MediaLabel, IsPutOnClipsCutFromItAfterwards) {
  // The whole point: label the source once and every shot from it arrives
  // already coloured.
  Project p = core::empty_project(1, 1);
  Media media;
  media.id = "f1";
  media.duration = 10.0;
  media.has_video = true;
  p.media = {media};

  p = set_media_label(std::move(p), "f1", "#8f7bb8");
  p = place_media(std::move(p), "f1", 0.0);

  ASSERT_FALSE(p.tracks.front().clips.empty());
  EXPECT_EQ(p.tracks.front().clips.front().label_color, "#8f7bb8");
}

TEST(MediaLabel, DoesNotRepaintClipsAlreadyOnTheTimeline) {
  // A label is exactly the thing people set by hand, and relabelling a source
  // must not undo that.
  Project p = core::empty_project(1, 1);
  Media media;
  media.id = "f1";
  media.duration = 10.0;
  media.has_video = true;
  p.media = {media};

  p = place_media(std::move(p), "f1", 0.0);
  const std::string clip_id = p.tracks.front().clips.front().id;
  p = set_clips_label(std::move(p), std::vector<std::string>{clip_id}, "#c05050");

  p = set_media_label(std::move(p), "f1", "#8f7bb8");
  EXPECT_EQ(find_clip(p, clip_id)->label_color, "#c05050")
      << "labelling the source repainted a clip somebody had coloured by hand";
}

TEST(MediaLabel, AnEmptyColourTakesItOff) {
  Project p = one_source_project();
  p = set_media_label(std::move(p), "f1", "#8f7bb8");
  p = set_media_label(std::move(p), "f1", "");
  EXPECT_TRUE(p.media[0].label_color.empty());
}

TEST(MediaLabel, LabellingSomethingThatIsNotThereChangesNothing) {
  const Project before = one_source_project();
  EXPECT_EQ(set_media_label(before, "gone", "#8f7bb8"), before);
}

// ------------------------------------------------------------------ proxies --

TEST(Proxies, AttachingOneKeepsTheOriginalPath) {
  // The whole arrangement rests on this: a proxy stands in for the original and
  // does not replace it, because the original is what gets exported.
  Project p = one_source_project();
  p.media[0].path = "D:/Footage/A001.mp4";
  p = set_proxy_path(std::move(p), "f1", "D:/Footage/Proxies/A001.mp4");

  EXPECT_EQ(p.media[0].path, "D:/Footage/A001.mp4");
  EXPECT_EQ(p.media[0].proxy_path, "D:/Footage/Proxies/A001.mp4");
}

TEST(Proxies, AnEmptyPathTakesTheProxyAway) {
  // What "make it again" needs, and what a proxy pointing at a file that is no
  // longer there needs.
  Project p = one_source_project();
  p = set_proxy_path(std::move(p), "f1", "D:/Proxies/A001.mp4");
  p = set_proxy_path(std::move(p), "f1", "");
  EXPECT_TRUE(p.media[0].proxy_path.empty());
}

TEST(Proxies, AttachingToASourceThatIsNotThereChangesNothing) {
  // A proxy can finish long after the source it was for was removed.
  Project p = one_source_project();
  p = set_proxy_path(std::move(p), "gone", "D:/Proxies/A001.mp4");
  EXPECT_TRUE(p.media[0].proxy_path.empty());
}

TEST(Proxies, TheSwitchIsAPropertyOfTheProject) {
  // So a cut opened on another machine is edited the way it was left rather
  // than the way that machine happens to be set.
  Project p = one_source_project();
  EXPECT_FALSE(p.use_proxies) << "a project should not start out reading from files it has none of";
  p = set_use_proxies(std::move(p), true);
  EXPECT_TRUE(p.use_proxies);
  p = set_use_proxies(std::move(p), false);
  EXPECT_FALSE(p.use_proxies);
}

TEST(Proxies, CountingThemIsWhatSaysWhetherTheSwitchWouldDoAnything) {
  Project p = one_source_project();
  Media second;
  second.id = "f2";
  second.duration = 10.0;
  p.media.push_back(second);

  EXPECT_EQ(proxy_count(p), 0u);
  p = set_proxy_path(std::move(p), "f1", "D:/Proxies/one.mp4");
  EXPECT_EQ(proxy_count(p), 1u);
  p = set_proxy_path(std::move(p), "f2", "D:/Proxies/two.mp4");
  EXPECT_EQ(proxy_count(p), 2u);
  p = set_proxy_path(std::move(p), "f1", "");
  EXPECT_EQ(proxy_count(p), 1u);
}

}  // namespace
}  // namespace cutline::core
