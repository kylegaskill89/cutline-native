#include "cutline/ui/skia_painter.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

// Skia's headers are noisy at /W4 and are not ours to keep warning-free.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageFilter.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"
#include "include/ports/SkTypeface_win.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace cutline::ui {
namespace {

[[nodiscard]] SkRect to_sk(const Rect& r) {
  return SkRect::MakeXYWH(static_cast<SkScalar>(r.x), static_cast<SkScalar>(r.y),
                          static_cast<SkScalar>(r.width), static_cast<SkScalar>(r.height));
}

[[nodiscard]] SkRRect rounded(const Rect& r, double radius) {
  // Clamped, because a radius larger than half the shorter side produces a
  // shape Skia has to reinterpret and the result stops matching the theme.
  const auto limit = static_cast<SkScalar>(std::min(r.width, r.height) / 2.0);
  const auto value = std::min(static_cast<SkScalar>(std::max(radius, 0.0)), limit);
  return SkRRect::MakeRectXY(to_sk(r), value, value);
}

[[nodiscard]] SkColor4f to_sk(const Color& c) {
  return SkColor4f{c.r, c.g, c.b, c.a};
}

/// Skia's blur takes a standard deviation; themes are written in the radius a
/// designer thinks in. This is the usual conversion between them.
[[nodiscard]] SkScalar sigma_for(double radius) {
  return static_cast<SkScalar>(std::max(radius, 0.0) * 0.5);
}

/// The two endpoints of a gradient across `bounds` at `angle_deg`, measured
/// clockwise from a top-to-bottom ramp — which is what nearly all chrome uses,
/// so zero has to mean vertical.
void gradient_points(const Rect& bounds, double angle_deg, SkPoint out[2]) {
  const double radians = angle_deg * std::numbers::pi / 180.0;
  const double dx = std::sin(radians);
  const double dy = std::cos(radians);

  const double cx = bounds.x + bounds.width / 2.0;
  const double cy = bounds.y + bounds.height / 2.0;
  // Half the projection of the box onto the gradient direction, so the ramp
  // spans the shape exactly at any angle.
  const double half = (std::abs(dx) * bounds.width + std::abs(dy) * bounds.height) / 2.0;

  out[0] = SkPoint::Make(static_cast<SkScalar>(cx - dx * half),
                         static_cast<SkScalar>(cy - dy * half));
  out[1] = SkPoint::Make(static_cast<SkScalar>(cx + dx * half),
                         static_cast<SkScalar>(cy + dy * half));
}

}  // namespace

struct SkiaPainter::Impl {
  SkCanvas* canvas = nullptr;
  sk_sp<SkFontMgr> fonts;
  sk_sp<SkTypeface> regular;
  sk_sp<SkTypeface> bold;

  [[nodiscard]] SkFont font_for(double size, bool want_bold) const {
    SkFont font(want_bold ? bold : regular, static_cast<SkScalar>(size));
    font.setSubpixel(true);
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    return font;
  }
};

SkiaPainter::SkiaPainter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SkiaPainter::~SkiaPainter() = default;

std::unique_ptr<SkiaPainter> SkiaPainter::create(void* canvas) {
  if (canvas == nullptr) return nullptr;

  auto impl = std::make_unique<Impl>();
  impl->canvas = static_cast<SkCanvas*>(canvas);
  impl->fonts = SkFontMgr_New_DirectWrite();
  if (impl->fonts != nullptr) {
    // Null family means the system default, which is what an interface should
    // follow rather than imposing a font of its own.
    impl->regular = impl->fonts->legacyMakeTypeface(nullptr, SkFontStyle::Normal());
    impl->bold = impl->fonts->legacyMakeTypeface(nullptr, SkFontStyle::Bold());
  }

  return std::unique_ptr<SkiaPainter>(new SkiaPainter(std::move(impl)));
}

void SkiaPainter::push_clip(const Rect& bounds, double corner_radius) {
  impl_->canvas->save();
  impl_->canvas->clipRRect(rounded(bounds, corner_radius), true);
}

void SkiaPainter::pop_clip() { impl_->canvas->restore(); }

void SkiaPainter::fill(const Rect& bounds, double corner_radius, const Fill& value) {
  if (bounds.empty()) return;

  SkPaint paint;
  paint.setAntiAlias(true);

  switch (value.kind) {
    case FillKind::Gradient: {
      if (value.stops.size() < 2) {
        paint.setColor4f(to_sk(value.color));
        break;
      }
      std::vector<SkColor4f> colors;
      std::vector<float> positions;
      colors.reserve(value.stops.size());
      positions.reserve(value.stops.size());
      for (const GradientStop& stop : value.stops) {
        colors.push_back(to_sk(stop.color));
        positions.push_back(stop.at);
      }

      SkPoint points[2];
      gradient_points(bounds, value.angle_deg, points);

      const SkGradient::Colors ramp(SkSpan<const SkColor4f>(colors),
                                    SkSpan<const float>(positions), SkTileMode::kClamp);
      const SkGradient gradient(ramp, SkGradient::Interpolation{});
      paint.setShader(SkShaders::LinearGradient(points, gradient));
      break;
    }
    case FillKind::Glass:
      // The blur already happened; what is left is the tint over it.
      paint.setColor4f(to_sk(value.color));
      break;
    case FillKind::Solid:
      paint.setColor4f(to_sk(value.color));
      break;
  }

  impl_->canvas->drawRRect(rounded(bounds, corner_radius), paint);
}

void SkiaPainter::stroke(const Rect& bounds, double corner_radius, const Color& color,
                         double width) {
  if (bounds.empty() || width <= 0.0) return;

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(static_cast<SkScalar>(width));
  paint.setColor4f(to_sk(color));

  // Skia centres a stroke on the path, so the outer half would fall outside the
  // control and make it a pixel bigger than it was laid out to be. Insetting by
  // half the width keeps the border inside its own bounds.
  const Rect inner = bounds.inset(width / 2.0);
  impl_->canvas->drawRRect(rounded(inner, std::max(0.0, corner_radius - width / 2.0)), paint);
}

void SkiaPainter::image(const Rect& bounds, const ImageView& view) {
  if (view.empty() || bounds.empty()) return;

  const SkImageInfo info = SkImageInfo::Make(view.width, view.height, kRGBA_8888_SkColorType,
                                             kUnpremul_SkAlphaType);
  const SkPixmap pixmap(info, view.pixels, static_cast<std::size_t>(view.row_bytes()));

  // Wraps the caller's pixels rather than copying them. A decoded frame is
  // megabytes and the draw happens before this returns, so there is nothing to
  // gain by taking a copy of it.
  const sk_sp<SkImage> image = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
  if (image == nullptr) return;

  SkPaint paint;
  paint.setAntiAlias(true);
  // Linear rather than nearest: a preview is nearly always being scaled down,
  // and point sampling makes a moving picture crawl.
  impl_->canvas->drawImageRect(image, to_sk(bounds),
                               SkSamplingOptions(SkFilterMode::kLinear), &paint);
}

void SkiaPainter::line(double x1, double y1, double x2, double y2, const Color& color,
                       double width) {
  if (width <= 0.0 || color.a <= 0.0f) return;

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(static_cast<SkScalar>(width));
  // Rounded ends, so the two strokes of a close button's cross meet cleanly
  // instead of leaving a notch where they cross.
  paint.setStrokeCap(SkPaint::kRound_Cap);
  paint.setColor4f(to_sk(color));

  impl_->canvas->drawLine(static_cast<SkScalar>(x1), static_cast<SkScalar>(y1),
                          static_cast<SkScalar>(x2), static_cast<SkScalar>(y2), paint);
}

void SkiaPainter::bevel(const Rect& bounds, const Bevel& value) {
  if (bounds.empty() || value.width <= 0.0) return;

  // Which edges are light is the whole point: swapping them is what turns a
  // raised control into a pressed one.
  const Color top_left = value.inset ? value.dark : value.light;
  const Color bottom_right = value.inset ? value.light : value.dark;

  SkPaint paint;
  paint.setAntiAlias(false);  // bevels are crisp single-pixel edges
  paint.setStyle(SkPaint::kFill_Style);

  const auto w = static_cast<SkScalar>(value.width);
  const SkRect r = to_sk(bounds);

  paint.setColor4f(to_sk(top_left));
  impl_->canvas->drawRect(SkRect::MakeLTRB(r.fLeft, r.fTop, r.fRight, r.fTop + w), paint);
  impl_->canvas->drawRect(SkRect::MakeLTRB(r.fLeft, r.fTop, r.fLeft + w, r.fBottom), paint);

  paint.setColor4f(to_sk(bottom_right));
  impl_->canvas->drawRect(SkRect::MakeLTRB(r.fLeft, r.fBottom - w, r.fRight, r.fBottom), paint);
  impl_->canvas->drawRect(SkRect::MakeLTRB(r.fRight - w, r.fTop, r.fRight, r.fBottom), paint);
}

void SkiaPainter::shadow(const Rect& bounds, double corner_radius, const Shadow& value) {
  if (bounds.empty()) return;

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setColor4f(to_sk(value.color));
  if (value.blur > 0.0) {
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma_for(value.blur)));
  }

  if (!value.inner) {
    Rect offset = bounds;
    offset.x += value.offset_x;
    offset.y += value.offset_y;
    impl_->canvas->drawRRect(rounded(offset, corner_radius), paint);
    return;
  }

  // An inner shadow is the shape's own edge glowing inwards. Stroking the
  // outline with a blur, clipped to the shape by the caller, gives that without
  // needing a second layer.
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(static_cast<SkScalar>(std::max(value.blur, 1.0)));

  Rect offset = bounds;
  offset.x += value.offset_x;
  offset.y += value.offset_y;
  impl_->canvas->drawRRect(rounded(offset, corner_radius), paint);
}

void SkiaPainter::backdrop_blur(const Rect& bounds, double corner_radius, double radius) {
  if (bounds.empty() || radius <= 0.0) return;

  // `saveLayer` with a backdrop filter is the only way to read what has already
  // been drawn. It is expensive, which is why `paint_surface` skips it whenever
  // the radius is zero.
  //
  // Blurred at a quarter scale and stretched back, rather than at full size. A
  // blur is a low-pass filter, so the detail a downsample throws away is very
  // nearly the detail the blur was about to destroy anyway — the result is hard
  // to tell apart at a sixteenth of the pixels.
  //
  // Worth about 45% of an Aero frame on a CPU raster surface, and no more than
  // that: the measurement says most of the cost is the layer each glass surface
  // needs rather than the blur kernel inside it, so this helps and does not
  // rescue. What would is fewer layers, or a GPU.
  constexpr float kScale = 0.25f;
  constexpr SkSamplingOptions kSmooth{SkFilterMode::kLinear};
  const SkScalar sigma = sigma_for(radius) * kScale;

  const sk_sp<SkImageFilter> blur = SkImageFilters::MatrixTransform(
      SkMatrix::Scale(1.0f / kScale, 1.0f / kScale), kSmooth,
      SkImageFilters::Blur(sigma, sigma,
                           SkImageFilters::MatrixTransform(SkMatrix::Scale(kScale, kScale),
                                                           kSmooth, nullptr)));

  impl_->canvas->save();
  impl_->canvas->clipRRect(rounded(bounds, corner_radius), true);

  SkCanvas::SaveLayerRec rec;
  const SkRect area = to_sk(bounds);
  rec.fBounds = &area;
  rec.fBackdrop = blur.get();
  impl_->canvas->saveLayer(rec);
  impl_->canvas->restore();

  impl_->canvas->restore();
}

void SkiaPainter::text(const TextRun& run) {
  if (run.text.empty()) return;

  const SkFont font = impl_->font_for(run.size, run.bold);
  const auto width = static_cast<double>(
      font.measureText(run.text.data(), run.text.size(), SkTextEncoding::kUTF8));

  double x = run.bounds.x;
  if (run.align == TextAlign::Center) {
    x = run.bounds.x + (run.bounds.width - width) / 2.0;
  } else if (run.align == TextAlign::Right) {
    x = run.bounds.right() - width;
  }

  // Centred on the cap height rather than the full line box, which is what
  // looks vertically centred to a reader.
  SkFontMetrics metrics;
  font.getMetrics(&metrics);
  const double baseline =
      run.bounds.y + (run.bounds.height - (metrics.fDescent - metrics.fAscent)) / 2.0 -
      metrics.fAscent;

  SkPaint paint;
  paint.setAntiAlias(true);

  // The halo goes down first, in the text's own colour, so a label stays
  // readable over glass whatever the backdrop happens to be.
  if (run.glow > 0.0) {
    SkPaint halo;
    halo.setAntiAlias(true);
    halo.setColor4f(to_sk(run.color));
    halo.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma_for(run.glow)));
    impl_->canvas->drawSimpleText(run.text.data(), run.text.size(), SkTextEncoding::kUTF8,
                                  static_cast<SkScalar>(x), static_cast<SkScalar>(baseline),
                                  font, halo);
  }

  paint.setColor4f(to_sk(run.color));
  impl_->canvas->drawSimpleText(run.text.data(), run.text.size(), SkTextEncoding::kUTF8,
                                static_cast<SkScalar>(x), static_cast<SkScalar>(baseline), font,
                                paint);
}

double SkiaPainter::measure(std::string_view text, double size, bool bold) const {
  if (text.empty()) return 0.0;
  const SkFont font = impl_->font_for(size, bold);
  return static_cast<double>(
      font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8));
}

}  // namespace cutline::ui
