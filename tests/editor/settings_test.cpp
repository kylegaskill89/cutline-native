/// The preferences file, and what it has to survive.
///
/// A settings file is read at startup on a machine nobody controls: it may be
/// absent, older, newer, truncated, or edited by hand. Every one of those has
/// to end with a running application, and the only one that may end with a
/// complaint is the file from the future — because reading that one wrongly is
/// worse than not reading it.

#include "cutline/editor/settings.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace cutline::editor {
namespace {

/// A scratch path that cleans itself up.
class TempSettings {
 public:
  TempSettings()
      : path_(std::filesystem::temp_directory_path() /
              ("cutline_settings_" + std::to_string(++counter_) + ".json")) {}

  ~TempSettings() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(std::filesystem::path(path_) += ".saving", ignored);
  }

  TempSettings(const TempSettings&) = delete;
  TempSettings& operator=(const TempSettings&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  void write(std::string_view text) const {
    std::ofstream file(path_, std::ios::binary | std::ios::trunc);
    file << text;
  }

 private:
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

/// Settings with every field away from its default, so a round trip that
/// dropped one is caught rather than passing on the defaults matching.
[[nodiscard]] Settings all_changed() {
  Settings settings;
  settings.theme = "Terminal";
  settings.snapping = false;
  settings.looping = true;
  settings.aspect_locked = true;
  settings.preview_scale = 0.5;
  settings.pool_sort = BrowserSort::Duration;
  settings.pool_descending = true;
  settings.pool_view = ui::BrowserView::Icons;
  settings.still_length = 3.0;
  settings.transition_length = 0.5;
  settings.autosave_seconds = 120;
  settings.label_names = {"Interview", "", "", "", "", "", "", "B-roll"};
  // Sized to the build rather than written out, because the reader pads to
  // however many kinds there are — so a list of six here and seven there is a
  // round trip that fails for a reason that has nothing to do with settings.
  settings.label_defaults.assign(ui::kMediaKindCount, "");
  settings.label_defaults.front() = "#8f7bb8";
  settings.label_defaults[static_cast<std::size_t>(ui::MediaKind::Adjustment)] = "#c07a92";
  settings.proxy_height = 720;
  settings.proxy_folder = "E:/Fast/Proxies";
  settings.autosave_versions = 12;
  settings.undo_depth = 250;
  settings.preroll = 1.5;
  settings.postroll = 2.5;
  settings.software_renderer = true;
  return settings;
}

// ------------------------------------------------------------ the defaults --

TEST(Settings, TheDefaultsAreWhatTheApplicationAlreadyDid) {
  // A fresh install must behave exactly as it did before this file existed, or
  // adding preferences would be a behaviour change dressed as a feature.
  const Settings settings;
  EXPECT_TRUE(settings.theme.empty()) << "no theme chosen means the first one";
  EXPECT_TRUE(settings.snapping);
  EXPECT_FALSE(settings.looping);
  EXPECT_FALSE(settings.aspect_locked);
  EXPECT_DOUBLE_EQ(settings.preview_scale, 1.0);
  EXPECT_EQ(settings.pool_sort, BrowserSort::Pool);
  EXPECT_FALSE(settings.pool_descending);
  EXPECT_EQ(settings.pool_view, ui::BrowserView::List);
  EXPECT_DOUBLE_EQ(settings.still_length, kStillLength);
  EXPECT_DOUBLE_EQ(settings.transition_length, kPreferredTransitionLength);
  EXPECT_EQ(settings.autosave_seconds, static_cast<int>(kAutosaveInterval.count()));
  EXPECT_EQ(settings.proxy_height, kDefaultProxyHeight);
  EXPECT_TRUE(settings.proxy_folder.empty()) << "beside the footage is the default";
  EXPECT_EQ(settings.autosave_versions, kAutosaveVersions);
  EXPECT_EQ(settings.undo_depth, static_cast<int>(core::kDefaultUndoDepth));
  EXPECT_DOUBLE_EQ(settings.preroll, 0.0) << "no run-up is what looping did before";
  EXPECT_DOUBLE_EQ(settings.postroll, 0.0);
  EXPECT_FALSE(settings.software_renderer) << "the GPU is the whole point of this application";
}

// ------------------------------------------------------------------ labels --

TEST(Settings, ALabelKeepsItsBuiltInNameUntilSomebodyChangesIt) {
  const Settings settings;
  EXPECT_EQ(label_name(settings, 0), clip_labels()[0].name);
  EXPECT_EQ(label_name(settings, 7), clip_labels()[7].name);
  EXPECT_TRUE(label_name(settings, 99).empty()) << "there is no ninth label to name";
}

TEST(Settings, ARenamedLabelIsWhatTheMenusSay) {
  // The whole point of the feature: a label is something people say out loud,
  // and renaming Violet to Interview is what makes the menu say Interview.
  Settings settings;
  settings.label_names.resize(clip_labels().size());
  settings.label_names[0] = "Interview";

  EXPECT_EQ(label_name(settings, 0), "Interview");
  EXPECT_EQ(label_name(settings, 1), clip_labels()[1].name) << "the others were not touched";
}

TEST(Settings, AnEmptyNameFallsBackRatherThanBlanking) {
  // A label with no name at all is a row of the menu nobody can ask for.
  Settings settings;
  settings.label_names.resize(clip_labels().size());
  settings.label_names[2] = "";
  EXPECT_EQ(label_name(settings, 2), clip_labels()[2].name);
}

TEST(Settings, NoKindIsLabelledByDefault) {
  const Settings settings;
  for (std::size_t kind = 0; kind < ui::kMediaKindCount; ++kind) {
    EXPECT_TRUE(label_default(settings, static_cast<ui::MediaKind>(kind)).empty());
  }
}

TEST(Settings, ADefaultIsFoundByKind) {
  Settings settings;
  settings.label_defaults.resize(ui::kMediaKindCount);
  settings.label_defaults[static_cast<std::size_t>(ui::MediaKind::Image)] = "#c07a92";

  EXPECT_EQ(label_default(settings, ui::MediaKind::Image), "#c07a92");
  EXPECT_TRUE(label_default(settings, ui::MediaKind::Video).empty());
}

TEST(Settings, LabelListsFromAFileAreSizedToThisBuild) {
  // A file from a version with more labels would otherwise reach past the
  // palette, and one from a version with fewer would leave the last name
  // unreadable. Both are read as far as they go and no further.
  const auto short_file = settings_from_json(R"({"label_names":["Interview"]})");
  ASSERT_TRUE(short_file.has_value()) << short_file.error();
  EXPECT_EQ(short_file->label_names.size(), clip_labels().size());
  EXPECT_EQ(label_name(*short_file, 0), "Interview");
  EXPECT_EQ(label_name(*short_file, 3), clip_labels()[3].name);

  const auto long_file = settings_from_json(
      R"({"label_defaults":["a","b","c","d","e","f","g","h","i","j","k","l"]})");
  ASSERT_TRUE(long_file.has_value()) << long_file.error();
  EXPECT_EQ(long_file->label_defaults.size(), ui::kMediaKindCount);
}

TEST(Settings, LabelsAreOnlyWrittenWhenSomebodyHasChangedOne) {
  // A fresh file saying "every label is called what it is already called" is
  // eight lines of nothing.
  EXPECT_EQ(to_json(Settings{}).find("label_names"), std::string::npos);
  EXPECT_EQ(to_json(Settings{}).find("label_defaults"), std::string::npos);

  Settings renamed;
  renamed.label_names.resize(clip_labels().size());
  renamed.label_names[0] = "Interview";
  EXPECT_NE(to_json(renamed).find("label_names"), std::string::npos);
}

TEST(Settings, AFileThatIsNotThereIsNotAnError) {
  // It means nobody has changed anything yet, which is the ordinary case on
  // every machine exactly once.
  const auto loaded = read_settings("Z:/definitely/not/here/settings.json");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(*loaded, Settings{});
}

// -------------------------------------------------------------- round trip --

TEST(Settings, EverythingSurvivesTheFile) {
  const TempSettings file;
  const Settings before = all_changed();

  const auto wrote = write_settings(file.path(), before);
  ASSERT_TRUE(wrote.has_value()) << wrote.error();

  const auto loaded = read_settings(file.path());
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(*loaded, before);
}

TEST(Settings, EverySortSurvivesByNameRatherThanByPosition) {
  // Written as names, so inserting an ordering does not silently reinterpret
  // every file written before it.
  for (const BrowserSort sort : {BrowserSort::Pool, BrowserSort::Name, BrowserSort::Kind,
                                 BrowserSort::Duration, BrowserSort::Uses}) {
    Settings settings;
    settings.pool_sort = sort;
    const auto loaded = settings_from_json(to_json(settings));
    ASSERT_TRUE(loaded.has_value()) << loaded.error();
    EXPECT_EQ(loaded->pool_sort, sort);
  }
}

TEST(Settings, TheThemeIsKeptByNameRatherThanByIndex) {
  // An index would quietly mean a different theme the first time one was
  // inserted into the list, and the list is ordered for reading.
  const auto loaded = settings_from_json(R"({"theme":"Aero"})");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->theme, "Aero");
}

// ------------------------------------------------------------- bad input --

TEST(Settings, AMissingKeyTakesItsDefaultRatherThanBeingTurnedOff) {
  // The trap this avoids: reading absent booleans as false would silently
  // switch snapping off for anybody whose file predates it.
  const auto loaded = settings_from_json(R"({"version":1,"looping":true})");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_TRUE(loaded->looping);
  EXPECT_TRUE(loaded->snapping) << "a setting nobody mentioned was turned off";
  EXPECT_DOUBLE_EQ(loaded->preview_scale, 1.0);
}

TEST(Settings, AnUnrecognisedKeyIsIgnored) {
  // A file written by a newer build should cost an older one its new settings,
  // not its ability to start.
  const auto loaded = settings_from_json(R"({"version":1,"snapping":false,"nonsense":42})");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_FALSE(loaded->snapping);
}

TEST(Settings, AFileFromTheFutureIsRefusedRatherThanHalfRead) {
  const auto loaded = settings_from_json(R"({"version":9999})");
  ASSERT_FALSE(loaded.has_value());
  EXPECT_FALSE(loaded.error().empty());
}

TEST(Settings, RubbishIsRefusedWithSomethingToRead) {
  EXPECT_FALSE(settings_from_json("{not json").has_value());
  EXPECT_FALSE(settings_from_json("[1,2,3]").has_value()) << "an array is not settings";
  EXPECT_FALSE(settings_from_json("").has_value());
}

TEST(Settings, APreviewScaleIsClampedRatherThanTrusted) {
  // Hand-edited to zero, this would divide the canvas to nothing and render a
  // frame with no pixels in it.
  const auto none = settings_from_json(R"({"preview_scale":0.0})");
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_GE(none->preview_scale, kMinPreviewScale);

  const auto huge = settings_from_json(R"({"preview_scale":8.0})");
  ASSERT_TRUE(huge.has_value()) << huge.error();
  EXPECT_DOUBLE_EQ(huge->preview_scale, kMaxPreviewScale);
}

TEST(Settings, TheDurationsAreClampedRatherThanTrusted) {
  // Zero is the one answer that has to be refused: a still of no length is the
  // bug this whole feature was born from, and a hand-edited file is exactly how
  // it would come back.
  const auto none = settings_from_json(
      R"({"still_length":0.0,"transition_length":0.0,"autosave_seconds":0})");
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_GE(none->still_length, kMinStillLength);
  EXPECT_GE(none->transition_length, kMinTransitionLength);
  EXPECT_GE(none->autosave_seconds, kMinAutosaveSeconds);

  const auto silly = settings_from_json(
      R"({"still_length":1e9,"transition_length":1e9,"autosave_seconds":999999})");
  ASSERT_TRUE(silly.has_value()) << silly.error();
  EXPECT_DOUBLE_EQ(silly->still_length, kMaxStillLength);
  EXPECT_DOUBLE_EQ(silly->transition_length, kMaxTransitionLength);
  EXPECT_EQ(silly->autosave_seconds, kMaxAutosaveSeconds);
}

TEST(Settings, ANegativeDurationCannotSurvive) {
  // Reachable by hand, and a negative still would place a clip that ends before
  // it starts.
  const auto loaded = settings_from_json(R"({"still_length":-5.0})");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_GT(loaded->still_length, 0.0);
}

TEST(Settings, TheUndoDepthIsClampedRatherThanTrusted) {
  // Zero would leave an editor with no undo at all, reached by a typing mistake
  // in a file rather than by any control.
  const auto none = settings_from_json(R"({"undo_depth":0})");
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_GE(none->undo_depth, kMinUndoDepth);

  const auto silly = settings_from_json(R"({"undo_depth":1000000})");
  ASSERT_TRUE(silly.has_value()) << silly.error();
  EXPECT_EQ(silly->undo_depth, kMaxUndoDepth);
}

TEST(Settings, TheNumberOfCopiesKeptIsClampedRatherThanTrusted) {
  // None would mean writing a recovery copy and deleting it, which is the one
  // answer that turns the feature off while looking as though it is on.
  const auto none = settings_from_json(R"({"autosave_versions":0})");
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_GE(none->autosave_versions, kMinAutosaveVersions);

  const auto silly = settings_from_json(R"({"autosave_versions":9999})");
  ASSERT_TRUE(silly.has_value()) << silly.error();
  EXPECT_EQ(silly->autosave_versions, kMaxAutosaveVersions);
}

TEST(Settings, TheRollsAreClampedRatherThanTrusted) {
  // A negative preroll would move the start of a loop past its end.
  const auto backwards = settings_from_json(R"({"preroll":-3.0,"postroll":-3.0})");
  ASSERT_TRUE(backwards.has_value()) << backwards.error();
  EXPECT_DOUBLE_EQ(backwards->preroll, kMinRoll);
  EXPECT_DOUBLE_EQ(backwards->postroll, kMinRoll);

  const auto silly = settings_from_json(R"({"preroll":600.0,"postroll":600.0})");
  ASSERT_TRUE(silly.has_value()) << silly.error();
  EXPECT_DOUBLE_EQ(silly->preroll, kMaxRoll);
  EXPECT_DOUBLE_EQ(silly->postroll, kMaxRoll);
}

TEST(Settings, AProxyHeightIsClampedRatherThanTrusted) {
  // A hand-edited zero would ask the encoder for a picture with no rows in it.
  const auto none = settings_from_json(R"({"proxy_height":0})");
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_GE(none->proxy_height, kMinProxyHeight);

  const auto huge = settings_from_json(R"({"proxy_height":99999})");
  ASSERT_TRUE(huge.has_value()) << huge.error();
  EXPECT_EQ(huge->proxy_height, kMaxProxyHeight);
}

TEST(Settings, AProxyFolderIsOnlyWrittenWhenThereIsOne) {
  // Beside the footage is the default and the absence of a key, so a fresh
  // file does not carry an empty path for somebody to wonder about.
  EXPECT_EQ(to_json(Settings{}).find("proxy_folder"), std::string::npos);

  Settings elsewhere;
  elsewhere.proxy_folder = "E:/Fast";
  EXPECT_NE(to_json(elsewhere).find("proxy_folder"), std::string::npos);
}

TEST(Settings, AnUnknownSortReadsAsPoolOrder) {
  const auto loaded = settings_from_json(R"({"pool_sort":"by-vibes"})");
  ASSERT_TRUE(loaded.has_value()) << loaded.error();
  EXPECT_EQ(loaded->pool_sort, BrowserSort::Pool);
}

// --------------------------------------------------------------- writing --

TEST(Settings, WritingLeavesNoStagingFileBehind) {
  // It is written through one so an interrupted save cannot leave a half-file
  // in place of the real one. What must not happen is the staging file
  // surviving a successful write.
  const TempSettings file;
  const auto wrote = write_settings(file.path(), all_changed());
  ASSERT_TRUE(wrote.has_value()) << wrote.error();

  EXPECT_TRUE(std::filesystem::exists(file.path()));
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(file.path()) += ".saving"));
}

TEST(Settings, WritingMakesTheDirectoryItNeeds) {
  // On a fresh install there is no Cutline folder at all, and the first thing
  // that writes one has to make it.
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "cutline_settings_fresh";
  std::error_code ignored;
  std::filesystem::remove_all(base, ignored);

  const std::filesystem::path path = base / "nested" / "settings.json";
  const auto wrote = write_settings(path, Settings{});
  EXPECT_TRUE(wrote.has_value()) << (wrote ? "" : wrote.error());
  EXPECT_TRUE(std::filesystem::exists(path));

  std::filesystem::remove_all(base, ignored);
}

TEST(Settings, TheFileSitsBesideTheOtherApplicationData) {
  const std::filesystem::path path = default_settings_path();
  EXPECT_EQ(path.filename(), "settings.json");
  EXPECT_EQ(path.parent_path().filename(), "Cutline")
      << "settings should live with the workspaces, the bins and the presets";
}

}  // namespace
}  // namespace cutline::editor
