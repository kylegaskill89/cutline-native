/// ZIP archives: what a Motion Graphics template is made of.

#include "cutline/editor/zip.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cutline::editor {
namespace {

[[nodiscard]] std::vector<ZipEntry> some_files() {
  return {{.name = "definition.json", .contents = R"({"capsuleName":"Lower Third"})"},
          {.name = "thumb.png", .contents = std::string("\x89PNG\x00\x01\x02", 7)},
          {.name = "empty.txt", .contents = ""},
          {.name = "long.txt", .contents = std::string(100000, 'a')}};
}

TEST(Zip, ADeflatedArchiveReadsBackAsItWent) {
  const auto files = read_zip(write_zip(some_files(), true));
  ASSERT_TRUE(files.has_value()) << files.error();
  ASSERT_EQ(files->size(), 4u);
  for (const ZipEntry& entry : some_files()) EXPECT_EQ(files->at(entry.name), entry.contents);
}

TEST(Zip, AStoredArchiveReadsBackAsItWent) {
  const auto files = read_zip(write_zip(some_files(), false));
  ASSERT_TRUE(files.has_value()) << files.error();
  for (const ZipEntry& entry : some_files()) EXPECT_EQ(files->at(entry.name), entry.contents);
}

TEST(Zip, AnArchiveInsideAnArchiveIsReadInTurn) {
  const std::string inner = write_zip({{.name = "project.prproj", .contents = "inner"}});
  const auto outer = read_zip(write_zip({{.name = "project.prgraphic", .contents = inner}}));
  ASSERT_TRUE(outer.has_value());
  const auto files = read_zip(outer->at("project.prgraphic"));
  ASSERT_TRUE(files.has_value());
  EXPECT_EQ(files->at("project.prproj"), "inner");
}

TEST(Zip, ACommentAfterTheDirectoryDoesNotHideIt) {
  std::string bytes = write_zip(some_files());
  // The comment's length is the last two bytes of the end record.
  const std::string comment = "made somewhere";
  bytes[bytes.size() - 2] = static_cast<char>(comment.size());
  bytes += comment;
  const auto files = read_zip(bytes);
  ASSERT_TRUE(files.has_value()) << files.error();
  EXPECT_EQ(files->size(), 4u);
}

TEST(Zip, SomethingThatIsNotAnArchiveIsRefused) {
  EXPECT_FALSE(read_zip("").has_value());
  EXPECT_FALSE(read_zip(std::string(500, 'x')).has_value());
  EXPECT_FALSE(read_zip(R"({"capsuleName":"Lower Third"})").has_value());
}

TEST(Zip, ADamagedEntryIsRefusedRatherThanReadWrong) {
  std::string bytes = write_zip({{.name = "a.txt", .contents = "hello there"}}, false);
  // The stored text starts after the 30-byte header and the name.
  bytes[30 + 5] = 'j';
  const auto files = read_zip(bytes);
  ASSERT_FALSE(files.has_value());
  EXPECT_NE(files.error().find("a.txt"), std::string::npos);
}

TEST(Zip, AnArchiveCutShortIsRefused) {
  const std::string bytes = write_zip(some_files());
  EXPECT_FALSE(read_zip(bytes.substr(0, bytes.size() / 2)).has_value());
}

}  // namespace
}  // namespace cutline::editor
