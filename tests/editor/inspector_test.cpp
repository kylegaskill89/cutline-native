/// The inspector's description of a clip.
///
/// The thing most worth pinning down is the unit conversion. A slider reads
/// 100% and the clip stores 1.0, and if those two ever disagree the symptom is
/// a control that jumps the moment it is touched.

#include "cutline/editor/inspector.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"

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

TEST(Inspector, VolumeRoundTripsToo) {
  Project project = sample_project();
  project = set_clip_parameter(std::move(project), "a1c", ClipParam::Gain, 150.0);

  EXPECT_DOUBLE_EQ(core::find_clip(project, "a1c")->gain, 1.5);
  const std::optional<ParamSpec> spec = find(clip_parameters(project, "a1c"), ClipParam::Gain);
  ASSERT_TRUE(spec.has_value());
  EXPECT_DOUBLE_EQ(spec->value, 150.0);
}

TEST(Inspector, TheVolumeRangeMatchesWhatTheModelAllows) {
  const std::optional<ParamSpec> spec = find(clip_parameters(sample_project(), "a1c"), ClipParam::Gain);
  ASSERT_TRUE(spec.has_value());
  EXPECT_DOUBLE_EQ(spec->range.maximum, core::kMaxGain * 100.0);
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

TEST(Inspector, EveryParameterHasAName) {
  for (const ClipParam param :
       {ClipParam::Opacity, ClipParam::X, ClipParam::Y, ClipParam::ScaleX, ClipParam::ScaleY,
        ClipParam::Rotation, ClipParam::Speed, ClipParam::Gain, ClipParam::FadeIn,
        ClipParam::FadeOut}) {
    EXPECT_NE(to_string(param), "unknown");
  }
}

}  // namespace
}  // namespace cutline::editor
