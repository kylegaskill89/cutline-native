/// Export presets: the built-ins, what each means against a sequence, saving
/// and deleting the person's own, and the presets file.

#include "cutline/editor/export_presets.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>

namespace cutline::editor {
namespace {

using core::WorkingColour;

[[nodiscard]] core::Project project(int width, int height, double fps = 30.0,
                                    WorkingColour colour = WorkingColour::Rec709) {
  core::Project out;
  out.sequence().canvas_w = width;
  out.sequence().canvas_h = height;
  out.sequence().fps = fps;
  out.sequence().colour = colour;
  return out;
}

[[nodiscard]] const ExportPreset& built_in(std::string_view name) {
  for (const ExportPreset& preset : built_in_export_presets()) {
    if (preset.name == name) return preset;
  }
  static const ExportPreset none;
  ADD_FAILURE() << "no built-in called " << name;
  return none;
}

TEST(ExportPresets, EveryBuiltInHasItsOwnNameAndIsMarkedBuiltIn) {
  std::set<std::string> seen;
  for (const ExportPreset& preset : built_in_export_presets()) {
    EXPECT_TRUE(seen.insert(preset.name).second) << preset.name;
    EXPECT_TRUE(preset.built_in) << preset.name;
  }
  EXPECT_GE(seen.size(), 8u);
}

TEST(ExportPresets, EveryFormatHasANameThatReadsBack) {
  for (const ExportFormat format : export_formats()) {
    EXPECT_EQ(export_format_from_string(to_string(format)), format);
    EXPECT_FALSE(format_label(format).empty());
  }
  EXPECT_FALSE(export_format_from_string("vp9").has_value());
}

TEST(ExportPresets, MatchSourceIsTheSequencesOwnSizeAndRate) {
  const auto plan = plan_for(built_in("Match Source - High Quality"), project(3840, 2160, 59.94));
  ASSERT_TRUE(plan.has_value()) << plan.error();
  EXPECT_EQ(plan->width, 3840);
  EXPECT_EQ(plan->height, 2160);
  EXPECT_DOUBLE_EQ(plan->fps, 59.94);
  EXPECT_EQ(plan->bit_depth, 8);
  EXPECT_EQ(plan->extension, ".mp4");
}

TEST(ExportPresets, AResolutionPresetScalesTheShortSideAndKeepsTheShape) {
  const auto wide = plan_for(built_in("YouTube 1080p Full HD"), project(3840, 2160));
  ASSERT_TRUE(wide.has_value()) << wide.error();
  EXPECT_EQ(wide->width, 1920);
  EXPECT_EQ(wide->height, 1080);
}

TEST(ExportPresets, AVerticalSequenceAtTenEightyStaysVertical) {
  // 1080p of a phone's picture is 1080 wide, not a landscape frame.
  const auto plan = plan_for(built_in("YouTube 1080p Full HD"), project(2160, 3840));
  ASSERT_TRUE(plan.has_value()) << plan.error();
  EXPECT_EQ(plan->width, 1080);
  EXPECT_EQ(plan->height, 1920);
}

TEST(ExportPresets, AnOddAspectRatioIsRoundedToEvenDimensions) {
  // The encoders want even sizes for 4:2:0.
  const auto plan = plan_for(built_in("Social Media 720p"), project(2560, 1097));
  ASSERT_TRUE(plan.has_value()) << plan.error();
  EXPECT_EQ(plan->height % 2, 0);
  EXPECT_EQ(plan->width % 2, 0);
  EXPECT_NEAR(plan->height, 720, 1);
  EXPECT_NEAR(plan->width, 1680, 2);
}

TEST(ExportPresets, TheIntermediatesAreWrittenToAMovAtTheirOwnDepth) {
  const auto prores = plan_for(built_in("Apple ProRes 422 HQ"), project(1920, 1080));
  ASSERT_TRUE(prores.has_value()) << prores.error();
  EXPECT_EQ(prores->extension, ".mov");
  EXPECT_EQ(prores->bit_depth, 10);
  const auto dnxhr = plan_for(built_in("Avid DNxHR HQ"), project(1920, 1080));
  ASSERT_TRUE(dnxhr.has_value()) << dnxhr.error();
  EXPECT_EQ(dnxhr->extension, ".mov");
  EXPECT_EQ(dnxhr->bit_depth, 8);
}

TEST(ExportPresets, TheHdrPresetWritesPqWhateverTheSequenceIs) {
  const auto plan = plan_for(built_in("HDR10 - HEVC 10-bit"), project(3840, 2160));
  ASSERT_TRUE(plan.has_value()) << plan.error();
  EXPECT_EQ(plan->colour, WorkingColour::Rec2100Pq);
  EXPECT_EQ(plan->bit_depth, 10);
}

TEST(ExportPresets, AnHdrSequenceTakesItsColourIntoAMatchingPreset) {
  const auto plan = plan_for(built_in("Match Source - HEVC"),
                             project(3840, 2160, 30.0, WorkingColour::Rec2100Hlg));
  ASSERT_TRUE(plan.has_value()) << plan.error();
  EXPECT_EQ(plan->colour, WorkingColour::Rec2100Hlg);
  EXPECT_EQ(plan->bit_depth, 10) << "HDR is never written at eight bits";
}

TEST(ExportPresets, MatchingAnHdrSequenceInHTwoSixtyFourWritesRecSevenOhNine) {
  const auto plan = plan_for(built_in("Match Source - High Quality"),
                             project(3840, 2160, 30.0, WorkingColour::Rec2100Pq));
  ASSERT_TRUE(plan.has_value()) << plan.error();
  EXPECT_EQ(plan->colour, WorkingColour::Rec709);
  EXPECT_EQ(plan->bit_depth, 8);
}

TEST(ExportPresets, AskingForHdrInHTwoSixtyFourIsRefusedAsPremiereRefusesIt) {
  ExportPreset preset = built_in("Match Source - High Quality");
  preset.colour = WorkingColour::Rec2100Pq;
  const auto plan = plan_for(preset, project(3840, 2160));
  ASSERT_FALSE(plan.has_value());
  EXPECT_NE(plan.error().find("HDR"), std::string::npos) << plan.error();
}

TEST(ExportPresets, DnxhrRefusesAPictureSmallerThanItsMinimum) {
  EXPECT_FALSE(plan_for(built_in("Avid DNxHR HQ"), project(160, 120)).has_value());
}

TEST(ExportPresets, ChangingASettingMakesThePresetCustom) {
  ExportPreset settings = built_in("YouTube 1080p Full HD");
  EXPECT_EQ(matching_preset_name({}, settings), "YouTube 1080p Full HD");
  settings.short_side = 720;
  EXPECT_EQ(matching_preset_name({}, settings), "Custom");
}

TEST(ExportPresets, AnIntermediatesQualityIsNotASettingOfIt) {
  // There is no quality to choose for ProRes, so a left-over one from the
  // preset before cannot make it read as Custom.
  ExportPreset settings = built_in("Apple ProRes 422 HQ");
  settings.quality = 25;
  EXPECT_EQ(matching_preset_name({}, settings), "Apple ProRes 422 HQ");
}

TEST(ExportPresets, ASavedPresetFollowsTheBuiltIns) {
  SavedExportPresets saved;
  ASSERT_TRUE(save_export_preset(saved, {.name = "Client review", .short_side = 540}).has_value());
  const auto all = all_export_presets(saved);
  ASSERT_EQ(all.size(), built_in_export_presets().size() + 1);
  EXPECT_EQ(all.back().name, "Client review");
  EXPECT_FALSE(all.back().built_in);
}

TEST(ExportPresets, SavingUnderASavedNameReplacesIt) {
  SavedExportPresets saved;
  ASSERT_TRUE(save_export_preset(saved, {.name = "Mine", .quality = 18}).has_value());
  ASSERT_TRUE(save_export_preset(saved, {.name = "Mine", .quality = 24}).has_value());
  ASSERT_EQ(saved.presets.size(), 1u);
  EXPECT_EQ(saved.presets.front().quality, 24);
}

TEST(ExportPresets, ABuiltInsNameCannotBeSavedOver) {
  SavedExportPresets saved;
  EXPECT_FALSE(save_export_preset(saved, {.name = "Avid DNxHR HQ"}).has_value());
  EXPECT_FALSE(save_export_preset(saved, {.name = ""}).has_value());
  EXPECT_TRUE(saved.presets.empty());
}

TEST(ExportPresets, DeletingRemovesOnlyASavedPreset) {
  SavedExportPresets saved;
  ASSERT_TRUE(save_export_preset(saved, {.name = "Mine"}).has_value());
  EXPECT_FALSE(delete_export_preset(saved, "Avid DNxHR HQ"));
  EXPECT_TRUE(delete_export_preset(saved, "Mine"));
  EXPECT_TRUE(saved.presets.empty());
}

TEST(ExportPresets, SavedPresetsSurviveARoundTripThroughJson) {
  SavedExportPresets saved;
  ASSERT_TRUE(save_export_preset(saved, {.name = "Grade",
                                         .format = ExportFormat::ProRes,
                                         .fps = 25.0,
                                         .colour = WorkingColour::Rec2100Hlg,
                                         .channels = 1,
                                         .separate_audio = true})
                  .has_value());
  ASSERT_TRUE(save_export_preset(saved, {.name = "Small", .quality = 26, .short_side = 360})
                  .has_value());
  const auto back = saved_export_presets_from_json(to_json(saved));
  ASSERT_TRUE(back.has_value()) << back.error();
  EXPECT_EQ(*back, saved);
}

TEST(ExportPresets, APresetInAFormatThisBuildLacksIsPassedOver) {
  const auto back = saved_export_presets_from_json(
      R"({"version":1,"presets":[{"name":"Future","format":"av2"},)"
      R"({"name":"Kept","format":"hevc10","quality":99}]})");
  ASSERT_TRUE(back.has_value()) << back.error();
  ASSERT_EQ(back->presets.size(), 1u);
  EXPECT_EQ(back->presets.front().name, "Kept");
  EXPECT_EQ(back->presets.front().quality, 51) << "clamped to the encoder's range";
}

TEST(ExportPresets, ANewerOrBrokenFileIsRefused) {
  EXPECT_FALSE(saved_export_presets_from_json("{").has_value());
  EXPECT_FALSE(saved_export_presets_from_json(R"({"version":99})").has_value());
}

TEST(ExportPresets, AMissingFileIsNoSavedPresets) {
  const auto saved =
      read_export_presets(std::filesystem::temp_directory_path() / "cutline-no-presets.json");
  ASSERT_TRUE(saved.has_value()) << saved.error();
  EXPECT_TRUE(saved->presets.empty());
}

TEST(ExportPresets, TheFileHoldsWhatWasWritten) {
  const auto path = std::filesystem::temp_directory_path() / "cutline-presets-test.json";
  SavedExportPresets saved;
  ASSERT_TRUE(save_export_preset(saved, {.name = "Written", .format = ExportFormat::Hevc})
                  .has_value());
  ASSERT_TRUE(write_export_presets(path, saved).has_value());
  const auto back = read_export_presets(path);
  std::filesystem::remove(path);
  ASSERT_TRUE(back.has_value()) << back.error();
  EXPECT_EQ(*back, saved);
}

}  // namespace
}  // namespace cutline::editor
