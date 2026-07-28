#pragma once

/// The Skia implementation of `Painter`.
///
/// Everything interesting about *what* to draw already happened in
/// `paint_surface`; this only knows how to put each primitive on a canvas. That
/// split is why the ordering rules are tested against `RecordingPainter` and
/// never need a GPU to check.
///
/// The canvas is passed untyped so Skia's headers stay inside this library.
/// They are large, they are slow to compile, and nothing above the UI layer has
/// any business seeing them.

#include "cutline/ui/painter.hpp"

#include <memory>
#include <string>

namespace cutline::ui {

class SkiaPainter final : public Painter {
 public:
  /// `canvas` is an `SkCanvas*`, owned by the caller and outliving this.
  ///
  /// Font lookup goes through DirectWrite, so the interface uses the same
  /// fonts as the rest of the system rather than shipping its own.
  [[nodiscard]] static std::unique_ptr<SkiaPainter> create(void* canvas);

  SkiaPainter(const SkiaPainter&) = delete;
  SkiaPainter& operator=(const SkiaPainter&) = delete;
  ~SkiaPainter() override;

  void push_clip(const Rect& bounds, double corner_radius) override;
  void pop_clip() override;
  void fill(const Rect& bounds, double corner_radius, const Fill& fill) override;
  void stroke(const Rect& bounds, double corner_radius, const Color& color,
              double width) override;
  void bevel(const Rect& bounds, const Bevel& bevel) override;
  void shadow(const Rect& bounds, double corner_radius, const Shadow& shadow) override;
  void backdrop_blur(const Rect& bounds, double corner_radius, double radius) override;
  void text(const TextRun& run) override;

  [[nodiscard]] double measure(std::string_view text, double size, bool bold) const override;

 private:
  struct Impl;
  explicit SkiaPainter(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace cutline::ui
