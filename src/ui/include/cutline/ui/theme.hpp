#pragma once

/// What the interface looks like, as data.
///
/// A theme here is not a palette. Swapping colours cannot turn a flat control
/// into a Windows XP one — that needs bevels with separate light and dark
/// edges, a glossy multi-stop gradient, and more padding to sit around the
/// bevel. Nor can it produce Vista's glass, which needs the backdrop blurred
/// and tinted. So a theme describes *chrome*: fills, bevels, borders, corner
/// radii, shadows, and the metrics that go with them.
///
/// Two consequences run through the whole widget layer:
///
///   - **Widgets never draw themselves.** A widget asks the theme for the style
///     of its kind and state and hands that to a painter. A widget that reached
///     for a colour directly would be the one control a theme could not change,
///     and there is no way to find those except by looking.
///   - **The theme owns metrics too.** Bevelled chrome needs different spacing
///     than flat chrome; a theme that could only repaint would come out cramped
///     or loose depending on which one it was written against.
///
/// All of this is pure data and pure lookup, deliberately: it is the half of
/// the UI that can be tested without a GPU, a window, or Skia.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::ui {

/// Non-linear sRGB with straight alpha, which is the space interface colours
/// are specified and picked in. Compositing video is linear; drawing a button
/// is not, and pretending otherwise would make every hand-picked colour wrong.
struct Color {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;

  friend bool operator==(const Color&, const Color&) = default;
};

/// Parses `#rgb`, `#rrggbb`, or `#rrggbbaa`. Returns the fallback on anything
/// else, so a malformed theme file degrades rather than failing to open.
[[nodiscard]] Color parse_color(std::string_view text, Color fallback = {}) noexcept;

/// The colour `t` of the way from `a` to `b`.
[[nodiscard]] Color mix(const Color& a, const Color& b, float t) noexcept;

/// `#rrggbbaa`, or `#rrggbb` when fully opaque.
[[nodiscard]] std::string to_hex(const Color& color);

struct GradientStop {
  /// Position along the gradient, 0 to 1.
  float at = 0.0f;
  Color color;

  friend bool operator==(const GradientStop&, const GradientStop&) = default;
};

/// How a surface is filled.
enum class FillKind {
  Solid,
  /// Multi-stop linear gradient. Two stops make a plain ramp; XP's gloss needs
  /// four, with a hard step at the midpoint.
  Gradient,
  /// Blurs whatever is behind and tints it. This is the one fill that cannot be
  /// faked with colours, and the reason Vista is on the list at all.
  Glass,
};

struct Fill {
  FillKind kind = FillKind::Solid;

  /// Used by `Solid`, and as the tint by `Glass`.
  Color color;

  /// Used by `Gradient`, in ascending order of `at`.
  std::vector<GradientStop> stops;
  /// Gradient direction in degrees, clockwise from a top-to-bottom ramp. Zero
  /// is vertical, which is what nearly all chrome uses.
  double angle_deg = 0.0;

  /// `Glass` only: how far the backdrop is blurred, in pixels.
  double blur_radius = 0.0;

  friend bool operator==(const Fill&, const Fill&) = default;

  [[nodiscard]] static Fill solid(Color color);
  [[nodiscard]] static Fill gradient(std::vector<GradientStop> stops, double angle_deg = 0.0);
  [[nodiscard]] static Fill glass(Color tint, double blur_radius);
};

/// A raised or sunken edge, drawn as light and dark borders on opposite sides.
///
/// This is what makes a control look pressed rather than merely darker, and it
/// is not expressible as a colour: the light and dark edges swap sides.
struct Bevel {
  double width = 1.0;
  /// Top and left when raised; bottom and right when inset.
  Color light;
  Color dark;
  bool inset = false;

  friend bool operator==(const Bevel&, const Bevel&) = default;
};

struct Shadow {
  double offset_x = 0.0;
  double offset_y = 0.0;
  double blur = 0.0;
  Color color;
  /// Drawn inside the shape rather than behind it, which is how a recessed
  /// well is drawn.
  bool inner = false;

  friend bool operator==(const Shadow&, const Shadow&) = default;
};

/// A band of its own fill just inside the border, round the whole surface.
///
/// Luna's buttons light up this way: an orange band under the pointer and a
/// blue one on the default button, two pixels wide and graded top to bottom,
/// so its sides fall halfway between its top and its bottom. A bevel cannot
/// draw it — a bevel is one colour on the top and left and another on the
/// bottom and right, and Luna's band is the same on both sides.
struct Rim {
  double width = 0.0;
  Fill fill;

  friend bool operator==(const Rim&, const Rim&) = default;
};

/// Everything needed to draw one surface in one state.
struct SurfaceStyle {
  Fill fill;
  std::optional<Bevel> bevel;
  std::optional<Shadow> shadow;
  std::optional<Rim> rim;
  /// A band of its own fill across the top, drawn over the border and clipped
  /// to the corners. Luna's tabs light up this way -- an orange line along the
  /// top of the one showing and the one under the pointer -- and a rim cannot,
  /// since it goes all the way round.
  std::optional<Rim> cap;
  /// The same along the bottom: the shaded rows under XP's list headings,
  /// orange under the pointer.
  std::optional<Rim> foot;

  Color border;
  double border_width = 0.0;
  double corner_radius = 0.0;

  Color text;
  /// A soft halo behind text, which is what keeps Aero's labels legible over
  /// glass. Zero means none.
  double text_glow = 0.0;
  /// A hard copy of the text behind it, offset; XP's caption had one.
  std::optional<Shadow> text_shadow;

  friend bool operator==(const SurfaceStyle&, const SurfaceStyle&) = default;
};

/// A style `t` of the way from `a` to `b`, for a control fading between two
/// states. Colours, widths and radii move; a gradient's stops move when both
/// have the same number of them, and otherwise the nearer style's are taken,
/// since two ramps of different shapes have no halfway.
[[nodiscard]] SurfaceStyle mix(const SurfaceStyle& a, const SurfaceStyle& b, float t);

/// The surfaces a theme can style. Adding one here is what makes it themeable;
/// a widget that invented its own look instead would be invisible to every
/// theme.
enum class Part {
  Window,       ///< the application background
  TitleBar,
  Panel,        ///< a docked region: timeline, inspector, browser
  PanelHeader,
  TabBar,       ///< the strip a docked group's tabs sit on
  Tab,          ///< one tab in it. `Selected` is the panel showing
  Splitter,     ///< the draggable strip between two docked regions
  Button,
  ToolButton,   ///< an icon-only button in a toolbar
  Input,        ///< a text field or numeric entry
  Slider,       ///< the groove
  SliderThumb,
  Menu,
  MenuItem,
  Tooltip,
  Clip,         ///< a clip block on the timeline
  TrackHeader,
  Playhead,
  Ruler,
  Scrollbar,
  ScrollThumb,
  Check,        ///< a check box's box. `Selected` styles the mark
  Radio,        ///< a radio button's circle. `Selected` styles the dot
  Progress,     ///< a progress bar's trough. `Selected` styles what is done
  Caption,      ///< a title bar's minimise and maximise buttons
  CaptionClose, ///< its close button, which XP and Vista drew in red
  DropButton,   ///< a dropdown's arrow, where `dropdown_button` draws one
  ListHeader,   ///< the strip of column headings over a list
};

/// The last part, for walking every one of them.
inline constexpr Part kLastPart = Part::ListHeader;

[[nodiscard]] std::string_view to_string(Part part) noexcept;
[[nodiscard]] std::optional<Part> part_from_string(std::string_view name) noexcept;

/// Interaction states. Not every part defines every one; lookup falls back.
enum class State {
  Normal,
  Hover,
  Pressed,
  Disabled,
  Selected,
  Focused,
};

[[nodiscard]] std::string_view to_string(State state) noexcept;
[[nodiscard]] std::optional<State> state_from_string(std::string_view name) noexcept;

/// How a theme's text is smoothed.
enum class TextSmoothing {
  /// ClearType: smoothed across the colour of each pixel, placed between
  /// pixels. Vista's default, and what a modern interface expects.
  ClearType,
  /// XP's "Standard": no smoothing at body sizes, each glyph hinted onto whole
  /// pixels. Crisp black stems, which is half of why XP looks like XP. Large
  /// type -- a caption -- was smoothed in grey, and is here too.
  Standard,
};

/// How a check box's tick is drawn.
enum class CheckMark {
  /// Two strokes, at any size.
  Stroked,
  /// XP's tick: a fixed pattern of twenty-one pixels in a thirteen-pixel box,
  /// scaled with the box. Its stepped edge is the look, and strokes cannot
  /// make it.
  Luna,
};

/// The shape of a slider's thumb.
enum class ThumbShape {
  /// A block across the whole groove, with the groove filled up to it.
  Block,
  /// XP's trackbar: a narrow thumb with a point at the bottom, over a thin
  /// channel that is never filled. Drawn a pixel at a time on an eleven by
  /// twenty-one grid, scaled with the control, for the same reason as the tick.
  Pointer,
  /// Vista's: ten pixels by seventeen in a white rim and a grey hairline, the
  /// button's two-tone face, a short point, over a hairline channel.
  VistaPointer,
};

/// How the caption buttons' glyphs are drawn.
enum class CaptionGlyphs {
  /// Hairlines, the way Windows 10 and later draw them.
  Stroked,
  /// XP's: heavy white pixel patterns on an eleven-pixel grid -- a bar, a box
  /// with a thick top, a cross three pixels wide.
  Luna,
};

/// How a theme shows where the keyboard is.
enum class FocusCue {
  /// A dotted rectangle just inside the control, one pixel on and one off --
  /// what XP and Vista drew, and only once the keyboard had been used.
  Dotted,
  /// A ring in the accent just outside it, the way a flat interface does.
  Ring,
};

/// Sizes and spacing. Owned by the theme because chrome and spacing are not
/// independent: a bevelled button needs room for its bevel, and flat chrome
/// looks wrong with the padding XP needs.
struct Metrics {
  double control_height = 24.0;
  double padding_x = 10.0;
  double padding_y = 4.0;
  /// Space between adjacent controls.
  double spacing = 6.0;
  /// Space between controls that read as one thing -- a row of tool buttons,
  /// a strip of sequence tabs. Close, but not touching, so each is still a
  /// target of its own.
  double tight_spacing = 2.0;
  /// Space between groups: one section of a settings page and the next, or a
  /// list and the page beside it. Larger than `spacing` so the eye sees the
  /// groups before it sees the controls.
  double group_spacing = 12.0;
  /// Space between a panel's edge and its contents.
  double panel_padding = 8.0;

  double title_bar_height = 30.0;
  double panel_header_height = 26.0;
  /// One row of a list: the browser's entries, and anything else that stacks
  /// identical rows. Its own metric rather than `control_height`, because a
  /// theme with roomy buttons does not necessarily want a roomy list.
  double list_row_height = 26.0;
  double scrollbar_width = 12.0;
  double splitter_width = 6.0;
  /// The smallest a pane may be dragged to. Stops a docked region being lost
  /// off the edge with no way to get it back.
  double min_pane = 48.0;

  /// The typeface, by family name, or empty for the system's. Each theme names
  /// the one its period used -- Tahoma for XP, Segoe UI for Vista -- from the
  /// system, never bundled.
  std::string font_family;
  double font_size = 13.0;
  double small_font_size = 11.0;
  /// The marks along a fader's or a knob's scale, which are read in passing
  /// and have to fit between the ticks.
  double tiny_font_size = 10.0;
  /// A section's heading inside a page or a dialog, drawn bold. A step above
  /// the body where the period did that -- Vista set its headings larger --
  /// and the body's own size where it did not.
  double heading_font_size = 14.0;
  /// Multiplied by the font size to get a line's height.
  double line_height = 1.35;

  /// Timeline geometry, which is chrome as much as anything else: XP-era tracks
  /// were taller because their clips had bevels.
  double track_height = 56.0;
  double audio_track_height = 44.0;
  double ruler_height = 24.0;
  /// Width of the column of track headers down the left of the timeline.
  double track_header_width = 140.0;

  /// How long a control takes to fade into how it looks under the pointer,
  /// and back out of it, in seconds. Zero snaps, which is XP; Vista's glow
  /// arrived quickly and left slowly.
  double hover_in = 0.0;
  double hover_out = 0.0;

  FocusCue focus_cue = FocusCue::Ring;
  TextSmoothing text_smoothing = TextSmoothing::ClearType;
  CheckMark check_mark = CheckMark::Stroked;
  /// Whether a chosen radio's dot is drawn in the `Selected` style's fill --
  /// lit like a bead, as XP's and Vista's were -- rather than flat in its ink.
  bool lit_radio_dot = false;
  ThumbShape slider_thumb = ThumbShape::Block;
  CaptionGlyphs caption_glyphs = CaptionGlyphs::Stroked;
  /// A caption button's side, as a square inside the title bar with a gap of
  /// `tight_spacing` after it; zero fills the bar's height and is wider than
  /// tall, as modern Windows draws them.
  double caption_button = 0.0;
  /// A progress bar drawn as blocks this wide, `progress_gap` apart, inside
  /// its trough, as XP drew it; zero fills it continuously.
  double progress_block = 0.0;
  double progress_gap = 0.0;
  /// How much shorter a tab that is not showing stands than the one that is.
  /// XP's showing tab rose two pixels above the others.
  double tab_rise = 0.0;
  /// The typeface and size of a window's title, or empty and zero for the
  /// body's. XP set its captions in Trebuchet MS, a step larger.
  std::string caption_font_family;
  double caption_font_size = 0.0;
  /// Whether the showing tab's title is bold. XP's was not.
  bool bold_active_tab = true;
  /// How long the lines of a grip across a scrollbar's thumb are, drawn in
  /// the thumb's text colour beside lines of its border's; zero for none.
  double scroll_grip = 0.0;
  /// A menu's rows and rules, and the room between its edge and its first and
  /// last rows. Zero rows and rules follow `list_row_height`; XP's menus were
  /// tighter than its lists, seventeen pixels a row with nine between groups.
  double menu_row_height = 0.0;
  double menu_separator_height = 0.0;
  double menu_padding = 4.0;
  /// Whether a list's headings are divided by a line in the header's border
  /// colour with a white one beside it, as XP's were.
  bool header_dividers = false;
  /// Whether a dropdown's arrow sits on a button of its own, as XP's did,
  /// rather than being a bare chevron.
  bool dropdown_button = false;

  /// How long a view takes to get where it was sent -- a zoom, a scroll by
  /// the wheel -- and how long a popup takes to arrive, in seconds. Zero
  /// jumps, which is what XP's timelines did.
  double motion = 0.0;
  double popup_fade = 0.0;

  friend bool operator==(const Metrics&, const Metrics&) = default;
};

/// The steps of a theme's spacing, by what the space is between rather than
/// by how many pixels, so a layout reads the same in every theme and changes
/// with it.
enum class Space { Tight, Normal, Group };

[[nodiscard]] constexpr double space(const Metrics& metrics, Space step) noexcept {
  switch (step) {
    case Space::Tight:
      return metrics.tight_spacing;
    case Space::Normal:
      return metrics.spacing;
    case Space::Group:
      return metrics.group_spacing;
  }
  return metrics.spacing;
}

/// A complete look.
///
/// Styles are stored sparsely: a theme defines what differs and lookup fills in
/// the rest. That is what keeps a theme file to a readable size instead of a
/// matrix of every part times every state.
struct Theme {
  std::string id;
  std::string name;
  /// Whether the palette is dark. Affects things chosen outside the theme,
  /// like which icon set reads better.
  bool dark = true;

  Metrics metrics;

  /// The style used when a part defines nothing at all.
  SurfaceStyle fallback;

  /// Accent, used for selection, focus rings and the playhead. Kept apart from
  /// the per-part styles because so many parts derive from it.
  Color accent;

  std::map<Part, std::map<State, SurfaceStyle>> styles;

  /// The style for a part in a state.
  ///
  /// Falls back from the exact state, to the part's `Normal`, to the theme's
  /// `fallback`. A missing entry is normal rather than an error — most parts
  /// look the same hovered as they do at rest.
  [[nodiscard]] const SurfaceStyle& style(Part part, State state = State::Normal) const noexcept;

  /// Whether an exact entry exists, without the fallback chain. For tests and
  /// for a theme editor, which wants to show what a theme actually sets.
  [[nodiscard]] bool defines(Part part, State state) const noexcept;

  /// Adds or replaces one entry.
  void set(Part part, State state, SurfaceStyle style);
};

/// The built-in themes, in the order the UI should offer them.
///
/// These are the argument for the whole design: they differ in chrome, not
/// merely in colour, and a palette-only theme system could not express any of
/// them.
[[nodiscard]] std::span<const Theme> built_in_themes();

/// A built-in theme by id, or null.
[[nodiscard]] const Theme* built_in_theme(std::string_view id) noexcept;

/// The theme used when none is chosen.
[[nodiscard]] const Theme& default_theme();

}  // namespace cutline::ui
