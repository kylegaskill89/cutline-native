#include "cutline/text/raster.hpp"

#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkTypeface_win.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace cutline::text {
namespace {

/// How far apart the baselines of a multi-line title sit, as a multiple of the
/// font size. The reference's canvas used 1.2, which is the CSS default.
constexpr double kLineHeight = 1.2;

/// Room left around the text for a stroke and a shadow, so neither is clipped
/// by the edge of the image.
constexpr double kShadowOffset = 0.04;  ///< of the font size
constexpr double kShadowBlur = 0.06;    ///< of the font size

/// The font manager, made once. DirectWrite enumeration is not cheap and a
/// title is rasterised whenever its text changes.
[[nodiscard]] const sk_sp<SkFontMgr>& font_manager() {
  static const sk_sp<SkFontMgr> manager = SkFontMgr_New_DirectWrite();
  return manager;
}

/// The first family in a CSS-style list that this machine actually has.
///
/// A project written elsewhere can name anything, and `system-ui, sans-serif`
/// is the model's own default. Falling through the list and then to the system
/// face is what every browser does with the same string.
[[nodiscard]] sk_sp<SkTypeface> typeface_for(const core::TextSpec& spec) {
  const sk_sp<SkFontMgr>& manager = font_manager();
  if (manager == nullptr) return nullptr;

  const SkFontStyle style(spec.bold ? SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight,
                          SkFontStyle::kNormal_Width,
                          spec.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);

  std::string_view families = spec.font_family;
  while (!families.empty()) {
    const std::size_t comma = families.find(',');
    std::string_view name = families.substr(0, comma);
    families = comma == std::string_view::npos ? std::string_view{}
                                              : families.substr(comma + 1);

    // Trim spaces and the quotes a CSS font list may carry.
    while (!name.empty() && (name.front() == ' ' || name.front() == '\'' || name.front() == '"')) {
      name.remove_prefix(1);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\'' || name.back() == '"')) {
      name.remove_suffix(1);
    }
    if (name.empty()) continue;

    // Generic families are not families: nothing is installed under the name
    // "sans-serif", and asking for it by name would fail where the system
    // default is exactly what is wanted.
    if (name == "system-ui" || name == "sans-serif" || name == "serif" || name == "monospace" ||
        name == "cursive" || name == "fantasy" || name == "ui-sans-serif") {
      break;
    }

    if (sk_sp<SkTypeface> found = manager->matchFamilyStyle(std::string(name).c_str(), style);
        found != nullptr) {
      return found;
    }
  }

  // Null family means the system default.
  return manager->legacyMakeTypeface(nullptr, style);
}

/// `#rgb`, `#rrggbb` or `#rrggbbaa`, falling back rather than failing: a
/// malformed colour in a project should not make a title disappear.
[[nodiscard]] SkColor4f parse(std::string_view text, SkColor4f fallback) {
  if (!text.empty() && text.front() == '#') text.remove_prefix(1);

  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };

  std::array<int, 8> values{};
  if (text.size() != 3 && text.size() != 6 && text.size() != 8) return fallback;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const int value = digit(text[i]);
    if (value < 0) return fallback;
    values[i] = value;
  }

  const auto channel = [&](std::size_t index) {
    // Shorthand repeats each digit, so #abc is #aabbcc.
    if (text.size() == 3) return static_cast<float>(values[index] * 17) / 255.0f;
    const std::size_t at = index * 2;
    return static_cast<float>(values[at] * 16 + values[at + 1]) / 255.0f;
  };

  return SkColor4f{channel(0), channel(1), channel(2),
                   text.size() == 8 ? channel(3) : 1.0f};
}

/// The lines of a title. Explicit breaks only.
[[nodiscard]] std::vector<std::string_view> lines_of(std::string_view content) {
  std::vector<std::string_view> lines;
  while (true) {
    const std::size_t newline = content.find('\n');
    if (newline == std::string_view::npos) {
      lines.push_back(content);
      return lines;
    }
    std::string_view line = content.substr(0, newline);
    // Carriage returns from a file written on Windows are not part of the text.
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    lines.push_back(line);
    content = content.substr(newline + 1);
  }
}

struct Layout {
  std::vector<std::string_view> lines;
  std::vector<double> widths;
  double text_width = 0.0;
  double text_height = 0.0;
  /// Room added around the text for the stroke and the shadow.
  double margin = 0.0;
  double line_step = 0.0;
  double ascent = 0.0;
};

[[nodiscard]] Layout lay_out(const core::TextSpec& spec, const SkFont& font) {
  Layout out;
  out.lines = lines_of(spec.content);

  SkFontMetrics metrics;
  font.getMetrics(&metrics);
  out.ascent = -static_cast<double>(metrics.fAscent);
  out.line_step = spec.font_size * kLineHeight;

  for (const std::string_view line : out.lines) {
    const auto width = static_cast<double>(
        font.measureText(line.data(), line.size(), SkTextEncoding::kUTF8));
    out.widths.push_back(width);
    out.text_width = std::max(out.text_width, width);
  }

  const auto count = static_cast<double>(out.lines.size());
  // The first line's own height plus a step for each after it, so a single line
  // is as tall as a line rather than as tall as a line and a gap.
  out.text_height = out.ascent + static_cast<double>(metrics.fDescent) +
                    (count - 1.0) * out.line_step;

  const double stroke = spec.stroke_color.has_value() ? std::max(0.0, spec.stroke_width) : 0.0;
  const double shadow =
      spec.shadow ? spec.font_size * (kShadowOffset + kShadowBlur * 2.0) : 0.0;
  out.margin = std::ceil(stroke + shadow + 2.0);
  return out;
}

}  // namespace

Size measure(const core::TextSpec& spec) {
  if (spec.content.empty() || spec.font_size <= 0.0) return {};

  const sk_sp<SkTypeface> face = typeface_for(spec);
  if (face == nullptr) return {};

  const SkFont font(face, static_cast<SkScalar>(spec.font_size));
  const Layout layout = lay_out(spec, font);
  return Size{layout.text_width + layout.margin * 2.0,
              layout.text_height + layout.margin * 2.0};
}

std::expected<Raster, std::string> rasterise(const core::TextSpec& spec) {
  if (spec.content.empty()) return std::unexpected("a title needs some text");
  if (spec.font_size <= 0.0) return std::unexpected("a title needs a positive font size");

  const sk_sp<SkTypeface> face = typeface_for(spec);
  if (face == nullptr) return std::unexpected("no fonts are available");

  SkFont font(face, static_cast<SkScalar>(spec.font_size));
  font.setEdging(SkFont::Edging::kAntiAlias);
  font.setSubpixel(true);

  const Layout layout = lay_out(spec, font);
  const int width = static_cast<int>(std::ceil(layout.text_width + layout.margin * 2.0));
  const int height = static_cast<int>(std::ceil(layout.text_height + layout.margin * 2.0));
  if (width <= 0 || height <= 0) return std::unexpected("the title measured to nothing");

  // Premultiplied, which is what the compositor samples and what survives
  // bilinear filtering without haloing the glyphs.
  const sk_sp<SkSurface> surface =
      SkSurfaces::Raster(SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
                                           kPremul_SkAlphaType));
  if (surface == nullptr) return std::unexpected("cannot make a surface to draw the title on");

  SkCanvas* canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);

  if (spec.background.has_value()) {
    SkPaint paint;
    paint.setColor4f(parse(*spec.background, SkColor4f{0.0f, 0.0f, 0.0f, 1.0f}));
    canvas->drawPaint(paint);
  }

  const SkColor4f fill = parse(spec.color, SkColor4f{1.0f, 1.0f, 1.0f, 1.0f});

  for (std::size_t i = 0; i < layout.lines.size(); ++i) {
    const std::string_view line = layout.lines[i];
    if (line.empty()) continue;

    double x = layout.margin;
    if (spec.align == core::TextAlign::Center) {
      x += (layout.text_width - layout.widths[i]) / 2.0;
    } else if (spec.align == core::TextAlign::Right) {
      x += layout.text_width - layout.widths[i];
    }
    const double baseline =
        layout.margin + layout.ascent + static_cast<double>(i) * layout.line_step;

    const auto draw = [&](const SkPaint& paint, double dx = 0.0, double dy = 0.0) {
      canvas->drawSimpleText(line.data(), line.size(), SkTextEncoding::kUTF8,
                             static_cast<SkScalar>(x + dx), static_cast<SkScalar>(baseline + dy),
                             font, paint);
    };

    // Shadow first, then the outline, then the fill: the order a title is built
    // up in every editor, and the only one where a stroke sits under its own
    // letterform rather than over it.
    if (spec.shadow) {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setColor4f(SkColor4f{0.0f, 0.0f, 0.0f, 0.6f});
      paint.setMaskFilter(SkMaskFilter::MakeBlur(
          kNormal_SkBlurStyle, static_cast<SkScalar>(spec.font_size * kShadowBlur)));
      draw(paint, spec.font_size * kShadowOffset, spec.font_size * kShadowOffset);
    }

    if (spec.stroke_color.has_value() && spec.stroke_width > 0.0) {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      // Doubled, because a stroke straddles the path: half of it falls inside
      // the glyph, where the fill then covers it. This is what makes a
      // 4-pixel outline look four pixels thick, as it does on a canvas.
      paint.setStrokeWidth(static_cast<SkScalar>(spec.stroke_width * 2.0));
      paint.setColor4f(parse(*spec.stroke_color, SkColor4f{0.0f, 0.0f, 0.0f, 1.0f}));
      draw(paint);
    }

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor4f(fill);
    draw(paint);
  }

  Raster raster;
  raster.width = width;
  raster.height = height;
  raster.pixels.resize(static_cast<std::size_t>(width) * height * 4);

  const SkImageInfo info =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  if (!surface->readPixels(SkPixmap(info, raster.pixels.data(), static_cast<std::size_t>(width) * 4),
                           0, 0)) {
    return std::unexpected("cannot read the drawn title back");
  }
  return raster;
}

}  // namespace cutline::text
