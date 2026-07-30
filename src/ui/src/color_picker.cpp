#include "cutline/ui/color_picker.hpp"

#include <algorithm>
#include <cmath>

namespace cutline::ui {
namespace {

/// The square's natural size. Not a metric: a colour square wants to be big
/// enough to pick a shade in, and that has nothing to do with how roomy a
/// theme's buttons are.
constexpr double kFieldWidth = 176.0;
constexpr double kFieldHeight = 132.0;
constexpr double kStripWidth = 14.0;

/// How far one arrow key moves along the square, as a fraction of it.
constexpr double kNudge = 0.02;
constexpr double kCoarse = 5.0;
/// And along the hue wheel, in degrees.
constexpr double kHueNudge = 4.0;

constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color kBlack{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Color kCheckerLight{0.80f, 0.80f, 0.80f, 1.0f};
constexpr Color kCheckerDark{0.60f, 0.60f, 0.60f, 1.0f};

[[nodiscard]] double fraction_in(double value, double from, double extent) noexcept {
  if (extent <= 0.0) return 0.0;
  return std::clamp((value - from) / extent, 0.0, 1.0);
}

/// A ring that can be seen against any colour, which is the whole difficulty
/// with a marker on a colour picker: white vanishes on white and black on
/// black, so it has to be both.
void paint_ring(Painter& painter, double cx, double cy, double radius) {
  const Rect outer{cx - radius - 1.5, cy - radius - 1.5, (radius + 1.5) * 2.0,
                   (radius + 1.5) * 2.0};
  painter.stroke(outer, radius + 1.5, kBlack, 1.5);
  const Rect inner{cx - radius, cy - radius, radius * 2.0, radius * 2.0};
  painter.stroke(inner, radius, kWhite, 1.5);
}

/// The same, for a strip: a bar across it rather than a ring on it.
void paint_bar(Painter& painter, const Rect& strip, double y) {
  const Rect outer{strip.x - 2.0, y - 4.0, strip.width + 4.0, 8.0};
  painter.stroke(outer, 3.0, kBlack, 1.5);
  painter.stroke(outer.inset(1.5), 2.0, kWhite, 1.5);
}

/// The six corners of the hue wheel, as gradient stops down a strip.
[[nodiscard]] std::vector<GradientStop> hue_stops() {
  std::vector<GradientStop> stops;
  stops.reserve(7);
  for (int i = 0; i <= 6; ++i) {
    stops.push_back(GradientStop{static_cast<float>(i) / 6.0f,
                                 from_hsv(Hsv{i * 60.0, 1.0, 1.0})});
  }
  return stops;
}

}  // namespace

// ------------------------------------------------------------- conversion --

Hsv to_hsv(const Color& color) noexcept {
  const double r = std::clamp(static_cast<double>(color.r), 0.0, 1.0);
  const double g = std::clamp(static_cast<double>(color.g), 0.0, 1.0);
  const double b = std::clamp(static_cast<double>(color.b), 0.0, 1.0);

  const double high = std::max({r, g, b});
  const double low = std::min({r, g, b});
  const double span = high - low;

  Hsv hsv;
  hsv.v = high;
  hsv.s = high <= 0.0 ? 0.0 : span / high;
  if (span <= 0.0) return hsv;  // a grey: no hue to report, and 0 is the convention

  if (high == r) {
    hsv.h = 60.0 * std::fmod((g - b) / span, 6.0);
  } else if (high == g) {
    hsv.h = 60.0 * ((b - r) / span + 2.0);
  } else {
    hsv.h = 60.0 * ((r - g) / span + 4.0);
  }
  if (hsv.h < 0.0) hsv.h += 360.0;
  return hsv;
}

Color from_hsv(const Hsv& hsv, float alpha) noexcept {
  const double s = std::clamp(hsv.s, 0.0, 1.0);
  const double v = std::clamp(hsv.v, 0.0, 1.0);
  double h = std::fmod(hsv.h, 360.0);
  if (h < 0.0) h += 360.0;

  const double chroma = v * s;
  const double sector = h / 60.0;
  const double second = chroma * (1.0 - std::abs(std::fmod(sector, 2.0) - 1.0));
  const double base = v - chroma;

  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  switch (static_cast<int>(sector)) {
    case 0: r = chroma; g = second; break;
    case 1: r = second; g = chroma; break;
    case 2: g = chroma; b = second; break;
    case 3: g = second; b = chroma; break;
    case 4: r = second; b = chroma; break;
    default: r = chroma; b = second; break;
  }

  return Color{static_cast<float>(r + base), static_cast<float>(g + base),
               static_cast<float>(b + base), std::clamp(alpha, 0.0f, 1.0f)};
}

// -------------------------------------------------------------- chequers --

void paint_checkerboard(Painter& painter, const Rect& bounds, double corner_radius,
                        double square) {
  if (bounds.empty() || square <= 0.0) return;

  painter.push_clip(bounds, corner_radius);
  painter.fill(bounds, corner_radius, Fill::solid(kCheckerLight));

  int row = 0;
  for (double y = bounds.y; y < bounds.bottom(); y += square, ++row) {
    int column = 0;
    for (double x = bounds.x; x < bounds.right(); x += square, ++column) {
      if ((row + column) % 2 == 0) continue;
      const Rect cell{x, y, std::min(square, bounds.right() - x),
                      std::min(square, bounds.bottom() - y)};
      painter.fill(cell, 0.0, Fill::solid(kCheckerDark));
    }
  }
  painter.pop_clip();
}

// ---------------------------------------------------------------- picker --

ColorPicker::ColorPicker(Color color) {
  set_focusable(true);

  hsv_ = to_hsv(color);
  alpha_ = std::clamp(color.a, 0.0f, 1.0f);

  auto& hex = emplace<TextField>(to_hex(color));
  hex.set_placeholder("#rrggbb");
  hex.set_on_commit([this](const std::string& text) {
    // The fallback is what the picker already has, so nonsense leaves the
    // colour alone rather than turning it black. The field is put back in
    // step by `refresh_hex`, which is how a typo visibly undoes itself.
    // Qualified: the constructor's own parameter is called `color` too, and it
    // is the one an unqualified name would find.
    gesture_start_ = this->color();
    set_color(parse_color(text, this->color()));
    changed();
    commit();
  });
  hex_ = &hex;
}

Color ColorPicker::color() const noexcept { return from_hsv(hsv_, alpha_); }

void ColorPicker::set_color(const Color& color) {
  Hsv next = to_hsv(color);
  // A grey has no hue in it and black has no saturation either, so reading
  // them back would throw away coordinates nobody changed. See the header.
  if (next.s <= 0.0) next.h = hsv_.h;
  if (next.v <= 0.0) next.s = hsv_.s;

  hsv_ = next;
  alpha_ = std::clamp(color.a, 0.0f, 1.0f);
  refresh_field();
  refresh_hex();
}

void ColorPicker::set_hsv(const Hsv& hsv) {
  hsv_.h = std::fmod(hsv.h, 360.0);
  if (hsv_.h < 0.0) hsv_.h += 360.0;
  hsv_.s = std::clamp(hsv.s, 0.0, 1.0);
  hsv_.v = std::clamp(hsv.v, 0.0, 1.0);
  refresh_field();
  refresh_hex();
}

void ColorPicker::set_alpha(float alpha) noexcept { alpha_ = std::clamp(alpha, 0.0f, 1.0f); }

void ColorPicker::set_alpha_enabled(bool enabled) {
  if (alpha_enabled_ == enabled) return;
  alpha_enabled_ = enabled;
  // The strip is part of the width, so this is a size change rather than a
  // repaint.
  invalidate_layout();
}

// ---------------------------------------------------------------- regions --

Rect ColorPicker::field() const {
  const Rect area = bounds().inset(padding_);
  if (area.empty()) return {};

  const double strips = (kStripWidth + gap_) * (alpha_enabled_ ? 2.0 : 1.0);
  const double width = std::max(0.0, area.width - strips);
  const double height = std::max(0.0, area.height - gap_ - hex_height_);
  return Rect{area.x, area.y, width, height};
}

Rect ColorPicker::hue_strip() const {
  const Rect square = field();
  if (square.empty()) return {};
  return Rect{square.right() + gap_, square.y, kStripWidth, square.height};
}

Rect ColorPicker::alpha_strip() const {
  if (!alpha_enabled_) return {};
  const Rect hue = hue_strip();
  if (hue.empty()) return {};
  return Rect{hue.right() + gap_, hue.y, kStripWidth, hue.height};
}

LayoutItem ColorPicker::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  const double padding = metrics.panel_padding;
  const double gap = metrics.spacing;

  if (axis == Axis::Vertical) {
    return LayoutItem::fixed(2.0 * padding + kFieldHeight + gap + metrics.control_height);
  }
  const double strips = (kStripWidth + gap) * (alpha_enabled_ ? 2.0 : 1.0);
  return LayoutItem::fixed(2.0 * padding + kFieldWidth + strips);
}

void ColorPicker::layout(const LayoutContext& context) {
  // Taken before the regions are worked out, since every one of them is
  // measured from these.
  padding_ = context.metrics().panel_padding;
  gap_ = context.metrics().spacing;
  hex_height_ = context.metrics().control_height;

  if (hex_ != nullptr) {
    const Rect area = bounds().inset(padding_);
    const Rect square = field();
    hex_->arrange(Rect{area.x, square.bottom() + gap_, area.width, hex_height_}, context);
  }

  refresh_field();
}

// --------------------------------------------------------------- painting --

void ColorPicker::refresh_field() {
  const Rect square = field();
  const int width = static_cast<int>(std::lround(square.width));
  const int height = static_cast<int>(std::lround(square.height));
  if (width <= 0 || height <= 0) {
    field_pixels_.clear();
    field_w_ = 0;
    field_h_ = 0;
    field_hue_ = -1.0;
    return;
  }
  // Nothing to do when neither the hue nor the size moved, which is every
  // frame of a saturation drag.
  if (width == field_w_ && height == field_h_ && field_hue_ == hsv_.h) return;

  field_pixels_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
  for (int y = 0; y < height; ++y) {
    const double value = 1.0 - (static_cast<double>(y) + 0.5) / height;
    for (int x = 0; x < width; ++x) {
      const double saturation = (static_cast<double>(x) + 0.5) / width;
      const Color pixel = from_hsv(Hsv{hsv_.h, saturation, value});
      const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4u;
      field_pixels_[at + 0] = static_cast<std::uint8_t>(std::lround(pixel.r * 255.0f));
      field_pixels_[at + 1] = static_cast<std::uint8_t>(std::lround(pixel.g * 255.0f));
      field_pixels_[at + 2] = static_cast<std::uint8_t>(std::lround(pixel.b * 255.0f));
      field_pixels_[at + 3] = 255;
    }
  }

  field_w_ = width;
  field_h_ = height;
  field_hue_ = hsv_.h;
}

void ColorPicker::refresh_hex() {
  // Left alone while it is being typed in: replacing the text under a caret
  // mid-word is how a field fights the person using it.
  if (hex_ == nullptr || hex_->focused()) return;
  hex_->set_text(to_hex(color()));
}

void ColorPicker::paint_content(Painter& painter, const Theme& theme) const {
  const Rect square = field();
  if (square.empty()) return;

  if (!field_pixels_.empty()) {
    painter.image(square, ImageView{field_pixels_.data(), field_w_, field_h_, field_w_ * 4});
  }
  paint_ring(painter, square.x + hsv_.s * square.width,
             square.y + (1.0 - hsv_.v) * square.height, 5.0);

  const Rect hue = hue_strip();
  painter.fill(hue, 2.0, Fill::gradient(hue_stops()));
  paint_bar(painter, hue, hue.y + (hsv_.h / 360.0) * hue.height);

  const Rect strip = alpha_strip();
  if (!strip.empty()) {
    paint_checkerboard(painter, strip, 2.0, 4.0);
    const Color opaque = from_hsv(hsv_, 1.0f);
    Color clear = opaque;
    clear.a = 0.0f;
    painter.fill(strip, 2.0, Fill::gradient({GradientStop{0.0f, opaque}, GradientStop{1.0f, clear}}));
    // Top is opaque, so the bar runs the other way from the hue's.
    paint_bar(painter, strip, strip.y + (1.0 - alpha_) * strip.height);
  }

  // A border round the square, so a nearly-white corner does not bleed into a
  // pale theme's popup.
  const SurfaceStyle& style = theme.style(part(), state());
  Color edge = style.text;
  edge.a *= 0.35f;
  painter.stroke(square, 0.0, edge, 1.0);
}

// ------------------------------------------------------------------ input --

ColorPicker::Region ColorPicker::region_at(double x, double y) const {
  if (field().contains(x, y)) return Region::Field;
  if (hue_strip().contains(x, y)) return Region::Hue;
  if (alpha_strip().contains(x, y)) return Region::Alpha;
  return Region::None;
}

void ColorPicker::drag_to(Region region, double x, double y) {
  switch (region) {
    case Region::Field: {
      const Rect square = field();
      hsv_.s = fraction_in(x, square.x, square.width);
      hsv_.v = 1.0 - fraction_in(y, square.y, square.height);
      break;
    }
    case Region::Hue: {
      const Rect strip = hue_strip();
      hsv_.h = fraction_in(y, strip.y, strip.height) * 360.0;
      refresh_field();
      break;
    }
    case Region::Alpha: {
      const Rect strip = alpha_strip();
      alpha_ = static_cast<float>(1.0 - fraction_in(y, strip.y, strip.height));
      break;
    }
    case Region::None:
      return;
  }
  changed();
}

void ColorPicker::changed() {
  refresh_hex();
  // The hex field's own measurements are stale until it is laid out again, and
  // its text just changed underneath them.
  invalidate_layout();
  if (on_change_) on_change_(color());
}

void ColorPicker::commit() {
  const Color now = color();
  if (now == gesture_start_) return;
  gesture_start_ = now;
  if (on_commit_) on_commit_(now);
}

bool ColorPicker::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;

  const Region region = region_at(event.x, event.y);
  if (region == Region::None) {
    // Still handled: a press on the picker's own padding must not fall through
    // to the dismissal that closes it.
    return true;
  }

  if (host() != nullptr) host()->set_focus(this);
  dragging_ = region;
  gesture_start_ = color();
  drag_to(region, event.x, event.y);
  return true;
}

bool ColorPicker::on_mouse_move(const MouseEvent& event) {
  if (dragging_ == Region::None) return false;
  drag_to(dragging_, event.x, event.y);
  return true;
}

bool ColorPicker::on_mouse_up(const MouseEvent& event) {
  if (event.button != MouseButton::Left || dragging_ == Region::None) return false;
  dragging_ = Region::None;
  commit();
  return true;
}

bool ColorPicker::on_key_down(const KeyEvent& event) {
  if (event.modifiers.alt) return false;

  const double step = kNudge * (event.modifiers.shift ? kCoarse : 1.0);
  const double hue_step = kHueNudge * (event.modifiers.shift ? kCoarse : 1.0);
  const Hsv before = hsv_;

  switch (event.key) {
    // Control turns the horizontal arrows into hue, which is the one
    // coordinate the square itself cannot reach.
    case Key::Left:
      if (event.modifiers.control) {
        hsv_.h = std::fmod(hsv_.h - hue_step + 360.0, 360.0);
        refresh_field();
      } else {
        hsv_.s = std::clamp(hsv_.s - step, 0.0, 1.0);
      }
      break;
    case Key::Right:
      if (event.modifiers.control) {
        hsv_.h = std::fmod(hsv_.h + hue_step, 360.0);
        refresh_field();
      } else {
        hsv_.s = std::clamp(hsv_.s + step, 0.0, 1.0);
      }
      break;
    case Key::Up:
      hsv_.v = std::clamp(hsv_.v + step, 0.0, 1.0);
      break;
    case Key::Down:
      hsv_.v = std::clamp(hsv_.v - step, 0.0, 1.0);
      break;
    default:
      return false;
  }

  if (hsv_ == before) return true;
  gesture_start_ = from_hsv(before, alpha_);
  changed();
  // A key press is a whole gesture: there is no release to wait for, and one
  // press is one edit.
  commit();
  return true;
}

// ---------------------------------------------------------------- swatch --

ColorSwatch::ColorSwatch(Color color) : color_(color) { set_focusable(true); }

ColorSwatch::~ColorSwatch() {
  // The open picker holds callbacks capturing this. Closing is deferred, but a
  // closing popup takes no input and is not painted, so nothing can reach them
  // after this returns.
  if (open_ && host() != nullptr) host()->close_popup();
}

void ColorSwatch::open() {
  WidgetHost* owner = host();
  if (owner == nullptr) return;

  auto picker = std::make_unique<ColorPicker>(color_);
  picker->set_alpha_enabled(alpha_enabled_);
  picker->set_on_change([this](const Color& color) {
    color_ = color;
    if (on_change_) on_change_(color);
  });
  picker->set_on_commit([this](const Color& color) {
    color_ = color;
    if (on_commit_) on_commit_(color);
  });

  open_ = true;
  owner->open_popup(std::move(picker), bounds());
}

Rect ColorSwatch::block() const {
  const Rect area = bounds();
  constexpr double inset = 4.0;
  const double side = std::max(0.0, area.height - 2.0 * inset);
  return Rect{area.x + inset, area.y + inset, std::min(side, std::max(0.0, area.width - inset)),
              side};
}

LayoutItem ColorSwatch::sizing(Axis axis, const LayoutContext& context) const {
  const Metrics& metrics = context.metrics();
  if (axis == Axis::Vertical) return LayoutItem::fixed(metrics.control_height);

  // Sized for `#rrggbbaa` whether or not this colour needs the alpha, so the
  // control does not change width as a colour is dragged towards transparent.
  const double text = context.text.measure("#rrggbbaa", metrics.font_size, false);
  return LayoutItem::fixed(metrics.control_height + metrics.spacing + text +
                           2.0 * metrics.padding_x);
}

void ColorSwatch::paint_content(Painter& painter, const Theme& theme) const {
  const SurfaceStyle& style = theme.style(part(), state());
  const Rect swatch = block();
  if (swatch.empty()) return;

  paint_checkerboard(painter, swatch, 2.0, 4.0);
  painter.fill(swatch, 2.0, Fill::solid(color_));

  Color edge = style.text;
  edge.a *= 0.45f;
  painter.stroke(swatch, 2.0, edge, 1.0);

  const Rect area = bounds();
  const double x = swatch.right() + 6.0;
  const Rect text{x, area.y, std::max(0.0, area.right() - x - 4.0), area.height};
  painter.text(text_run(text, to_hex(color_), style, 13.0, TextAlign::Left, false));
}

bool ColorSwatch::on_mouse_down(const MouseEvent& event) {
  if (event.button != MouseButton::Left) return false;
  if (host() != nullptr) host()->set_focus(this);
  open();
  return true;
}

bool ColorSwatch::on_key_down(const KeyEvent& event) {
  if (!event.modifiers.none()) return false;
  if (event.key != Key::Enter && event.key != Key::Space) return false;
  open();
  return true;
}

}  // namespace cutline::ui
