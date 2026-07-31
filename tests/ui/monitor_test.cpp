/// The program monitor.
///
/// Letterboxing is the whole of it, and it is worth pinning down because
/// getting it wrong is the one thing a monitor exists not to do: a picture
/// stretched to fill the panel is lying about the framing, and every judgement
/// made while looking at it is wrong.

#include "cutline/ui/monitor.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() {
  return LayoutContext{default_theme(), measurer()};
}

/// Somewhere for a view to point at. The values do not matter; only that the
/// pointer is valid and the dimensions are what they claim.
struct Pixels {
  explicit Pixels(int w, int h) : width(w), height(h), bytes(static_cast<std::size_t>(w * h * 4), 0x40) {}

  [[nodiscard]] ImageView view() const {
    return ImageView{.pixels = bytes.data(), .width = width, .height = height};
  }

  int width;
  int height;
  std::vector<std::uint8_t> bytes;
};

// ----------------------------------------------------------- fitting maths --

TEST(FitAspect, AWideBoxInATallOneLeavesBarsAboveAndBelow) {
  const Rect fitted = fit_aspect(Rect{0.0, 0.0, 400.0, 400.0}, 2.0);

  EXPECT_DOUBLE_EQ(fitted.width, 400.0);
  EXPECT_DOUBLE_EQ(fitted.height, 200.0);
  EXPECT_DOUBLE_EQ(fitted.y, 100.0) << "it should be centred";
  EXPECT_DOUBLE_EQ(fitted.x, 0.0);
}

TEST(FitAspect, ATallBoxInAWideOneLeavesBarsEitherSide) {
  const Rect fitted = fit_aspect(Rect{0.0, 0.0, 400.0, 200.0}, 0.5);

  EXPECT_DOUBLE_EQ(fitted.height, 200.0);
  EXPECT_DOUBLE_EQ(fitted.width, 100.0);
  EXPECT_DOUBLE_EQ(fitted.x, 150.0);
}

TEST(FitAspect, AnExactMatchFillsItCompletely) {
  const Rect fitted = fit_aspect(Rect{10.0, 20.0, 320.0, 180.0}, 16.0 / 9.0);
  EXPECT_NEAR(fitted.width, 320.0, 1e-9);
  EXPECT_NEAR(fitted.height, 180.0, 1e-9);
  EXPECT_NEAR(fitted.x, 10.0, 1e-9);
}

TEST(FitAspect, NeverOverflowsWhateverTheShapes) {
  for (const double aspect : {0.1, 0.5, 1.0, 1.777, 2.35, 12.0}) {
    const Rect bounds{5.0, 7.0, 313.0, 197.0};
    const Rect fitted = fit_aspect(bounds, aspect);

    EXPECT_LE(fitted.width, bounds.width + 1e-9) << "at " << aspect;
    EXPECT_LE(fitted.height, bounds.height + 1e-9) << "at " << aspect;
    EXPECT_GE(fitted.x, bounds.x - 1e-9);
    EXPECT_GE(fitted.y, bounds.y - 1e-9);
    EXPECT_LE(fitted.right(), bounds.right() + 1e-9);
    EXPECT_LE(fitted.bottom(), bounds.bottom() + 1e-9);
    // And it should touch one pair of edges, or it is not the largest that fits.
    const bool fills_width = std::abs(fitted.width - bounds.width) < 1e-6;
    const bool fills_height = std::abs(fitted.height - bounds.height) < 1e-6;
    EXPECT_TRUE(fills_width || fills_height) << "at " << aspect;
  }
}

TEST(FitAspect, ANonsenseAspectOrAnEmptyBoxGivesNothing) {
  EXPECT_TRUE(fit_aspect(Rect{0.0, 0.0, 100.0, 100.0}, 0.0).empty());
  EXPECT_TRUE(fit_aspect(Rect{0.0, 0.0, 100.0, 100.0}, -2.0).empty());
  EXPECT_TRUE(fit_aspect(Rect{0.0, 0.0, 0.0, 100.0}, 1.5).empty());
}

// ---------------------------------------------------------------- monitor --

TEST(Monitor, LetterboxesToTheFramesOwnShape) {
  const Pixels frame(200, 100);
  MonitorView monitor;
  // Forty wider and taller than the picture wants: there is a border around it,
  // so the transform handles of a layer filling the frame are inside the widget
  // and can be pressed at all.
  monitor.set_frame(frame.view());
  monitor.arrange(Rect{0.0, 0.0, 440.0, 440.0}, flat_context());

  const Rect picture = monitor.picture();
  EXPECT_DOUBLE_EQ(picture.width, 400.0);
  EXPECT_DOUBLE_EQ(picture.height, 200.0);
  EXPECT_GT(picture.x, 0.0);
}

TEST(Monitor, UsesTheSequenceShapeUntilAFrameArrives) {
  // Otherwise the picture jumps into a different rectangle the instant the
  // first frame is decoded.
  MonitorView monitor;
  monitor.set_canvas_aspect(2.0);
  monitor.arrange(Rect{0.0, 0.0, 440.0, 440.0}, flat_context());

  const Rect empty = monitor.picture();
  EXPECT_DOUBLE_EQ(empty.height, 200.0);

  const Pixels frame(200, 100);
  monitor.set_frame(frame.view());
  EXPECT_EQ(monitor.picture(), empty) << "the picture moved when the frame arrived";
}

TEST(Monitor, ANonsenseAspectIsIgnored) {
  MonitorView monitor;
  monitor.set_canvas_aspect(16.0 / 9.0);
  monitor.set_canvas_aspect(0.0);
  EXPECT_DOUBLE_EQ(monitor.canvas_aspect(), 16.0 / 9.0);
}

TEST(Monitor, DrawsTheFrameIntoThePicture) {
  const Pixels frame(320, 180);
  MonitorView monitor;
  monitor.set_frame(frame.view());
  monitor.arrange(Rect{0.0, 0.0, 640.0, 480.0}, flat_context());

  RecordingPainter painter;
  monitor.paint(painter, default_theme());

  const DrawCall* drawn = painter.first(DrawCall::Kind::Image);
  ASSERT_NE(drawn, nullptr);
  EXPECT_EQ(drawn->bounds, monitor.picture());
  EXPECT_EQ(drawn->image.pixels, frame.bytes.data()) << "it drew somebody else's pixels";
  EXPECT_EQ(drawn->image.width, 320);
}

TEST(Monitor, WithNoFrameItSaysSoInsteadOfDrawingNothing) {
  MonitorView monitor;
  monitor.arrange(Rect{0.0, 0.0, 640.0, 480.0}, flat_context());

  RecordingPainter painter;
  monitor.paint(painter, default_theme());

  EXPECT_EQ(painter.count(DrawCall::Kind::Image), 0u);
  const DrawCall* text = painter.first(DrawCall::Kind::Text);
  ASSERT_NE(text, nullptr);
  EXPECT_FALSE(text->run->text.empty());
}

TEST(Monitor, ClearingAFrameGoesBackToTheEmptyState) {
  const Pixels frame(320, 180);
  MonitorView monitor;
  monitor.set_frame(frame.view());
  monitor.clear_frame();
  monitor.arrange(Rect{0.0, 0.0, 640.0, 480.0}, flat_context());

  RecordingPainter painter;
  monitor.paint(painter, default_theme());
  EXPECT_EQ(painter.count(DrawCall::Kind::Image), 0u);
}

TEST(Monitor, InAPanelTooSmallToShowAnythingItDrawsNothing) {
  const Pixels frame(320, 180);
  MonitorView monitor;
  monitor.set_frame(frame.view());
  monitor.arrange(Rect{0.0, 0.0, 0.0, 0.0}, flat_context());

  RecordingPainter painter;
  monitor.paint(painter, default_theme());
  EXPECT_TRUE(painter.calls().empty());
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(Monitor, PointsMapIntoThePicture) {
  const Pixels frame(200, 100);
  MonitorView monitor;
  monitor.set_frame(frame.view());
  monitor.arrange(Rect{0.0, 0.0, 400.0, 400.0}, flat_context());

  const Rect picture = monitor.picture();  // 400x200 at y=100
  const auto centre = monitor.to_picture(picture.x + picture.width / 2.0,
                                         picture.y + picture.height / 2.0);
  ASSERT_TRUE(centre.has_value());
  EXPECT_NEAR(centre->first, 0.5, 1e-9);
  EXPECT_NEAR(centre->second, 0.5, 1e-9);

  const auto corner = monitor.to_picture(picture.x, picture.y);
  ASSERT_TRUE(corner.has_value());
  EXPECT_NEAR(corner->first, 0.0, 1e-9);
  EXPECT_NEAR(corner->second, 0.0, 1e-9);
}

TEST(Monitor, PointsOutsideThePictureRunPastItRatherThanStopping) {
  // A transform being dragged past the edge of the frame is a normal thing to
  // do, and clamping here would stop it dead at the boundary.
  const Pixels frame(200, 100);
  MonitorView monitor;
  monitor.set_frame(frame.view());
  monitor.arrange(Rect{0.0, 0.0, 400.0, 400.0}, flat_context());

  const auto above = monitor.to_picture(0.0, 0.0);
  ASSERT_TRUE(above.has_value());
  EXPECT_LT(above->second, 0.0) << "the letterbox bar should be outside the picture";
}

TEST(Monitor, WithNoRoomThereIsNowhereToMapTo) {
  MonitorView monitor;
  monitor.arrange(Rect{0.0, 0.0, 0.0, 0.0}, flat_context());
  EXPECT_FALSE(monitor.to_picture(0.0, 0.0).has_value());
}

TEST(Monitor, AnImageViewKnowsItsOwnShape) {
  const Pixels frame(320, 180);
  EXPECT_NEAR(frame.view().aspect(), 16.0 / 9.0, 1e-9);
  EXPECT_EQ(frame.view().row_bytes(), 320 * 4);
  EXPECT_FALSE(frame.view().empty());

  EXPECT_TRUE(ImageView{}.empty());
  EXPECT_DOUBLE_EQ(ImageView{}.aspect(), 0.0);
}

TEST(Monitor, AStrideIsHonouredWhenGiven) {
  // A decoded frame is very often padded to a row alignment the width does not
  // account for.
  const Pixels frame(320, 180);
  ImageView padded = frame.view();
  padded.stride = 320 * 4 + 64;
  EXPECT_EQ(padded.row_bytes(), 320 * 4 + 64);
}

// ------------------------------------------------------- frames on the GPU --

/// A texture the monitor can hold without there being a GPU anywhere.
///
/// Nothing here dereferences the pointer — the whole point of `TextureView`
/// being untyped is that this layer never does — so an address that is merely
/// distinctive is a truthful stand-in for a real resource.
[[nodiscard]] TextureView fake_texture(int width, int height) {
  static int marker = 0;
  return TextureView{.texture = &marker, .width = width, .height = height};
}

TEST(Monitor, DrawsATextureIntoThePicture) {
  const TextureView frame = fake_texture(320, 180);
  MonitorView monitor;
  monitor.set_texture(frame);
  monitor.arrange(Rect{0.0, 0.0, 640.0, 480.0}, flat_context());

  RecordingPainter painter;
  monitor.paint(painter, default_theme());

  const DrawCall* drawn = painter.first(DrawCall::Kind::Texture);
  ASSERT_NE(drawn, nullptr);
  EXPECT_EQ(drawn->bounds, monitor.picture());
  EXPECT_EQ(drawn->frame.texture, frame.texture) << "it drew somebody else's frame";
  EXPECT_EQ(painter.count(DrawCall::Kind::Image), 0u)
      << "a frame already on the card should not also be drawn as pixels";
}

TEST(Monitor, LetterboxesToTheTexturesOwnShape) {
  MonitorView monitor;
  monitor.set_canvas_aspect(4.0 / 3.0);
  monitor.set_texture(fake_texture(320, 180));
  monitor.arrange(Rect{0.0, 0.0, 640.0, 480.0}, flat_context());

  // The frame's shape wins over the sequence's, exactly as it does for pixels.
  EXPECT_NEAR(monitor.picture().width / monitor.picture().height, 16.0 / 9.0, 1e-9);
}

TEST(Monitor, TheTwoKindsOfFrameAreAlternativesNotLayers) {
  const Pixels pixels(320, 180);
  MonitorView monitor;

  monitor.set_frame(pixels.view());
  monitor.set_texture(fake_texture(320, 180));
  EXPECT_TRUE(monitor.frame().empty()) << "the pixels should have been forgotten";
  EXPECT_FALSE(monitor.texture().empty());

  monitor.set_frame(pixels.view());
  EXPECT_TRUE(monitor.texture().empty()) << "the texture should have been forgotten";
  EXPECT_FALSE(monitor.frame().empty());
}

TEST(Monitor, ATextureCountsAsHavingAPicture) {
  MonitorView monitor;
  EXPECT_FALSE(monitor.has_picture());

  monitor.set_texture(fake_texture(320, 180));
  EXPECT_TRUE(monitor.has_picture());
  monitor.arrange(Rect{0.0, 0.0, 640.0, 480.0}, flat_context());

  RecordingPainter painter;
  monitor.paint(painter, default_theme());
  // No placeholder: there is a picture, it just does not live in this process's
  // memory. Saying "No preview" over a perfectly good frame is the bug here.
  EXPECT_EQ(painter.count(DrawCall::Kind::Text), 0u);
}

TEST(Monitor, ClearingForgetsATextureToo) {
  MonitorView monitor;
  monitor.set_texture(fake_texture(320, 180));
  monitor.clear_frame();
  EXPECT_FALSE(monitor.has_picture());

  monitor.arrange(Rect{0.0, 0.0, 640.0, 480.0}, flat_context());
  RecordingPainter painter;
  monitor.paint(painter, default_theme());
  EXPECT_EQ(painter.count(DrawCall::Kind::Texture), 0u);
}

TEST(Monitor, ATextureViewKnowsItsOwnShape) {
  EXPECT_NEAR(fake_texture(320, 180).aspect(), 16.0 / 9.0, 1e-9);
  EXPECT_FALSE(fake_texture(320, 180).empty());

  EXPECT_TRUE(TextureView{}.empty());
  EXPECT_DOUBLE_EQ(TextureView{}.aspect(), 0.0);
  // A resource with no size is as useless as no resource at all, and the
  // monitor would divide by it.
  EXPECT_TRUE(fake_texture(0, 180).empty());
}

// ------------------------------------------------------ transform handles --

/// A monitor with a picture, a square canvas, and a layer filling half of it.
/// Square so a rotation is a rotation on screen rather than something that has
/// to be corrected for while reading the test.
/// Placed so that the picture inside its border lands exactly on the origin at
/// 400 by 400, which keeps every coordinate below a plain fraction of it.
const Rect kPlaced{-kMonitorInset, -kMonitorInset, 400.0 + kMonitorInset * 2.0,
                   400.0 + kMonitorInset * 2.0};

struct WithLayer {
  WithLayer() {
    monitor.set_canvas_aspect(1.0);
    monitor.set_transform(MonitorBox{.x = 0.5, .y = 0.5, .width = 0.5, .height = 0.5});
    monitor.arrange(kPlaced, flat_context());
  }

  void press(double x, double y, Modifiers modifiers = {}) {
    monitor.on_mouse_down(MouseEvent{.x = x, .y = y, .modifiers = modifiers});
  }
  void move(double x, double y, Modifiers modifiers = {}) {
    monitor.on_mouse_move(MouseEvent{.x = x, .y = y, .modifiers = modifiers});
  }
  void release(double x, double y, Modifiers modifiers = {}) {
    monitor.on_mouse_up(MouseEvent{.x = x, .y = y, .modifiers = modifiers});
  }

  MonitorView monitor;
};

TEST(TransformHandles, WithNoLayerThereIsNothingToGrab) {
  MonitorView monitor;
  monitor.arrange(kPlaced, flat_context());
  EXPECT_EQ(monitor.handle_at(200.0, 200.0), TransformHandle::None);
  EXPECT_TRUE(monitor.handle_rect(TransformHandle::TopLeft).empty());
}

TEST(TransformHandles, TheCornersSitOnTheLayersCorners) {
  const WithLayer fixture;
  // The layer spans a quarter to three quarters of a 400-pixel picture.
  const Rect top_left = fixture.monitor.handle_rect(TransformHandle::TopLeft);
  EXPECT_NEAR(top_left.x + top_left.width * 0.5, 100.0, 0.001);
  EXPECT_NEAR(top_left.y + top_left.height * 0.5, 100.0, 0.001);

  const Rect bottom_right = fixture.monitor.handle_rect(TransformHandle::BottomRight);
  EXPECT_NEAR(bottom_right.x + bottom_right.width * 0.5, 300.0, 0.001);
  EXPECT_NEAR(bottom_right.y + bottom_right.height * 0.5, 300.0, 0.001);
}

TEST(TransformHandles, TheRotationHandleFloatsAboveTheTopEdge) {
  const WithLayer fixture;
  const Rect spin = fixture.monitor.handle_rect(TransformHandle::Rotate);
  EXPECT_NEAR(spin.x + spin.width * 0.5, 200.0, 0.001);
  // Above the top edge, and clear of the corners either side of it.
  EXPECT_LT(spin.y + spin.height * 0.5, 100.0);
}

// A corner handle sits inside the two edge handles it belongs to, so an edge
// tested first would make the corners unreachable.
TEST(TransformHandles, ACornerWinsOverTheEdgesItTouches) {
  const WithLayer fixture;
  EXPECT_EQ(fixture.monitor.handle_at(100.0, 100.0), TransformHandle::TopLeft);
}

TEST(TransformHandles, TheInsideOfTheLayerMovesIt) {
  const WithLayer fixture;
  EXPECT_EQ(fixture.monitor.handle_at(200.0, 200.0), TransformHandle::Move);
  EXPECT_EQ(fixture.monitor.handle_at(20.0, 20.0), TransformHandle::None);
}

TEST(TransformHandles, DraggingTheInsideMovesTheLayerWithThePointer) {
  WithLayer fixture;
  fixture.monitor.set_snapping(false);
  fixture.press(200.0, 200.0);
  fixture.move(260.0, 220.0);

  EXPECT_NEAR(fixture.monitor.transform()->x, 0.65, 1e-9);
  EXPECT_NEAR(fixture.monitor.transform()->y, 0.55, 1e-9);
  // Moving does not resize.
  EXPECT_NEAR(fixture.monitor.transform()->width, 0.5, 1e-9);
}

TEST(TransformHandles, EveryPixelOfADragIsReportedAndTheReleaseCommitsOnce) {
  WithLayer fixture;
  fixture.monitor.set_snapping(false);
  int changes = 0;
  int commits = 0;
  fixture.monitor.set_on_transform_change([&](const MonitorBox&) { ++changes; });
  fixture.monitor.set_on_transform_commit([&](const MonitorBox&) { ++commits; });

  fixture.press(200.0, 200.0);
  fixture.move(220.0, 200.0);
  fixture.move(240.0, 200.0);
  fixture.release(240.0, 200.0);

  EXPECT_EQ(changes, 2);
  EXPECT_EQ(commits, 1);
}

// One gesture is one undo entry, and a gesture that changed nothing is none.
TEST(TransformHandles, ADragThatEndedWhereItStartedCommitsNothing) {
  WithLayer fixture;
  int commits = 0;
  fixture.monitor.set_on_transform_commit([&](const MonitorBox&) { ++commits; });

  fixture.press(200.0, 200.0);
  fixture.release(200.0, 200.0);
  EXPECT_EQ(commits, 0);
}

TEST(TransformHandles, DraggingACornerLeavesTheOppositeOneWhereItWas) {
  WithLayer fixture;
  fixture.press(100.0, 100.0);
  fixture.move(50.0, 60.0);

  const MonitorBox& box = *fixture.monitor.transform();
  // The bottom right was at (300, 300) and has to still be.
  EXPECT_NEAR((box.x + box.width * 0.5) * 400.0, 300.0, 0.001);
  EXPECT_NEAR((box.y + box.height * 0.5) * 400.0, 300.0, 0.001);
  EXPECT_NEAR((box.x - box.width * 0.5) * 400.0, 50.0, 0.001);
  EXPECT_NEAR((box.y - box.height * 0.5) * 400.0, 60.0, 0.001);
}

TEST(TransformHandles, AnEdgeHandleChangesOneAxisOnly) {
  WithLayer fixture;
  fixture.press(300.0, 200.0);  // the right edge
  fixture.move(360.0, 260.0);

  const MonitorBox& box = *fixture.monitor.transform();
  EXPECT_NEAR(box.width * 400.0, 260.0, 0.001);
  EXPECT_NEAR(box.height * 400.0, 200.0, 0.001);
  EXPECT_NEAR(box.y, 0.5, 1e-9);
}

TEST(TransformHandles, ShiftOnACornerKeepsTheShape) {
  WithLayer fixture;
  fixture.press(300.0, 300.0);
  fixture.move(400.0, 320.0, Modifiers{.shift = true});

  const MonitorBox& box = *fixture.monitor.transform();
  EXPECT_NEAR(box.width, box.height, 1e-9);
  // Pulled hardest along x, so that is the axis it followed.
  EXPECT_NEAR(box.width * 400.0, 300.0, 0.001);
}

TEST(TransformHandles, ALayerCannotBeScaledDownToNothing) {
  WithLayer fixture;
  fixture.press(300.0, 300.0);
  // Dragged well past the anchor, which would otherwise turn the box inside
  // out and leave no handle to grab it by.
  fixture.move(20.0, 20.0);

  const MonitorBox& box = *fixture.monitor.transform();
  EXPECT_GT(box.width, 0.0);
  EXPECT_GT(box.height, 0.0);
}

TEST(TransformHandles, TheRotationHandleTurnsTheLayer) {
  WithLayer fixture;
  const Rect spin = fixture.monitor.handle_rect(TransformHandle::Rotate);
  fixture.press(spin.x + spin.width * 0.5, spin.y + spin.height * 0.5);
  // Out to the right of the centre, which is a quarter turn clockwise.
  fixture.move(340.0, 200.0);

  EXPECT_NEAR(fixture.monitor.transform()->rotation, 90.0, 0.001);
}

TEST(TransformHandles, ShiftSnapsTheRotationToWholeSteps) {
  WithLayer fixture;
  const Rect spin = fixture.monitor.handle_rect(TransformHandle::Rotate);
  fixture.press(spin.x + spin.width * 0.5, spin.y + spin.height * 0.5);
  fixture.move(338.0, 190.0, Modifiers{.shift = true});

  const double turned = fixture.monitor.transform()->rotation;
  EXPECT_NEAR(std::fmod(turned, 15.0), 0.0, 1e-9);
}

// The handles follow the layer round, so a turned layer is grabbed where it
// looks rather than where its bounding rectangle is.
TEST(TransformHandles, AQuarterTurnPutsTheTopLeftHandleWhereTheTopRightWas) {
  MonitorView monitor;
  monitor.set_canvas_aspect(1.0);
  monitor.set_transform(
      MonitorBox{.x = 0.5, .y = 0.5, .width = 0.5, .height = 0.5, .rotation = 90.0});
  monitor.arrange(kPlaced, flat_context());

  const Rect top_left = monitor.handle_rect(TransformHandle::TopLeft);
  EXPECT_NEAR(top_left.x + top_left.width * 0.5, 300.0, 0.001);
  EXPECT_NEAR(top_left.y + top_left.height * 0.5, 100.0, 0.001);
}

TEST(TransformHandles, ATurnedLayerIsGrabbedWhereItLooks) {
  MonitorView monitor;
  monitor.set_canvas_aspect(1.0);
  monitor.set_transform(
      MonitorBox{.x = 0.5, .y = 0.5, .width = 0.8, .height = 0.2, .rotation = 90.0});
  monitor.arrange(kPlaced, flat_context());

  // Turned upright, so it is tall and thin: a point above the centre is inside
  // it and a point beside the centre is not.
  EXPECT_EQ(monitor.handle_at(200.0, 130.0), TransformHandle::Move);
  EXPECT_EQ(monitor.handle_at(130.0, 200.0), TransformHandle::None);
}

// ---------------------------------------------------------------- snapping --

TEST(TransformSnapping, TheCentreSnapsToTheCentreOfTheFrame) {
  WithLayer fixture;
  fixture.press(200.0, 200.0);
  fixture.move(202.0, 200.0);

  EXPECT_NEAR(fixture.monitor.transform()->x, 0.5, 1e-9);

  // Two guides, not one: the layer was already centred vertically, so it is
  // lined up on both axes and both lines are true.
  ASSERT_EQ(fixture.monitor.guides().size(), 2u);
  EXPECT_EQ(fixture.monitor.guides()[0], (SnapGuide{.vertical = true, .at = 0.5}));
  EXPECT_EQ(fixture.monitor.guides()[1], (SnapGuide{.vertical = false, .at = 0.5}));
}

TEST(TransformSnapping, AnEdgeSnapsToTheEdgeOfTheFrameAndTheGuideGoesThere) {
  WithLayer fixture;
  fixture.press(200.0, 200.0);
  // Left edge two pixels short of the frame's left.
  fixture.move(102.0, 200.0);

  const MonitorBox& box = *fixture.monitor.transform();
  EXPECT_NEAR((box.x - box.width * 0.5) * 400.0, 0.0, 1e-9);
  ASSERT_FALSE(fixture.monitor.guides().empty());
  EXPECT_DOUBLE_EQ(fixture.monitor.guides().front().at, 0.0);
}

TEST(TransformSnapping, ControlIsTheWayOutOfASnap) {
  WithLayer fixture;
  fixture.press(200.0, 200.0);
  fixture.move(202.0, 200.0, Modifiers{.control = true});

  EXPECT_GT(fixture.monitor.transform()->x, 0.5);
  EXPECT_TRUE(fixture.monitor.guides().empty());
}

TEST(TransformSnapping, TurnedOffEntirelyNothingSnaps) {
  WithLayer fixture;
  fixture.monitor.set_snapping(false);
  fixture.press(200.0, 200.0);
  fixture.move(202.0, 200.0);

  EXPECT_GT(fixture.monitor.transform()->x, 0.5);
}

// A turned layer's edges are not the edges of anything, so lining one up with
// the frame would be a coincidence rather than an alignment. Its centre is
// still a centre.
TEST(TransformSnapping, ATurnedLayerSnapsByItsCentreOnly) {
  MonitorView monitor;
  monitor.set_canvas_aspect(1.0);
  monitor.set_transform(
      MonitorBox{.x = 0.5, .y = 0.5, .width = 0.5, .height = 0.5, .rotation = 30.0});
  monitor.arrange(kPlaced, flat_context());

  monitor.on_mouse_down(MouseEvent{.x = 200.0, .y = 200.0});
  monitor.on_mouse_move(MouseEvent{.x = 102.0, .y = 200.0});
  // No edge snap: the left edge lands where the pointer put it.
  EXPECT_NEAR(monitor.transform()->x * 400.0, 102.0, 1e-9);

  monitor.on_mouse_move(MouseEvent{.x = 202.0, .y = 200.0});
  EXPECT_NEAR(monitor.transform()->x, 0.5, 1e-9);
}

TEST(TransformSnapping, TheGuidesGoWhenTheDragDoes) {
  WithLayer fixture;
  fixture.press(200.0, 200.0);
  fixture.move(202.0, 200.0);
  ASSERT_FALSE(fixture.monitor.guides().empty());

  fixture.release(202.0, 200.0);
  EXPECT_TRUE(fixture.monitor.guides().empty());
}

// ------------------------------------------------------------- the drawing --

TEST(TransformOverlay, NothingIsDrawnWithoutALayer) {
  MonitorView monitor;
  monitor.set_canvas_aspect(1.0);
  monitor.arrange(kPlaced, flat_context());
  const Pixels pixels(400, 400);
  monitor.set_frame(pixels.view());

  RecordingPainter painter;
  monitor.paint(painter, default_theme());
  const std::size_t bare = painter.count(DrawCall::Kind::Line);

  monitor.set_transform(MonitorBox{});
  RecordingPainter with;
  monitor.paint(with, default_theme());
  EXPECT_GT(with.count(DrawCall::Kind::Line), bare);
}

TEST(TransformOverlay, SelectingAndThenNotLeavesNoHandlesBehind) {
  WithLayer fixture;
  fixture.monitor.set_transform(std::nullopt);
  EXPECT_EQ(fixture.monitor.handle_at(200.0, 200.0), TransformHandle::None);
  EXPECT_FALSE(fixture.monitor.transform().has_value());
}

}  // namespace
}  // namespace cutline::ui
