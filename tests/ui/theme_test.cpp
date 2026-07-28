/// The theme model, and whether it can express what was asked of it.
///
/// The requirement was themes that change *chrome*, not merely colours. That is
/// a testable claim: if every built-in theme differed only in its palette, the
/// checks at the bottom of this file would fail.

#include "cutline/ui/theme.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace cutline::ui {
namespace {

// ------------------------------------------------------------------ colour --

TEST(Color, ParsesTheUsualHexForms) {
  EXPECT_EQ(parse_color("#ff0000"), (Color{1.0f, 0.0f, 0.0f, 1.0f}));
  EXPECT_EQ(parse_color("ff0000"), (Color{1.0f, 0.0f, 0.0f, 1.0f}));
  EXPECT_EQ(parse_color("#f00"), (Color{1.0f, 0.0f, 0.0f, 1.0f}));
}

TEST(Color, ParsesAnAlphaChannel) {
  const Color c = parse_color("#00ff0080");
  EXPECT_FLOAT_EQ(c.g, 1.0f);
  EXPECT_NEAR(c.a, 0.5f, 0.01f);
}

TEST(Color, ShorthandExpandsEachDigit) {
  // #abc is #aabbcc, not #a0b0c0.
  EXPECT_EQ(parse_color("#abc"), parse_color("#aabbcc"));
}

TEST(Color, NonsenseFallsBackRatherThanFailing) {
  // A malformed theme file should degrade, not refuse to open.
  const Color fallback{0.5f, 0.5f, 0.5f, 1.0f};
  EXPECT_EQ(parse_color("", fallback), fallback);
  EXPECT_EQ(parse_color("#12345", fallback), fallback);
  EXPECT_EQ(parse_color("#gggggg", fallback), fallback);
  EXPECT_EQ(parse_color("rebeccapurple", fallback), fallback);
}

TEST(Color, RoundTripsThroughHex) {
  for (const std::string text : {"#000000", "#ffffff", "#3f9fdf", "#ffb00080"}) {
    EXPECT_EQ(to_hex(parse_color(text)), text);
  }
}

TEST(Color, OpaqueColoursDropTheAlphaDigits) {
  EXPECT_EQ(to_hex(Color{1.0f, 0.0f, 0.0f, 1.0f}), "#ff0000");
}

// ------------------------------------------------------------------- names --

TEST(ThemeNames, PartsRoundTrip) {
  for (int i = 0; i <= static_cast<int>(Part::ScrollThumb); ++i) {
    const auto part = static_cast<Part>(i);
    const auto back = part_from_string(to_string(part));
    ASSERT_TRUE(back.has_value()) << to_string(part);
    EXPECT_EQ(*back, part);
  }
}

TEST(ThemeNames, StatesRoundTrip) {
  for (int i = 0; i <= static_cast<int>(State::Focused); ++i) {
    const auto state = static_cast<State>(i);
    const auto back = state_from_string(to_string(state));
    ASSERT_TRUE(back.has_value()) << to_string(state);
    EXPECT_EQ(*back, state);
  }
}

TEST(ThemeNames, PartNamesAreDistinct) {
  // Two parts sharing a name would silently collide when a theme file is read.
  std::set<std::string_view> seen;
  for (int i = 0; i <= static_cast<int>(Part::ScrollThumb); ++i) {
    EXPECT_TRUE(seen.insert(to_string(static_cast<Part>(i))).second)
        << "duplicate name " << to_string(static_cast<Part>(i));
  }
}

TEST(ThemeNames, AnUnknownNameIsRejected) {
  EXPECT_FALSE(part_from_string("hyperbutton").has_value());
  EXPECT_FALSE(state_from_string("smug").has_value());
}

// ------------------------------------------------------------------ lookup --

TEST(ThemeLookup, AnEmptyThemeReturnsItsFallback) {
  Theme t;
  t.fallback.text = parse_color("#123456");
  EXPECT_EQ(t.style(Part::Button, State::Hover).text, parse_color("#123456"));
}

TEST(ThemeLookup, AnExactMatchWins) {
  Theme t;
  t.set(Part::Button, State::Normal, SurfaceStyle{.text = parse_color("#111111")});
  t.set(Part::Button, State::Hover, SurfaceStyle{.text = parse_color("#222222")});
  EXPECT_EQ(t.style(Part::Button, State::Hover).text, parse_color("#222222"));
}

TEST(ThemeLookup, AMissingStateFallsBackToNormal) {
  // Most parts look the same hovered as at rest, so this is the common case
  // rather than an omission — it is what keeps a theme file to a readable size.
  Theme t;
  t.set(Part::Button, State::Normal, SurfaceStyle{.text = parse_color("#111111")});
  EXPECT_EQ(t.style(Part::Button, State::Pressed).text, parse_color("#111111"));
}

TEST(ThemeLookup, AMissingPartFallsBackToTheTheme) {
  Theme t;
  t.fallback.text = parse_color("#999999");
  t.set(Part::Button, State::Normal, SurfaceStyle{.text = parse_color("#111111")});
  EXPECT_EQ(t.style(Part::Tooltip).text, parse_color("#999999"));
}

TEST(ThemeLookup, AStateWithoutANormalStillFallsBack) {
  Theme t;
  t.fallback.text = parse_color("#999999");
  t.set(Part::Button, State::Hover, SurfaceStyle{.text = parse_color("#111111")});
  EXPECT_EQ(t.style(Part::Button, State::Pressed).text, parse_color("#999999"));
}

TEST(ThemeLookup, DefinesReportsWhatIsActuallySet) {
  // A theme editor needs to distinguish "set to the same value" from "inherited".
  Theme t;
  t.set(Part::Button, State::Normal, SurfaceStyle{});
  EXPECT_TRUE(t.defines(Part::Button, State::Normal));
  EXPECT_FALSE(t.defines(Part::Button, State::Hover));
  EXPECT_FALSE(t.defines(Part::Tooltip, State::Normal));
}

TEST(ThemeLookup, SetReplacesAnExistingEntry) {
  Theme t;
  t.set(Part::Button, State::Normal, SurfaceStyle{.text = parse_color("#111111")});
  t.set(Part::Button, State::Normal, SurfaceStyle{.text = parse_color("#222222")});
  EXPECT_EQ(t.style(Part::Button).text, parse_color("#222222"));
}

// ------------------------------------------------------------- the built-ins --

TEST(BuiltInThemes, TheNamedOnesArePresent) {
  // The specific looks that were asked for.
  EXPECT_NE(built_in_theme("xp"), nullptr) << "Windows XP / Frutiger Aero";
  EXPECT_NE(built_in_theme("aero"), nullptr) << "Windows Vista glass";
  EXPECT_NE(built_in_theme("flat"), nullptr);
  EXPECT_NE(built_in_theme("terminal"), nullptr);
  EXPECT_GE(built_in_themes().size(), 4u);
}

TEST(BuiltInThemes, IdsAreUniqueAndNamed) {
  std::set<std::string> ids;
  for (const Theme& theme : built_in_themes()) {
    EXPECT_FALSE(theme.id.empty());
    EXPECT_FALSE(theme.name.empty()) << theme.id;
    EXPECT_TRUE(ids.insert(theme.id).second) << "duplicate id " << theme.id;
  }
}

TEST(BuiltInThemes, AnUnknownIdIsNull) {
  EXPECT_EQ(built_in_theme("windows-95"), nullptr);
}

TEST(BuiltInThemes, ThereIsADefault) {
  EXPECT_FALSE(default_theme().id.empty());
  EXPECT_NE(built_in_theme(default_theme().id), nullptr);
}

TEST(BuiltInThemes, EveryPartIsStyledEverywhere) {
  // A part no theme styles would be a widget nobody can theme — exactly the
  // thing this design exists to prevent. The fallback covers it, but silently.
  for (const Theme& theme : built_in_themes()) {
    for (int i = 0; i <= static_cast<int>(Part::ScrollThumb); ++i) {
      const auto part = static_cast<Part>(i);
      const SurfaceStyle& style = theme.style(part);
      // Text has to be visible against something; fully transparent text is
      // always a mistake.
      EXPECT_GT(style.text.a, 0.0f) << theme.id << " / " << to_string(part);
    }
  }
}

TEST(BuiltInThemes, EveryButtonRespondsToBeingPressed) {
  // A control that looks identical pressed and at rest feels broken, whatever
  // it looks like.
  for (const Theme& theme : built_in_themes()) {
    EXPECT_NE(theme.style(Part::Button, State::Pressed),
              theme.style(Part::Button, State::Normal))
        << theme.id << " does not change when a button is pressed";
    EXPECT_NE(theme.style(Part::Button, State::Hover),
              theme.style(Part::Button, State::Normal))
        << theme.id << " does not change on hover";
  }
}

TEST(BuiltInThemes, GradientStopsAreOrderedAndInRange) {
  const auto check = [](const Fill& fill, const std::string& where) {
    if (fill.kind != FillKind::Gradient) return;
    ASSERT_GE(fill.stops.size(), 2u) << where << ": a gradient needs two stops";
    float previous = -1.0f;
    for (const GradientStop& stop : fill.stops) {
      EXPECT_GE(stop.at, 0.0f) << where;
      EXPECT_LE(stop.at, 1.0f) << where;
      EXPECT_GE(stop.at, previous) << where << ": stops are out of order";
      previous = stop.at;
    }
  };

  for (const Theme& theme : built_in_themes()) {
    for (const auto& [part, states] : theme.styles) {
      for (const auto& [state, style] : states) {
        check(style.fill,
              theme.id + "/" + std::string(to_string(part)) + "/" + std::string(to_string(state)));
      }
    }
  }
}

TEST(BuiltInThemes, MetricsAreSane) {
  for (const Theme& theme : built_in_themes()) {
    EXPECT_GT(theme.metrics.control_height, 0.0) << theme.id;
    EXPECT_GT(theme.metrics.font_size, 0.0) << theme.id;
    EXPECT_GT(theme.metrics.line_height, 0.5) << theme.id;
    EXPECT_GT(theme.metrics.track_height, 0.0) << theme.id;
    EXPECT_GT(theme.metrics.scrollbar_width, 0.0) << theme.id;
    // A control shorter than its own text would clip it.
    EXPECT_GT(theme.metrics.control_height, theme.metrics.font_size) << theme.id;
  }
}

// -------------------------------------------------- chrome, not just colour --
//
// The requirement, stated as tests. Each of these would fail if the theme
// system could only swap palettes.

TEST(ThemeChrome, MetricsDifferBetweenThemes) {
  // Bevelled chrome needs different spacing than flat chrome. A theme that
  // could only repaint would come out cramped or loose.
  const Theme& xp = *built_in_theme("xp");
  const Theme& flat = *built_in_theme("flat");
  EXPECT_NE(xp.metrics, flat.metrics);
  EXPECT_NE(xp.metrics.scrollbar_width, flat.metrics.scrollbar_width)
      << "XP scrollbars were noticeably chunkier";
}

TEST(ThemeChrome, XpUsesBevelsAndFlatDoesNot) {
  EXPECT_TRUE(built_in_theme("xp")->style(Part::Button).bevel.has_value());
  EXPECT_FALSE(built_in_theme("flat")->style(Part::Button).bevel.has_value());
}

TEST(ThemeChrome, PressingAnXpButtonInvertsItsBevel) {
  // The case a colour-only theme cannot express at all: the light and dark
  // edges swap sides, which is what reads as "pressed" rather than "darker".
  const Theme& xp = *built_in_theme("xp");
  const auto normal = xp.style(Part::Button, State::Normal).bevel;
  const auto pressed = xp.style(Part::Button, State::Pressed).bevel;

  ASSERT_TRUE(normal.has_value());
  ASSERT_TRUE(pressed.has_value());
  EXPECT_FALSE(normal->inset);
  EXPECT_TRUE(pressed->inset);
}

TEST(ThemeChrome, XpGlossHasTheHardStepAtItsMidpoint) {
  // Two stops make a ramp; the plastic look needs a discontinuity partway down,
  // where the surface curves away from the light.
  const Fill& fill = built_in_theme("xp")->style(Part::Button).fill;
  ASSERT_EQ(fill.kind, FillKind::Gradient);
  ASSERT_GE(fill.stops.size(), 4u);

  bool found_step = false;
  for (std::size_t i = 1; i < fill.stops.size(); ++i) {
    const float gap = fill.stops[i].at - fill.stops[i - 1].at;
    if (gap > 0.0f && gap < 0.1f) found_step = true;
  }
  EXPECT_TRUE(found_step) << "no hard step, so this is a plain ramp";
}

TEST(ThemeChrome, AeroUsesGlassAndNoOtherThemeDoes) {
  // Blurring the backdrop is the one fill that cannot be faked with colours,
  // and the reason Vista is on the list.
  const auto uses_glass = [](const Theme& theme) {
    for (const auto& [part, states] : theme.styles) {
      for (const auto& [state, style] : states) {
        if (style.fill.kind == FillKind::Glass) return true;
      }
    }
    return false;
  };

  EXPECT_TRUE(uses_glass(*built_in_theme("aero")));
  EXPECT_FALSE(uses_glass(*built_in_theme("flat")));
  EXPECT_FALSE(uses_glass(*built_in_theme("xp")));
}

TEST(ThemeChrome, AeroGlassActuallyBlurs) {
  const Fill& fill = built_in_theme("aero")->style(Part::TitleBar).fill;
  ASSERT_EQ(fill.kind, FillKind::Glass);
  EXPECT_GT(fill.blur_radius, 0.0) << "glass with no blur is just a translucent fill";
  EXPECT_LT(fill.color.a, 1.0f) << "glass has to be translucent to show a backdrop";
}

TEST(ThemeChrome, AeroTextGlowsAndFlatTextDoesNot) {
  // Dark text on glass is unreadable wherever the backdrop is dark; the halo is
  // what makes Aero legible, and it is not a colour.
  EXPECT_GT(built_in_theme("aero")->style(Part::TitleBar).text_glow, 0.0);
  EXPECT_EQ(built_in_theme("flat")->style(Part::TitleBar).text_glow, 0.0);
}

TEST(ThemeChrome, CornerRadiiDiffer) {
  const double xp = built_in_theme("xp")->style(Part::Panel).corner_radius;
  const double aero = built_in_theme("aero")->style(Part::Panel).corner_radius;
  const double terminal = built_in_theme("terminal")->style(Part::Panel).corner_radius;
  EXPECT_NE(aero, terminal);
  EXPECT_LT(terminal, aero) << "a CRT terminal has square corners";
  EXPECT_LT(xp, aero);
}

TEST(ThemeChrome, TerminalGlowsFromWithin) {
  // Built almost entirely from inner shadows, which exercises that path the way
  // Aero exercises the blur path.
  const Theme& terminal = *built_in_theme("terminal");
  const auto shadow = terminal.style(Part::Panel).shadow;
  ASSERT_TRUE(shadow.has_value());
  EXPECT_TRUE(shadow->inner);
  EXPECT_GT(terminal.style(Part::Panel).text_glow, 0.0);
}

TEST(ThemeChrome, TheThemesAreNotRecoloursOfEachOther) {
  // The summarising claim. If two themes shared every structural property and
  // differed only in colour, this would catch it.
  const auto shape_of = [](const Theme& theme) {
    const SurfaceStyle& button = theme.style(Part::Button);
    return std::tuple{button.fill.kind,
                      button.bevel.has_value(),
                      button.shadow.has_value(),
                      button.corner_radius,
                      button.border_width,
                      theme.metrics.control_height,
                      theme.metrics.padding_x};
  };

  const auto themes = built_in_themes();
  for (std::size_t i = 0; i < themes.size(); ++i) {
    for (std::size_t j = i + 1; j < themes.size(); ++j) {
      EXPECT_NE(shape_of(themes[i]), shape_of(themes[j]))
          << themes[i].id << " and " << themes[j].id << " differ only in colour";
    }
  }
}

}  // namespace
}  // namespace cutline::ui
