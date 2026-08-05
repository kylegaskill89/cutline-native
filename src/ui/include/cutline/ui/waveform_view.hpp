#pragma once

/// A source's whole envelope, drawn large.
///
/// What the source monitor shows when there is no picture to show. A sound file
/// in a monitor that renders video is a black rectangle, which is indis-
/// tinguishable from a file that failed to decode — and the thing somebody
/// wants to see about a sound file is its shape, which is what says where the
/// words start and where the take is quiet.
///
/// The timeline draws envelopes too, inside a clip and a few pixels tall. This
/// is the same data given a whole panel: one bucket per column rather than one
/// per several, and a middle line so silence reads as silence rather than as a
/// gap in the drawing.

#include "cutline/ui/timeline.hpp"
#include "cutline/ui/widget.hpp"

#include <memory>

namespace cutline::ui {

class WaveformView : public Widget {
 public:
  WaveformView();

  /// The envelope to draw. Null while the worker is still reading it, which is
  /// drawn as an empty trough rather than as nothing — a panel that appears
  /// only once the answer arrives looks like one that is broken until then.
  void set_waveform(std::shared_ptr<const Waveform> wave);
  [[nodiscard]] const std::shared_ptr<const Waveform>& waveform() const noexcept {
    return wave_;
  }

  /// How long the source is. Taken from the media rather than from the envelope
  /// so the drawing spans the whole file while only part of it has been read.
  void set_duration(double seconds) noexcept;
  [[nodiscard]] double duration() const noexcept { return duration_; }

  /// Where the playhead is, in source seconds. Drawn as an upright, so the
  /// picture and the scrub bar below it agree about where they are.
  void set_playhead(double seconds) noexcept;
  [[nodiscard]] double playhead() const noexcept { return playhead_; }

  /// The area the envelope is drawn in, inside the padding.
  [[nodiscard]] Rect plot_area() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return false; }

  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

 private:
  std::shared_ptr<const Waveform> wave_;
  double duration_ = 0.0;
  double playhead_ = 0.0;
  Metrics metrics_;
};

}  // namespace cutline::ui
