/// The render bar: which stretches of the picture need rendering, and how a
/// rendered one is recognised again.

#include "cutline/editor/render_previews.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <process.h>

namespace cutline::editor {
namespace {

using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

/// Footage the sequence's size: two clips of it back to back, plain.
[[nodiscard]] Project plain_cut() {
  Project p;
  p.sequence().canvas_w = 1920;
  p.sequence().canvas_h = 1080;
  p.media = {Media{.id = "m",
                   .path = "C:/footage/a.mp4",
                   .duration = 60.0,
                   .has_video = true,
                   .width = 1920,
                   .height = 1080}};
  Track video{.id = "v1", .kind = TrackKind::Video};
  video.clips = {Clip{.id = "a", .media_id = "m", .source_in = 0.0, .source_out = 4.0, .start = 0.0},
                 Clip{.id = "b", .media_id = "m", .source_in = 10.0, .source_out = 14.0,
                      .start = 4.0}};
  p.sequence().tracks = {video};
  return p;
}

class RenderBarTest : public ::testing::Test {
 protected:
  void SetUp() override {
    folder_ = std::filesystem::temp_directory_path() /
              ("cutline_previews_" + std::to_string(_getpid()));
    std::filesystem::remove_all(folder_);
    std::filesystem::create_directories(folder_);
  }
  void TearDown() override { std::filesystem::remove_all(folder_); }
  std::filesystem::path folder_;
};

TEST_F(RenderBarTest, PlainFootageNeedsNoRendering) {
  const auto pieces = render_pieces(plain_cut(), folder_);
  ASSERT_EQ(pieces.size(), 2u);
  for (const RenderPiece& piece : pieces) {
    EXPECT_EQ(piece.status, RenderStatus::None);
    EXPECT_TRUE(piece.key.empty());
  }
}

TEST_F(RenderBarTest, AnEffectMakesItYellowAndABlurMakesItRed) {
  Project p = plain_cut();
  p.sequence().tracks[0].clips[0].effects = {core::ClipEffect{.type = "contrast"}};
  p.sequence().tracks[0].clips[1].effects = {core::ClipEffect{.type = "blur"}};
  const auto pieces = render_pieces(p, folder_);
  ASSERT_EQ(pieces.size(), 2u);
  EXPECT_EQ(pieces[0].status, RenderStatus::Unrendered);
  EXPECT_EQ(pieces[1].status, RenderStatus::Heavy);
}

TEST_F(RenderBarTest, ATransitionIsAPieceOfItsOwn) {
  Project p = plain_cut();
  p.sequence().tracks[0].clips[0].transition_out =
      core::Transition{.kind = core::TransitionKind::Dissolve, .duration = 1.0};
  const auto pieces = render_pieces(p, folder_);
  // Plain, the half before the cut, the half after, plain again.
  ASSERT_EQ(pieces.size(), 4u);
  EXPECT_EQ(pieces[0].status, RenderStatus::None);
  EXPECT_DOUBLE_EQ(pieces[1].start, 3.5);
  EXPECT_EQ(pieces[1].status, RenderStatus::Unrendered);
  EXPECT_DOUBLE_EQ(pieces[2].end, 4.5);
  EXPECT_EQ(pieces[3].status, RenderStatus::None);
}

TEST_F(RenderBarTest, FootageOfAnotherSizeOrAScaledClipNeedsRendering) {
  Project other = plain_cut();
  other.media[0].width = 1280;
  EXPECT_EQ(render_pieces(other, folder_)[0].status, RenderStatus::Unrendered);
  Project scaled = plain_cut();
  scaled.sequence().tracks[0].clips[0].transform.scale_x = 0.5;
  EXPECT_EQ(render_pieces(scaled, folder_)[0].status, RenderStatus::Unrendered);
}

TEST_F(RenderBarTest, AFileOfThePiecesNameMakesItGreen) {
  Project p = plain_cut();
  p.sequence().tracks[0].clips[0].effects = {core::ClipEffect{.type = "contrast"}};
  const RenderPiece piece = render_pieces(p, folder_)[0];
  std::ofstream(preview_file(folder_, piece.key)) << "rendered";
  EXPECT_EQ(render_pieces(p, folder_)[0].status, RenderStatus::Rendered);
}

TEST_F(RenderBarTest, AnEditToAPieceGivesItANewNameAndUndoingGivesTheOldOneBack) {
  Project p = plain_cut();
  p.sequence().tracks[0].clips[0].effects = {
      core::ClipEffect{.type = "contrast", .params = {{"amount", 120.0}}}};
  const std::string before = render_pieces(p, folder_)[0].key;
  Project changed = p;
  changed.sequence().tracks[0].clips[0].effects[0].params["amount"] = 150.0;
  EXPECT_NE(render_pieces(changed, folder_)[0].key, before);
  EXPECT_EQ(render_pieces(p, folder_)[0].key, before);
}

TEST_F(RenderBarTest, MovingAStretchOfTheCutKeepsWhatWasRendered) {
  Project p = plain_cut();
  p.sequence().tracks[0].clips[1].effects = {core::ClipEffect{.type = "contrast"}};
  const std::string before = render_pieces(p, folder_)[1].key;
  // The second clip moved later, a gap opening before it.
  p.sequence().tracks[0].clips[1].start = 6.0;
  const auto pieces = render_pieces(p, folder_);
  ASSERT_EQ(pieces.size(), 2u);
  EXPECT_DOUBLE_EQ(pieces[1].start, 6.0);
  EXPECT_EQ(pieces[1].key, before);
}

TEST_F(RenderBarTest, AnEditElsewhereLeavesAPieceAlone) {
  Project p = plain_cut();
  p.sequence().tracks[0].clips[0].effects = {core::ClipEffect{.type = "contrast"}};
  const std::string before = render_pieces(p, folder_)[0].key;
  p.sequence().tracks[0].clips[1].effects = {core::ClipEffect{.type = "blur"}};
  EXPECT_EQ(render_pieces(p, folder_)[0].key, before);
}

TEST_F(RenderBarTest, RenderInToOutTakesTheUnrenderedPiecesItTouches) {
  Project p = plain_cut();
  for (Clip& clip : p.sequence().tracks[0].clips) {
    clip.effects = {core::ClipEffect{.type = "contrast"}};
  }
  const auto pieces = render_pieces(p, folder_);
  EXPECT_EQ(pieces_to_render(pieces, 3.0, 5.0).size(), 2u);
  EXPECT_EQ(pieces_to_render(pieces, 0.0, 2.0).size(), 1u);
  std::ofstream(preview_file(folder_, pieces[0].key)) << "rendered";
  EXPECT_EQ(pieces_to_render(render_pieces(p, folder_), 0.0, 8.0).size(), 1u);
  EXPECT_EQ(delete_preview_files(folder_), 1u);
  EXPECT_EQ(pieces_to_render(render_pieces(p, folder_), 0.0, 8.0).size(), 2u);
}

}  // namespace
TEST(PreviewsFolder, FollowsTheProjectsScratchDisk) {
  core::Project project;
  const std::filesystem::path file = "D:/Work/Interview/cut.cutline";
  EXPECT_EQ(previews_folder(project, file), default_previews_folder());

  project.video_previews.where = core::ScratchDisk::Where::SameAsProject;
  EXPECT_EQ(previews_folder(project, file),
            std::filesystem::path("D:/Work/Interview") / "Cutline Video Previews");
  EXPECT_EQ(previews_folder(project, {}), default_previews_folder())
      << "a project never saved has nowhere to be beside";

  project.video_previews = {.where = core::ScratchDisk::Where::Folder, .folder = "E:/Scratch"};
  EXPECT_EQ(previews_folder(project, file), std::filesystem::path("E:/Scratch"));
}

}  // namespace cutline::editor
