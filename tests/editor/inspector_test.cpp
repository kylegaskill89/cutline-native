/// The inspector's description of a clip.
///
/// The thing most worth pinning down is the unit conversion. A slider reads
/// 100% and the clip stores 1.0, and if those two ever disagree the symptom is
/// a control that jumps the moment it is touched.

#include "cutline/editor/inspector.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
// The volume row's floor is the timeline rubber band's, and the two must agree.
#include "cutline/ui/timeline.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

[[nodiscard]] Project sample_project() {
  Project project;
  project.fps = 30.0;
  project.media = {
      Media{.id = "m1", .name = "wide.mp4", .duration = 60.0, .has_video = true},
      Media{.id = "m2", .name = "music.wav", .duration = 60.0, .audio_stream_count = 1},
  };

  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {Clip{.id = "c1", .media_id = "m1", .source_in = 0.0, .source_out = 8.0,
                      .start = 0.0}};

  Track audio{.id = "a1", .kind = TrackKind::Audio};
  audio.clips = {Clip{.id = "a1c", .media_id = "m2", .kind = TrackKind::Audio,
                      .source_in = 0.0, .source_out = 8.0, .start = 0.0}};

  project.tracks = {std::move(video), std::move(audio)};
  return project;
}

/// By value rather than by pointer, deliberately.
///
/// `clip_parameters` returns a vector, so nearly every call site here passes a
/// temporary. A helper handing back a pointer into it dangles at the semicolon,
/// and the symptom is a test that reads a plausible number out of freed memory
/// and passes â€” which is exactly what happened before this returned a copy.
[[nodiscard]] std::optional<ParamSpec> find(const std::vector<ParamSpec>& specs,
                                            ClipParam param) {
  const auto found = std::ranges::find(specs, param, &ParamSpec::param);
  if (found == specs.end()) return std::nullopt;
  return *found;
}

[[nodiscard]] bool has(const std::vector<ParamSpec>& specs, ClipParam param) {
  return find(specs, param).has_value();
}

// ----------------------------------------------------------- what is shown --

TEST(Inspector, AVideoClipGetsItsTransform) {
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "c1");

  EXPECT_TRUE(has(specs, ClipParam::Opacity));
  EXPECT_TRUE(has(specs, ClipParam::X));
  EXPECT_TRUE(has(specs, ClipParam::ScaleY));
  EXPECT_TRUE(has(specs, ClipParam::Rotation));
  EXPECT_FALSE(has(specs, ClipParam::Gain)) << "a video clip has no volume of its own";
}

TEST(Inspector, AnAudioClipGetsVolumeAndNoGeometry) {
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "a1c");

  EXPECT_TRUE(has(specs, ClipParam::Gain));
  EXPECT_FALSE(has(specs, ClipParam::Rotation)) << "rotating audio means nothing";
  EXPECT_FALSE(has(specs, ClipParam::Opacity));
}

TEST(Inspector, BothKindsGetSpeedAndFades) {
  for (const char* id : {"c1", "a1c"}) {
    const std::vector<ParamSpec> specs = clip_parameters(sample_project(), id);
    EXPECT_TRUE(has(specs, ClipParam::Speed)) << id;
    EXPECT_TRUE(has(specs, ClipParam::FadeIn)) << id;
    EXPECT_TRUE(has(specs, ClipParam::FadeOut)) << id;
  }
}

TEST(Inspector, NoClipMeansNoParameters) {
  EXPECT_TRUE(clip_parameters(sample_project(), "ghost").empty());
  EXPECT_TRUE(clip_parameters(sample_project(), "").empty());
}

TEST(Inspector, EveryParameterIsNamedAndBounded) {
  for (const ParamSpec& spec : clip_parameters(sample_project(), "c1")) {
    EXPECT_FALSE(spec.name.empty()) << to_string(spec.param);
    EXPECT_LT(spec.range.minimum, spec.range.maximum) << to_string(spec.param);
    // The value on show has to be one the control can actually represent.
    EXPECT_DOUBLE_EQ(spec.range.clamp(spec.value), spec.value) << to_string(spec.param);
    EXPECT_DOUBLE_EQ(spec.range.clamp(spec.fallback), spec.fallback) << to_string(spec.param);
  }
}

TEST(Inspector, ParameterNamesAreDistinct) {
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "c1");
  for (std::size_t i = 0; i < specs.size(); ++i) {
    for (std::size_t j = i + 1; j < specs.size(); ++j) {
      EXPECT_NE(specs[i].name, specs[j].name);
      EXPECT_NE(specs[i].param, specs[j].param);
    }
  }
}

TEST(Inspector, FadesAreBoundedByTheClipTheyAreOn) {
  // A two second fade on a one second clip is not a shorter fade, it is a
  // mistake, and the control should not offer it in the first place.
  const Project project = sample_project();
  const double length = core::clip_duration(*core::find_clip(project, "c1"));

  const std::optional<ParamSpec> fade = find(clip_parameters(project, "c1"), ClipParam::FadeIn);
  ASSERT_TRUE(fade.has_value());
  EXPECT_DOUBLE_EQ(fade->range.maximum, length);
}

// ------------------------------------------------------------------ units --

TEST(Inspector, OpacityIsShownAsAPercentage) {
  Project project = sample_project();
  project = core::set_clip_opacity(std::move(project), "c1", 0.5);

  const std::optional<ParamSpec> spec = find(clip_parameters(project, "c1"), ClipParam::Opacity);
  ASSERT_TRUE(spec.has_value());
  EXPECT_DOUBLE_EQ(spec->value, 50.0);
  EXPECT_EQ(spec->suffix, "%");
}

TEST(Inspector, SettingAPercentageStoresAFraction) {
  const Project after = set_clip_parameter(sample_project(), "c1", ClipParam::Opacity, 25.0);
  EXPECT_DOUBLE_EQ(core::find_clip(after, "c1")->opacity, 0.25);
}

TEST(Inspector, ReadingBackWhatWasWrittenGivesTheSameNumber) {
  // The property that keeps a control from jumping the instant it is touched:
  // set 40%, ask what it is, get 40%.
  Project project = sample_project();

  for (const auto& [param, value] : std::vector<std::pair<ClipParam, double>>{
           {ClipParam::Opacity, 40.0},
           {ClipParam::X, 25.0},
           {ClipParam::Y, 75.0},
           {ClipParam::ScaleX, 150.0},
           {ClipParam::ScaleY, 80.0},
           {ClipParam::Rotation, -35.0},
           {ClipParam::FadeIn, 1.5},
       }) {
    project = set_clip_parameter(std::move(project), "c1", param, value);
    const std::optional<ParamSpec> spec = find(clip_parameters(project, "c1"), param);
    ASSERT_TRUE(spec.has_value()) << to_string(param);
    EXPECT_NEAR(spec->value, value, 1e-9) << to_string(param);
  }
}

// Volume is the one row that is not its stored value scaled. It was a
// percentage, which made the slider unusable: half its travel covered +0 to
// +6 dB and everything from a gentle trim down to silence was squeezed into the
// last tenth of it.
TEST(Inspector, VolumeIsShownInDecibels) {
  const std::optional<ParamSpec> spec =
      find(clip_parameters(sample_project(), "a1c"), ClipParam::Gain);
  ASSERT_TRUE(spec.has_value());

  EXPECT_EQ(spec->suffix, "dB");
  // Unity is nought decibels, and it is where a double-click puts it back to.
  EXPECT_NEAR(spec->value, 0.0, 1e-9);
  EXPECT_DOUBLE_EQ(spec->fallback, 0.0);
}

TEST(Inspector, VolumeRoundTripsToo) {
  Project project = sample_project();
  project = set_clip_parameter(std::move(project), "a1c", ClipParam::Gain, -6.0);

  EXPECT_NEAR(core::find_clip(project, "a1c")->gain, 0.5011872336, 1e-9);
  const std::optional<ParamSpec> spec = find(clip_parameters(project, "a1c"), ClipParam::Gain);
  ASSERT_TRUE(spec.has_value());
  EXPECT_NEAR(spec->value, -6.0, 1e-9);
}

// The point of the scale: equal steps in the row are equal steps in loudness,
// so the useful range is not squeezed into one end of the control.
TEST(Inspector, HalvingTheVolumeTwiceIsTwoEqualSteps) {
  const auto shown = [](double gain) {
    Project project = sample_project();
    project.tracks[1].clips[0].gain = gain;
    return find(clip_parameters(project, "a1c"), ClipParam::Gain)->value;
  };

  EXPECT_NEAR(shown(1.0) - shown(0.5), shown(0.5) - shown(0.25), 1e-9);
}

TEST(Inspector, TheVolumeRangeMatchesWhatTheModelAllows) {
  const std::optional<ParamSpec> spec =
      find(clip_parameters(sample_project(), "a1c"), ClipParam::Gain);
  ASSERT_TRUE(spec.has_value());

  // The top is the loudest gain the model will store rather than a round number
  // of decibels, or the last of the slider's travel would be refused by the
  // clamp in the core.
  const Project loud =
      set_clip_parameter(sample_project(), "a1c", ClipParam::Gain, spec->range.maximum);
  EXPECT_DOUBLE_EQ(core::find_clip(loud, "a1c")->gain, core::kMaxGain);
}

// The slider and the timeline's rubber band are two views of one number, and
// both have to agree that the bottom of the range is silence rather than merely
// very quiet — or a clip dragged off on one reads as audible on the other.
TEST(Inspector, TheBottomOfTheVolumeRangeIsSilence) {
  const std::optional<ParamSpec> spec =
      find(clip_parameters(sample_project(), "a1c"), ClipParam::Gain);
  ASSERT_TRUE(spec.has_value());
  EXPECT_DOUBLE_EQ(spec->range.minimum, ui::kGainFloorDb);

  const Project silent =
      set_clip_parameter(sample_project(), "a1c", ClipParam::Gain, spec->range.minimum);
  EXPECT_DOUBLE_EQ(core::find_clip(silent, "a1c")->gain, 0.0);

  // And a silent clip reads back at the foot rather than at minus infinity,
  // which is not a number a slider can be put at.
  Project quiet = sample_project();
  quiet.tracks[1].clips[0].gain = 0.0;
  EXPECT_DOUBLE_EQ(find(clip_parameters(quiet, "a1c"), ClipParam::Gain)->value,
                   ui::kGainFloorDb);
}

TEST(Inspector, SpeedIsShownAsAMultiplier) {
  Project project = sample_project();
  project = set_clip_parameter(std::move(project), "c1", ClipParam::Speed, 2.0);

  EXPECT_DOUBLE_EQ(core::clip_speed(*core::find_clip(project, "c1")), 2.0);
  const std::optional<ParamSpec> spec = find(clip_parameters(project, "c1"), ClipParam::Speed);
  ASSERT_TRUE(spec.has_value());
  EXPECT_DOUBLE_EQ(spec->value, 2.0);
  EXPECT_EQ(spec->suffix, "x");
}

TEST(Inspector, TheSpeedSliderIsNarrowerThanTheModelAllows) {
  // Deliberate. The model permits 0.05 to 100, and a linear control spanning
  // that puts every speed anyone wants in its first few pixels.
  const std::optional<ParamSpec> spec = find(clip_parameters(sample_project(), "c1"), ClipParam::Speed);
  ASSERT_TRUE(spec.has_value());
  EXPECT_GT(spec->range.minimum, core::kMinSpeed);
  EXPECT_LT(spec->range.maximum, core::kMaxSpeed);
  EXPECT_DOUBLE_EQ(spec->range.clamp(1.0), 1.0) << "unity must still be reachable";
}

// ------------------------------------------------------------- the edits --

TEST(Inspector, AnUnknownClipChangesNothing) {
  const Project before = sample_project();
  EXPECT_EQ(set_clip_parameter(before, "ghost", ClipParam::Opacity, 50.0), before);
}

TEST(Inspector, SettingWhatIsAlreadyThereChangesNothing) {
  // Which is what lets the session skip the undo entry.
  const Project before = sample_project();
  const std::optional<ParamSpec> spec = find(clip_parameters(before, "c1"), ClipParam::Opacity);
  ASSERT_TRUE(spec.has_value());

  EXPECT_EQ(set_clip_parameter(before, "c1", ClipParam::Opacity, spec->value), before);
}

TEST(Inspector, TheModelStillClampsWhatItIsGiven) {
  // The control bounds are a convenience, not the rule. The model owns that.
  const Project after = set_clip_parameter(sample_project(), "c1", ClipParam::Opacity, 900.0);
  EXPECT_LE(core::find_clip(after, "c1")->opacity, 1.0);

  const Project quiet = set_clip_parameter(sample_project(), "a1c", ClipParam::Gain, -50.0);
  EXPECT_GE(core::find_clip(quiet, "a1c")->gain, 0.0);
}

// -------------------------------------------------------------- keyframes --

TEST(Inspector, TheTransformAndOpacityCanBeAnimatedAndTheFadesCannot) {
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "c1");
  EXPECT_TRUE(find(specs, ClipParam::Opacity)->animatable);
  EXPECT_TRUE(find(specs, ClipParam::X)->animatable);
  EXPECT_TRUE(find(specs, ClipParam::Rotation)->animatable);

  // A fade whose length changed over its own duration is not something the
  // model can express, and neither is a speed that varies within a clip.
  EXPECT_FALSE(find(specs, ClipParam::FadeIn)->animatable);
  EXPECT_FALSE(find(specs, ClipParam::Speed)->animatable);
}

TEST(Inspector, VolumeCanBeAnimated) {
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "a1c");
  EXPECT_TRUE(find(specs, ClipParam::Gain)->animatable);
}

TEST(Inspector, TurningTheStopwatchOnKeepsTheValueItHad) {
  Project p = set_clip_parameter(sample_project(), "c1", ClipParam::Opacity, 60.0);
  p = set_clip_parameter_animated(std::move(p), "c1", ClipParam::Opacity, true, 2.0);

  const std::optional<ParamSpec> row = find(clip_parameters(p, "c1", 2.0), ClipParam::Opacity);
  ASSERT_TRUE(row.has_value());
  EXPECT_TRUE(row->animated);
  EXPECT_TRUE(row->keyed_here);
  EXPECT_DOUBLE_EQ(row->value, 60.0) << "pressing the stopwatch must not change the picture";
}

TEST(Inspector, AnAnimatedRowReadsItsValueAtTheGivenTime) {
  Project p = set_clip_parameter(sample_project(), "c1", ClipParam::Opacity, 0.0);
  p = set_clip_parameter_animated(std::move(p), "c1", ClipParam::Opacity, true, 0.0);
  p = set_clip_parameter(std::move(p), "c1", ClipParam::Opacity, 100.0, 4.0);

  EXPECT_DOUBLE_EQ(find(clip_parameters(p, "c1", 0.0), ClipParam::Opacity)->value, 0.0);
  EXPECT_DOUBLE_EQ(find(clip_parameters(p, "c1", 2.0), ClipParam::Opacity)->value, 50.0);
  EXPECT_DOUBLE_EQ(find(clip_parameters(p, "c1", 4.0), ClipParam::Opacity)->value, 100.0);
}

TEST(Inspector, SettingAnAnimatedParameterWritesAKeyframeRatherThanTheStoredValue) {
  Project p = set_clip_parameter_animated(sample_project(), "c1", ClipParam::X, true, 0.0);
  // Three quarters across a 1920-wide canvas, in the pixels the row is in.
  p = set_clip_parameter(std::move(p), "c1", ClipParam::X, 1440.0, 3.0);

  const Clip& clip = p.tracks.front().clips.front();
  const auto& frames = clip.keyframes[core::anim_prop_index(core::AnimProp::X)];
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_DOUBLE_EQ(frames[1].t, 3.0);
  // Stored as a fraction of the canvas, shown in pixels of it.
  EXPECT_DOUBLE_EQ(frames[1].v, 0.75);
}

// ------------------------------------------------------------- position --
//
// The one display unit whose factor is the project rather than a constant.
// Premiere shows Position in pixels, which is what anybody reads and types; the
// model keeps a fraction, which is what makes a layout independent of the
// export resolution.

TEST(Position, IsShownInPixelsOfTheCanvas) {
  const Project p = sample_project();
  const std::vector<ParamSpec> specs = clip_parameters(p, "c1");

  // A centred clip on a 1920x1080 sequence.
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::X)->value, 960.0);
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::Y)->value, 540.0);
  EXPECT_TRUE(find(specs, ClipParam::X)->suffix.empty()) << "pixels carry no unit here";
}

TEST(Position, TheDefaultIsTheMiddleOfTheCanvas) {
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "c1");
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::X)->fallback, 960.0);
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::Y)->fallback, 540.0);
}

TEST(Position, ReachesHalfACanvasEitherSideOfTheFrame) {
  // So a layer can be parked well off screen and brought back.
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "c1");
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::X)->range.minimum, -960.0);
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::X)->range.maximum, 2880.0);
}

TEST(Position, FollowsTheSequenceSizeWithoutMovingTheClip) {
  // Changing the canvas changes what the rows read out and nothing about where
  // the picture is: the model stores a fraction.
  Project p = sample_project();
  p = set_clip_parameter(std::move(p), "c1", ClipParam::X, 480.0);
  const double stored = p.tracks.front().clips.front().transform.x;

  p.canvas_w = 3840;
  EXPECT_DOUBLE_EQ(p.tracks.front().clips.front().transform.x, stored);
  EXPECT_DOUBLE_EQ(find(clip_parameters(p, "c1"), ClipParam::X)->value, 960.0);
}

TEST(Position, AnAnimatedRowReadsItsKeyframeInPixelsToo) {
  // Read through the constant scale instead, an animated Position showed a
  // number a hundred times too small.
  Project p = set_clip_parameter_animated(sample_project(), "c1", ClipParam::X, true, 0.0);
  p = set_clip_parameter(std::move(p), "c1", ClipParam::X, 1440.0, 0.0);

  EXPECT_DOUBLE_EQ(find(clip_parameters(p, "c1", 0.0), ClipParam::X)->value, 1440.0);
}

TEST(Position, SurvivesTheStopwatchBeingTurnedOffAgain) {
  // Switching animation off reads the value out and writes it back as the
  // static one, and the two halves have to be in the same units. Read in
  // pixels and written as though it were a fraction, a layer three quarters
  // across the frame jumped to the very edge of it.
  Project p = set_clip_parameter_animated(sample_project(), "c1", ClipParam::X, true, 0.0);
  p = set_clip_parameter(std::move(p), "c1", ClipParam::X, 1440.0, 0.0);
  p = set_clip_parameter_animated(std::move(p), "c1", ClipParam::X, false, 0.0);

  EXPECT_DOUBLE_EQ(p.tracks.front().clips.front().transform.x, 0.75);
  EXPECT_DOUBLE_EQ(find(clip_parameters(p, "c1"), ClipParam::X)->value, 1440.0);
}

TEST(Position, ACanvasOfNothingIsNotADivisionByZero) {
  Project p = sample_project();
  p.canvas_w = 0;
  p.canvas_h = 0;
  const std::vector<ParamSpec> specs = clip_parameters(p, "c1");
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::X)->value, 0.5) << "the stored fraction, shown raw";

  const Project written = set_clip_parameter(p, "c1", ClipParam::X, 0.25);
  EXPECT_DOUBLE_EQ(written.tracks.front().clips.front().transform.x, 0.25);
}

// ---------------------------------------------------------- anchor point --
//
// In pixels of the **layer**, not of the canvas: the anchor belongs to the
// picture being placed rather than to the sequence it is placed in, which is
// what makes "0, 0 is its top left corner" mean anything.

TEST(AnchorPoint, DefaultsToTheMiddleOfTheLayer) {
  Project p = sample_project();
  p.media.front().width = 1280;
  p.media.front().height = 720;

  // 1280x720 fitted to a 1920x1080 canvas is the whole frame, so the middle of
  // the layer is 960, 540 here as well — and it will not be for a layer that
  // does not fill the frame, which is the next test.
  const std::vector<ParamSpec> specs = clip_parameters(p, "c1");
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::AnchorX)->value, 960.0);
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::AnchorY)->value, 540.0);
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::AnchorX)->fallback, 960.0);
}

TEST(AnchorPoint, IsMeasuredInPixelsOfTheLayerRatherThanTheCanvas) {
  Project p = sample_project();
  // A square source letterboxes into 1080x1080 on a 1920x1080 canvas.
  p.media.front().width = 500;
  p.media.front().height = 500;

  const std::vector<ParamSpec> specs = clip_parameters(p, "c1");
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::AnchorX)->value, 540.0);
  EXPECT_DOUBLE_EQ(find(specs, ClipParam::AnchorY)->value, 540.0);
}

TEST(AnchorPoint, WritesTheFractionTheModelStores) {
  Project p = sample_project();
  p.media.front().width = 1920;
  p.media.front().height = 1080;

  // The layer's top left corner.
  p = set_clip_parameter(std::move(p), "c1", ClipParam::AnchorX, 0.0);
  p = set_clip_parameter(std::move(p), "c1", ClipParam::AnchorY, 0.0);
  EXPECT_DOUBLE_EQ(p.tracks.front().clips.front().transform.anchor_x, 0.0);

  p = set_clip_parameter(std::move(p), "c1", ClipParam::AnchorX, 1920.0);
  EXPECT_DOUBLE_EQ(p.tracks.front().clips.front().transform.anchor_x, 1.0);
}

TEST(AnchorPoint, CanBeAnimated) {
  Project p = sample_project();
  p.media.front().width = 1920;
  p.media.front().height = 1080;
  p = set_clip_parameter_animated(std::move(p), "c1", ClipParam::AnchorX, true, 0.0);
  p = set_clip_parameter(std::move(p), "c1", ClipParam::AnchorX, 0.0, 4.0);

  const std::optional<ParamSpec> row = find(clip_parameters(p, "c1", 2.0), ClipParam::AnchorX);
  ASSERT_TRUE(row.has_value());
  EXPECT_TRUE(row->animated);
  EXPECT_DOUBLE_EQ(row->value, 480.0) << "halfway from the middle to the left edge";
}

TEST(AnchorPoint, ComesAfterRotation) {
  // Premiere's order, and the order that reads best: the one you reach for
  // least often, and the one that changes what the others mean.
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "c1");
  const auto index = [&specs](ClipParam param) {
    return std::ranges::find(specs, param, &ParamSpec::param) - specs.begin();
  };
  EXPECT_GT(index(ClipParam::AnchorX), index(ClipParam::Rotation));
  EXPECT_LT(index(ClipParam::AnchorX), index(ClipParam::AnchorY));
}

TEST(AnchorPoint, IsNotOfferedForAudio) {
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "a1c");
  EXPECT_FALSE(has(specs, ClipParam::AnchorX));
  EXPECT_FALSE(has(specs, ClipParam::AnchorY));
}

// -------------------------------------------------------------- balance --
//
// Shown -100 to 100 because that is what a panner reads, stored -1 to 1.

TEST(Balance, IsOfferedOnAudioAndNotOnPicture) {
  const Project p = sample_project();
  EXPECT_TRUE(has(clip_parameters(p, "a1c"), ClipParam::Pan));
  EXPECT_FALSE(has(clip_parameters(p, "c1"), ClipParam::Pan));
}

TEST(Balance, StartsCentredAndReadsInHundredths) {
  const std::optional<ParamSpec> row =
      find(clip_parameters(sample_project(), "a1c"), ClipParam::Pan);
  ASSERT_TRUE(row.has_value());
  EXPECT_DOUBLE_EQ(row->value, 0.0);
  EXPECT_DOUBLE_EQ(row->fallback, 0.0);
  EXPECT_DOUBLE_EQ(row->range.minimum, -100.0);
  EXPECT_DOUBLE_EQ(row->range.maximum, 100.0);
}

TEST(Balance, WritesTheFractionTheModelStores) {
  Project p = set_clip_parameter(sample_project(), "a1c", ClipParam::Pan, -50.0);
  EXPECT_DOUBLE_EQ(p.tracks[1].clips.front().pan, -0.5);
  EXPECT_DOUBLE_EQ(find(clip_parameters(p, "a1c"), ClipParam::Pan)->value, -50.0);
}

TEST(Balance, CannotBeDraggedPastEitherEnd) {
  const Project p = set_clip_parameter(sample_project(), "a1c", ClipParam::Pan, 400.0);
  EXPECT_DOUBLE_EQ(p.tracks[1].clips.front().pan, 1.0);
}

TEST(Balance, CanBeAnimated) {
  Project p = set_clip_parameter_animated(sample_project(), "a1c", ClipParam::Pan, true, 0.0);
  p = set_clip_parameter(std::move(p), "a1c", ClipParam::Pan, 100.0, 4.0);

  const std::optional<ParamSpec> row = find(clip_parameters(p, "a1c", 2.0), ClipParam::Pan);
  ASSERT_TRUE(row.has_value());
  EXPECT_TRUE(row->animated);
  EXPECT_DOUBLE_EQ(row->value, 50.0) << "halfway across";
  // And the stored value is left alone, the way every animated property is.
  EXPECT_DOUBLE_EQ(p.tracks[1].clips.front().pan, 0.0);
}

TEST(Balance, ComesAfterVolume) {
  const std::vector<ParamSpec> specs = clip_parameters(sample_project(), "a1c");
  const auto index = [&specs](ClipParam param) {
    return std::ranges::find(specs, param, &ParamSpec::param) - specs.begin();
  };
  EXPECT_GT(index(ClipParam::Pan), index(ClipParam::Gain));
}

TEST(Inspector, TurningTheStopwatchOffKeepsTheValueAtThatTime) {
  Project p = set_clip_parameter(sample_project(), "c1", ClipParam::Opacity, 0.0);
  p = set_clip_parameter_animated(std::move(p), "c1", ClipParam::Opacity, true, 0.0);
  p = set_clip_parameter(std::move(p), "c1", ClipParam::Opacity, 100.0, 4.0);
  p = set_clip_parameter_animated(std::move(p), "c1", ClipParam::Opacity, false, 1.0);

  const Clip& clip = p.tracks.front().clips.front();
  EXPECT_TRUE(clip.keyframes[core::anim_prop_index(core::AnimProp::Opacity)].empty());
  EXPECT_DOUBLE_EQ(clip.opacity, 0.25) << "a quarter of the way along the ramp";
}

TEST(Inspector, AKeyframeCanBeAddedAndTakenAwayWithoutChangingTheAnimation) {
  Project p = set_clip_parameter(sample_project(), "c1", ClipParam::Opacity, 0.0);
  p = set_clip_parameter_animated(std::move(p), "c1", ClipParam::Opacity, true, 0.0);
  p = set_clip_parameter(std::move(p), "c1", ClipParam::Opacity, 100.0, 4.0);

  const Project added = toggle_clip_parameter_keyframe(p, "c1", ClipParam::Opacity, 2.0);
  ASSERT_EQ(added.tracks.front().clips.front()
                .keyframes[core::anim_prop_index(core::AnimProp::Opacity)]
                .size(),
            3u);
  EXPECT_DOUBLE_EQ(find(clip_parameters(added, "c1", 2.0), ClipParam::Opacity)->value, 50.0)
      << "adding a keyframe on the ramp must not bend it";

  const Project removed = toggle_clip_parameter_keyframe(added, "c1", ClipParam::Opacity, 2.0);
  EXPECT_EQ(removed, p);
}

TEST(Inspector, AKeyframeMarkerDoesNothingUntilTheStopwatchIsOn) {
  const Project before = sample_project();
  EXPECT_EQ(toggle_clip_parameter_keyframe(before, "c1", ClipParam::Opacity, 1.0), before);
}

TEST(Inspector, AParameterThatCannotBeAnimatedRefusesToBe) {
  const Project before = sample_project();
  EXPECT_EQ(set_clip_parameter_animated(before, "c1", ClipParam::Speed, true, 0.0), before);
  EXPECT_EQ(set_clip_parameter_animated(before, "c1", ClipParam::FadeIn, true, 0.0), before);
}

TEST(Inspector, EveryParameterHasAName) {
  for (const ClipParam param :
       {ClipParam::Opacity, ClipParam::X, ClipParam::Y, ClipParam::ScaleX, ClipParam::ScaleY,
        ClipParam::Rotation, ClipParam::Speed, ClipParam::Gain, ClipParam::FadeIn,
        ClipParam::FadeOut}) {
    EXPECT_NE(to_string(param), "unknown");
  }
}

// ---------------------------------------------------------- interpolation --

TEST(Interp, EveryModeIsNamed) {
  EXPECT_EQ(interp_name(core::Interp::Linear), "Linear");
  EXPECT_EQ(interp_name(core::Interp::Hold), "Hold");
  EXPECT_EQ(interp_name(core::Interp::Ease), "Ease");
}

TEST(Interp, CyclingWalksAllThreeAndComesBack) {
  // What a chip that cycles needs, and the whole of that claim: three is short
  // enough to walk round rather than pick from.
  core::Interp mode = core::Interp::Linear;
  mode = next_interp(mode);
  EXPECT_EQ(mode, core::Interp::Hold);
  mode = next_interp(mode);
  EXPECT_EQ(mode, core::Interp::Ease);
  mode = next_interp(mode);
  EXPECT_EQ(mode, core::Interp::Linear);
}

TEST(Interp, AnAnimatedRowReportsItsCurve) {
  Project p = set_clip_parameter_animated(sample_project(), "c1", ClipParam::Opacity, true, 0.0);
  p = set_clip_parameter_interp(std::move(p), "c1", ClipParam::Opacity, core::Interp::Ease);

  const std::vector<ParamSpec> rows = clip_parameters(p, "c1");
  const auto opacity = std::ranges::find(rows, ClipParam::Opacity, &ParamSpec::param);
  ASSERT_NE(opacity, rows.end());
  EXPECT_TRUE(opacity->animated);
  EXPECT_EQ(opacity->interp, core::Interp::Ease);
}

TEST(Interp, SettingItOnSomethingNotAnimatedDoesNothing) {
  // There are no keyframes to set it on, and storing it anyway would be a
  // setting silently discarded the moment the stopwatch was pressed.
  const Project before = sample_project();
  EXPECT_EQ(set_clip_parameter_interp(before, "c1", ClipParam::Opacity, core::Interp::Hold),
            before);
  EXPECT_EQ(set_clip_parameter_interp(before, "c1", ClipParam::Speed, core::Interp::Hold),
            before)
      << "and speed cannot animate at all";
}

TEST(Interp, GainCarriesItsOwn) {
  Project p = set_clip_parameter_animated(sample_project(), "a1c", ClipParam::Gain, true, 0.0);
  p = set_clip_parameter_interp(std::move(p), "a1c", ClipParam::Gain, core::Interp::Hold);

  const std::vector<ParamSpec> rows = clip_parameters(p, "a1c");
  const auto gain = std::ranges::find(rows, ClipParam::Gain, &ParamSpec::param);
  ASSERT_NE(gain, rows.end());
  EXPECT_EQ(gain->interp, core::Interp::Hold);
}

}  // namespace
}  // namespace cutline::editor
