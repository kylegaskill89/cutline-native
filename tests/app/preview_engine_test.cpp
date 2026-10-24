/// The monitor's render thread: what the interface asks for arrives, asking
/// never waits, the latest request wins, and playing renders ahead.
///
/// Colour mattes only, so these run with no footage; what is being tested is
/// the hand-off and the threads, which no decoder changes.

#include "cutline/app/preview_engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

namespace cutline::app {
namespace {

using namespace std::chrono_literals;
using core::Clip;
using core::Media;
using core::Project;
using core::Track;
using core::TrackKind;

std::shared_ptr<gpu::Device> engine_device() {
  static std::shared_ptr<gpu::Device> device = [] {
    auto created = gpu::Device::create({.allow_software = true});
    return created ? *created : nullptr;
  }();
  return device;
}

/// A sequence of one matte, `seconds` long.
[[nodiscard]] std::shared_ptr<const Project> matte_project(const char* color, int width = 64,
                                                           int height = 64,
                                                           double seconds = 10.0) {
  Project p;
  p.sequence().canvas_w = width;
  p.sequence().canvas_h = height;
  p.sequence().fps = 30.0;
  Media m;
  m.id = "matte";
  m.is_color = true;
  m.color = color;
  p.media = {m};
  Clip c;
  c.id = "c";
  c.media_id = "matte";
  c.source_out = seconds;
  Track t;
  t.id = "v1";
  t.kind = TrackKind::Video;
  t.clips = {c};
  p.sequence().tracks = {t};
  return std::make_shared<const Project>(std::move(p));
}

/// The same, made expensive: a large canvas under a stack of wide blurs, so a
/// frame takes long enough that "the call returned while it was rendering" is a
/// statement about the thread rather than about a fast card.
[[nodiscard]] std::shared_ptr<const Project> slow_project() {
  auto p = std::make_shared<Project>(*matte_project("#ff0000", 3840, 2160));
  for (int i = 0; i < 16; ++i) {
    p->sequence().tracks[0].clips[0].effects.push_back(
        core::ClipEffect{.type = "blur", .params = {{"amount", 50.0}}});
  }
  return p;
}

class PreviewEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!engine_device()) GTEST_SKIP() << "no Direct3D 12 device available";
    auto made = PreviewEngine::create(engine_device(), {.slots = 4});
    ASSERT_TRUE(made.has_value()) << made.error();
    engine_ = std::move(*made);
  }

  /// Takes until a picture arrives, as the interface does each time it turns.
  [[nodiscard]] bool await_picture(double t, std::chrono::milliseconds limit = 5000ms) {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
      auto got = engine_->take(t);
      EXPECT_TRUE(got.has_value()) << (got ? "" : got.error());
      if (got && got->has_value()) return true;
      std::this_thread::sleep_for(2ms);
    }
    return false;
  }

  [[nodiscard]] int shown_red() {
    auto image = engine_->read_shown();
    EXPECT_TRUE(image.has_value()) << (image ? "" : image.error());
    return image ? image->pixels[(static_cast<std::size_t>(image->height / 2) * image->width +
                                  static_cast<std::size_t>(image->width / 2)) *
                                 4]
                 : -1;
  }

  std::unique_ptr<PreviewEngine> engine_;
};

TEST_F(PreviewEngineTest, AWantIsAnsweredWithItsPicture) {
  engine_->want(matte_project("#ff0000"), 0.0, false);
  ASSERT_TRUE(await_picture(0.0));
  EXPECT_EQ(shown_red(), 255);
}

TEST_F(PreviewEngineTest, AskingNeverWaitsForARender) {
  // The whole reason the thread exists. A seek across a cut used to stop the
  // window for as long as the decode took; asking must cost the same whether
  // the thread is idle or in the middle of the most expensive frame it has.
  const auto slow = slow_project();
  engine_->want(slow, 0.0, false);
  std::this_thread::sleep_for(5ms);  // so the thread is well into it

  const auto began = std::chrono::steady_clock::now();
  for (int i = 1; i <= 20; ++i) engine_->want(slow, i / 30.0, false);
  (void)engine_->take(0.0);
  const auto spent = std::chrono::steady_clock::now() - began;
  EXPECT_LT(spent, 20ms) << "twenty wants and a take waited on the render thread";
}

TEST_F(PreviewEngineTest, TheLatestWantWins) {
  // A scrub is a want per mouse move. Rendering every one of them in turn
  // would leave the picture further behind the pointer with every move.
  const auto slow = slow_project();
  for (int i = 0; i < 30; ++i) engine_->want(slow, i / 30.0, false);
  ASSERT_TRUE(await_picture(29 / 30.0, 30000ms));
  // Settle on the last one, which may be a moment behind the first picture.
  for (int i = 0; i < 100 && engine_->frames_rendered() < 2; ++i) {
    (void)engine_->take(29 / 30.0);
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_LE(engine_->frames_rendered(), 3u) << "rendered positions nobody was still asking for";
}

TEST_F(PreviewEngineTest, PlayingRendersAhead) {
  const auto green = matte_project("#00ff00");
  engine_->want(green, 0.0, true);
  // Nothing is taken, so everything it does is ahead of the playhead: one
  // frame per slot less the one the interface may be copying from.
  for (int i = 0; i < 500 && engine_->frames_rendered() < 3; ++i) {
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(engine_->frames_rendered(), 3u);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(engine_->frames_rendered(), 3u) << "rendered past the room it had to keep them";

  // And the playhead moving on, as it does each turn of the interface's loop,
  // lets it carry on into the room the frames behind it leave.
  engine_->want(green, 2 / 30.0, true);
  ASSERT_TRUE(await_picture(2 / 30.0));
  for (int i = 0; i < 500 && engine_->frames_rendered() < 5; ++i) {
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_GE(engine_->frames_rendered(), 5u);
}

TEST_F(PreviewEngineTest, AChangedProjectReplacesWhatWasRenderedForTheOldOne) {
  // An edit during playback: pictures of the old project waiting in the ring
  // must not be shown after it, or the edit appears a few frames late.
  engine_->want(matte_project("#ff0000"), 0.0, true);
  for (int i = 0; i < 500 && engine_->frames_rendered() < 3; ++i) {
    std::this_thread::sleep_for(2ms);
  }
  engine_->want(matte_project("#000000"), 0.0, true);
  ASSERT_TRUE(await_picture(0.0));
  EXPECT_EQ(shown_red(), 0);
}

TEST_F(PreviewEngineTest, ACaptureIsTheFrameItWasAskedFor) {
  engine_->want(matte_project("#000000"), 0.0, false);
  const auto image = engine_->capture(matte_project("#ff0000"), 0.0);
  ASSERT_TRUE(image.has_value()) << image.error();
  EXPECT_EQ(image->pixels[0], 255);
}

TEST_F(PreviewEngineTest, ItStopsWhileARenderIsRunning) {
  // Closing the window during a slow frame must neither hang nor crash.
  engine_->want(slow_project(), 0.0, true);
  std::this_thread::sleep_for(5ms);
  engine_.reset();
  SUCCEED();
}

}  // namespace
}  // namespace cutline::app
