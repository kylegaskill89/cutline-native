/// The whole stack, end to end: a file on disk becomes pixels in the monitor.
///
/// Everything each layer does on its own is already tested without any media
/// present. What is left is whether they agree when a real file is involved,
/// and that is only answerable with one — so these skip unless
/// `CUTLINE_TEST_MEDIA_DIR` points at the reference footage.

#include "cutline/app/preview.hpp"

#include "cutline/core/properties.hpp"
#include "cutline/core/query.hpp"
#include "cutline/editor/import.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace cutline::app {
namespace {

/// The reference clip, or empty when there is none to test with.
[[nodiscard]] std::string reference_clip() {
  const char* dir = std::getenv("CUTLINE_TEST_MEDIA_DIR");
  if (dir == nullptr) return {};
  const std::filesystem::path path = std::filesystem::path(dir) / "Boiler.mp4";
  return std::filesystem::exists(path) ? path.string() : std::string{};
}

class WithFootage : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = reference_clip();
    if (path_.empty()) GTEST_SKIP() << "set CUTLINE_TEST_MEDIA_DIR to the reference footage";
  }
  std::string path_;
};

// ---------------------------------------------------------------- probing --

TEST_F(WithFootage, ProbingDescribesTheFile) {
  const auto source = probe_source(path_);
  ASSERT_TRUE(source.has_value()) << source.error();

  EXPECT_EQ(source->path, path_);
  EXPECT_EQ(source->name, "Boiler.mp4");
  EXPECT_GT(source->duration, 0.0);
  EXPECT_TRUE(source->has_video);
  EXPECT_FALSE(source->is_image) << "a video should not be taken for a still";
  ASSERT_TRUE(source->width.has_value());
  EXPECT_GT(*source->width, 0);
  ASSERT_TRUE(source->fps.has_value());
  EXPECT_GT(*source->fps, 0.0);
}

TEST(Probing, AFileThatIsNotThereSaysSo) {
  const auto source = probe_source("D:/definitely/not/here.mp4");
  ASSERT_FALSE(source.has_value());
  EXPECT_FALSE(source.error().empty());
}

TEST(Probing, AContainerWithNothingInItIsRefused) {
  // A file libavformat opens but finds no streams in is not media, whatever
  // it managed to identify the container as.
  const auto source = probe_source("");
  EXPECT_FALSE(source.has_value());
}

TEST(Probing, TextIsKeptOutByItsExtensionRatherThanByTheProbe) {
  // Worth stating because it is surprising: libavformat reads a text file
  // quite happily, as a *video* stream — its tty demuxer renders ANSI art. So
  // probing cannot be what rejects it, and making the probe guess which
  // formats are "really" media would reject legitimate unusual containers
  // along with the rubbish. The extension filter is the defence, on the
  // dialog and here.
  EXPECT_FALSE(editor::looks_like_media("CMakeLists.txt"));
  EXPECT_FALSE(editor::looks_like_media("notes.txt"));
  EXPECT_TRUE(editor::looks_like_media("footage.mp4"));
}

// -------------------------------------------------------------- rendering --

TEST_F(WithFootage, AnImportedClipRendersToPixels) {
  const auto source = probe_source(path_);
  ASSERT_TRUE(source.has_value());

  core::Project project = core::empty_project(1, 2);
  project.canvas_w = 640;
  project.canvas_h = 360;
  project = editor::import_and_place(std::move(project), *source, 0.0);
  ASSERT_FALSE(project.media.empty());

  auto preview = ProjectPreview::create(project.canvas_w, project.canvas_h);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();

  const auto frame = (*preview)->frame_at(project, 0.5);
  ASSERT_TRUE(frame.has_value()) << frame.error();

  EXPECT_EQ(frame->width, 640);
  EXPECT_EQ(frame->height, 360);
  EXPECT_FALSE(frame->empty());
  EXPECT_TRUE((*preview)->missing_media().empty()) << "the file it just imported went missing";

  // Something was actually drawn. An all-black frame is what a broken decode,
  // a failed upload and an empty timeline all look like.
  bool lit = false;
  const int count = frame->width * frame->height * 4;
  for (int i = 0; i < count; ++i) {
    if (frame->pixels[static_cast<std::size_t>(i)] > 16) lit = true;
  }
  EXPECT_TRUE(lit) << "the frame came out black";
}

TEST_F(WithFootage, DifferentTimesGiveDifferentFrames) {
  // Otherwise the picture is right once and then never moves, which looks
  // exactly like a working preview until the playhead is dragged.
  const auto source = probe_source(path_);
  ASSERT_TRUE(source.has_value());
  ASSERT_GT(source->duration, 2.0) << "the reference clip is too short to test with";

  core::Project project = core::empty_project(1, 2);
  project.canvas_w = 320;
  project.canvas_h = 180;
  project = editor::import_and_place(std::move(project), *source, 0.0);

  auto preview = ProjectPreview::create(project.canvas_w, project.canvas_h);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();

  const auto early = (*preview)->frame_at(project, 0.2);
  ASSERT_TRUE(early.has_value()) << early.error();
  const std::vector<std::uint8_t> first(early->pixels,
                                        early->pixels + early->width * early->height * 4);

  const auto later = (*preview)->frame_at(project, 2.0);
  ASSERT_TRUE(later.has_value()) << later.error();
  const std::vector<std::uint8_t> second(later->pixels,
                                         later->pixels + later->width * later->height * 4);

  EXPECT_NE(first, second) << "the preview showed the same frame at two different times";
}

TEST_F(WithFootage, TheRendererFollowsTheSequenceSize) {
  const auto source = probe_source(path_);
  ASSERT_TRUE(source.has_value());

  core::Project project = core::empty_project(1, 2);
  project.canvas_w = 320;
  project.canvas_h = 180;
  project = editor::import_and_place(std::move(project), *source, 0.0);

  auto preview = ProjectPreview::create(1920, 1080);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();

  // Opening a project of a different shape must not keep rendering at the old
  // one, or the monitor letterboxes the wrong way round.
  const auto frame = (*preview)->frame_at(project, 0.5);
  ASSERT_TRUE(frame.has_value()) << frame.error();
  EXPECT_EQ(frame->width, 320);
  EXPECT_EQ((*preview)->width(), 320);
}

TEST(Preview, ACanvasWithNoSizeIsRefused) {
  EXPECT_FALSE(ProjectPreview::create(0, 100).has_value());
  EXPECT_FALSE(ProjectPreview::create(100, -4).has_value());
}

TEST(Preview, AnEmptyProjectRendersWithoutComplaining) {
  auto preview = ProjectPreview::create(160, 90);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();

  const core::Project project = core::empty_project(1, 1);
  const auto frame = (*preview)->frame_at(project, 0.0);
  ASSERT_TRUE(frame.has_value()) << frame.error();

  // At the *project's* size, not the one the preview was made at: the sequence
  // decides, which is what stops a monitor letterboxing to a stale shape.
  EXPECT_EQ(frame->width, project.canvas_w);
}

}  // namespace
}  // namespace cutline::app
