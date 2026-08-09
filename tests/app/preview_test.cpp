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
#include "cutline/gpu/device.hpp"

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
  project = editor::import_and_place(std::move(project), *source, 0.0);
  ASSERT_FALSE(project.media.empty());

  // After the import, because importing into an empty sequence takes the
  // footage's shape — setting it first would be setting it only to have the
  // 4K reference clip overwrite it.
  project.canvas_w = 640;
  project.canvas_h = 360;

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
  project = editor::import_and_place(std::move(project), *source, 0.0);
  project.canvas_w = 320;
  project.canvas_h = 180;

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

TEST_F(WithFootage, ProxiesAreReadFromOnlyWhenTheProjectAsks) {
  // Asserted through a proxy path that is deliberately not there: with proxies
  // on the source cannot be opened and is reported missing, and with them off
  // it renders. That says which file was opened without needing a second real
  // one — and it says the switch is honoured *per render*, since the same
  // renderer answers both ways.
  const auto source = probe_source(path_);
  ASSERT_TRUE(source.has_value());

  core::Project project = editor::import_and_place(core::empty_project(1, 2), *source, 0.0);
  project.canvas_w = 320;
  project.canvas_h = 180;
  ASSERT_FALSE(project.media.empty());
  ASSERT_FALSE(project.tracks.front().clips.empty()) << "nothing was placed to decode";

  auto preview = ProjectPreview::create(project.canvas_w, project.canvas_h);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();

  ASSERT_TRUE((*preview)->frame_at(project, 0.5).has_value());
  EXPECT_TRUE((*preview)->missing_media().empty()) << "the original would not open";

  project.media[0].proxy_path = "Z:/definitely/not/here.mp4";
  project.use_proxies = true;
  ASSERT_TRUE((*preview)->frame_at(project, 0.5).has_value());
  EXPECT_FALSE((*preview)->missing_media().empty())
      << "the proxy was not read from, so the switch does nothing";

  // And back, which is the case that needs the open decoder dropped: it is
  // holding the wrong file.
  project.use_proxies = false;
  ASSERT_TRUE((*preview)->frame_at(project, 0.5).has_value());
  EXPECT_TRUE((*preview)->missing_media().empty())
      << "turning proxies off left the renderer on the proxy";
}

TEST_F(WithFootage, TheRendererFollowsTheSequenceSize) {
  const auto source = probe_source(path_);
  ASSERT_TRUE(source.has_value());

  core::Project project = core::empty_project(1, 2);
  project = editor::import_and_place(std::move(project), *source, 0.0);
  project.canvas_w = 320;
  project.canvas_h = 180;

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

TEST(Preview, RendersToATextureWithoutCopyingItBack) {
  auto preview = ProjectPreview::create(160, 90);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();

  const core::Project project = core::empty_project(1, 1);
  const auto frame = (*preview)->texture_at(project, 0.0);
  ASSERT_TRUE(frame.has_value()) << frame.error();

  EXPECT_FALSE(frame->empty());
  EXPECT_EQ(frame->width, project.canvas_w);
  EXPECT_EQ(frame->height, project.canvas_h);
}

TEST(Preview, AGivenDeviceIsTheOneItRendersOn) {
  auto device = gpu::Device::create({.allow_software = true});
  if (!device.has_value()) GTEST_SKIP() << "no usable device: " << device.error();

  auto preview = ProjectPreview::create(160, 90, *device);
  ASSERT_TRUE(preview.has_value()) << preview.error();

  // The point of passing one in: the frame and whoever draws it end up in the
  // same memory. Two devices and the texture would be meaningless to the
  // window, which is the difference between handing a frame over and copying
  // it through the CPU twice.
  EXPECT_EQ((*preview)->device().get(), device->get());
}

TEST(Preview, WithoutADeviceItMakesItsOwn) {
  auto preview = ProjectPreview::create(160, 90);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();
  EXPECT_NE((*preview)->device(), nullptr);
}


// ------------------------------------------------------------- resizing --

TEST(Preview, ResizingHandsBackADifferentGeneration) {
  // The guard that stops a caller drawing a texture that has been freed. A
  // resize throws the compositor away and Direct3D reuses addresses freely, so
  // the handle alone says nothing — the generation is what says "this is not
  // the texture you were given".
  auto preview = ProjectPreview::create(160, 90);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();

  core::Project project = core::empty_project(1, 1);
  project.canvas_w = 160;
  project.canvas_h = 90;
  const auto first = (*preview)->texture_at(project, 0.0);
  ASSERT_TRUE(first.has_value()) << first.error();
  const unsigned was = first->generation;

  project.canvas_w = 320;
  project.canvas_h = 180;
  const auto second = (*preview)->texture_at(project, 0.0);
  ASSERT_TRUE(second.has_value()) << second.error();

  EXPECT_NE(second->generation, was);
  EXPECT_EQ(second->width, 320);
}

TEST(Preview, TwoPreviewsNeverShareAGeneration) {
  // The fault this was written for: the count used to start at zero in every
  // compositor, and a resize builds a *new* one. Two previews, or one preview
  // resized twice, would hand out the same generation for different memory —
  // at which point the guard says "unchanged" about a freed texture and the
  // caller draws it.
  auto one = ProjectPreview::create(160, 90);
  if (!one.has_value()) GTEST_SKIP() << "no usable device: " << one.error();
  auto two = ProjectPreview::create(160, 90, (*one)->device());
  ASSERT_TRUE(two.has_value()) << two.error();

  core::Project project = core::empty_project(1, 1);
  project.canvas_w = 160;
  project.canvas_h = 90;

  const auto from_one = (*one)->texture_at(project, 0.0);
  const auto from_two = (*two)->texture_at(project, 0.0);
  ASSERT_TRUE(from_one.has_value()) << from_one.error();
  ASSERT_TRUE(from_two.has_value()) << from_two.error();

  EXPECT_NE(from_one->generation, from_two->generation);
}

TEST(Preview, ResizingToTheSameShapeKeepsEverything) {
  // The early return matters: a rebuild costs every open decoder, and the
  // preview is asked to match the project on every single frame.
  auto preview = ProjectPreview::create(160, 90);
  if (!preview.has_value()) GTEST_SKIP() << "no usable device: " << preview.error();

  core::Project project = core::empty_project(1, 1);
  project.canvas_w = 160;
  project.canvas_h = 90;

  const auto first = (*preview)->texture_at(project, 0.0);
  ASSERT_TRUE(first.has_value()) << first.error();
  const auto again = (*preview)->texture_at(project, 0.0);
  ASSERT_TRUE(again.has_value()) << again.error();

  EXPECT_EQ(again->generation, first->generation);
}

}  // namespace
}  // namespace cutline::app
