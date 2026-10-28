#include "cutline/text/raster.hpp"

#include "include/codec/SkCodec.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkTypeface_win.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>

namespace cutline::text {
namespace {

using core::GraphicKind;
using core::GraphicLayer;
using core::GraphicStyle;

/// How far apart the baselines of a multi-line title sit, as a multiple of the
/// font size, before any leading. The reference's canvas used 1.2, which is
/// the CSS default.
constexpr double kLineHeight = 1.2;

/// The font manager, made once. DirectWrite enumeration is not cheap and a
/// title is rasterised whenever its text changes.
[[nodiscard]] const sk_sp<SkFontMgr>& font_manager() {
  static const sk_sp<SkFontMgr> manager = SkFontMgr_New_DirectWrite();
  return manager;
}

/// The first family in a CSS-style list that this machine actually has.
///
/// A project written elsewhere can name anything, and `system-ui, sans-serif`
/// was the old titles' own default. Falling through the list and then to the
/// system face is what every browser does with the same string.
[[nodiscard]] sk_sp<SkTypeface> typeface_for(const GraphicStyle& spec) {
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

[[nodiscard]] SkColor4f faded(SkColor4f colour, double opacity) {
  colour.fA *= static_cast<float>(std::clamp(opacity, 0.0, 1.0));
  return colour;
}

/// UTF-8 to code points. A broken byte becomes the replacement character
/// rather than ending the title there.
[[nodiscard]] std::vector<SkUnichar> code_points(std::string_view text) {
  std::vector<SkUnichar> out;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    int length = 1;
    SkUnichar value = 0xFFFD;
    if (lead < 0x80) {
      value = lead;
    } else if ((lead & 0xE0) == 0xC0) {
      length = 2;
      value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
      length = 3;
      value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
      length = 4;
      value = lead & 0x07;
    }
    if (length > 1) {
      if (i + static_cast<std::size_t>(length) > text.size()) {
        value = 0xFFFD;
        length = 1;
      } else {
        for (int k = 1; k < length; ++k) {
          const auto next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)]);
          if ((next & 0xC0) != 0x80) {
            value = 0xFFFD;
            length = 1;
            break;
          }
          value = (value << 6) | (next & 0x3F);
        }
      }
    }
    out.push_back(value);
    i += static_cast<std::size_t>(length);
  }
  return out;
}

/// One line of laid-out text: its glyphs, where each one starts, and which
/// character of the whole text each glyph is -- what a typewriter counts by.
struct Line {
  std::vector<SkGlyphID> glyphs;
  std::vector<SkScalar> x;
  std::vector<std::size_t> character;
  double width = 0.0;
};

struct TextLayout {
  std::vector<Line> lines;
  double width = 0.0;
  double height = 0.0;
  double ascent = 0.0;
  double step = 0.0;
  double underline_at = 0.0;
  double underline_thickness = 0.0;
};

[[nodiscard]] SkFont font_for(const GraphicStyle& style, sk_sp<SkTypeface> face) {
  SkFont font(std::move(face), static_cast<SkScalar>(std::max(1.0, style.font_size)));
  font.setEdging(SkFont::Edging::kAntiAlias);
  font.setSubpixel(true);
  return font;
}

/// Lays the text out: paragraphs where it breaks, and within a paragraph text
/// box, lines wrapped at spaces to fit it -- Premiere's point and paragraph
/// text.
[[nodiscard]] TextLayout lay_out(const GraphicLayer& layer, const SkFont& font) {
  const GraphicStyle& style = layer.style;
  TextLayout out;

  SkFontMetrics metrics;
  font.getMetrics(&metrics);
  out.ascent = -static_cast<double>(metrics.fAscent);
  const double descent = static_cast<double>(metrics.fDescent);
  out.step = style.font_size * kLineHeight + style.leading;
  SkScalar underline_position = 0.0f;
  SkScalar underline_thickness = 0.0f;
  out.underline_at = metrics.hasUnderlinePosition(&underline_position)
                         ? static_cast<double>(underline_position)
                         : style.font_size * 0.1;
  out.underline_thickness = metrics.hasUnderlineThickness(&underline_thickness)
                                ? std::max(1.0, static_cast<double>(underline_thickness))
                                : std::max(1.0, style.font_size / 18.0);

  const std::vector<SkUnichar> text = code_points(core::displayed_text(layer));
  const double tracking = style.tracking / 1000.0 * style.font_size;
  const std::optional<double> box =
      layer.box_width.has_value() && *layer.box_width > 0.0 ? layer.box_width : std::nullopt;

  // Per character: its glyph and how far it advances.
  std::vector<SkGlyphID> glyphs(text.size());
  font.unicharsToGlyphs(text, glyphs);
  std::vector<SkScalar> widths(text.size());
  font.getWidths(glyphs, widths);

  const auto finish = [&](Line& line) {
    if (!line.x.empty()) {
      line.width = static_cast<double>(line.x.back()) + static_cast<double>(widths[line.character.back()]);
    }
    out.width = std::max(out.width, line.width);
    out.lines.push_back(std::move(line));
    line = Line{};
  };

  Line line;
  double pen = 0.0;
  // Where the line could last be broken: after this many of its glyphs, the
  // last of them a space.
  std::optional<std::size_t> break_glyphs;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const SkUnichar c = text[i];
    if (c == '\r') continue;
    if (c == '\n') {
      finish(line);
      pen = 0.0;
      break_glyphs.reset();
      continue;
    }
    const double advance = static_cast<double>(widths[i]);
    if (box.has_value() && pen + advance > *box && !line.glyphs.empty() && c != ' ' &&
        break_glyphs.has_value()) {
      // Wrapped at the last space: what follows it starts the next line.
      Line next;
      for (std::size_t g = *break_glyphs; g < line.glyphs.size(); ++g) {
        next.glyphs.push_back(line.glyphs[g]);
        next.character.push_back(line.character[g]);
      }
      line.glyphs.resize(*break_glyphs);
      line.x.resize(*break_glyphs);
      line.character.resize(*break_glyphs);
      // The space a line breaks at belongs to neither line.
      while (!line.character.empty() && text[line.character.back()] == ' ') {
        line.glyphs.pop_back();
        line.x.pop_back();
        line.character.pop_back();
      }
      finish(line);
      pen = 0.0;
      for (std::size_t g = 0; g < next.glyphs.size(); ++g) {
        next.x.push_back(static_cast<SkScalar>(pen));
        pen += static_cast<double>(widths[next.character[g]]) + tracking;
      }
      line = std::move(next);
      break_glyphs.reset();
    }
    line.glyphs.push_back(glyphs[i]);
    line.x.push_back(static_cast<SkScalar>(pen));
    line.character.push_back(i);
    pen += advance + tracking;
    if (c == ' ') break_glyphs = line.glyphs.size();
  }
  finish(line);

  if (box.has_value()) out.width = *box;
  const auto count = static_cast<double>(out.lines.size());
  // The first line's own height plus a step for each after it, so a single line
  // is as tall as a line rather than as tall as a line and a gap.
  out.height = out.ascent + descent + (count - 1.0) * out.step;
  return out;
}

[[nodiscard]] double total_stroke(const GraphicStyle& style) {
  double total = 0.0;
  for (const core::GraphicStroke& stroke : style.strokes) total += std::max(0.0, stroke.width);
  return total;
}

/// An image layer's picture, decoded.
[[nodiscard]] sk_sp<SkImage> picture_of(const GraphicLayer& layer) {
  if (layer.path.empty()) return nullptr;
  sk_sp<SkData> data = SkData::MakeFromFileName(layer.path.c_str());
  if (data == nullptr) return nullptr;
  const std::array decoders{SkPngDecoder::Decoder(), SkJpegDecoder::Decoder()};
  std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(data), decoders);
  if (codec == nullptr) return nullptr;
  auto [image, result] = codec->getImage();
  return result == SkCodec::kSuccess ? image : nullptr;
}

[[nodiscard]] core::Size picture_size(const GraphicLayer& layer) {
  if (layer.path.empty()) return {};
  sk_sp<SkData> data = SkData::MakeFromFileName(layer.path.c_str());
  if (data == nullptr) return {};
  const std::array decoders{SkPngDecoder::Decoder(), SkJpegDecoder::Decoder()};
  std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(data), decoders);
  if (codec == nullptr) return {};
  return {static_cast<double>(codec->getInfo().width()),
          static_cast<double>(codec->getInfo().height())};
}

}  // namespace

core::LayerExtent measure(const GraphicLayer& layer) {
  core::LayerExtent extent = core::measure_without_fonts(layer);
  if (layer.kind == GraphicKind::Image) {
    extent.content = picture_size(layer);
  } else if (layer.kind == GraphicKind::Text) {
    if (layer.text.empty() || layer.style.font_size <= 0.0) return extent;
    const sk_sp<SkTypeface> face = typeface_for(layer.style);
    if (face == nullptr) return extent;
    const TextLayout layout = lay_out(layer, font_for(layer.style, face));
    extent.content = {layout.width, layout.height};
  }
  return extent;
}

std::expected<Raster, std::string> rasterise(const GraphicLayer& layer, std::size_t characters) {
  const GraphicStyle& style = layer.style;
  const core::LayerExtent extent = measure(layer);
  const core::Size drawn = extent.drawn();
  if (extent.content.width <= 0.0 || extent.content.height <= 0.0) {
    return std::unexpected(layer.kind == GraphicKind::Image ? "the picture could not be read"
                                                            : "the layer has nothing to draw");
  }
  const int width = static_cast<int>(std::ceil(drawn.width));
  const int height = static_cast<int>(std::ceil(drawn.height));

  // Premultiplied, which is what the compositor samples and what survives
  // bilinear filtering without haloing the edges.
  const SkImageInfo info =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  const sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
  if (surface == nullptr) return std::unexpected("cannot make a surface to draw the layer on");
  SkCanvas* canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);

  const double left = extent.left;
  const double top = extent.top;
  const double content_w = extent.content.width;
  const double content_h = extent.content.height;
  const double strokes = total_stroke(style);

  // The shadow falls away from the light: Premiere's angle is where the light
  // is, measured anticlockwise from the right, and screen y runs downwards.
  double shadow_dx = 0.0;
  double shadow_dy = 0.0;
  if (style.shadow) {
    const double radians = style.shadow->angle * std::numbers::pi / 180.0;
    shadow_dx = -std::cos(radians) * style.shadow->distance;
    shadow_dy = std::sin(radians) * style.shadow->distance;
  }

  // Everything is built up the same way whatever the layer is: shadow, then
  // the strokes from the outermost in, then the fill. The order every editor
  // uses, and the only one where a stroke sits under its own letterform.
  const auto build = [&](const auto& draw) {
    if (style.shadow) {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setColor4f(faded(parse(style.shadow->color, SkColor4f{0, 0, 0, 1}),
                             style.shadow->opacity));
      const double spread = strokes + std::max(0.0, style.shadow->size);
      if (spread > 0.0) {
        paint.setStyle(SkPaint::kStrokeAndFill_Style);
        paint.setStrokeWidth(static_cast<SkScalar>(spread * 2.0));
      }
      if (style.shadow->blur > 0.0) {
        paint.setMaskFilter(SkMaskFilter::MakeBlur(
            kNormal_SkBlurStyle, static_cast<SkScalar>(style.shadow->blur / 2.0)));
      }
      draw(paint, shadow_dx, shadow_dy);
    }
    // Each stroke reaches to the sum of its own width and those inside it,
    // doubled because a stroke straddles the edge and the fill covers the
    // inner half.
    for (std::size_t k = style.strokes.size(); k-- > 0;) {
      double reach = 0.0;
      for (std::size_t inner = 0; inner <= k; ++inner) {
        reach += std::max(0.0, style.strokes[inner].width);
      }
      if (reach <= 0.0) continue;
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeJoin(SkPaint::kRound_Join);
      paint.setStrokeWidth(static_cast<SkScalar>(reach * 2.0));
      paint.setColor4f(parse(style.strokes[k].color, SkColor4f{0, 0, 0, 1}));
      draw(paint, 0.0, 0.0);
    }
    if (style.fill_enabled) {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setColor4f(parse(style.fill, SkColor4f{1, 1, 1, 1}));
      draw(paint, 0.0, 0.0);
    }
  };

  switch (layer.kind) {
    case GraphicKind::Text: {
      const sk_sp<SkTypeface> face = typeface_for(style);
      if (face == nullptr) return std::unexpected("no fonts are available");
      const SkFont font = font_for(style, face);
      const TextLayout layout = lay_out(layer, font);

      if (style.background) {
        const double pad = std::max(0.0, style.background->padding);
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor4f(faded(parse(style.background->color, SkColor4f{0, 0, 0, 1}),
                               style.background->opacity));
        // Round the words rather than the whole paragraph box, as Premiere's
        // background sits: a caption wrapped at four-fifths of the frame and
        // saying three words has a box the size of the three words.
        double widest = 0.0;
        for (const Line& line : layout.lines) widest = std::max(widest, line.width);
        const double fitted = std::min(content_w, widest);
        double from = left;
        if (style.align == core::TextAlign::Center) from += (content_w - fitted) / 2.0;
        else if (style.align == core::TextAlign::Right) from += content_w - fitted;
        const SkRect box = SkRect::MakeXYWH(
            static_cast<SkScalar>(from - pad), static_cast<SkScalar>(top - pad),
            static_cast<SkScalar>(fitted + pad * 2.0), static_cast<SkScalar>(content_h + pad * 2.0));
        const auto radius = static_cast<SkScalar>(std::max(0.0, style.background->corner_radius));
        canvas->drawRRect(SkRRect::MakeRectXY(box, radius, radius), paint);
      }

      // The lines as blobs, each placed for the alignment and holding only
      // the characters a typewriter has reached.
      struct Placed {
        sk_sp<SkTextBlob> blob;
        double x = 0.0;
        double baseline = 0.0;
        double width = 0.0;
      };
      std::vector<Placed> placed;
      for (std::size_t i = 0; i < layout.lines.size(); ++i) {
        const Line& line = layout.lines[i];
        double x = left;
        if (style.align == core::TextAlign::Center) {
          x += (content_w - line.width) / 2.0;
        } else if (style.align == core::TextAlign::Right) {
          x += content_w - line.width;
        }
        const double baseline = top + layout.ascent + static_cast<double>(i) * layout.step;
        std::size_t shown = 0;
        while (shown < line.glyphs.size() && line.character[shown] < characters) ++shown;
        if (shown == 0) continue;
        sk_sp<SkTextBlob> blob = SkTextBlob::MakeFromPosHGlyphs(
            SkSpan<const SkGlyphID>(line.glyphs.data(), shown),
            SkSpan<const SkScalar>(line.x.data(), shown), 0.0f, font);
        const double shown_width =
            shown == line.glyphs.size()
                ? line.width
                : static_cast<double>(line.x[shown]);
        placed.push_back({std::move(blob), x, baseline, shown_width});
      }

      build([&](const SkPaint& paint, double dx, double dy) {
        for (const Placed& at : placed) {
          if (at.blob == nullptr) continue;
          canvas->drawTextBlob(at.blob, static_cast<SkScalar>(at.x + dx),
                               static_cast<SkScalar>(at.baseline + dy), paint);
          if (style.underline) {
            SkPaint bar = paint;
            bar.setStyle(paint.getStyle() == SkPaint::kStroke_Style ? SkPaint::kStroke_Style
                                                                     : SkPaint::kFill_Style);
            canvas->drawRect(
                SkRect::MakeXYWH(static_cast<SkScalar>(at.x + dx),
                                 static_cast<SkScalar>(at.baseline + layout.underline_at + dy),
                                 static_cast<SkScalar>(at.width),
                                 static_cast<SkScalar>(layout.underline_thickness)),
                bar);
          }
        }
      });
      break;
    }

    case GraphicKind::Rectangle:
    case GraphicKind::Ellipse: {
      const SkRect rect =
          SkRect::MakeXYWH(static_cast<SkScalar>(left), static_cast<SkScalar>(top),
                           static_cast<SkScalar>(content_w), static_cast<SkScalar>(content_h));
      const auto radius = static_cast<SkScalar>(
          std::clamp(layer.corner_radius, 0.0, std::min(content_w, content_h) / 2.0));
      build([&](const SkPaint& paint, double dx, double dy) {
        const SkRect at = rect.makeOffset(static_cast<SkScalar>(dx), static_cast<SkScalar>(dy));
        if (layer.kind == GraphicKind::Ellipse) {
          canvas->drawOval(at, paint);
        } else {
          canvas->drawRRect(SkRRect::MakeRectXY(at, radius, radius), paint);
        }
      });
      break;
    }

    case GraphicKind::Image: {
      const sk_sp<SkImage> picture = picture_of(layer);
      if (picture == nullptr) return std::unexpected("the picture could not be read");
      const SkRect rect =
          SkRect::MakeXYWH(static_cast<SkScalar>(left), static_cast<SkScalar>(top),
                           static_cast<SkScalar>(content_w), static_cast<SkScalar>(content_h));
      if (style.shadow) {
        // The picture's own outline, which for a PNG with transparency is its
        // shape and for a JPEG its rectangle.
        SkPaint paint;
        paint.setColor4f(faded(parse(style.shadow->color, SkColor4f{0, 0, 0, 1}),
                               style.shadow->opacity));
        if (style.shadow->blur > 0.0) {
          paint.setMaskFilter(SkMaskFilter::MakeBlur(
              kNormal_SkBlurStyle, static_cast<SkScalar>(style.shadow->blur / 2.0)));
        }
        canvas->drawRect(rect.makeOffset(static_cast<SkScalar>(shadow_dx),
                                         static_cast<SkScalar>(shadow_dy)),
                         paint);
      }
      canvas->drawImageRect(picture, rect,
                            SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear));
      break;
    }
  }

  Raster raster;
  raster.width = width;
  raster.height = height;
  raster.pixels.resize(static_cast<std::size_t>(width) * height * 4);
  if (!surface->readPixels(
          SkPixmap(info, raster.pixels.data(), static_cast<std::size_t>(width) * 4), 0, 0)) {
    return std::unexpected("cannot read the drawn layer back");
  }
  return raster;
}

std::vector<std::string> font_families() {
  std::vector<std::string> out;
  const sk_sp<SkFontMgr>& manager = font_manager();
  if (manager == nullptr) return out;
  const int count = manager->countFamilies();
  for (int i = 0; i < count; ++i) {
    SkString name;
    manager->getFamilyName(i, &name);
    if (name.size() > 0 && name.c_str()[0] != '@') out.emplace_back(name.c_str());
  }
  std::ranges::sort(out);
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace cutline::text
