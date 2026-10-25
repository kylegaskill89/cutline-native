/// Graphics templates: the built-ins, saving and reading them, and the
/// pictures they carry.

#include "cutline/editor/graphic_templates.hpp"

#include "cutline/core/query.hpp"
#include "cutline/editor/graphics.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <process.h>
#include <set>
#include <string>

namespace cutline::editor {
namespace {

/// A folder of its own that is gone afterwards.
class Folder {
 public:
  Folder()
      : path_(std::filesystem::temp_directory_path() /
              ("cutline_templates_" + std::to_string(_getpid()) + "_" +
               std::to_string(++counter_))) {
    std::filesystem::create_directories(path_);
  }
  ~Folder() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  Folder(const Folder&) = delete;
  Folder& operator=(const Folder&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

[[nodiscard]] core::Graphic saying(std::string words) {
  core::GraphicLayer layer = text_layer(std::move(words));
  layer.id = "layer1";
  return core::Graphic{.layers = {layer}};
}

TEST(Base64, ComesBackAsItWent) {
  for (const std::string bytes : {std::string{}, std::string("a"), std::string("ab"),
                                  std::string("abc"), std::string("\x00\xff\x10\x80", 4)}) {
    const auto back = from_base64(to_base64(bytes));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, bytes);
  }
  EXPECT_EQ(to_base64("Man"), "TWFu");
  EXPECT_FALSE(from_base64("not*base64").has_value());
}

TEST(Templates, TheBuiltInsAreNamedOnceEachAndHaveSomethingInThem) {
  std::set<std::string> names;
  for (const GraphicTemplate& made : built_in_templates()) {
    EXPECT_TRUE(names.insert(made.name).second) << made.name;
    EXPECT_FALSE(made.graphic.layers.empty()) << made.name;
    EXPECT_TRUE(made.built_in());
    EXPECT_GT(made.length, 0.0);
  }
  EXPECT_GE(names.size(), 4u);
}

TEST(Templates, ASavedTemplateReadsBackAsItWasSaved) {
  const Folder folder;
  core::Graphic graphic = saying("Hello");
  graphic.scroll.mode = core::ScrollMode::Crawl;
  const auto path = save_template(folder.path(), "Greeting", graphic, 7.5);
  ASSERT_TRUE(path.has_value()) << path.error();

  const auto made = read_template(*path);
  ASSERT_TRUE(made.has_value()) << made.error();
  EXPECT_EQ(made->name, "Greeting");
  EXPECT_DOUBLE_EQ(made->length, 7.5);
  EXPECT_EQ(made->graphic, graphic);
  EXPECT_FALSE(made->built_in());
}

TEST(Templates, SavingUnderTheSameNameReplacesIt) {
  const Folder folder;
  ASSERT_TRUE(save_template(folder.path(), "One", saying("First"), 5.0).has_value());
  ASSERT_TRUE(save_template(folder.path(), "One", saying("Second"), 5.0).has_value());
  const auto all = read_templates(folder.path());
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all.front().graphic.layers.front().text, "Second");
}

TEST(Templates, ANameAFileCannotHaveIsRefused) {
  const Folder folder;
  EXPECT_FALSE(save_template(folder.path(), "", saying("x"), 5.0).has_value());
  EXPECT_FALSE(save_template(folder.path(), "a/b", saying("x"), 5.0).has_value());
  EXPECT_FALSE(save_template(folder.path(), "what?", saying("x"), 5.0).has_value());
}

TEST(Templates, APictureTravelsInsideTheTemplate) {
  const Folder folder;
  const std::filesystem::path picture = folder.path() / "logo.png";
  const std::string bytes("\x89PNG not really a png, but bytes", 32);
  { std::ofstream(picture, std::ios::binary) << bytes; }

  core::Graphic graphic;
  core::GraphicLayer logo = image_layer(picture.string());
  logo.id = "logo";
  graphic.layers.push_back(logo);
  const auto path = save_template(folder.path(), "Branded", graphic, 5.0);
  ASSERT_TRUE(path.has_value()) << path.error();

  // The original gone, as it would be on another machine.
  std::filesystem::remove(picture);
  const auto made = read_template(*path);
  ASSERT_TRUE(made.has_value()) << made.error();
  const std::string& now = made->graphic.layers.front().path;
  EXPECT_NE(now, picture.string());
  std::ifstream in(now, std::ios::binary);
  const std::string back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(back, bytes);
  EXPECT_EQ(std::filesystem::path(now).extension(), ".png");
}

TEST(Templates, AMissingPictureIsSaidRatherThanLeftOut) {
  const Folder folder;
  core::Graphic graphic;
  graphic.layers.push_back(image_layer((folder.path() / "gone.png").string()));
  EXPECT_FALSE(save_template(folder.path(), "Broken", graphic, 5.0).has_value());
}

TEST(Templates, TheFolderIsListedByNameAndJunkIsPassedOver) {
  const Folder folder;
  ASSERT_TRUE(save_template(folder.path(), "Zed", saying("z"), 5.0).has_value());
  ASSERT_TRUE(save_template(folder.path(), "Alpha", saying("a"), 5.0).has_value());
  { std::ofstream(folder.path() / "junk.cutgfx") << "not json"; }
  { std::ofstream(folder.path() / "notes.txt") << "{}"; }
  const auto all = read_templates(folder.path());
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].name, "Alpha");
  EXPECT_EQ(all[1].name, "Zed");
}

TEST(Templates, DeletingTakesItAway) {
  const Folder folder;
  const auto path = save_template(folder.path(), "Gone", saying("x"), 5.0);
  ASSERT_TRUE(path.has_value());
  EXPECT_TRUE(delete_template(*path));
  EXPECT_TRUE(read_templates(folder.path()).empty());
  EXPECT_FALSE(delete_template(*path));
}

TEST(Templates, ATemplateIsPlacedAsLongAsItSays) {
  core::Project project;
  project.sequence().tracks = {core::Track{.id = "v1", .kind = core::TrackKind::Video}};
  const GraphicTemplate roll = [] {
    for (const GraphicTemplate& made : built_in_templates()) {
      if (made.name == "Credits Roll") return made;
    }
    return GraphicTemplate{};
  }();
  std::string clip_id;
  project = add_template_at(std::move(project), roll, 1.0, &clip_id);
  const core::Clip* clip = core::find_clip(project, clip_id);
  ASSERT_NE(clip, nullptr);
  EXPECT_DOUBLE_EQ(core::clip_duration(*clip), roll.length);
  EXPECT_DOUBLE_EQ(clip->start, 1.0);
}

}  // namespace
}  // namespace cutline::editor
