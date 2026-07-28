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

/// Everything needed to draw one surface in one state.
struct SurfaceStyle {
  Fill fill;
  std::optional<Bevel> bevel;
  std::optional<Shadow> shadow;

  Color border;
  double border_width = 0.0;
  double corner_radius = 0.0;

  Color text;
  /// A soft halo behind text, which is what keeps Aero's labels legible over
  /// glass. Zero means none.
  double text_glow = 0.0;

  friend bool operator==(const SurfaceStyle&, const SurfaceStyle&) = default;
};

/// The surfaces a theme can style. Adding one here is what makes it themeable;
/// a widget that invented its own look instead would be invisible to every
/// theme.
enum class Part {
  Window,       ///< the application background
  TitleBar,
  Panel,        ///< a docked region: timeline, inspector, browser
  PanelHeader,
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
};

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

/// Sizes and spacing. Owned by the theme because chrome and spacing are not
/// independent: a bevelled button needs room for its bevel, and flat chrome
/// looks wrong with the padding XP needs.
struct Metrics {
  double control_height = 24.0;
  double padding_x = 10.0;
  double padding_y = 4.0;
  /// Space between adjacent controls.
  double spacing = 6.0;
  /// Space between a panel's edge and its contents.
  double panel_padding = 8.0;

  double title_bar_height = 30.0;
  double panel_header_height = 26.0;
  double scrollbar_width = 12.0;

  double font_size = 13.0;
  double small_font_size = 11.0;
  /// Multiplied by the font size to get a line's height.
  double line_height = 1.35;

  /// Timeline geometry, which is chrome as much as anything else: XP-era tracks
  /// were taller because their clips had bevels.
  double track_height = 56.0;
  double audio_track_height = 44.0;
  double ruler_height = 24.0;

  friend bool operator==(const Metrics&, const Metrics&) = default;
};

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
