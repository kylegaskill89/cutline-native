/// The hand-off between a render thread and the interface: what goes in one
/// side comes out the other, each slot holds its own picture, and the texture
/// the interface samples stays the same texture.
///
/// Single-threaded on purpose. What is being asserted is the bookkeeping and
/// the pixels; the threads are the preview engine's business and its tests'.

#include "cutline/gpu/compositor.hpp"
#include "cutline/gpu/device.hpp"
#include "cutline/gpu/frame_ring.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace cutline::gpu {
namespace {

constexpr int kSide = 32;

std::shared_ptr<Device> ring_device() {
  static std::shared_ptr<Device> device = [] {
    auto created = Device::create({.allow_software = true});
    return created ? *created : nullptr;
  }();
  return device;
}

class FrameRingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    device_ = ring_device();
    if (!device_) GTEST_SKIP() << "no Direct3D 12 device available";
    auto made = Compositor::create(device_, kSide, kSide);
    ASSERT_TRUE(made.has_value()) << made.error();
    compositor_ = std::move(*made);
    auto ring = FrameRing::create(device_, 3);
    ASSERT_TRUE(ring.has_value()) << ring.error();
    ring_ = std::move(*ring);
  }

  /// Composites one flat colour and stores it, which is the render thread's
  /// whole part in this.
  [[nodiscard]] std::size_t store(Color color, Compositor* into = nullptr) {
    Compositor& compositor = into != nullptr ? *into : *compositor_;
    Layer layer;
    layer.color = color;
    layer.quad = {compositor.width() * 0.5f, compositor.height() * 0.5f,
                  static_cast<float>(compositor.width()),
                  static_cast<float>(compositor.height()), 0.0f};
    EXPECT_TRUE(compositor.compose({&layer, 1}).has_value());
    EXPECT_TRUE(compositor.display_texture().has_value());
    auto slot = ring_->store(compositor);
    EXPECT_TRUE(slot.has_value()) << (slot ? "" : slot.error());
    return slot ? *slot : 0;
  }

  /// The red channel at the middle of what the interface would draw.
  [[nodiscard]] int shown_red() {
    auto image = ring_->read_shown();
    EXPECT_TRUE(image.has_value()) << (image ? "" : image.error());
    if (!image) return -1;
    const std::size_t middle = (static_cast<std::size_t>(image->height / 2) * image->width +
                                static_cast<std::size_t>(image->width / 2)) *
                               4;
    return image->pixels[middle];
  }

  std::shared_ptr<Device> device_;
  std::unique_ptr<Compositor> compositor_;
  std::unique_ptr<FrameRing> ring_;
};

TEST_F(FrameRingTest, WhatIsStoredIsWhatIsShown) {
  const std::size_t slot = store(Color{1.0f, 0.0f, 0.0f, 1.0f});
  ASSERT_TRUE(ring_->show(slot).has_value());
  EXPECT_EQ(shown_red(), 255);
}

TEST_F(FrameRingTest, EachSlotKeepsItsOwnPicture) {
  // The point of rendering ahead: the compositor goes on to the next frame,
  // and the one before it is still there to be shown.
  const std::size_t red = store(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const std::size_t black = store(Color{0.0f, 0.0f, 0.0f, 1.0f});
  ASSERT_NE(red, black);

  ASSERT_TRUE(ring_->show(black).has_value());
  EXPECT_EQ(shown_red(), 0);
  ASSERT_TRUE(ring_->show(red).has_value());
  EXPECT_EQ(shown_red(), 255);
}

TEST_F(FrameRingTest, AFullRingRefusesRatherThanWaits) {
  // A render thread that blocked here would be blocked on the interface, which
  // is the dependency the ring exists to remove.
  for (std::size_t i = 0; i < ring_->capacity(); ++i) (void)store(Color{});
  EXPECT_FALSE(ring_->has_room());
  EXPECT_FALSE(ring_->store(*compositor_).has_value());

  ASSERT_TRUE(ring_->show(0).has_value());
  EXPECT_TRUE(ring_->has_room());
}

TEST_F(FrameRingTest, ADiscardedSlotIsFreeAgain) {
  for (std::size_t i = 0; i < ring_->capacity(); ++i) (void)store(Color{});
  ring_->discard(1);
  EXPECT_TRUE(ring_->has_room());
  EXPECT_FALSE(ring_->show(1).has_value()) << "a discarded slot has nothing to show";
}

TEST_F(FrameRingTest, TheShownTextureStaysTheSameTexture) {
  // A painter wraps the texture once and keeps the wrapper. A new texture per
  // frame would be a new wrapper per frame, which is the removed device the
  // painter's cache was built to prevent.
  const auto first = ring_->show(store(Color{1.0f, 0.0f, 0.0f, 1.0f}));
  const auto second = ring_->show(store(Color{0.0f, 1.0f, 0.0f, 1.0f}));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->resource, second->resource);
  EXPECT_EQ(first->generation, second->generation);
}

TEST_F(FrameRingTest, ANewSizeIsANewGeneration) {
  const auto small = ring_->show(store(Color{}));
  auto larger = Compositor::create(device_, kSide * 2, kSide);
  ASSERT_TRUE(larger.has_value());
  const auto wide = ring_->show(store(Color{}, larger->get()));
  ASSERT_TRUE(small.has_value());
  ASSERT_TRUE(wide.has_value());
  EXPECT_EQ(wide->width, kSide * 2);
  EXPECT_NE(small->generation, wide->generation);
}

}  // namespace
}  // namespace cutline::gpu
