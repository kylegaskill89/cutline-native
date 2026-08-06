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
