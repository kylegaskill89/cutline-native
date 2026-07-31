#pragma once

/// The scopes, drawn.
///
/// Handed measurements rather than pixels, and it never measures anything
/// itself: `cutline::render` counts, this draws. That keeps the arithmetic
/// testable without a widget and the drawing testable without a frame — and it
/// is why a scope can never affect what is rendered, since the only thing
/// flowing between them is a histogram.

#include "cutline/render/scopes.hpp"
#include "cutline/ui/widget.hpp"

#include <memory>

namespace cutline::ui {

/// Which scope is on show. Tabs, in the order the spec names them.
enum class ScopeKind { Histogram, Waveform, Parade, Vectorscope };

[[nodiscard]] std::string_view to_string(ScopeKind kind) noexcept;

/// Everything measured from one frame.
///
/// All four at once rather than only the one showing, because they are computed
/// from a frame that has already been fetched and scaled down — the second and
/// third cost a pass over a small buffer, while fetching the frame again to
/// switch tabs would cost a decode.
struct ScopeReadings {
  render::Histogram histogram;
  render::Waveform waveform;
  render::Parade parade;
  render::Vectorscope vectorscope;
  /// False until a frame has been measured, which is what makes the panel say
  /// so rather than drawing four convincing empty graphs.
  bool measured = false;
};

class ScopesView : public Widget {
 public:
  ScopesView();

  [[nodiscard]] ScopeKind kind() const noexcept { return kind_; }
  void set_kind(ScopeKind kind) noexcept { kind_ = kind; }

  /// Shared rather than copied: a full set is a couple of hundred kilobytes and
  /// the panel is repainted far more often than the frame changes.
  void set_readings(std::shared_ptr<const ScopeReadings> readings);
  [[nodiscard]] const ScopeReadings* readings() const noexcept { return readings_.get(); }

  /// Where the graph itself goes, inside the panel's padding.
  [[nodiscard]] Rect graph_area() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return true; }

  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  void paint_histogram(Painter& painter, const Theme& theme, const Rect& area) const;
  void paint_waveform(Painter& painter, const Theme& theme, const Rect& area,
                      const render::Waveform& wave, const Color& ink) const;
  void paint_parade(Painter& painter, const Theme& theme, const Rect& area) const;
  void paint_vectorscope(Painter& painter, const Theme& theme, const Rect& area) const;

  ScopeKind kind_ = ScopeKind::Histogram;
  std::shared_ptr<const ScopeReadings> readings_;
  Metrics metrics_;
};

}  // namespace cutline::ui
