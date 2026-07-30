/// Choosing a colour.
///
/// Most of the weight here is on one thing: HSV is a lossy view of a colour,
/// and a picker that forgets that throws away the coordinate somebody just
/// chose. Dragging the value to black and back must return the same hue, and
/// nothing about the conversion alone can promise that — only the picker
/// holding its own coordinates can.

#include "cutline/ui/color_picker.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace cutline::ui {
namespace {

const RecordingPainter& measurer() {
  static const RecordingPainter shared;
  return shared;
}

[[nodiscard]] LayoutContext flat_context() {
  return LayoutContext{default_theme(), measurer()};
}

[[nodiscard]] MouseEvent press(double x, double y) {
  return MouseEvent{.x = x, .y = y, .button = MouseButton::Left};
}

[[nodiscard]] KeyEvent key(Key which, Modifiers modifiers = {}) {
  return KeyEvent{.key = which, .modifiers = modifiers};
}

/// A picker laid out at a known size, so its regions have real coordinates.
[[nodiscard]] std::unique_ptr<ColorPicker> laid_out(Color color, bool alpha = true) {
  auto picker = std::make_unique<ColorPicker>(color);
  picker->set_alpha_enabled(alpha);
  const LayoutContext context = flat_context();
  const LayoutItem width = picker->sizing(Axis::Horizontal, context);
  const LayoutItem height = picker->sizing(Axis::Vertical, context);
  picker->arrange(Rect{0.0, 0.0, width.basis, height.basis}, context);
  return picker;
}

constexpr double kEps = 1e-6;

void expect_color_near(const Color& got, const Color& want, float tolerance = 0.01f) {
  EXPECT_NEAR(got.r, want.r, tolerance);
  EXPECT_NEAR(got.g, want.g, tolerance);
  EXPECT_NEAR(got.b, want.b, tolerance);
  EXPECT_NEAR(got.a, want.a, tolerance);
}

// ------------------------------------------------------------ conversion --

TEST(Hsv, ConvertsThePrimaries) {
  EXPECT_NEAR(to_hsv(Color{1.0f, 0.0f, 0.0f, 1.0f}).h, 0.0, kEps);
  EXPECT_NEAR(to_hsv(Color{0.0f, 1.0f, 0.0f, 1.0f}).h, 120.0, kEps);
  EXPECT_NEAR(to_hsv(Color{0.0f, 0.0f, 1.0f, 1.0f}).h, 240.0, kEps);
  EXPECT_NEAR(to_hsv(Color{1.0f, 1.0f, 0.0f, 1.0f}).h, 60.0, kEps);
  EXPECT_NEAR(to_hsv(Color{0.0f, 1.0f, 1.0f, 1.0f}).h, 180.0, kEps);
  EXPECT_NEAR(to_hsv(Color{1.0f, 0.0f, 1.0f, 1.0f}).h, 300.0, kEps);
}

TEST(Hsv, RoundTripsEveryColourItCanRepresent) {
  // Every eighth of the cube, which covers the six sectors and both ends.
  for (int r = 0; r <= 8; ++r) {
    for (int g = 0; g <= 8; ++g) {
      for (int b = 0; b <= 8; ++b) {
        const Color original{static_cast<float>(r) / 8.0f, static_cast<float>(g) / 8.0f,
                             static_cast<float>(b) / 8.0f, 1.0f};
        expect_color_near(from_hsv(to_hsv(original)), original, 1e-5f);
      }
    }
  }
}

TEST(Hsv, GreyHasNoHueAndNoSaturation) {
  const Hsv grey = to_hsv(Color{0.5f, 0.5f, 0.5f, 1.0f});
  EXPECT_DOUBLE_EQ(grey.h, 0.0);
  EXPECT_DOUBLE_EQ(grey.s, 0.0);
  EXPECT_NEAR(grey.v, 0.5, kEps);
}

TEST(Hsv, HueWrapsRatherThanClamping) {
  // 360 is 0, and so is -360. A picker dragged past the end of the strip must
  // come back round rather than sticking at magenta.
  expect_color_near(from_hsv(Hsv{360.0, 1.0, 1.0}), from_hsv(Hsv{0.0, 1.0, 1.0}));
  expect_color_near(from_hsv(Hsv{-120.0, 1.0, 1.0}), from_hsv(Hsv{240.0, 1.0, 1.0}));
}

TEST(Hsv, SaturationAndValueAreClamped) {
  expect_color_near(from_hsv(Hsv{0.0, 5.0, 5.0}), Color{1.0f, 0.0f, 0.0f, 1.0f});
  expect_color_near(from_hsv(Hsv{0.0, -1.0, -1.0}), Color{0.0f, 0.0f, 0.0f, 1.0f});
}

TEST(Hsv, AlphaIsCarriedThroughUntouched) {
  EXPECT_NEAR(from_hsv(Hsv{200.0, 0.5, 0.5}, 0.25f).a, 0.25f, 1e-6f);
}

// ---------------------------------------------------------- the lossy bit --

TEST(ColorPicker, KeepsItsHueThroughBlack) {
  // The bug this design exists to avoid. Drag the value to zero and the colour
  // is black, which has no hue in it at all; a picker that re-read its
  // coordinates from the colour would come back red.
  ColorPicker picker{from_hsv(Hsv{240.0, 1.0, 1.0})};
  picker.set_color(Color{0.0f, 0.0f, 0.0f, 1.0f});

  EXPECT_NEAR(picker.hsv().h, 240.0, kEps);
  EXPECT_NEAR(picker.hsv().s, 1.0, kEps);
  EXPECT_NEAR(picker.hsv().v, 0.0, kEps);

  picker.set_hsv(Hsv{picker.hsv().h, picker.hsv().s, 1.0});
  expect_color_near(picker.color(), Color{0.0f, 0.0f, 1.0f, 1.0f});
}

TEST(ColorPicker, KeepsItsHueThroughGrey) {
  ColorPicker picker{from_hsv(Hsv{120.0, 1.0, 1.0})};
  picker.set_color(Color{0.5f, 0.5f, 0.5f, 1.0f});

  EXPECT_NEAR(picker.hsv().h, 120.0, kEps);
  EXPECT_DOUBLE_EQ(picker.hsv().s, 0.0);
  EXPECT_NEAR(picker.hsv().v, 0.5, kEps);
}

TEST(ColorPicker, ACoulourWithHueInItReplacesTheRememberedOne) {
  // The preservation is only for the coordinates a colour cannot carry. One
  // that can must win, or setting the colour would not work at all.
  ColorPicker picker{from_hsv(Hsv{240.0, 1.0, 1.0})};
  picker.set_color(Color{1.0f, 0.0f, 0.0f, 1.0f});
  EXPECT_NEAR(picker.hsv().h, 0.0, kEps);
  EXPECT_NEAR(picker.hsv().s, 1.0, kEps);
}

TEST(ColorPicker, SettingAColourDoesNotCallBack) {
  ColorPicker picker;
  int changes = 0;
  picker.set_on_change([&changes](const Color&) { ++changes; });
  picker.set_color(Color{0.2f, 0.4f, 0.6f, 1.0f});
  picker.set_hsv(Hsv{10.0, 0.5, 0.5});
  EXPECT_EQ(changes, 0);
}

// -------------------------------------------------------------- regions --

TEST(ColorPicker, LaysTheSquareAndTheStripsOutSideBySide) {
  const auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});

  const Rect square = picker->field();
  const Rect hue = picker->hue_strip();
  const Rect alpha = picker->alpha_strip();

  EXPECT_FALSE(square.empty());
  EXPECT_FALSE(hue.empty());
  EXPECT_FALSE(alpha.empty());

  EXPECT_GT(hue.x, square.right());
  EXPECT_GT(alpha.x, hue.right());
  // The strips run the full height of the square, which is what makes the
  // whole of the hue wheel reachable in one drag.
  EXPECT_DOUBLE_EQ(hue.y, square.y);
  EXPECT_DOUBLE_EQ(hue.height, square.height);
  EXPECT_LE(alpha.right(), picker->bounds().right());
}

TEST(ColorPicker, WithoutAlphaThereIsNoAlphaStripAndNoRoomTakenForOne) {
  const auto with = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f}, true);
  const auto without = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f}, false);

  EXPECT_TRUE(without->alpha_strip().empty());
  EXPECT_LT(without->bounds().width, with->bounds().width);
  // The square is the same size either way: the strip's absence is width the
  // popup gives up, not width the square swallows.
  EXPECT_DOUBLE_EQ(without->field().width, with->field().width);
}

TEST(ColorPicker, TheHexFieldSitsUnderTheSquare) {
  const auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  ASSERT_EQ(picker->children().size(), 1u);

  const Widget& hex = *picker->children()[0];
  EXPECT_GE(hex.bounds().y, picker->field().bottom());
  EXPECT_LE(hex.bounds().bottom(), picker->bounds().bottom());
  EXPECT_TRUE(hex.wants_text());
}

TEST(ColorPicker, TheHexFieldShowsTheColour) {
  const auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const auto* hex = dynamic_cast<const TextField*>(picker->children()[0].get());
  ASSERT_NE(hex, nullptr);
  EXPECT_EQ(hex->text(), "#ff0000");
}

// -------------------------------------------------------------- dragging --

TEST(ColorPicker, PressingTheSquareSetsSaturationAndValue) {
  auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const Rect square = picker->field();

  // Top right is full saturation at full value: the pure hue.
  picker->on_mouse_down(press(square.right() - 1.0, square.y + 1.0));
  EXPECT_NEAR(picker->hsv().s, 1.0, 0.02);
  EXPECT_NEAR(picker->hsv().v, 1.0, 0.02);

  // Bottom left is black, whatever the hue.
  picker->on_mouse_down(press(square.x, square.bottom() - 1.0));
  EXPECT_NEAR(picker->hsv().s, 0.0, 0.02);
  EXPECT_NEAR(picker->hsv().v, 0.0, 0.02);
}

TEST(ColorPicker, DraggingTheHueStripRunsRightRoundTheWheel) {
  auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const Rect hue = picker->hue_strip();

  picker->on_mouse_down(press(hue.x + 2.0, hue.y));
  EXPECT_NEAR(picker->hsv().h, 0.0, 2.0);

  picker->on_mouse_move(press(hue.x + 2.0, hue.y + hue.height * 0.5));
  EXPECT_NEAR(picker->hsv().h, 180.0, 2.0);

  picker->on_mouse_move(press(hue.x + 2.0, hue.bottom()));
  EXPECT_NEAR(picker->hsv().h, 360.0, 2.0);
}

TEST(ColorPicker, DraggingTheHueDoesNotDisturbSaturationOrValue) {
  auto picker = laid_out(from_hsv(Hsv{0.0, 0.4, 0.6}));
  const Rect hue = picker->hue_strip();

  picker->on_mouse_down(press(hue.x + 2.0, hue.y + hue.height * 0.25));
  EXPECT_NEAR(picker->hsv().s, 0.4, 0.01);
  EXPECT_NEAR(picker->hsv().v, 0.6, 0.01);
}

TEST(ColorPicker, TheAlphaStripRunsOpaqueToTransparent) {
  auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const Rect strip = picker->alpha_strip();

  picker->on_mouse_down(press(strip.x + 2.0, strip.y));
  EXPECT_NEAR(picker->alpha(), 1.0f, 0.02f);

  picker->on_mouse_move(press(strip.x + 2.0, strip.bottom()));
  EXPECT_NEAR(picker->alpha(), 0.0f, 0.02f);
}

TEST(ColorPicker, ADragBelongsToTheRegionItStartedIn) {
  // Sliding off the square onto the hue strip keeps setting saturation. The
  // alternative — the pointer changing what it means halfway through a gesture
  // — throws away the hue every time somebody drags to the right-hand edge.
  auto picker = laid_out(from_hsv(Hsv{200.0, 0.5, 0.5}));
  const Rect square = picker->field();
  const Rect hue = picker->hue_strip();

  picker->on_mouse_down(press(square.x + square.width * 0.5, square.y + square.height * 0.5));
  picker->on_mouse_move(press(hue.x + 2.0, hue.y + hue.height * 0.9));

  // Not `kEps`: the colour it was built from is float, so the hue comes back a
  // millionth of a degree off. What matters is that it did not move 120.
  EXPECT_NEAR(picker->hsv().h, 200.0, 1e-3);
  EXPECT_NEAR(picker->hsv().s, 1.0, 0.05);
}

TEST(ColorPicker, ADragOutsideTheWidgetIsClampedRatherThanIgnored) {
  auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const Rect square = picker->field();

  picker->on_mouse_down(press(square.x + 10.0, square.y + 10.0));
  picker->on_mouse_move(press(square.x - 500.0, square.y + 800.0));

  EXPECT_DOUBLE_EQ(picker->hsv().s, 0.0);
  EXPECT_DOUBLE_EQ(picker->hsv().v, 0.0);
}

TEST(ColorPicker, ReportsEveryChangeButCommitsOncePerDrag) {
  auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const Rect square = picker->field();

  int changes = 0;
  int commits = 0;
  picker->set_on_change([&changes](const Color&) { ++changes; });
  picker->set_on_commit([&commits](const Color&) { ++commits; });

  picker->on_mouse_down(press(square.x + 10.0, square.y + 10.0));
  picker->on_mouse_move(press(square.x + 20.0, square.y + 10.0));
  picker->on_mouse_move(press(square.x + 30.0, square.y + 10.0));
  picker->on_mouse_up(press(square.x + 30.0, square.y + 10.0));

  EXPECT_EQ(changes, 3);
  EXPECT_EQ(commits, 1);
}

TEST(ColorPicker, AGestureThatChangesNothingCommitsNothing) {
  auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const Rect square = picker->field();
  const MouseEvent middle = press(square.x + square.width * 0.5,
                                  square.y + square.height * 0.5);

  // The first click puts the colour exactly where the second one would, which
  // is the only way to state this without a rounding tolerance.
  picker->on_mouse_down(middle);
  picker->on_mouse_up(middle);

  int commits = 0;
  picker->set_on_commit([&commits](const Color&) { ++commits; });
  picker->on_mouse_down(middle);
  picker->on_mouse_up(middle);
  EXPECT_EQ(commits, 0);
}

TEST(ColorPicker, APressOnItsOwnPaddingIsSwallowed) {
  // Handled but inert. Unhandled, the host would read it as a press outside
  // the popup and close the picker the moment somebody clipped its edge.
  auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});
  const Color before = picker->color();

  EXPECT_TRUE(picker->on_mouse_down(press(1.0, 1.0)));
  EXPECT_EQ(picker->color(), before);
}

// -------------------------------------------------------------- keyboard --

TEST(ColorPicker, ArrowsMoveAroundTheSquare) {
  auto picker = laid_out(from_hsv(Hsv{180.0, 0.5, 0.5}));

  picker->on_key_down(key(Key::Right));
  EXPECT_GT(picker->hsv().s, 0.5);
  picker->on_key_down(key(Key::Left));
  EXPECT_NEAR(picker->hsv().s, 0.5, kEps);

  picker->on_key_down(key(Key::Up));
  EXPECT_GT(picker->hsv().v, 0.5);
  picker->on_key_down(key(Key::Down));
  EXPECT_NEAR(picker->hsv().v, 0.5, kEps);

  // And none of it touched the hue.
  EXPECT_NEAR(picker->hsv().h, 180.0, kEps);
}

TEST(ColorPicker, ShiftMovesFurther) {
  auto fine = laid_out(from_hsv(Hsv{0.0, 0.5, 0.5}));
  auto coarse = laid_out(from_hsv(Hsv{0.0, 0.5, 0.5}));

  fine->on_key_down(key(Key::Right));
  coarse->on_key_down(key(Key::Right, Modifiers{.shift = true}));
  EXPECT_GT(coarse->hsv().s - 0.5, fine->hsv().s - 0.5);
}

TEST(ColorPicker, ControlWithTheArrowsReachesTheHue) {
  // The one coordinate the square cannot express, so the keyboard needs
  // somewhere else to put it.
  auto picker = laid_out(from_hsv(Hsv{180.0, 0.5, 0.5}));

  picker->on_key_down(key(Key::Right, Modifiers{.control = true}));
  EXPECT_GT(picker->hsv().h, 180.0);
  EXPECT_NEAR(picker->hsv().s, 0.5, kEps);
}

TEST(ColorPicker, AKeyPressIsAWholeGesture) {
  // There is no release to wait for, so a press has to change and commit.
  auto picker = laid_out(from_hsv(Hsv{0.0, 0.5, 0.5}));

  int changes = 0;
  int commits = 0;
  picker->set_on_change([&changes](const Color&) { ++changes; });
  picker->set_on_commit([&commits](const Color&) { ++commits; });

  picker->on_key_down(key(Key::Up));
  EXPECT_EQ(changes, 1);
  EXPECT_EQ(commits, 1);
}

TEST(ColorPicker, AKeyAtTheEndOfARangeReportsNothing) {
  auto picker = laid_out(from_hsv(Hsv{0.0, 0.0, 1.0}));

  int commits = 0;
  picker->set_on_commit([&commits](const Color&) { ++commits; });

  picker->on_key_down(key(Key::Left));  // already at zero saturation
  picker->on_key_down(key(Key::Up));    // already at full value
  EXPECT_EQ(commits, 0);
}

// -------------------------------------------------------------- painting --

TEST(ColorPicker, DrawsTheSquareAsAnImageOfItsOwnSize) {
  const auto picker = laid_out(Color{1.0f, 0.0f, 0.0f, 1.0f});

  RecordingPainter painter;
  picker->paint(painter, default_theme());

  const DrawCall* image = painter.first(DrawCall::Kind::Image);
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(image->bounds, picker->field());
  EXPECT_EQ(image->image.width, static_cast<int>(std::lround(picker->field().width)));
  EXPECT_EQ(image->image.height, static_cast<int>(std::lround(picker->field().height)));
  EXPECT_TRUE(painter.clips_balanced());
}

TEST(ColorPicker, TheSquareIsTheHueAtEveryCorner) {
  // Read the pixels the widget actually generated rather than recomputing
  // them: this is what catches the axes being the wrong way round.
  const auto picker = laid_out(from_hsv(Hsv{120.0, 1.0, 1.0}));

  RecordingPainter painter;
  picker->paint(painter, default_theme());
  const DrawCall* image = painter.first(DrawCall::Kind::Image);
  ASSERT_NE(image, nullptr);

  const auto at = [&](int x, int y) {
    const std::uint8_t* p = image->image.pixels + (static_cast<std::size_t>(y) *
                                                   image->image.row_bytes()) +
                            static_cast<std::size_t>(x) * 4u;
    return Color{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
  };
  const int last_x = image->image.width - 1;
  const int last_y = image->image.height - 1;

  expect_color_near(at(last_x, 0), Color{0.0f, 1.0f, 0.0f, 1.0f}, 0.02f);  // pure green
  expect_color_near(at(0, 0), Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.02f);       // white
  expect_color_near(at(0, last_y), Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.02f);  // black
  expect_color_near(at(last_x, last_y), Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.02f);
}

TEST(ColorPicker, RedrawsTheSquareWhenTheHueChangesAndNotOtherwise) {
  auto picker = laid_out(from_hsv(Hsv{0.0, 1.0, 1.0}));
  const auto corner = [&picker] {
    RecordingPainter painter;
    picker->paint(painter, default_theme());
    const DrawCall* image = painter.first(DrawCall::Kind::Image);
    const std::uint8_t* p = image->image.pixels +
                            static_cast<std::size_t>(image->image.width - 1) * 4u;
    return Color{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1.0f};
  };

  expect_color_near(corner(), Color{1.0f, 0.0f, 0.0f, 1.0f}, 0.02f);
  picker->set_hsv(Hsv{240.0, 1.0, 1.0});
  expect_color_near(corner(), Color{0.0f, 0.0f, 1.0f, 1.0f}, 0.02f);
}

// --------------------------------------------------------------- swatch --

TEST(ColorSwatch, ShowsItsColourAndItsHex) {
  ColorSwatch swatch{Color{0.0f, 0.5f, 1.0f, 1.0f}};
  swatch.arrange(Rect{0.0, 0.0, 140.0, 24.0}, flat_context());

  RecordingPainter painter;
  swatch.paint(painter, default_theme());

  bool has_block = false;
  for (const DrawCall& call : painter.calls()) {
    if (call.kind == DrawCall::Kind::Fill && call.fill.color == swatch.color()) has_block = true;
  }
  EXPECT_TRUE(has_block);

  const DrawCall* text = painter.first(DrawCall::Kind::Text);
  ASSERT_NE(text, nullptr);
  ASSERT_TRUE(text->run.has_value());
  EXPECT_EQ(text->run->text, "#0080ff");
}

TEST(ColorSwatch, TheBlockSitsInsideTheControl) {
  ColorSwatch swatch;
  swatch.arrange(Rect{10.0, 20.0, 140.0, 24.0}, flat_context());

  const Rect block = swatch.block();
  EXPECT_GE(block.x, 10.0);
  EXPECT_GE(block.y, 20.0);
  EXPECT_LE(block.bottom(), 44.0);
  EXPECT_FALSE(block.empty());
}

TEST(ColorSwatch, IsWideEnoughForAnAlphaHexEvenWhenOpaque) {
  // Otherwise the control changes width the moment a colour is dragged towards
  // transparent, which moves everything else on the row.
  const LayoutContext context = flat_context();
  ColorSwatch opaque{Color{1.0f, 1.0f, 1.0f, 1.0f}};
  ColorSwatch translucent{Color{1.0f, 1.0f, 1.0f, 0.5f}};

  EXPECT_DOUBLE_EQ(opaque.sizing(Axis::Horizontal, context).basis,
                   translucent.sizing(Axis::Horizontal, context).basis);
}

TEST(ColorSwatch, OpensAPickerOnTheHostsPopupLayer) {
  auto root = std::make_unique<Box>(Axis::Horizontal);
  auto& swatch = root->emplace<ColorSwatch>(Color{1.0f, 0.0f, 0.0f, 1.0f});
  WidgetHost host{std::move(root)};
  host.resize(Rect{0.0, 0.0, 600.0, 400.0}, flat_context());

  EXPECT_FALSE(swatch.is_open());
  host.mouse_down(press(swatch.bounds().x + 4.0, swatch.bounds().y + 4.0));
  host.update_layout(flat_context());

  EXPECT_TRUE(swatch.is_open());
  ASSERT_NE(host.popup(), nullptr);
  EXPECT_NE(dynamic_cast<ColorPicker*>(host.popup()), nullptr);
}

TEST(ColorSwatch, ThePickerStartsOnTheSwatchsColour) {
  auto root = std::make_unique<Widget>();
  auto& swatch = root->emplace<ColorSwatch>(from_hsv(Hsv{200.0, 0.6, 0.8}));
  WidgetHost host{std::move(root)};
  host.resize(Rect{0.0, 0.0, 600.0, 400.0}, flat_context());

  swatch.open();
  host.update_layout(flat_context());

  const auto* picker = dynamic_cast<const ColorPicker*>(host.popup());
  ASSERT_NE(picker, nullptr);
  EXPECT_NEAR(picker->hsv().h, 200.0, 0.5);
  EXPECT_NEAR(picker->hsv().s, 0.6, 0.01);
}

TEST(ColorSwatch, TakesTheColourThePickerReports) {
  auto root = std::make_unique<Widget>();
  auto& swatch = root->emplace<ColorSwatch>(Color{1.0f, 0.0f, 0.0f, 1.0f});
  WidgetHost host{std::move(root)};
  host.resize(Rect{0.0, 0.0, 600.0, 400.0}, flat_context());

  Color committed{};
  int commits = 0;
  swatch.set_on_commit([&](const Color& color) {
    committed = color;
    ++commits;
  });

  swatch.open();
  host.update_layout(flat_context());
  auto* picker = dynamic_cast<ColorPicker*>(host.popup());
  ASSERT_NE(picker, nullptr);

  const Rect square = picker->field();
  host.mouse_down(press(square.x, square.bottom() - 1.0));
  host.mouse_up(press(square.x, square.bottom() - 1.0));

  EXPECT_EQ(commits, 1);
  expect_color_near(committed, Color{0.0f, 0.0f, 0.0f, 1.0f}, 0.02f);
  expect_color_near(swatch.color(), committed);
}

TEST(ColorSwatch, ClosesItsPickerWhenItGoesAway) {
  // The picker holds callbacks capturing the swatch. A panel rebuilding under
  // an open picker is the ordinary case, not an exotic one.
  auto root = std::make_unique<Widget>();
  auto& holder = root->emplace<Widget>();
  auto& swatch = holder.emplace<ColorSwatch>();
  WidgetHost host{std::move(root)};
  host.resize(Rect{0.0, 0.0, 600.0, 400.0}, flat_context());

  swatch.open();
  host.update_layout(flat_context());
  ASSERT_TRUE(host.popup_open());

  holder.clear_children();
  EXPECT_FALSE(host.popup_open());
  host.update_layout(flat_context());
  EXPECT_EQ(host.popup(), nullptr);
}

TEST(ColorSwatch, APressOutsideTheOpenPickerClosesIt) {
  auto root = std::make_unique<Widget>();
  auto& swatch = root->emplace<ColorSwatch>();
  WidgetHost host{std::move(root)};
  host.resize(Rect{0.0, 0.0, 600.0, 400.0}, flat_context());

  swatch.open();
  host.update_layout(flat_context());
  ASSERT_TRUE(host.popup_open());

  host.mouse_down(press(590.0, 390.0));
  EXPECT_FALSE(host.popup_open());
}

// ---------------------------------------------------------- chequerboard --

TEST(Checkerboard, FillsItsBoundsAndBalancesItsClip) {
  RecordingPainter painter;
  paint_checkerboard(painter, Rect{0.0, 0.0, 20.0, 20.0}, 0.0, 5.0);

  EXPECT_TRUE(painter.clips_balanced());
  // One base fill plus the dark squares of a four-by-four board.
  EXPECT_EQ(painter.count(DrawCall::Kind::Fill), 1u + 8u);
}

TEST(Checkerboard, NeverDrawsOutsideItsBounds) {
  // The partial squares along the far edge are the part that gets this wrong.
  const Rect bounds{3.0, 7.0, 13.0, 11.0};
  RecordingPainter painter;
  paint_checkerboard(painter, bounds, 0.0, 5.0);

  for (const DrawCall& call : painter.calls()) {
    if (call.kind != DrawCall::Kind::Fill) continue;
    EXPECT_GE(call.bounds.x, bounds.x);
    EXPECT_GE(call.bounds.y, bounds.y);
    EXPECT_LE(call.bounds.right(), bounds.right());
    EXPECT_LE(call.bounds.bottom(), bounds.bottom());
  }
}

}  // namespace
}  // namespace cutline::ui
