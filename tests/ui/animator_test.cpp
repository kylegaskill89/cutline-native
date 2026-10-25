/// The interface's clock, and a control fading through it.
///
/// Time is stepped by hand: a fade measured by sleeping is a test that passes
/// on a quiet machine and fails on a busy one.

#include "cutline/ui/animator.hpp"
#include "cutline/ui/controls.hpp"

#include "cutline/ui/recording_painter.hpp"
#include "cutline/ui/theme.hpp"
#include "cutline/ui/widget.hpp"
#include "cutline/ui/widgets.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

namespace cutline::ui {
namespace {

/// A clock that only moves when told to.
struct Stepped {
  double now = 100.0;
  [[nodiscard]] Animator::Clock clock() {
    return [this] { return now; };
  }
};

const int kOwner = 0;

TEST(Animator, AChannelStartsWhereItIsGoing) {
  // Nothing fades in merely by being drawn for the first time.
  Stepped time;
  Animator animator(time.clock());
  EXPECT_EQ(animator.toward(&kOwner, 0, true, 0.2, 0.2), 1.0f);
  EXPECT_FALSE(animator.running());
}

TEST(Animator, AFadeTakesTheTimeItIsGivenAndThenStops) {
  Stepped time;
  Animator animator(time.clock());
  EXPECT_EQ(animator.toward(&kOwner, 0, false, 0.2, 0.2), 0.0f);

  EXPECT_EQ(animator.toward(&kOwner, 0, true, 0.2, 0.2), 0.0f);
  EXPECT_TRUE(animator.running());

  time.now += 0.1;
  // Halfway in time is halfway, smoothstep being symmetric about its middle.
  EXPECT_NEAR(animator.toward(&kOwner, 0, true, 0.2, 0.2), 0.5f, 1e-5f);
  EXPECT_TRUE(animator.running());

  time.now += 0.1;
  EXPECT_EQ(animator.toward(&kOwner, 0, true, 0.2, 0.2), 1.0f);
  EXPECT_FALSE(animator.running());
}

// A control at rest is not asked about while nothing is painted. The pause
// before the pointer arrives is not part of the fade: counted, it finished
// every fade in its first frame, which is how this was found on screen.
TEST(Animator, ACycleGoesRoundForAsLongAsItIsAskedAbout) {
  Stepped time;
  Animator animator(time.clock());
  int owner = 0;
  EXPECT_FLOAT_EQ(animator.cycle(&owner, channel::kPulse, 2.0), 0.0f);
  time.now += 0.5;
  EXPECT_FLOAT_EQ(animator.cycle(&owner, channel::kPulse, 2.0), 0.25f);
  time.now += 2.0;
  EXPECT_FLOAT_EQ(animator.cycle(&owner, channel::kPulse, 2.0), 0.25f) << "round again";
  EXPECT_TRUE(animator.running());

  // A frame that does not ask lets it go, and the loop with it.
  animator.begin_frame();
  animator.begin_frame();
  EXPECT_FALSE(animator.running());
}

TEST(Animator, TimeAtRestBeforeAChangeIsNotFadeTime) {
  Stepped time;
  Animator animator(time.clock());
  (void)animator.toward(&kOwner, 0, false, 0.2, 0.2);
  time.now += 5.0;
  EXPECT_EQ(animator.toward(&kOwner, 0, true, 0.2, 0.2), 0.0f);
  time.now += 0.1;
  EXPECT_NEAR(animator.toward(&kOwner, 0, true, 0.2, 0.2), 0.5f, 1e-5f);
}

TEST(Animator, ItCanArriveQuicklyAndLeaveSlowly) {
  // Vista's glow: in at once, out over a while.
  Stepped time;
  Animator animator(time.clock());
  (void)animator.toward(&kOwner, 0, true, 0.1, 0.4);

  (void)animator.toward(&kOwner, 0, false, 0.1, 0.4);
  time.now += 0.2;
  EXPECT_NEAR(animator.toward(&kOwner, 0, false, 0.1, 0.4), 0.5f, 1e-5f);
}

TEST(Animator, NoTimeAtAllSnaps) {
  Stepped time;
  Animator animator(time.clock());
  (void)animator.toward(&kOwner, 0, false, 0.0, 0.0);
  EXPECT_EQ(animator.toward(&kOwner, 0, true, 0.0, 0.0), 1.0f);
  EXPECT_FALSE(animator.running());
}

TEST(Animator, WhatWasNotDrawnLastFrameIsForgotten) {
  // A widget that has gone must not keep the loop awake for ever.
  Stepped time;
  Animator animator(time.clock());
  (void)animator.toward(&kOwner, 0, false, 0.2, 0.2);
  (void)animator.toward(&kOwner, 0, true, 0.2, 0.2);
  ASSERT_TRUE(animator.running());

  animator.begin_frame();  // the frame it was drawn in ends
  animator.begin_frame();  // and one in which nobody asked about it
  EXPECT_EQ(animator.size(), 0u);
  EXPECT_FALSE(animator.running());
}

// -------------------------------------------------------------------- mix --

TEST(StyleMix, TheEndsAreTheTwoStylesAndTheMiddleIsBetween) {
  SurfaceStyle a;
  a.fill = Fill::solid(Color{0.0f, 0.0f, 0.0f, 1.0f});
  a.border_width = 1.0;
  SurfaceStyle b;
  b.fill = Fill::solid(Color{1.0f, 0.5f, 0.0f, 1.0f});
  b.border_width = 3.0;

  EXPECT_EQ(mix(a, b, 0.0f), a);
  EXPECT_EQ(mix(a, b, 1.0f), b);
  const SurfaceStyle half = mix(a, b, 0.5f);
  EXPECT_FLOAT_EQ(half.fill.color.r, 0.5f);
  EXPECT_FLOAT_EQ(half.fill.color.g, 0.25f);
  EXPECT_DOUBLE_EQ(half.border_width, 2.0);
}

TEST(StyleMix, AGlowArrivingFromNothingFadesIn) {
  SurfaceStyle rest;
  SurfaceStyle lit;
  lit.shadow = Shadow{.blur = 8.0, .color = Color{0.3f, 0.6f, 1.0f, 0.8f}};
  const SurfaceStyle partway = mix(rest, lit, 0.25f);
  ASSERT_TRUE(partway.shadow.has_value());
  EXPECT_FLOAT_EQ(partway.shadow->color.a, 0.2f);
}

// -------------------------------------------------------- a control fading --

// The Aero button under the pointer: at rest when first drawn, partway a
// moment after the pointer arrives, there once the theme's time has passed --
// and the host says it wants painting for exactly as long as that takes.
TEST(Fading, AerosDefaultButtonPulsesAndOthersHoldStill) {
  // Vista's default button pulsed towards a bluer glow and back every two
  // seconds while it was the default; the other themes' do not move.
  const RecordingPainter measurer;
  for (const char* id : {"aero", "flat"}) {
    const Theme& theme = *built_in_theme(id);
    auto made = std::make_unique<Button>("OK");
    Button* button = made.get();
    button->set_default(true);
    WidgetHost host(std::move(made));
    Stepped time;
    host.animator().set_clock(time.clock());
    host.resize(Rect{0.0, 0.0, 200.0, 40.0}, LayoutContext{theme, measurer});

    RecordingPainter painter;
    host.paint(painter, theme);
    const SurfaceStyle first = button->surface_style(theme);
    time.now += 0.5;
    host.paint(painter, theme);
    const SurfaceStyle later = button->surface_style(theme);
    if (std::string_view(id) == "aero") {
      EXPECT_NE(first, later) << "the pulse should have moved";
      EXPECT_TRUE(host.animating());
    } else {
      EXPECT_EQ(first, later) << id;
    }
  }
}

TEST(Fading, AerosProgressBarShinesNowAndThen) {
  // A light crosses what is done, then is gone until the next time round.
  const Theme& aero = *built_in_theme("aero");
  const RecordingPainter measurer;
  auto made = std::make_unique<ProgressBar>(0.6);
  WidgetHost host(std::move(made));
  Stepped time;
  host.animator().set_clock(time.clock());
  host.resize(Rect{0.0, 0.0, 300.0, 20.0}, LayoutContext{aero, measurer});

  const auto shining = [&] {
    RecordingPainter painter;
    host.paint(painter, aero);
    for (const DrawCall& call : painter.calls()) {
      if (call.kind == DrawCall::Kind::Fill && call.fill.kind == FillKind::Gradient &&
          call.fill.stops.size() == 3 && call.fill.stops.front().color.a == 0.0f) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(shining());
  time.now += aero.metrics.progress_shine_crossing / 2.0;
  EXPECT_TRUE(shining());
  time.now += aero.metrics.progress_shine_crossing;
  EXPECT_FALSE(shining()) << "between crossings";
  EXPECT_TRUE(host.animating()) << "and still waiting for the next";
}

TEST(Fading, AButtonGlowsInOverTheThemesTime) {
  const Theme& aero = *built_in_theme("aero");
  ASSERT_GT(aero.metrics.hover_in, 0.0);
  const RecordingPainter measurer;

  auto made = std::make_unique<Button>("Fade");
  Button* button = made.get();
  WidgetHost host(std::move(made));
  Stepped time;
  host.animator().set_clock(time.clock());
  host.resize(Rect{0.0, 0.0, 200.0, 40.0}, LayoutContext{aero, measurer});

  const SurfaceStyle rest = aero.style(Part::Button, State::Normal);
  const SurfaceStyle lit = aero.style(Part::Button, State::Hover);
  ASSERT_NE(rest, lit) << "the theme has nothing to fade between";

  RecordingPainter painter;
  host.paint(painter, aero);
  EXPECT_EQ(button->surface_style(aero), rest);

  host.mouse_move(MouseEvent{.x = 100.0, .y = 20.0});
  host.paint(painter, aero);
  EXPECT_TRUE(host.animating());

  time.now += aero.metrics.hover_in / 2.0;
  host.paint(painter, aero);
  const SurfaceStyle partway = button->surface_style(aero);
  EXPECT_NE(partway, rest);
  EXPECT_NE(partway, lit);
  EXPECT_TRUE(host.animating());

  time.now += aero.metrics.hover_in;
  host.paint(painter, aero);
  EXPECT_EQ(button->surface_style(aero), lit);
  EXPECT_FALSE(host.animating());
}

// Only the root of a tree knows its host. A button inside a panel inside the
// root has to find it, or it never fades -- which is how every control in the
// application behaved while this test only tried a button at the root.
TEST(Fading, AButtonDeepInTheTreeFadesToo) {
  const Theme& aero = *built_in_theme("aero");
  const RecordingPainter measurer;
  auto panel = std::make_unique<Panel>();
  Button* button = &panel->emplace<Button>("Deep");
  WidgetHost host(std::move(panel));
  Stepped time;
  host.animator().set_clock(time.clock());
  host.resize(Rect{0.0, 0.0, 200.0, 80.0}, LayoutContext{aero, measurer});

  RecordingPainter painter;
  host.paint(painter, aero);
  const Rect where = button->bounds();
  host.mouse_move(MouseEvent{.x = where.x + where.width / 2.0, .y = where.y + where.height / 2.0});
  host.paint(painter, aero);
  time.now += aero.metrics.hover_in / 2.0;
  host.paint(painter, aero);
  EXPECT_TRUE(host.animating());
  EXPECT_NE(button->surface_style(aero), aero.style(Part::Button, State::Hover));
}

TEST(Fading, AThemeThatSnapsNeverAnimates) {
  const Theme& xp = *built_in_theme("xp");
  const RecordingPainter measurer;
  WidgetHost host(std::make_unique<Button>("Snap"));
  host.resize(Rect{0.0, 0.0, 200.0, 40.0}, LayoutContext{xp, measurer});
  RecordingPainter painter;
  host.paint(painter, xp);
  host.mouse_move(MouseEvent{.x = 100.0, .y = 20.0});
  host.paint(painter, xp);
  EXPECT_FALSE(host.animating());
}

}  // namespace
}  // namespace cutline::ui
