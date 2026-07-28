/// The built-in themes.
///
/// These exist to prove the theme model can express real chrome, not to be a
/// palette gallery. Each one is here because it needs something the others do
/// not: XP needs bevels and a hard gradient step, Aero needs a blurred
/// backdrop, Flat needs neither and gets its definition from borders, and
/// Terminal needs an inner glow and a monospace rhythm. If any of them had to
/// be faked, the model would be wrong.

#include "cutline/ui/theme.hpp"

#include <array>

namespace cutline::ui {
namespace {

[[nodiscard]] Color hex(std::string_view text) { return parse_color(text); }

/// A raised bevel with the usual light-above, dark-below arrangement.
[[nodiscard]] Bevel raised(std::string_view light, std::string_view dark, double width = 1.0) {
  return Bevel{.width = width, .light = hex(light), .dark = hex(dark), .inset = false};
}

[[nodiscard]] Bevel sunken(std::string_view light, std::string_view dark, double width = 1.0) {
  return Bevel{.width = width, .light = hex(light), .dark = hex(dark), .inset = true};
}

// ------------------------------------------------- Windows XP / Frutiger Aero --

/// Luna Blue, and the glossy plastic look that came with it.
///
/// The signature is the gradient: a light band, a *hard step* at the midpoint,
/// then a second ramp. That step is what reads as a curved plastic surface
/// catching a light, and it is why two-stop gradients cannot imitate this.
[[nodiscard]] Theme make_xp() {
  Theme t;
  t.id = "xp";
  t.name = "Luna";
  t.dark = false;
  t.accent = hex("#2a5fd6");

  t.metrics.control_height = 23.0;
  t.metrics.padding_x = 12.0;
  t.metrics.padding_y = 3.0;
  t.metrics.spacing = 6.0;
  t.metrics.panel_padding = 6.0;
  t.metrics.title_bar_height = 30.0;
  t.metrics.panel_header_height = 24.0;
  t.metrics.font_size = 12.0;
  t.metrics.small_font_size = 11.0;
  t.metrics.scrollbar_width = 17.0;  // XP scrollbars were chunky
  t.metrics.track_height = 60.0;     // bevelled clips need the room
  t.metrics.audio_track_height = 48.0;
  t.metrics.ruler_height = 22.0;

  t.fallback = SurfaceStyle{
      .fill = Fill::solid(hex("#ece9d8")),
      .border = hex("#a0a0a0"),
      .border_width = 1.0,
      .text = hex("#000000"),
  };

  t.set(Part::Window, State::Normal, SurfaceStyle{.fill = Fill::solid(hex("#ece9d8")),
                                                  .text = hex("#000000")});

  // The Luna title bar: blue, glossy, with the highlight in the top third.
  t.set(Part::TitleBar, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.00f, hex("#0058e6")},
                                    {0.09f, hex("#3593ff")},
                                    {0.18f, hex("#288eff")},
                                    {0.25f, hex("#127dff")},
                                    {0.50f, hex("#036ffc")},
                                    {0.75f, hex("#0262ee")},
                                    {0.90f, hex("#0057e5")},
                                    {1.00f, hex("#0054e3")}}),
            .corner_radius = 8.0,
            .text = hex("#ffffff"),
            .text_glow = 2.0,
        });

  t.set(Part::Panel, State::Normal,
        SurfaceStyle{
            .fill = Fill::solid(hex("#ece9d8")),
            .bevel = raised("#ffffff", "#aca899"),
            .border = hex("#919b9c"),
            .border_width = 1.0,
            .text = hex("#000000"),
        });

  t.set(Part::PanelHeader, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#f8f8f4")}, {1.0f, hex("#d6d3c4")}}),
            .border = hex("#aca899"),
            .border_width = 1.0,
            .text = hex("#0a246a"),
        });

  // The button. Note the hard step at 0.5 -- above it the surface is bright and
  // ramps gently, below it darker and ramps back up, which is the plastic look.
  const SurfaceStyle button{
      .fill = Fill::gradient({{0.00f, hex("#ffffff")},
                              {0.45f, hex("#f2f1e8")},
                              {0.50f, hex("#e3e1d2")},
                              {1.00f, hex("#f5f4ec")}}),
      .bevel = raised("#ffffff", "#9d9a8f"),
      .border = hex("#7b7a70"),
      .border_width = 1.0,
      .corner_radius = 3.0,
      .text = hex("#000000"),
  };
  t.set(Part::Button, State::Normal, button);

  SurfaceStyle button_hover = button;
  button_hover.fill = Fill::gradient({{0.00f, hex("#fffdf4")},
                                      {0.45f, hex("#fff4c8")},
                                      {0.50f, hex("#ffe894")},
                                      {1.00f, hex("#fff2c0")}});
  button_hover.border = hex("#e0a000");
  t.set(Part::Button, State::Hover, button_hover);

  // Pressed inverts the bevel rather than merely darkening the fill. This is
  // the case a colour-only theme cannot express at all.
  SurfaceStyle button_pressed = button;
  button_pressed.fill = Fill::gradient({{0.00f, hex("#d8d5c6")},
                                        {0.50f, hex("#e6e3d5")},
                                        {1.00f, hex("#f2f0e6")}});
  button_pressed.bevel = sunken("#ffffff", "#8a8779");
  t.set(Part::Button, State::Pressed, button_pressed);

  SurfaceStyle button_disabled = button;
  button_disabled.fill = Fill::solid(hex("#ece9d8"));
  button_disabled.bevel = raised("#ffffff", "#d4d0c0");
  button_disabled.text = hex("#aca899");
  t.set(Part::Button, State::Disabled, button_disabled);

  t.set(Part::Input, State::Normal,
        SurfaceStyle{
            .fill = Fill::solid(hex("#ffffff")),
            .bevel = sunken("#ffffff", "#7f9db9"),
            .border = hex("#7f9db9"),
            .border_width = 1.0,
            .text = hex("#000000"),
        });

  t.set(Part::Clip, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#c6d9f1")}, {0.5f, hex("#a8c4e8")},
                                    {1.0f, hex("#8fb3e0")}}),
            .bevel = raised("#ffffff", "#5a7fa8"),
            .border = hex("#4a6d94"),
            .border_width = 1.0,
            .corner_radius = 3.0,
            .text = hex("#0a246a"),
        });
  t.set(Part::Clip, State::Selected,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#ffe9a8")}, {0.5f, hex("#ffd76b")},
                                    {1.0f, hex("#ffc93c")}}),
            .bevel = raised("#ffffff", "#c08a10"),
            .border = hex("#e0a000"),
            .border_width = 2.0,
            .corner_radius = 3.0,
            .text = hex("#3a2a00"),
        });

  t.set(Part::TrackHeader, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#f4f2e8")}, {1.0f, hex("#dedac8")}}),
            .bevel = raised("#ffffff", "#aca899"),
            .border = hex("#aca899"),
            .border_width = 1.0,
            .text = hex("#000000"),
        });

  t.set(Part::Ruler, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ece9d8")),
                     .border = hex("#aca899"),
                     .border_width = 1.0,
                     .text = hex("#000000")});
  t.set(Part::Playhead, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#d02020")), .text = hex("#d02020")});

  t.set(Part::Scrollbar, State::Normal, SurfaceStyle{.fill = Fill::solid(hex("#f1efe2"))});
  t.set(Part::ScrollThumb, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#ffffff")}, {0.5f, hex("#e3e1d2")},
                                    {1.0f, hex("#f5f4ec")}}),
            .bevel = raised("#ffffff", "#9d9a8f"),
            .border = hex("#7b7a70"),
            .border_width = 1.0,
        });

  t.set(Part::Menu, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ffffff")),
                     .shadow = Shadow{.offset_x = 2.0, .offset_y = 2.0, .blur = 4.0,
                                      .color = hex("#00000040")},
                     .border = hex("#aca899"),
                     .border_width = 1.0,
                     .text = hex("#000000")});
  t.set(Part::MenuItem, State::Hover,
        SurfaceStyle{.fill = Fill::solid(hex("#316ac5")), .text = hex("#ffffff")});

  t.set(Part::Tooltip, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ffffe1")),
                     .border = hex("#000000"),
                     .border_width = 1.0,
                     .text = hex("#000000")});

  return t;
}

// ------------------------------------------------------------ Windows Vista --

/// Aero glass: translucent chrome with the desktop blurred behind it.
///
/// The blur is the whole point, and it is the one thing in this file that a
/// painter cannot do with fills alone — it has to sample what is already on the
/// surface. Text carries a glow because dark-on-glass is otherwise unreadable
/// wherever the backdrop happens to be dark.
[[nodiscard]] Theme make_aero() {
  Theme t;
  t.id = "aero";
  t.name = "Aero";
  t.dark = false;
  t.accent = hex("#3f9fdf");

  t.metrics.control_height = 25.0;
  t.metrics.padding_x = 14.0;
  t.metrics.padding_y = 4.0;
  t.metrics.spacing = 8.0;
  t.metrics.panel_padding = 10.0;
  t.metrics.title_bar_height = 34.0;
  t.metrics.panel_header_height = 28.0;
  t.metrics.font_size = 13.0;
  t.metrics.scrollbar_width = 16.0;
  t.metrics.track_height = 58.0;
  t.metrics.audio_track_height = 46.0;

  t.fallback = SurfaceStyle{
      .fill = Fill::glass(hex("#ffffff40"), 18.0),
      .border = hex("#ffffff60"),
      .border_width = 1.0,
      .corner_radius = 4.0,
      .text = hex("#1a1a1a"),
      .text_glow = 3.0,
  };

  t.set(Part::Window, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#2c4a66")), .text = hex("#ffffff")});

  t.set(Part::TitleBar, State::Normal,
        SurfaceStyle{
            .fill = Fill::glass(hex("#b8d8f088"), 24.0),
            .border = hex("#ffffff80"),
            .border_width = 1.0,
            .corner_radius = 8.0,
            .text = hex("#0d1a26"),
            .text_glow = 4.0,
        });

  t.set(Part::Panel, State::Normal,
        SurfaceStyle{
            .fill = Fill::glass(hex("#e8f2fa66"), 16.0),
            .border = hex("#ffffff70"),
            .border_width = 1.0,
            .corner_radius = 6.0,
            .text = hex("#12222e"),
            .text_glow = 2.0,
        });

  t.set(Part::PanelHeader, State::Normal,
        SurfaceStyle{
            .fill = Fill::glass(hex("#ffffff55"), 12.0),
            .border = hex("#ffffff66"),
            .border_width = 1.0,
            .corner_radius = 4.0,
            .text = hex("#12222e"),
            .text_glow = 2.0,
        });

  // Aero buttons are still glossy, but the gloss is a translucent white sheen
  // over glass rather than XP's opaque plastic.
  const SurfaceStyle button{
      .fill = Fill::gradient({{0.00f, hex("#ffffffcc")},
                              {0.48f, hex("#ffffff66")},
                              {0.52f, hex("#d8e8f855")},
                              {1.00f, hex("#eaf4ffaa")}}),
      .shadow = Shadow{.offset_x = 0.0, .offset_y = 1.0, .blur = 3.0, .color = hex("#00000030")},
      .border = hex("#8fb8d8"),
      .border_width = 1.0,
      .corner_radius = 4.0,
      .text = hex("#12222e"),
  };
  t.set(Part::Button, State::Normal, button);

  SurfaceStyle button_hover = button;
  button_hover.fill = Fill::gradient({{0.00f, hex("#ffffffee")},
                                      {0.48f, hex("#d8f0ffaa")},
                                      {0.52f, hex("#b8e4ff88")},
                                      {1.00f, hex("#dff2ffcc")}});
  button_hover.border = hex("#3f9fdf");
  // The Aero hover glow, drawn inside the control.
  button_hover.shadow = Shadow{.offset_x = 0.0, .offset_y = 0.0, .blur = 8.0,
                               .color = hex("#7fd0ff80"), .inner = true};
  t.set(Part::Button, State::Hover, button_hover);

  SurfaceStyle button_pressed = button;
  button_pressed.fill = Fill::gradient({{0.0f, hex("#a8c8e0aa")}, {1.0f, hex("#cfe4f5cc")}});
  button_pressed.shadow = Shadow{.offset_x = 0.0, .offset_y = 1.0, .blur = 4.0,
                                 .color = hex("#00000050"), .inner = true};
  t.set(Part::Button, State::Pressed, button_pressed);

  SurfaceStyle button_disabled = button;
  button_disabled.fill = Fill::solid(hex("#ffffff33"));
  button_disabled.text = hex("#12222e66");
  button_disabled.shadow.reset();
  t.set(Part::Button, State::Disabled, button_disabled);

  t.set(Part::Input, State::Normal,
        SurfaceStyle{
            .fill = Fill::solid(hex("#ffffffdd")),
            .shadow = Shadow{.offset_x = 0.0, .offset_y = 1.0, .blur = 3.0,
                             .color = hex("#00000030"), .inner = true},
            .border = hex("#7fa8c8"),
            .border_width = 1.0,
            .corner_radius = 3.0,
            .text = hex("#0d1a26"),
        });

  t.set(Part::Clip, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#9fd4f5dd")}, {0.5f, hex("#6fb4e5cc")},
                                    {1.0f, hex("#4f94d5dd")}}),
            .border = hex("#ffffff80"),
            .border_width = 1.0,
            .corner_radius = 5.0,
            .text = hex("#08202e"),
            .text_glow = 2.0,
        });
  t.set(Part::Clip, State::Selected,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#ffe8a0ee")}, {1.0f, hex("#ffc850ee")}}),
            .shadow = Shadow{.offset_x = 0.0, .offset_y = 0.0, .blur = 10.0,
                             .color = hex("#ffd070aa")},
            .border = hex("#ffffffcc"),
            .border_width = 2.0,
            .corner_radius = 5.0,
            .text = hex("#3a2600"),
        });

  t.set(Part::TrackHeader, State::Normal,
        SurfaceStyle{
            .fill = Fill::glass(hex("#ffffff44"), 10.0),
            .border = hex("#ffffff55"),
            .border_width = 1.0,
            .text = hex("#12222e"),
            .text_glow = 2.0,
        });

  t.set(Part::Ruler, State::Normal,
        SurfaceStyle{.fill = Fill::glass(hex("#ffffff33"), 8.0), .text = hex("#12222e")});
  t.set(Part::Playhead, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ff4444")), .text = hex("#ff4444")});

  t.set(Part::Menu, State::Normal,
        SurfaceStyle{.fill = Fill::glass(hex("#f4fafeee"), 20.0),
                     .shadow = Shadow{.offset_x = 0.0, .offset_y = 4.0, .blur = 12.0,
                                      .color = hex("#00000055")},
                     .border = hex("#ffffff99"),
                     .border_width = 1.0,
                     .corner_radius = 6.0,
                     .text = hex("#0d1a26")});
  t.set(Part::MenuItem, State::Hover,
        SurfaceStyle{.fill = Fill::gradient({{0.0f, hex("#d8f0ff")}, {1.0f, hex("#a8d8f8")}}),
                     .border = hex("#7fc0e8"),
                     .border_width = 1.0,
                     .corner_radius = 3.0,
                     .text = hex("#0d1a26")});

  t.set(Part::Scrollbar, State::Normal,
        SurfaceStyle{.fill = Fill::glass(hex("#ffffff22"), 6.0)});
  t.set(Part::ScrollThumb, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#ffffffcc")}, {1.0f, hex("#c8dcecaa")}}),
            .border = hex("#8fb8d8"),
            .border_width = 1.0,
            .corner_radius = 6.0,
        });

  return t;
}

// ------------------------------------------------------------- Flat / dark --

/// The modern default: no bevel, no gloss, definition from borders and spacing.
///
/// Included as the plain case. It is what most of the interface will be used
/// with, and it is the one that proves the model does not *force* ornament.
[[nodiscard]] Theme make_flat() {
  Theme t;
  t.id = "flat";
  t.name = "Slate";
  t.dark = true;
  t.accent = hex("#4c9aff");

  t.metrics.control_height = 28.0;
  t.metrics.padding_x = 12.0;
  t.metrics.padding_y = 6.0;
  t.metrics.spacing = 8.0;
  t.metrics.panel_padding = 12.0;
  t.metrics.title_bar_height = 36.0;
  t.metrics.panel_header_height = 30.0;
  t.metrics.font_size = 13.0;
  t.metrics.scrollbar_width = 10.0;
  t.metrics.track_height = 56.0;
  t.metrics.audio_track_height = 44.0;

  t.fallback = SurfaceStyle{
      .fill = Fill::solid(hex("#1e2228")),
      .border = hex("#2f353d"),
      .border_width = 1.0,
      .corner_radius = 4.0,
      .text = hex("#d8dee6"),
  };

  t.set(Part::Window, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#15181d")), .text = hex("#d8dee6")});
  t.set(Part::TitleBar, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#1a1e24")),
                     .border = hex("#2f353d"),
                     .border_width = 1.0,
                     .text = hex("#e6ebf2")});

  t.set(Part::Panel, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#1a1e24")),
                     .border = hex("#2a3037"),
                     .border_width = 1.0,
                     .corner_radius = 6.0,
                     .text = hex("#d8dee6")});
  t.set(Part::PanelHeader, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#20252c")),
                     .border = hex("#2a3037"),
                     .border_width = 1.0,
                     .text = hex("#9aa4b0")});

  const SurfaceStyle button{
      .fill = Fill::solid(hex("#272d35")),
      .border = hex("#39414b"),
      .border_width = 1.0,
      .corner_radius = 5.0,
      .text = hex("#d8dee6"),
  };
  t.set(Part::Button, State::Normal, button);

  SurfaceStyle button_hover = button;
  button_hover.fill = Fill::solid(hex("#323a44"));
  button_hover.border = hex("#4a545f");
  t.set(Part::Button, State::Hover, button_hover);

  SurfaceStyle button_pressed = button;
  button_pressed.fill = Fill::solid(hex("#1c2129"));
  t.set(Part::Button, State::Pressed, button_pressed);

  SurfaceStyle button_disabled = button;
  button_disabled.fill = Fill::solid(hex("#1e2228"));
  button_disabled.text = hex("#5a636e");
  t.set(Part::Button, State::Disabled, button_disabled);

  SurfaceStyle button_focused = button;
  button_focused.border = hex("#4c9aff");
  button_focused.border_width = 2.0;
  t.set(Part::Button, State::Focused, button_focused);

  t.set(Part::Input, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#12151a")),
                     .border = hex("#39414b"),
                     .border_width = 1.0,
                     .corner_radius = 4.0,
                     .text = hex("#e6ebf2")});

  t.set(Part::Clip, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#2d5a8e")),
                     .border = hex("#3d74b4"),
                     .border_width = 1.0,
                     .corner_radius = 4.0,
                     .text = hex("#e6f0fa")});
  t.set(Part::Clip, State::Selected,
        SurfaceStyle{.fill = Fill::solid(hex("#3d74b4")),
                     .border = hex("#4c9aff"),
                     .border_width = 2.0,
                     .corner_radius = 4.0,
                     .text = hex("#ffffff")});

  t.set(Part::TrackHeader, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#1a1e24")),
                     .border = hex("#2a3037"),
                     .border_width = 1.0,
                     .text = hex("#9aa4b0")});

  t.set(Part::Ruler, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#15181d")), .text = hex("#7a838e")});
  t.set(Part::Playhead, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ff5555")), .text = hex("#ff5555")});

  t.set(Part::Menu, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#20252c")),
                     .shadow = Shadow{.offset_x = 0.0, .offset_y = 4.0, .blur = 16.0,
                                      .color = hex("#00000080")},
                     .border = hex("#39414b"),
                     .border_width = 1.0,
                     .corner_radius = 6.0,
                     .text = hex("#d8dee6")});
  t.set(Part::MenuItem, State::Hover,
        SurfaceStyle{.fill = Fill::solid(hex("#2d5a8e")),
                     .corner_radius = 4.0,
                     .text = hex("#ffffff")});

  t.set(Part::Tooltip, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#2a3037")),
                     .border = hex("#4a545f"),
                     .border_width = 1.0,
                     .corner_radius = 4.0,
                     .text = hex("#e6ebf2")});

  t.set(Part::Scrollbar, State::Normal, SurfaceStyle{.fill = Fill::solid(hex("#15181d"))});
  t.set(Part::ScrollThumb, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#39414b")), .corner_radius = 5.0});
  t.set(Part::ScrollThumb, State::Hover,
        SurfaceStyle{.fill = Fill::solid(hex("#4a545f")), .corner_radius = 5.0});

  return t;
}

// ---------------------------------------------------------------- Terminal --

/// Amber phosphor: a CRT terminal, glow and all.
///
/// The fun one, and it earns its place technically — it is built almost
/// entirely from inner glows and a single hue, so it exercises the shadow path
/// the way Aero exercises the blur path. Everything glows because a phosphor
/// display does.
[[nodiscard]] Theme make_terminal() {
  Theme t;
  t.id = "terminal";
  t.name = "Phosphor";
  t.dark = true;
  t.accent = hex("#ffb000");

  t.metrics.control_height = 24.0;
  t.metrics.padding_x = 10.0;
  t.metrics.padding_y = 4.0;
  t.metrics.spacing = 8.0;
  t.metrics.panel_padding = 10.0;
  t.metrics.title_bar_height = 28.0;
  t.metrics.panel_header_height = 24.0;
  t.metrics.font_size = 13.0;
  t.metrics.small_font_size = 12.0;
  t.metrics.line_height = 1.5;  // terminals breathe
  t.metrics.scrollbar_width = 12.0;
  t.metrics.track_height = 52.0;
  t.metrics.audio_track_height = 40.0;

  const Color amber = hex("#ffb000");
  const Color dim = hex("#a06800");

  t.fallback = SurfaceStyle{
      .fill = Fill::solid(hex("#0a0800")),
      .border = dim,
      .border_width = 1.0,
      .text = amber,
      .text_glow = 4.0,
  };

  t.set(Part::Window, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#0a0800")), .text = amber, .text_glow = 4.0});
  t.set(Part::TitleBar, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#140f00")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 6.0});

  t.set(Part::Panel, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#0a0800")),
                     .shadow = Shadow{.offset_x = 0.0, .offset_y = 0.0, .blur = 12.0,
                                      .color = hex("#ffb00020"), .inner = true},
                     .border = dim,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 4.0});
  t.set(Part::PanelHeader, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#1a1200")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 5.0});

  const SurfaceStyle button{
      .fill = Fill::solid(hex("#140f00")),
      .border = dim,
      .border_width = 1.0,
      .text = amber,
      .text_glow = 4.0,
  };
  t.set(Part::Button, State::Normal, button);

  SurfaceStyle button_hover = button;
  button_hover.fill = Fill::solid(hex("#2a1e00"));
  button_hover.border = amber;
  button_hover.shadow = Shadow{.offset_x = 0.0, .offset_y = 0.0, .blur = 10.0,
                               .color = hex("#ffb00060"), .inner = true};
  button_hover.text_glow = 7.0;
  t.set(Part::Button, State::Hover, button_hover);

  SurfaceStyle button_pressed = button;
  button_pressed.fill = Fill::solid(amber);
  button_pressed.text = hex("#0a0800");
  button_pressed.text_glow = 0.0;
  t.set(Part::Button, State::Pressed, button_pressed);

  SurfaceStyle button_disabled = button;
  button_disabled.border = hex("#503400");
  button_disabled.text = hex("#805400");
  button_disabled.text_glow = 1.0;
  t.set(Part::Button, State::Disabled, button_disabled);

  t.set(Part::Input, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#000000")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 4.0});

  t.set(Part::Clip, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#2a1e00")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 3.0});
  t.set(Part::Clip, State::Selected,
        SurfaceStyle{.fill = Fill::solid(hex("#4a3400")),
                     .shadow = Shadow{.offset_x = 0.0, .offset_y = 0.0, .blur = 12.0,
                                      .color = hex("#ffb00080")},
                     .border = amber,
                     .border_width = 2.0,
                     .text = hex("#ffd060"),
                     .text_glow = 6.0});

  t.set(Part::TrackHeader, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#140f00")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 3.0});

  t.set(Part::Ruler, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#0a0800")), .text = dim, .text_glow = 3.0});
  t.set(Part::Playhead, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ffd060")),
                     .shadow = Shadow{.offset_x = 0.0, .offset_y = 0.0, .blur = 8.0,
                                      .color = hex("#ffb000")},
                     .text = hex("#ffd060")});

  t.set(Part::Menu, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#140f00")),
                     .border = amber,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 4.0});
  t.set(Part::MenuItem, State::Hover,
        SurfaceStyle{.fill = Fill::solid(amber), .text = hex("#0a0800")});

  t.set(Part::Tooltip, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#1a1200")),
                     .border = amber,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 4.0});

  t.set(Part::Scrollbar, State::Normal, SurfaceStyle{.fill = Fill::solid(hex("#0a0800"))});
  t.set(Part::ScrollThumb, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#503400")), .border = dim, .border_width = 1.0});

  return t;
}

/// Built once. Themes are immutable and shared, and rebuilding this list per
/// lookup would be needless work in a paint loop.
[[nodiscard]] const std::array<Theme, 4>& all_themes() {
  static const std::array<Theme, 4> themes{make_flat(), make_xp(), make_aero(), make_terminal()};
  return themes;
}

}  // namespace

std::span<const Theme> built_in_themes() { return all_themes(); }

const Theme* built_in_theme(std::string_view id) noexcept {
  for (const Theme& theme : all_themes()) {
    if (theme.id == id) return &theme;
  }
  return nullptr;
}

const Theme& default_theme() { return all_themes().front(); }

}  // namespace cutline::ui
