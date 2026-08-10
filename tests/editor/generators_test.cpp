/// Colour mattes and adjustment layers.
///
/// The same shape as titles: media the editor creates rather than imports,
/// with a length that has to be chosen because there is no source to take one
/// from. What is worth checking is that both actually reach the timeline —
/// `place_media` refuses anything that does not claim to contribute picture,
/// and an adjustment layer contributes a filter rather than a picture, which is
/// exactly the sort of thing that gets a flag set wrong.

#include "cutline/editor/generators.hpp"

#include "cutline/core/edit.hpp"
#include "cutline/core/query.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cutline::editor {
namespace {

using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

[[nodiscard]] Project empty_sequence() {
  Project project;
  project.sequence().fps = 30.0;
  project.sequence().tracks = {Track{.id = "v1", .kind = TrackKind::Video},
                    Track{.id = "a1", .kind = TrackKind::Audio}};
  return project;
}

[[nodiscard]] std::size_t clip_count(const Project& project) {
  std::size_t count = 0;
  for (const Track& track : project.sequence().tracks) count += track.clips.size();
  return count;
}

// ------------------------------------------------------------ the pool --

TEST(Matte, ArrivesInThePoolAsAColourMatte) {
  const Project p = add_color_matte(empty_sequence());
  ASSERT_EQ(p.media.size(), 1u);
  EXPECT_TRUE(p.media.front().is_color);
  EXPECT_TRUE(p.media.front().has_video) << "or it could never be placed";
  EXPECT_DOUBLE_EQ(p.media.front().duration, kDefaultGeneratorLength);
}

TEST(Matte, TakesTheModelsDefaultColourWhenNobodySaysOtherwise) {
  const Project p = add_color_matte(empty_sequence());
  EXPECT_EQ(p.media.front().color, core::kDefaultMatteColor);
  EXPECT_FALSE(p.media.front().gradient.has_value());
}

TEST(Matte, IsNamedForItsColour) {
  const Project p = add_color_matte(empty_sequence(), MatteFill{.color = "#ff0000"});
  EXPECT_NE(p.media.front().name.find("#ff0000"), std::string::npos);
}

TEST(Matte, TwoOfTheSameColourAreTwoMattes) {
  // No pool to deduplicate against, which is the whole reason this is not
  // `import.hpp`.
  Project p = add_color_matte(empty_sequence());
  p = add_color_matte(std::move(p));
  ASSERT_EQ(p.media.size(), 2u);
  EXPECT_NE(p.media[0].id, p.media[1].id);
}

TEST(Adjustment, ArrivesInThePoolAsOne) {
  const Project p = add_adjustment_layer(empty_sequence());
  ASSERT_EQ(p.media.size(), 1u);
  EXPECT_TRUE(p.media.front().is_adjustment);
  EXPECT_FALSE(p.media.front().is_color);
  EXPECT_TRUE(p.media.front().has_video)
      << "it reaches a video track like anything else, and draws a filter there";
}

// -------------------------------------------------------- the timeline --

TEST(Matte, PlacedLandsOnTheTimelineAndReportsItsClip) {
  std::string clip_id;
  const Project p = add_color_matte_at(empty_sequence(), MatteFill{}, 2.0, {}, &clip_id);

  ASSERT_FALSE(clip_id.empty());
  EXPECT_EQ(clip_count(p), 1u);

  const core::Clip* clip = core::find_clip(p, clip_id);
  ASSERT_NE(clip, nullptr);
  EXPECT_DOUBLE_EQ(clip->start, 2.0);
  EXPECT_DOUBLE_EQ(core::clip_duration(*clip), kDefaultGeneratorLength);
}

TEST(Adjustment, PlacedLandsOnTheTimelineToo) {
  // The one most likely to be quietly refused: it draws nothing of its own, and
  // a flag set honestly would have kept it off every video track.
  std::string clip_id;
  const Project p = add_adjustment_layer_at(empty_sequence(), 1.0, {}, &clip_id);

  ASSERT_FALSE(clip_id.empty());
  EXPECT_EQ(clip_count(p), 1u);
  EXPECT_TRUE(clip_is_adjustment(p, clip_id));
}

TEST(Generators, ReportTheClipTheyMadeRatherThanGuessingAtIt) {
  // Found by elimination against what was there. Guessing at the id
  // generator's next value would work until something else called it first.
  Project p = add_color_matte_at(empty_sequence(), MatteFill{}, 0.0);
  std::string clip_id;
  p = add_color_matte_at(std::move(p), MatteFill{}, 10.0, {}, &clip_id);

  ASSERT_FALSE(clip_id.empty());
  EXPECT_DOUBLE_EQ(core::find_clip(p, clip_id)->start, 10.0);
}

// ------------------------------------------------------------- the fill --

TEST(Matte, ReportsItsFill) {
  std::string clip_id;
  const Project p = add_color_matte_at(
      empty_sequence(),
      MatteFill{.color = "#204080",
                .gradient = core::MatteGradient{.color2 = "#000000", .angle = 90.0}},
      0.0, {}, &clip_id);

  const std::optional<MatteFill> fill = clip_matte_fill(p, clip_id);
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(fill->color, "#204080");
  ASSERT_TRUE(fill->gradient.has_value());
  EXPECT_EQ(fill->gradient->color2, "#000000");
  EXPECT_DOUBLE_EQ(fill->gradient->angle, 90.0);
}

TEST(Matte, IsNotAskedOfSomethingThatIsNotOne) {
  const Project p = add_adjustment_layer(empty_sequence());
  EXPECT_FALSE(matte_fill(p, p.media.front().id).has_value());
  EXPECT_FALSE(clip_matte_fill(p, "ghost").has_value());
}

TEST(Matte, SettingTheFillReplacesIt) {
  Project p = add_color_matte(empty_sequence());
  const std::string media_id = p.media.front().id;

  p = set_matte_fill(std::move(p), media_id, MatteFill{.color = "#00ff00"});
  EXPECT_EQ(p.media.front().color, "#00ff00");
  EXPECT_FALSE(p.media.front().gradient.has_value());
}

TEST(Matte, SettingTheFillItAlreadyHasChangesNothing) {
  const Project before = add_color_matte(empty_sequence());
  const std::string media_id = before.media.front().id;
  EXPECT_EQ(set_matte_fill(before, media_id, MatteFill{}), before);
}

TEST(Matte, TheNameFollowsTheColourUntilSomebodyRenamesIt) {
  Project p = add_color_matte(empty_sequence(), MatteFill{.color = "#ff0000"});
  const std::string media_id = p.media.front().id;

  p = set_matte_fill(std::move(p), media_id, MatteFill{.color = "#00ff00"});
  EXPECT_NE(p.media.front().name.find("#00ff00"), std::string::npos);

  p.media.front().name = "Background";
  p = set_matte_fill(std::move(p), media_id, MatteFill{.color = "#0000ff"});
  EXPECT_EQ(p.media.front().name, "Background") << "a name somebody chose is theirs";
}

TEST(Matte, TurningAGradientOnAndOffAgainLeavesASolid) {
  Project p = add_color_matte(empty_sequence(), MatteFill{.color = "#ff0000"});
  const std::string media_id = p.media.front().id;

  p = set_matte_fill(std::move(p), media_id,
                     MatteFill{.color = "#ff0000",
                               .gradient = core::MatteGradient{.color2 = "#000000"}});
  ASSERT_TRUE(p.media.front().gradient.has_value());

  p = set_matte_fill(std::move(p), media_id, MatteFill{.color = "#ff0000"});
  EXPECT_FALSE(p.media.front().gradient.has_value());
  EXPECT_EQ(p.media.front().color, "#ff0000");
}

TEST(Matte, SettingTheFillOfSomethingElseChangesNothing) {
  const Project before = add_adjustment_layer(empty_sequence());
  EXPECT_EQ(set_matte_fill(before, before.media.front().id, MatteFill{.color = "#ff0000"}),
            before);
  EXPECT_EQ(set_matte_fill(before, "nowhere", MatteFill{}), before);
}

TEST(Adjustment, IsOnlyTrueOfAnAdjustmentLayer) {
  std::string matte_clip;
  const Project p = add_color_matte_at(empty_sequence(), MatteFill{}, 0.0, {}, &matte_clip);
  EXPECT_FALSE(clip_is_adjustment(p, matte_clip));
  EXPECT_FALSE(clip_is_adjustment(p, "ghost"));
}

}  // namespace
}  // namespace cutline::editor
