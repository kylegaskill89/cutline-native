/// The built-in themes.
///
/// These exist to prove the theme model can express real chrome, not to be a
/// palette gallery. Each one is here because it needs something the others do
/// not: XP needs bevels and bands of colour inside an edge, Aero needs a blurred
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

  t.metrics.control_height = 21.0;  // XP's dialog buttons, measured
  t.metrics.padding_x = 12.0;
  t.metrics.padding_y = 3.0;
  t.metrics.spacing = 6.0;
  t.metrics.panel_padding = 6.0;
  t.metrics.title_bar_height = 30.0;
  t.metrics.panel_header_height = 24.0;
  t.metrics.list_row_height = 20.0;  // Explorer's lists were tight
  t.metrics.font_family = "Tahoma";
  // Tahoma 8 point, which at 96 DPI is eleven pixels, unsmoothed: XP's
  // "Standard" smoothing left body text alone.
  t.metrics.font_size = 11.0;
  t.metrics.text_smoothing = TextSmoothing::Standard;
  t.metrics.small_font_size = 11.0;
  t.metrics.heading_font_size = 12.0;  // XP headed a section in bold, not larger
  t.metrics.group_spacing = 10.0;
  t.metrics.scrollbar_width = 17.0;  // XP scrollbars were chunky
  t.metrics.splitter_width = 8.0;    // and its splitters were raised bars
  t.metrics.track_height = 60.0;     // bevelled clips need the room
  // XP's controls changed state in a single frame.
  t.metrics.hover_in = 0.0;
  t.metrics.hover_out = 0.0;
  t.metrics.focus_cue = FocusCue::Dotted;
  t.metrics.popup_fade = 0.12;  // XP faded its menus in, and nothing else
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

  // The Luna title bar, measured from XP's: a bright line under its top
  // edge, a flat dark middle, light again near the bottom and a dark last
  // row. The title is Trebuchet in white with a hard navy shadow.
  t.metrics.caption_font_family = "Trebuchet MS";
  t.metrics.caption_font_size = 13.0;
  t.set(Part::TitleBar, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.02f, hex("#0058eb")},
                                    {0.05f, hex("#3d95ff")},
                                    {0.08f, hex("#2b90ff")},
                                    {0.12f, hex("#0372ff")},
                                    {0.15f, hex("#0365f1")},
                                    {0.22f, hex("#0058e6")},
                                    {0.25f, hex("#0055e5")},
                                    {0.48f, hex("#0055e5")},
                                    {0.60f, hex("#0058ee")},
                                    {0.68f, hex("#0060f9")},
                                    {0.82f, hex("#036eff")},
                                    {0.88f, hex("#0368fa")},
                                    {0.92f, hex("#0060f9")},
                                    {0.95f, hex("#004de3")},
                                    {0.98f, hex("#0144d0")}}),
            .corner_radius = 8.0,
            .text = hex("#ffffff"),
            .text_shadow = Shadow{.offset_x = 1.0, .offset_y = 1.0, .color = hex("#0a1883")},
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

  // Luna's tabs, measured from XP's: a grey-blue hairline round a face that
  // fades from white to the dialog's beige, and an orange line along the top
  // of the one showing and of the one under the pointer. The one showing is
  // plain white, stands two pixels taller, and its title is not bold.
  t.metrics.tab_rise = 2.0;
  t.metrics.bold_active_tab = false;
  t.set(Part::TabBar, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ece9d8")), .text = hex("#000000")});
  const Rim luna_tab_line{.width = 3.0,
                          .fill = Fill::gradient({{0.17f, hex("#e68b2c")},
                                                  {0.50f, hex("#ffc83c")},
                                                  {0.83f, hex("#ffc73c")}})};
  const SurfaceStyle tab{
      .fill = Fill::gradient({{0.0f, hex("#fefefe")}, {0.5f, hex("#f4f4f0")}, {1.0f, hex("#e9e8e1")}}),
      .border = hex("#919b9c"),
      .border_width = 1.0,
      .corner_radius = 3.0,
      .text = hex("#000000"),
  };
  t.set(Part::Tab, State::Normal, tab);
  SurfaceStyle tab_hover = tab;
  tab_hover.cap = luna_tab_line;
  t.set(Part::Tab, State::Hover, tab_hover);
  SurfaceStyle tab_selected = tab_hover;
  tab_selected.fill = Fill::solid(hex("#fcfcfe"));
  t.set(Part::Tab, State::Selected, tab_selected);

  // The button, measured from XP's own: a navy hairline with three-pixel
  // corners round a face that is nearly white at the top and falls away over
  // its last three rows. There is no step in the middle — that was the title
  // bar's look, guessed onto the button.
  const Fill luna_face = Fill::gradient({{0.05f, hex("#ffffff")},
                                         {0.80f, hex("#f0f0ea")},
                                         {0.85f, hex("#ecebe6")},
                                         {0.90f, hex("#e2dfd6")},
                                         {0.95f, hex("#d6d0c5")}});
  // The same face seen from inside a two-pixel band, where only its middle
  // shows.
  const Fill luna_face_inner = Fill::gradient({{0.0f, hex("#fcfcfb")},
                                               {0.85f, hex("#f0f0ea")},
                                               {1.0f, hex("#ecebe6")}});
  const SurfaceStyle button{
      .fill = luna_face,
      .border = hex("#003c74"),
      .border_width = 1.0,
      .corner_radius = 3.0,
      .text = hex("#000000"),
  };
  t.set(Part::Button, State::Normal, button);

  // Under the pointer the face stays and an orange band lights inside the
  // hairline: pale at the top, deep amber at the bottom.
  SurfaceStyle button_hover = button;
  button_hover.fill = luna_face_inner;
  button_hover.rim = Rim{.width = 2.0,
                         .fill = Fill::gradient({{0.05f, hex("#fff0cf")},
                                                 {0.10f, hex("#fdd889")},
                                                 {0.90f, hex("#f8b230")},
                                                 {0.95f, hex("#e59700")}})};
  t.set(Part::Button, State::Hover, button_hover);

  // The default button, and the one with the keyboard: the same band in blue.
  SurfaceStyle button_default = button_hover;
  button_default.rim = Rim{.width = 2.0,
                           .fill = Fill::gradient({{0.05f, hex("#cee7ff")},
                                                   {0.10f, hex("#bcd4f6")},
                                                   {0.90f, hex("#89ade4")},
                                                   {0.95f, hex("#6982ee")}})};
  t.set(Part::Button, State::Focused, button_default);

  // Held down: the face goes grey and its light moves to the bottom, as if the
  // button had turned away from it.
  SurfaceStyle button_pressed = button;
  button_pressed.fill = Fill::gradient({{0.05f, hex("#d1ccc1")},
                                        {0.10f, hex("#dcd8cf")},
                                        {0.15f, hex("#e5e4dd")},
                                        {0.80f, hex("#e0e0d7")},
                                        {0.90f, hex("#eae9e3")},
                                        {0.95f, hex("#f2f1ee")}});
  t.set(Part::Button, State::Pressed, button_pressed);

  SurfaceStyle button_disabled = button;
  button_disabled.fill = Fill::solid(hex("#f5f4ea"));
  button_disabled.border = hex("#c9c7ba");
  button_disabled.text = hex("#aca899");
  t.set(Part::Button, State::Disabled, button_disabled);

  // Latched: which theme is showing, which tool is chosen, which parameter is
  // animated. XP drew this as a pressed-in button with the selection wash over
  // it, which is a different thing again from held-down — the bevel is sunken
  // but the fill is tinted rather than merely darker.
  SurfaceStyle button_selected = button;
  button_selected.fill = Fill::gradient({{0.00f, hex("#ffe9a8")},
                                         {0.50f, hex("#ffdc80")},
                                         {1.00f, hex("#ffefc0")}});
  button_selected.bevel = sunken("#ffffff", "#9d9a8f");
  button_selected.border = hex("#c08000");
  t.set(Part::Button, State::Selected, button_selected);

  // Toolbar buttons: an icon in a row, not a labelled control. XP drew them
  // flat until the pointer arrived, which is the whole reason a toolbar full of
  // them does not look like a wall of buttons.
  SurfaceStyle tool = button;
  tool.fill = Fill::solid(hex("#00000000"));
  tool.bevel.reset();
  tool.border = hex("#00000000");
  tool.border_width = 0.0;
  t.set(Part::ToolButton, State::Normal, tool);

  SurfaceStyle tool_hover = tool;
  tool_hover.fill = Fill::gradient({{0.0f, hex("#fffdf4")}, {1.0f, hex("#ffe894")}});
  tool_hover.bevel = raised("#ffffff", "#c0a060");
  tool_hover.border = hex("#e0a000");
  tool_hover.border_width = 1.0;
  t.set(Part::ToolButton, State::Hover, tool_hover);

  SurfaceStyle tool_pressed = tool_hover;
  tool_pressed.fill = Fill::solid(hex("#e6e3d5"));
  tool_pressed.bevel = sunken("#ffffff", "#8a8779");
  t.set(Part::ToolButton, State::Pressed, tool_pressed);

  SurfaceStyle tool_selected = tool_hover;
  tool_selected.fill = Fill::solid(hex("#ffdc80"));
  tool_selected.bevel = sunken("#ffffff", "#9d9a8f");
  tool_selected.border = hex("#c08000");
  t.set(Part::ToolButton, State::Selected, tool_selected);

  SurfaceStyle tool_disabled = tool;
  tool_disabled.text = hex("#aca899");
  t.set(Part::ToolButton, State::Disabled, tool_disabled);

  // The trackbar, measured from XP's: a channel four pixels deep, dark above
  // and white below, under a pointed thumb whose band across the top is green
  // at rest, orange under the pointer and a darker green while held. The
  // thumb's face is lit from the left; its band and edges are the rim and the
  // bevel, which is how `paint_pointer` reads them.
  t.metrics.slider_thumb = ThumbShape::Pointer;
  t.set(Part::Slider, State::Normal,
        SurfaceStyle{.fill = Fill::gradient({{0.0f, hex("#f2f1e9")}, {1.0f, hex("#f0ede0")}}),
                     .bevel = sunken("#ffffff", "#9d9c99"),
                     .corner_radius = 2.0,
                     .text = hex("#000000")});
  const auto luna_band = [](std::string_view top, std::string_view middle,
                            std::string_view bottom) {
    return Rim{.width = 3.0,
               .fill = Fill::gradient(
                   {{0.17f, hex(top)}, {0.50f, hex(middle)}, {0.83f, hex(bottom)}})};
  };
  const SurfaceStyle thumb{.fill = Fill::gradient({{0.05f, hex("#f7f7f4")},
                                                   {0.17f, hex("#f5f5f2")},
                                                   {0.28f, hex("#f3f3ef")},
                                                   {0.72f, hex("#f3f3ef")},
                                                   {0.83f, hex("#dcdcd9")},
                                                   {0.95f, hex("#c3c3c0")}}),
                           .bevel = raised("#b5c4cd", "#778892"),
                           .rim = luna_band("#66ce64", "#47c446", "#21b81f"),
                           .text = hex("#000000")};
  t.set(Part::SliderThumb, State::Normal, thumb);
  SurfaceStyle thumb_hover = thumb;
  thumb_hover.rim = luna_band("#fbcb74", "#fac158", "#f9b435");
  t.set(Part::SliderThumb, State::Hover, thumb_hover);
  SurfaceStyle thumb_pressed = thumb;
  thumb_pressed.rim = luna_band("#66b65b", "#48a73b", "#229512");
  t.set(Part::SliderThumb, State::Pressed, thumb_pressed);

  // The progress bar: a grey hairline round a white trough, and green blocks
  // shaded as a tube, darkest along the middle.
  t.metrics.progress_block = 6.0;
  t.metrics.progress_gap = 2.0;
  t.set(Part::Progress, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ffffff")),
                     .bevel = sunken("#efefef", "#bebebe"),
                     .border = hex("#686868"),
                     .border_width = 1.0,
                     .corner_radius = 3.0,
                     .text = hex("#000000")});
  t.set(Part::Progress, State::Selected,
        SurfaceStyle{.fill = Fill::gradient({{0.07f, hex("#abedac")},
                                             {0.21f, hex("#7ce47e")},
                                             {0.36f, hex("#4dda50")},
                                             {0.50f, hex("#2ed331")},
                                             {0.64f, hex("#41d744")},
                                             {0.79f, hex("#76e277")},
                                             {0.93f, hex("#8fe890")}}),
                     .text = hex("#000000")});

  // The caption buttons: squares with a white edge and three-pixel corners,
  // lit from the top left, blue but for the red close. Under the pointer they
  // brighten; held, they darken and their glyph dims.
  t.metrics.caption_width = 21.0;
  t.metrics.caption_height = 21.0;
  t.metrics.caption_gap = 2.0;
  t.metrics.caption_glyphs = CaptionGlyphs::Luna;
  const auto caption = [](std::string_view light, std::string_view middle, std::string_view dark,
                          std::string_view edge_light, std::string_view edge_dark,
                          std::string_view ink) {
    return SurfaceStyle{
        .fill = Fill::gradient({{0.0f, hex(light)}, {0.5f, hex(middle)}, {1.0f, hex(dark)}},
                               315.0),
        .bevel = raised(edge_light, edge_dark),
        .border = hex("#ffffff"),
        .border_width = 1.0,
        .corner_radius = 3.0,
        .text = hex(ink),
    };
  };
  t.set(Part::Caption, State::Normal,
        caption("#9ab7fa", "#4078f5", "#0253e6", "#7aa0f8", "#1546b9", "#ffffff"));
  t.set(Part::Caption, State::Hover,
        caption("#b8cfff", "#5d90ff", "#1a6cf2", "#9ab7fa", "#2458cc", "#ffffff"));
  t.set(Part::Caption, State::Pressed,
        caption("#0a3a98", "#0047a7", "#0a63d0", "#0a3a98", "#3a78e0", "#ffffff99"));
  t.set(Part::CaptionClose, State::Normal,
        caption("#f1ac9a", "#e66b4e", "#e03802", "#eb8b73", "#ad3a1d", "#ffffff"));
  t.set(Part::CaptionClose, State::Hover,
        caption("#ffbcb2", "#fd7c62", "#f04d22", "#ffa08c", "#c43a1a", "#ffffff"));
  t.set(Part::CaptionClose, State::Pressed,
        caption("#8a2a12", "#b23a18", "#cc4a24", "#8a2a12", "#d8603c", "#ffffff99"));

  // Menus were white with the highlight in Luna blue.
  // The border is the colour of the rules between groups; a resting row draws
  // no surface, so it is never stroked.
  t.set(Part::MenuItem, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ffffff")),
                     .border = hex("#aca899"),
                     .text = hex("#000000")});
  t.set(Part::MenuItem, State::Hover,
        SurfaceStyle{.fill = Fill::solid(hex("#316ac5")), .text = hex("#ffffff")});
  t.set(Part::MenuItem, State::Selected,
        SurfaceStyle{.fill = Fill::solid(hex("#d8e4f8")),
                     .border = hex("#316ac5"),
                     .border_width = 1.0,
                     .text = hex("#000000")});

  t.set(Part::Input, State::Normal,
        SurfaceStyle{
            .fill = Fill::solid(hex("#ffffff")),
            .bevel = sunken("#ffffff", "#7f9db9"),
            .border = hex("#7f9db9"),
            .border_width = 1.0,
            .text = hex("#000000"),
        });

  // The check box and the radio, measured from XP's: a navy hairline round a
  // face that lightens from the top left corner to the bottom right, an orange
  // band under the pointer, and a grey well while held. The marks are green.
  t.metrics.check_mark = CheckMark::Luna;
  t.metrics.lit_radio_dot = true;
  // Menus, measured from XP's: seventeen-pixel rows, nine between groups, and
  // two pixels of white inside the edge.
  t.metrics.menu_row_height = 17.0;
  t.metrics.menu_separator_height = 9.0;
  t.metrics.menu_padding = 2.0;

  // A list's headings, measured from Explorer's: beige, with three shaded
  // rows along the bottom that turn orange under the pointer, and a dark line
  // and a white one between columns.
  t.metrics.header_dividers = true;
  const SurfaceStyle header{
      .fill = Fill::solid(hex("#ebeadb")),
      .foot = Rim{.width = 3.0,
                  .fill = Fill::gradient(
                      {{0.17f, hex("#e2decd")}, {0.50f, hex("#d6d2c2")}, {0.83f, hex("#cbc7b8")}})},
      .border = hex("#c7c5b2"),
      .text = hex("#000000"),
  };
  t.set(Part::ListHeader, State::Normal, header);
  SurfaceStyle header_hover = header;
  header_hover.fill = Fill::solid(hex("#faf8f3"));
  header_hover.foot = Rim{.width = 3.0,
                          .fill = Fill::gradient({{0.17f, hex("#f9a900")},
                                                  {0.50f, hex("#fcc247")},
                                                  {0.83f, hex("#f9b119")}})};
  t.set(Part::ListHeader, State::Hover, header_hover);

  // A dropdown's arrow is a button, lit as the scroll bar's arrows are: from
  // the top left at rest, brighter under the pointer, from below when held.
  t.metrics.dropdown_button = true;
  const auto drop = [](std::string_view light, std::string_view middle, std::string_view dark) {
    return SurfaceStyle{
        .fill = Fill::gradient({{0.0f, hex(light)}, {0.5f, hex(middle)}, {1.0f, hex(dark)}},
                               315.0),
        .corner_radius = 2.0,
        .text = hex("#4d6185"),
    };
  };
  t.set(Part::DropButton, State::Normal, drop("#e6eefc", "#c8d6fb", "#b0c5f2"));
  t.set(Part::DropButton, State::Hover, drop("#fdffff", "#dbe8ff", "#a9c0e0"));
  t.set(Part::DropButton, State::Pressed, drop("#6e8ef1", "#9db5f5", "#c3d1f5"));
  const Fill luna_well = Fill::gradient({{0.0f, hex("#dcdcd7")}, {1.0f, hex("#ffffff")}}, 315.0);
  const SurfaceStyle check{.fill = luna_well,
                           .border = hex("#1c5180"),
                           .border_width = 1.0,
                           .text = hex("#000000")};
  t.set(Part::Check, State::Normal, check);
  SurfaceStyle check_hover = check;
  check_hover.fill = Fill::solid(hex("#e7e7e3"));
  check_hover.rim = Rim{.width = 2.0,
                        .fill = Fill::gradient({{0.0f, hex("#fff0cf")}, {1.0f, hex("#f8b330")}},
                                               315.0)};
  t.set(Part::Check, State::Hover, check_hover);
  SurfaceStyle check_pressed = check;
  check_pressed.fill = Fill::gradient({{0.0f, hex("#b0b0a7")}, {1.0f, hex("#f1efdf")}}, 315.0);
  t.set(Part::Check, State::Pressed, check_pressed);
  SurfaceStyle check_disabled = check;
  check_disabled.fill = Fill::solid(hex("#ffffff"));
  check_disabled.border = hex("#cac8bb");
  check_disabled.text = hex("#aca899");
  t.set(Part::Check, State::Disabled, check_disabled);
  // The mark's colour.
  t.set(Part::Check, State::Selected, SurfaceStyle{.text = hex("#21a121")});

  SurfaceStyle radio = check;
  radio.border = hex("#215481");
  t.set(Part::Radio, State::Normal, radio);
  SurfaceStyle radio_hover = check_hover;
  radio_hover.border = hex("#215481");
  t.set(Part::Radio, State::Hover, radio_hover);
  SurfaceStyle radio_pressed = check_pressed;
  radio_pressed.border = hex("#215481");
  t.set(Part::Radio, State::Pressed, radio_pressed);
  SurfaceStyle radio_disabled = check_disabled;
  t.set(Part::Radio, State::Disabled, radio_disabled);
  // The dot: lit from the top left, as everything in Luna is.
  t.set(Part::Radio, State::Selected,
        SurfaceStyle{.fill = Fill::gradient({{0.0f, hex("#55d551")},
                                             {0.5f, hex("#21a121")},
                                             {1.0f, hex("#139210")}},
                                            315.0),
                     .text = hex("#21a121")});

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
  // A clip that is not being rendered. Drained of colour rather than dimmed to
  // nothing: it still has to be readable, findable and clickable — it is a clip
  // somebody switched off, not a hole in the sequence.
  t.set(Part::Clip, State::Disabled,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#d8d8d8")}, {0.5f, hex("#c0c0c0")},
                                    {1.0f, hex("#aaaaaa")}}),
            .bevel = raised("#ffffff", "#909090"),
            .border = hex("#8a8a8a"),
            .border_width = 1.0,
            .corner_radius = 3.0,
            .text = hex("#606060"),
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

  // Luna's splitters were raised bars in the face colour, not gaps.
  t.set(Part::Splitter, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ece9d8")),
                     .bevel = raised("#ffffff", "#aca899"),
                     .text = hex("#000000")});

  // The scroll bar, measured from XP's: a track lit from one side between two
  // beige lines, and a lavender thumb with a white edge, a blue line below and
  // to the right of it, and a grip of four lines across its middle. Under the
  // pointer, and held, it brightens; XP drew the two alike.
  t.metrics.scroll_grip = 6.0;
  t.set(Part::Scrollbar, State::Normal,
        SurfaceStyle{.fill = Fill::gradient({{0.00f, hex("#eeede5")},
                                             {0.06f, hex("#f3f1ec")},
                                             {0.50f, hex("#f9f9f7")},
                                             {0.94f, hex("#fefefb")},
                                             {1.00f, hex("#eeede5")}},
                                            90.0)});
  const auto scroll_thumb = [](std::string_view edge, std::string_view light,
                               std::string_view middle, std::string_view shade,
                               std::string_view grip) {
    return SurfaceStyle{
        .fill = Fill::gradient({{0.10f, hex(edge)},
                                {0.17f, hex(light)},
                                {0.45f, hex(middle)},
                                {0.83f, hex(shade)},
                                {0.90f, hex(edge)}},
                               90.0),
        .shadow = Shadow{.offset_x = 1.0, .offset_y = 1.0, .color = hex("#9fb5d2")},
        .border = hex("#ffffff"),
        .border_width = 1.0,
        .corner_radius = 2.0,
        .text = hex(grip),
    };
  };
  t.set(Part::ScrollThumb, State::Normal,
        scroll_thumb("#b7caf5", "#c8d6fb", "#c6d6fd", "#b6cdfb", "#8cb0f8"));
  const SurfaceStyle scroll_hot =
      scroll_thumb("#acceff", "#d8e8ff", "#d6e7ff", "#cce1ff", "#9cc5ff");
  t.set(Part::ScrollThumb, State::Hover, scroll_hot);
  t.set(Part::ScrollThumb, State::Pressed, scroll_hot);

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

/// Windows Vista: glass round the window, and opaque, softly lit controls
/// inside it.
///
/// Measured from captures of Vista itself. The glass is the frame only -- the
/// title bar, blurred and tinted, with its text haloed because dark text on
/// glass is unreadable wherever the backdrop is dark. Everything inside the
/// frame is opaque: a light grey face, white pages, and controls whose faces
/// are two tones with a hard step across the middle, grey at rest and pale blue
/// under the pointer. The blur is still the one thing here a painter cannot do
/// with fills alone.
[[nodiscard]] Theme make_aero() {
  Theme t;
  t.id = "aero";
  t.name = "Aero";
  t.dark = false;
  t.accent = hex("#3399ff");

  // Segoe UI at nine points, and the twenty-three pixel buttons of Vista's
  // own dialogs.
  t.metrics.control_height = 23.0;
  t.metrics.padding_x = 12.0;
  t.metrics.padding_y = 4.0;
  t.metrics.spacing = 7.0;
  t.metrics.panel_padding = 9.0;
  t.metrics.title_bar_height = 30.0;
  t.metrics.panel_header_height = 24.0;
  t.metrics.list_row_height = 21.0;
  t.metrics.font_family = "Segoe UI";
  t.metrics.font_size = 12.0;
  t.metrics.small_font_size = 11.0;
  t.metrics.heading_font_size = 15.0;  // Vista's "main instructions" stood a step up
  t.metrics.group_spacing = 14.0;
  t.metrics.scrollbar_width = 17.0;
  t.metrics.track_height = 58.0;
  t.metrics.audio_track_height = 46.0;
  // Vista's buttons glowed in quickly and let go slowly.
  t.metrics.hover_in = 0.15;
  t.metrics.hover_out = 0.5;
  t.metrics.focus_cue = FocusCue::Dotted;  // Vista kept XP's
  t.metrics.motion = 0.25;
  t.metrics.popup_fade = 0.15;
  t.metrics.lit_radio_dot = true;
  t.metrics.slider_thumb = ThumbShape::VistaPointer;
  t.metrics.tab_rise = 2.0;
  t.metrics.bold_active_tab = false;
  t.metrics.header_dividers = true;
  t.metrics.menu_row_height = 22.0;
  t.metrics.menu_separator_height = 7.0;
  t.metrics.menu_padding = 3.0;
  // Hung from the frame's top edge, the close nearly twice as wide as the
  // others, with Vista's outlined glyphs.
  t.metrics.caption_width = 26.0;
  t.metrics.caption_height = 18.0;
  t.metrics.caption_close_width = 43.0;
  t.metrics.caption_hangs = true;
  t.metrics.caption_glyphs = CaptionGlyphs::Vista;
  t.metrics.dropdown_is_button = true;
  t.metrics.default_pulse = 2.0;
  t.metrics.progress_shine = 3.0;
  t.metrics.progress_shine_crossing = 1.5;

  const Color ink = hex("#000000");
  const Color greyed = hex("#838383");

  t.fallback = SurfaceStyle{
      .fill = Fill::solid(hex("#f0f0f0")),
      .border = hex("#898c95"),
      .border_width = 1.0,
      .text = ink,
  };

  t.set(Part::Window, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#f0f0f0")), .text = ink});

  // The glass: the frame, and only the frame.
  t.set(Part::TitleBar, State::Normal,
        SurfaceStyle{
            .fill = Fill::glass(hex("#b8d8f088"), 24.0),
            .border = hex("#ffffff80"),
            .border_width = 1.0,
            .corner_radius = 8.0,
            .text = hex("#0d1a26"),
            .text_glow = 4.0,
        });

  // A page is white with a grey hairline, as a tab's page and Explorer's
  // panes were.
  t.set(Part::Panel, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ffffff")),
                     .border = hex("#898c95"),
                     .border_width = 1.0,
                     .text = ink});

  // A list's headings, and the headers of panels, measured from Explorer's:
  // white above a soft step, a grey line below, pale blue under the pointer.
  const SurfaceStyle header{
      .fill = Fill::gradient({{0.00f, hex("#ffffff")},
                              {0.40f, hex("#ffffff")},
                              {0.42f, hex("#f7f8fa")},
                              {1.00f, hex("#f1f2f4")}}),
      .foot = Rim{.width = 1.0, .fill = Fill::solid(hex("#d5d5d5"))},
      .border = hex("#d5d5d5"),
      .text = ink,
  };
  SurfaceStyle header_hover = header;
  header_hover.fill = Fill::gradient({{0.00f, hex("#e3f7ff")},
                                      {0.40f, hex("#e3f7ff")},
                                      {0.42f, hex("#bdedff")},
                                      {1.00f, hex("#b7e7fb")}});
  header_hover.foot = Rim{.width = 1.0, .fill = Fill::solid(hex("#93c9e3"))};
  t.set(Part::ListHeader, State::Normal, header);
  t.set(Part::ListHeader, State::Hover, header_hover);
  t.set(Part::PanelHeader, State::Normal, header);
  t.set(Part::Ruler, State::Normal, header);

  // The button, measured from Vista's: a grey hairline with three-pixel
  // corners, a white line inside it, and a face in two tones with a hard step
  // just above the middle. Pale blue under the pointer, deeper blue held, and
  // the default button ringed in cyan.
  const auto two_tone = [](std::string_view top, std::string_view above, std::string_view below,
                           std::string_view bottom) {
    return Fill::gradient({{0.00f, hex(top)},
                           {0.45f, hex(above)},
                           {0.46f, hex(below)},
                           {1.00f, hex(bottom)}});
  };
  const SurfaceStyle button{
      .fill = two_tone("#f2f2f2", "#ebebeb", "#dddddd", "#cfcfcf"),
      .rim = Rim{.width = 1.0, .fill = Fill::solid(hex("#fcfcfc"))},
      .border = hex("#707070"),
      .border_width = 1.0,
      .corner_radius = 3.0,
      .text = ink,
  };
  t.set(Part::Button, State::Normal, button);

  SurfaceStyle button_hover = button;
  button_hover.fill = two_tone("#eaf6fd", "#d9f0fc", "#bee6fd", "#a7d9f5");
  button_hover.rim = Rim{.width = 1.0, .fill = Fill::solid(hex("#fafdfe"))};
  button_hover.border = hex("#3c7fb1");
  t.set(Part::Button, State::Hover, button_hover);

  SurfaceStyle button_pressed = button;
  button_pressed.fill = two_tone("#e4f3fb", "#c4e5f6", "#98d1ef", "#68b2da");
  button_pressed.rim.reset();
  button_pressed.shadow = Shadow{.offset_x = 0.0, .offset_y = 1.0, .blur = 2.0,
                                 .color = hex("#00000040"), .inner = true};
  button_pressed.border = hex("#2c628b");
  t.set(Part::Button, State::Pressed, button_pressed);

  SurfaceStyle button_disabled = button;
  button_disabled.fill = Fill::solid(hex("#f4f4f4"));
  button_disabled.rim.reset();
  button_disabled.border = hex("#adb2b5");
  button_disabled.text = greyed;
  t.set(Part::Button, State::Disabled, button_disabled);

  SurfaceStyle button_default = button;
  button_default.fill = two_tone("#eef3f5", "#e9eef1", "#d7e1e7", "#c8d5dd");
  button_default.rim = Rim{.width = 1.0, .fill = Fill::solid(hex("#40d7fc"))};
  button_default.border = hex("#3c7fb1");
  t.set(Part::Button, State::Focused, button_default);
  // The height of its pulse: bluer, the cyan ring brighter.
  SurfaceStyle button_glow = button_default;
  button_glow.fill = two_tone("#ebf6fc", "#dff1fb", "#c8e6f6", "#b8ddf2");
  button_glow.rim = Rim{.width = 1.0, .fill = Fill::solid(hex("#30d4ff"))};
  t.set(Part::DefaultGlow, State::Normal, button_glow);

  // Latched: held down, as a toggle on a command bar stays.
  SurfaceStyle button_selected = button_pressed;
  button_selected.shadow.reset();
  t.set(Part::Button, State::Selected, button_selected);

  // A command bar's buttons are nothing until pointed at, then the button's
  // blue.
  const SurfaceStyle tool{.fill = Fill::solid(hex("#ffffff00")), .text = ink};
  t.set(Part::ToolButton, State::Normal, tool);
  SurfaceStyle tool_hover = button_hover;
  tool_hover.corner_radius = 2.0;
  t.set(Part::ToolButton, State::Hover, tool_hover);
  SurfaceStyle tool_pressed = button_pressed;
  tool_pressed.corner_radius = 2.0;
  t.set(Part::ToolButton, State::Pressed, tool_pressed);
  SurfaceStyle tool_selected = button_selected;
  tool_selected.corner_radius = 2.0;
  t.set(Part::ToolButton, State::Selected, tool_selected);
  SurfaceStyle tool_disabled = tool;
  tool_disabled.text = greyed;
  t.set(Part::ToolButton, State::Disabled, tool_disabled);

  // The tabs: the one showing is the page's white and stands taller, the
  // rest are buttons at rest, and the one under the pointer is a button under
  // the pointer.
  t.set(Part::TabBar, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#f0f0f0")), .text = ink});
  SurfaceStyle tab = button;
  tab.border = hex("#898c95");
  tab.corner_radius = 2.0;
  t.set(Part::Tab, State::Normal, tab);
  SurfaceStyle tab_hover = button_hover;
  tab_hover.corner_radius = 2.0;
  t.set(Part::Tab, State::Hover, tab_hover);
  SurfaceStyle tab_selected = tab;
  tab_selected.fill = Fill::solid(hex("#ffffff"));
  tab_selected.rim.reset();
  t.set(Part::Tab, State::Selected, tab_selected);

  // A text field: white, with a top edge darker than its sides and bottom,
  // which go blue under the pointer and bluer with the keyboard.
  const auto field = [&ink](std::string_view top, std::string_view sides, std::string_view bottom) {
    return SurfaceStyle{
        .fill = Fill::solid(hex("#ffffff")),
        .cap = Rim{.width = 1.0, .fill = Fill::solid(hex(top))},
        .foot = Rim{.width = 1.0, .fill = Fill::solid(hex(bottom))},
        .border = hex(sides),
        .border_width = 1.0,
        .text = ink,
    };
  };
  t.set(Part::Input, State::Normal, field("#abadb3", "#e2e3ea", "#e3e9ef"));
  t.set(Part::Input, State::Hover, field("#5794bf", "#b7d5ea", "#c7e2f1"));
  t.set(Part::Input, State::Focused, field("#3d7bad", "#a4c9e3", "#b7d9ed"));
  SurfaceStyle field_selected = field("#3d7bad", "#a4c9e3", "#b7d9ed");
  field_selected.fill = Fill::solid(t.accent);
  field_selected.text = hex("#ffffff");
  t.set(Part::Input, State::Selected, field_selected);
  SurfaceStyle field_disabled = field("#afafaf", "#dadada", "#e5e5e5");
  field_disabled.fill = Fill::solid(hex("#f4f4f4"));
  field_disabled.text = greyed;
  t.set(Part::Input, State::Disabled, field_disabled);

  // The check box and the radio: a grey hairline, a pale ring inside it, and a
  // well lit from the bottom right, blue under the pointer and deeper blue
  // while held. The tick and the dot's ring are navy; the dot is a bead.
  const auto well = [&ink](std::string_view edge, std::string_view ring, std::string_view shade,
                           std::string_view mid, std::string_view light, std::string_view lightest) {
    return SurfaceStyle{
        .fill = Fill::gradient({{0.00f, hex(shade)},
                                {0.15f, hex(mid)},
                                {0.65f, hex(light)},
                                {1.00f, hex(lightest)}},
                               315.0),
        .rim = Rim{.width = 1.0, .fill = Fill::solid(hex(ring))},
        .border = hex(edge),
        .border_width = 1.0,
        .text = ink,
    };
  };
  const SurfaceStyle check = well("#8e8f8f", "#f4f4f4", "#aeb3b9", "#cbcfd5", "#eeeeee", "#f6f6f6");
  const SurfaceStyle check_hover =
      well("#5586a3", "#def9fa", "#79c6f9", "#b1dffd", "#def3fe", "#e8f7fe");
  const SurfaceStyle check_pressed =
      well("#2c628b", "#c2e4f6", "#5eb6f7", "#9dd5fc", "#d9f2fe", "#e1f4fe");
  SurfaceStyle check_disabled = well("#b1b1b1", "#f4f4f4", "#f4f4f4", "#f4f4f4", "#f4f4f4", "#f4f4f4");
  check_disabled.text = greyed;
  t.set(Part::Check, State::Normal, check);
  t.set(Part::Check, State::Hover, check_hover);
  t.set(Part::Check, State::Pressed, check_pressed);
  t.set(Part::Check, State::Disabled, check_disabled);
  t.set(Part::Check, State::Selected, SurfaceStyle{.text = hex("#1d3e57")});
  SurfaceStyle radio = check;
  radio.border = hex("#919292");
  t.set(Part::Radio, State::Normal, radio);
  SurfaceStyle radio_hover = check_hover;
  radio_hover.border = hex("#5989a5");
  t.set(Part::Radio, State::Hover, radio_hover);
  t.set(Part::Radio, State::Pressed, check_pressed);
  t.set(Part::Radio, State::Disabled, check_disabled);
  t.set(Part::Radio, State::Selected,
        SurfaceStyle{.fill = Fill::gradient({{0.0f, hex("#90d5e5")},
                                             {0.5f, hex("#1898d1")},
                                             {1.0f, hex("#0f3f5d")}},
                                            315.0),
                     .text = hex("#1d3e57")});

  // The trackbar: a hairline channel, dark above and white below, under a
  // pointed thumb faced like a button.
  t.set(Part::Slider, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#e7eaea")),
                     .bevel = sunken("#fcfcfc", "#b0b0b0"),
                     .text = ink});
  SurfaceStyle thumb = button;
  thumb.fill = two_tone("#f2f2f2", "#ebebeb", "#dbdbdb", "#d6d6d6");
  thumb.rim = Rim{.width = 1.0, .fill = Fill::solid(hex("#fbfbfb"))};
  t.set(Part::SliderThumb, State::Normal, thumb);
  t.set(Part::SliderThumb, State::Hover, button_hover);
  SurfaceStyle thumb_pressed = button_pressed;
  thumb_pressed.rim = Rim{.width = 1.0, .fill = Fill::solid(hex("#e4f3fb"))};
  t.set(Part::SliderThumb, State::Pressed, thumb_pressed);

  // The progress bar: a grey trough glazed like a button, and a green bar
  // glazed the same way.
  t.set(Part::Progress, State::Normal,
        SurfaceStyle{.fill = Fill::gradient({{0.00f, hex("#f3f3f3")},
                                             {0.10f, hex("#fcfcfc")},
                                             {0.45f, hex("#dbdbdb")},
                                             {0.50f, hex("#cacaca")},
                                             {0.90f, hex("#d5d5d5")},
                                             {1.00f, hex("#eaeaea")}}),
                     .border = hex("#9e9e9e"),
                     .border_width = 1.0,
                     .corner_radius = 2.0,
                     .text = ink});
  t.set(Part::Progress, State::Selected,
        SurfaceStyle{.fill = Fill::gradient({{0.00f, hex("#c6f7c9")},
                                             {0.45f, hex("#95e6a1")},
                                             {0.50f, hex("#0cbb2b")},
                                             {0.85f, hex("#3fd457")},
                                             {1.00f, hex("#6aec79")}}),
                     .border = hex("#9e9e9e"),
                     .border_width = 1.0,
                     .corner_radius = 2.0,
                     .text = ink});

  // Menus: a grey hairline, two pixels of near-white inside it, a light grey
  // body, and the row under the pointer a pale blue rounded box. The rules
  // between groups are the border colour.
  t.set(Part::Menu, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#f0f0f0")),
                     .shadow = Shadow{.offset_x = 2.0, .offset_y = 2.0, .blur = 4.0,
                                      .color = hex("#00000040")},
                     .rim = Rim{.width = 2.0, .fill = Fill::solid(hex("#f5f5f5"))},
                     .border = hex("#979797"),
                     .border_width = 1.0,
                     .text = ink});
  t.set(Part::MenuItem, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ffffff00")), .border = hex("#e0e0e0"), .text = ink});
  t.set(Part::MenuItem, State::Hover,
        SurfaceStyle{.fill = Fill::gradient({{0.0f, hex("#eef5f8")}, {1.0f, hex("#dcebf3")}}),
                     .border = hex("#a8d8eb"),
                     .border_width = 1.0,
                     .corner_radius = 3.0,
                     .text = ink});
  t.set(Part::MenuItem, State::Selected,
        SurfaceStyle{.fill = Fill::gradient({{0.0f, hex("#f2f8fa")}, {1.0f, hex("#e5f0f6")}}),
                     .border = hex("#c4e3ef"),
                     .border_width = 1.0,
                     .corner_radius = 3.0,
                     .text = ink});

  // A tooltip, as Vista drew it without glass: pale yellow, a grey hairline.
  t.set(Part::Tooltip, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#ffffe1")),
                     .border = hex("#646464"),
                     .border_width = 1.0,
                     .text = ink});

  // The scroll bar: a pale track, and a thumb glazed across its width in grey,
  // blue under the pointer.
  t.set(Part::Scrollbar, State::Normal,
        SurfaceStyle{.fill = Fill::gradient({{0.00f, hex("#e3e3e3")},
                                             {0.10f, hex("#f1f1f1")},
                                             {0.90f, hex("#f3f3f3")},
                                             {1.00f, hex("#eaeaea")}},
                                            90.0)});
  const auto scroll_thumb = [](std::string_view edge, std::string_view light,
                               std::string_view above, std::string_view below,
                               std::string_view dark, std::string_view last) {
    return SurfaceStyle{
        .fill = Fill::gradient({{0.00f, hex(light)},
                                {0.45f, hex(above)},
                                {0.46f, hex(below)},
                                {0.90f, hex(dark)},
                                {1.00f, hex(last)}},
                               90.0),
        .border = hex(edge),
        .border_width = 1.0,
        .corner_radius = 2.0,
        .text = hex("#000000"),
    };
  };
  t.set(Part::ScrollThumb, State::Normal,
        scroll_thumb("#979797", "#f5f5f5", "#e9e9eb", "#d9dadc", "#c0c0c4", "#cfcfd1"));
  const SurfaceStyle scroll_hot =
      scroll_thumb("#3c7fb1", "#e3f4fc", "#d6eefb", "#a9d9f2", "#9bc9e2", "#b9dcef");
  t.set(Part::ScrollThumb, State::Hover, scroll_hot);
  t.set(Part::ScrollThumb, State::Pressed, scroll_hot);

  t.set(Part::Splitter, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#f0f0f0")), .text = ink});

  // The caption buttons on the glass: the minimise and maximise glass
  // themselves, a darker hairline round a sheen; the close a red that glows
  // under the pointer. From public references, since the captures were taken
  // without glass.
  const auto caption = [](std::string_view top, std::string_view above, std::string_view below,
                          std::string_view bottom, std::string_view edge, std::string_view glyph) {
    return SurfaceStyle{
        .fill = Fill::gradient({{0.00f, hex(top)},
                                {0.45f, hex(above)},
                                {0.50f, hex(below)},
                                {1.00f, hex(bottom)}}),
        .rim = Rim{.width = 1.0, .fill = Fill::solid(hex("#ffffff60"))},
        .border = hex(edge),
        .border_width = 1.0,
        .corner_radius = 3.0,
        .text = hex(glyph),
        .text_shadow = Shadow{.color = hex("#535666")},
    };
  };
  t.set(Part::Caption, State::Normal,
        caption("#ffffff99", "#ffffff40", "#00000010", "#ffffff30", "#00000060", "#ffffff"));
  t.set(Part::Caption, State::Hover,
        caption("#c9e9ff", "#8ccdf9", "#3e9ee8", "#74ccff", "#00000080", "#ffffff"));
  t.set(Part::Caption, State::Pressed,
        caption("#8cb4d8", "#4f8cc4", "#1a5c9c", "#3a8ad0", "#00000099", "#ffffff"));
  t.set(Part::CaptionClose, State::Normal,
        caption("#e8a493", "#d8775f", "#c4422a", "#d8654a", "#4c1a0f", "#ffffff"));
  t.set(Part::CaptionClose, State::Hover,
        caption("#f6b8a6", "#ee8a70", "#e0482d", "#f88c5c", "#4c1a0f", "#ffffff"));
  t.set(Part::CaptionClose, State::Pressed,
        caption("#b8735f", "#a8523a", "#8c2410", "#b04a2a", "#3a1208", "#ffffff"));

  // The timeline is no Vista control. Its pieces are drawn in Vista's
  // materials: clips glazed in blue, the headers of tracks a list's headings.
  t.set(Part::Clip, State::Normal,
        SurfaceStyle{
            .fill = Fill::gradient({{0.00f, hex("#d6ecfb")},
                                    {0.45f, hex("#b9dcf5")},
                                    {0.46f, hex("#8fc6ee")},
                                    {1.00f, hex("#6fb0e2")}}),
            .border = hex("#3c7fb1"),
            .border_width = 1.0,
            .corner_radius = 3.0,
            .text = hex("#0b2236"),
        });
  t.set(Part::Clip, State::Disabled,
        SurfaceStyle{
            .fill = Fill::gradient({{0.0f, hex("#eeeeee")}, {1.0f, hex("#cfcfcf")}}),
            .border = hex("#adb2b5"),
            .border_width = 1.0,
            .corner_radius = 3.0,
            .text = greyed,
        });
  t.set(Part::Clip, State::Selected,
        SurfaceStyle{
            .fill = Fill::gradient({{0.00f, hex("#fff3c8")},
                                    {0.45f, hex("#ffe08a")},
                                    {0.46f, hex("#ffcf55")},
                                    {1.00f, hex("#ffbf2a")}}),
            .border = hex("#c08a00"),
            .border_width = 2.0,
            .corner_radius = 3.0,
            .text = hex("#3a2600"),
        });
  SurfaceStyle track_header = header;
  track_header.border_width = 1.0;
  t.set(Part::TrackHeader, State::Normal, track_header);
  t.set(Part::Playhead, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#e81123")), .text = hex("#e81123")});

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
  t.metrics.list_row_height = 30.0;  // flat chrome leans roomy
  t.metrics.font_family = "Segoe UI";
  t.metrics.font_size = 13.0;
  t.metrics.scrollbar_width = 10.0;
  t.metrics.heading_font_size = 14.0;
  t.metrics.group_spacing = 16.0;
  t.metrics.splitter_width = 4.0;  // flat chrome wants a hairline, not a bar
  t.metrics.track_height = 56.0;
  t.metrics.hover_in = 0.1;
  t.metrics.hover_out = 0.2;
  t.metrics.motion = 0.18;
  t.metrics.popup_fade = 0.1;
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

  // Flat chrome has no bevel to lean on, so the tab showing is marked by an
  // accent underline and by being the only one the panel colour runs into.
  t.set(Part::TabBar, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#171b21")),
                     .border = hex("#2a3037"),
                     .border_width = 1.0,
                     .text = hex("#9aa4b0")});

  t.set(Part::Tab, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#171b21")), .text = hex("#7c8794")});
  t.set(Part::Tab, State::Hover,
        SurfaceStyle{.fill = Fill::solid(hex("#20252c")), .text = hex("#c8d2dd")});
  t.set(Part::Tab, State::Selected,
        SurfaceStyle{.fill = Fill::solid(hex("#20252c")),
                     .shadow = Shadow{.offset_y = -2.0, .color = t.accent, .inner = true},
                     .text = hex("#e6ecf2")});

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

  // Latched. A flat theme has no bevel to invert, so this is the one case where
  // the accent has to carry the whole difference: filled rather than outlined.
  SurfaceStyle button_selected = button;
  button_selected.fill = Fill::solid(hex("#2b4c78"));
  button_selected.border = t.accent;
  button_selected.text = hex("#ffffff");
  t.set(Part::Button, State::Selected, button_selected);

  // Toolbar buttons carry no surface at all until they are pointed at. This
  // theme is defined by its borders, and a row of icons each in its own box is
  // exactly the noise it exists to avoid.
  SurfaceStyle tool = button;
  tool.fill = Fill::solid(hex("#00000000"));
  tool.border = hex("#00000000");
  tool.border_width = 0.0;
  tool.text = hex("#b9c2cc");
  t.set(Part::ToolButton, State::Normal, tool);

  SurfaceStyle tool_hover = tool;
  tool_hover.fill = Fill::solid(hex("#2b323b"));
  tool_hover.text = hex("#e6ebf2");
  t.set(Part::ToolButton, State::Hover, tool_hover);

  SurfaceStyle tool_pressed = tool_hover;
  tool_pressed.fill = Fill::solid(hex("#1c2129"));
  t.set(Part::ToolButton, State::Pressed, tool_pressed);

  SurfaceStyle tool_selected = tool;
  tool_selected.fill = Fill::solid(hex("#2b4c78"));
  tool_selected.border = t.accent;
  tool_selected.border_width = 1.0;
  tool_selected.text = hex("#ffffff");
  t.set(Part::ToolButton, State::Selected, tool_selected);

  SurfaceStyle tool_disabled = tool;
  tool_disabled.text = hex("#5a636e");
  t.set(Part::ToolButton, State::Disabled, tool_disabled);

  // A thin channel and a small round handle. The thumb's fill is also the
  // filled part of the groove, so the accent shows how far along the value is.
  t.set(Part::Slider, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#181c22")),
                     .border = hex("#2f353d"),
                     .border_width = 1.0,
                     .corner_radius = 3.0,
                     .text = hex("#d8dee6")});
  t.set(Part::SliderThumb, State::Normal,
        SurfaceStyle{.fill = Fill::solid(t.accent),
                     .corner_radius = 4.0,
                     .text = hex("#0d1218")});

  t.set(Part::MenuItem, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#00000000")), .text = hex("#d8dee6")});
  t.set(Part::MenuItem, State::Hover,
        SurfaceStyle{.fill = Fill::solid(hex("#2b323b")),
                     .corner_radius = 4.0,
                     .text = hex("#ffffff")});
  t.set(Part::MenuItem, State::Selected,
        SurfaceStyle{.fill = Fill::solid(hex("#2b4c78")),
                     .corner_radius = 4.0,
                     .text = hex("#ffffff")});

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
  t.set(Part::Clip, State::Disabled,
        SurfaceStyle{.fill = Fill::solid(hex("#33383f")),
                     .border = hex("#454b53"),
                     .border_width = 1.0,
                     .corner_radius = 4.0,
                     .text = hex("#7e858d")});
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

  t.set(Part::Splitter, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#0f1216")), .text = hex("#e6ebf2")});
  t.set(Part::Splitter, State::Hover,
        SurfaceStyle{.fill = Fill::solid(hex("#2b6cb0")), .text = hex("#e6ebf2")});

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
  t.metrics.list_row_height = 24.0;
  t.metrics.font_family = "Consolas";
  t.metrics.font_size = 13.0;
  // A size down from the others: a monospaced face is about a tenth wider, and
  // the sentences that explain things were written to fit a proportional one.
  t.metrics.small_font_size = 11.0;
  t.metrics.line_height = 1.5;  // terminals breathe
  t.metrics.heading_font_size = 13.0;  // one size of type, as a terminal has
  t.metrics.group_spacing = 12.0;
  t.metrics.scrollbar_width = 12.0;
  t.metrics.track_height = 52.0;
  // Phosphor: lit at once, and an afterglow on the way out.
  t.metrics.hover_in = 0.0;
  t.metrics.hover_out = 0.25;
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

  // A terminal has no chrome to speak of, so the tab showing is the one that
  // is lit: full amber with a glow, against dim outlines for the rest.
  t.set(Part::TabBar, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#0a0700")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = dim});

  t.set(Part::Tab, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#0a0700")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = dim});
  t.set(Part::Tab, State::Hover,
        SurfaceStyle{.fill = Fill::solid(hex("#1a1200")),
                     .border = amber,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 3.0});
  t.set(Part::Tab, State::Selected,
        SurfaceStyle{.fill = Fill::solid(hex("#241900")),
                     .border = amber,
                     .border_width = 1.0,
                     .text = amber,
                     .text_glow = 6.0});

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

  // Latched, and deliberately not the same as held down: pressed inverts to
  // solid amber, this stays dark and burns brighter. On a phosphor display the
  // difference between on and off is how much it glows.
  SurfaceStyle button_selected = button;
  button_selected.fill = Fill::solid(hex("#4a3400"));
  button_selected.border = amber;
  button_selected.border_width = 2.0;
  button_selected.shadow = Shadow{.offset_x = 0.0, .offset_y = 0.0, .blur = 12.0,
                                  .color = hex("#ffb00080")};
  button_selected.text = hex("#ffd060");
  button_selected.text_glow = 8.0;
  t.set(Part::Button, State::Selected, button_selected);

  // Toolbar buttons: no box, and the dimmer amber that everything unlit uses.
  SurfaceStyle tool = button;
  tool.fill = Fill::solid(hex("#00000000"));
  tool.border = hex("#00000000");
  tool.border_width = 0.0;
  tool.text = dim;
  tool.text_glow = 2.0;
  t.set(Part::ToolButton, State::Normal, tool);

  SurfaceStyle tool_hover = tool;
  tool_hover.fill = Fill::solid(hex("#2a1e00"));
  tool_hover.text = amber;
  tool_hover.text_glow = 6.0;
  t.set(Part::ToolButton, State::Hover, tool_hover);

  SurfaceStyle tool_pressed = tool_hover;
  tool_pressed.fill = Fill::solid(amber);
  tool_pressed.text = hex("#0a0800");
  tool_pressed.text_glow = 0.0;
  t.set(Part::ToolButton, State::Pressed, tool_pressed);

  t.set(Part::ToolButton, State::Selected, button_selected);

  SurfaceStyle tool_disabled = tool;
  tool_disabled.text = hex("#805400");
  tool_disabled.text_glow = 1.0;
  t.set(Part::ToolButton, State::Disabled, tool_disabled);

  // A dim channel that lights up behind the handle, since the filled part of
  // the groove takes the thumb's own fill.
  t.set(Part::Slider, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#0a0700")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = amber});
  t.set(Part::SliderThumb, State::Normal,
        SurfaceStyle{.fill = Fill::solid(amber),
                     .shadow = Shadow{.offset_x = 0.0, .offset_y = 0.0, .blur = 8.0,
                                      .color = hex("#ffb00070")},
                     .text = hex("#0a0800")});

  // Inverse video for the row under the pointer, which is what a terminal menu
  // has always been.
  t.set(Part::MenuItem, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#00000000")), .text = dim, .text_glow = 2.0});
  t.set(Part::MenuItem, State::Hover,
        SurfaceStyle{.fill = Fill::solid(amber), .text = hex("#0a0800")});
  t.set(Part::MenuItem, State::Selected,
        SurfaceStyle{.fill = Fill::solid(hex("#4a3400")),
                     .border = amber,
                     .border_width = 1.0,
                     .text = hex("#ffd060"),
                     .text_glow = 5.0});

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
  t.set(Part::Clip, State::Disabled,
        SurfaceStyle{.fill = Fill::solid(hex("#141414")),
                     .border = hex("#3a3a3a"),
                     .border_width = 1.0,
                     .text = hex("#6a6a6a")});
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

  t.set(Part::Splitter, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#0a0800")),
                     .border = dim,
                     .border_width = 1.0,
                     .text = amber});

  t.set(Part::Scrollbar, State::Normal, SurfaceStyle{.fill = Fill::solid(hex("#0a0800"))});
  t.set(Part::ScrollThumb, State::Normal,
        SurfaceStyle{.fill = Fill::solid(hex("#503400")), .border = dim, .border_width = 1.0});

  return t;
}

/// Gives `to` every state of `from` it does not style itself.
///
/// Check boxes and radios were drawn as text fields until Luna needed them to
/// be drawn as XP drew them; a theme that has not been told otherwise goes on
/// drawing them exactly as it did.
[[nodiscard]] Theme with_inherited(Theme theme, Part to, Part from) {
  for (int i = 0; i <= static_cast<int>(State::Focused); ++i) {
    const auto state = static_cast<State>(i);
    if (!theme.defines(to, state) && theme.defines(from, state)) {
      theme.set(to, state, theme.style(from, state));
    }
  }
  return theme;
}

[[nodiscard]] Theme finished(Theme theme) {
  theme = with_inherited(std::move(theme), Part::Check, Part::Input);
  theme = with_inherited(std::move(theme), Part::Radio, Part::Input);
  // A progress bar was a slider's groove, filled with its thumb, and the
  // caption buttons were tool buttons, until Luna drew them as XP did.
  if (!theme.defines(Part::Progress, State::Selected)) {
    theme.set(Part::Progress, State::Selected, theme.style(Part::SliderThumb, State::Normal));
  }
  theme = with_inherited(std::move(theme), Part::Progress, Part::Slider);
  theme = with_inherited(std::move(theme), Part::Caption, Part::ToolButton);
  // A list's headings were a tool button's surface; a dropdown's arrow is one
  // only where `dropdown_button` says so, and is styled like one regardless.
  theme = with_inherited(std::move(theme), Part::ListHeader, Part::ToolButton);
  theme = with_inherited(std::move(theme), Part::DropButton, Part::ToolButton);
  // Without a pulse of its own, the default button's glow is how it already
  // looks, and pulsing towards it changes nothing.
  if (!theme.defines(Part::DefaultGlow, State::Normal)) {
    theme.set(Part::DefaultGlow, State::Normal, theme.style(Part::Button, State::Focused));
  }
  return with_inherited(std::move(theme), Part::CaptionClose, Part::Caption);
}

/// Built once. Themes are immutable and shared, and rebuilding this list per
/// lookup would be needless work in a paint loop.
[[nodiscard]] const std::array<Theme, 4>& all_themes() {
  static const std::array<Theme, 4> themes{finished(make_flat()), finished(make_xp()),
                                           finished(make_aero()), finished(make_terminal())};
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
