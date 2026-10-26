#pragma once

/// The controls a colour grade is made with: Lumetri's wheels, curves and the
/// HSL secondary's range sliders.
///
/// Like the colour picker, their insides are colour and no theme restyles
/// them; only the frame and the marks drawn over them follow the theme. What
/// colour each point of a wheel or a track shows is handed in rather than
/// worked out here, so the control and the grade it drives cannot disagree
/// about where red is.

#include "cutline/ui/painter.hpp"
#include "cutline/ui/widget.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace cutline::ui {

/// A colour wheel: a puck on a disc, each of whose points is a direction and
/// an amount of colour. The centre is none.
class ColorWheel : public Widget {
 public:
  /// The colour shown at a point of the disc, `x` and `y` from -1 to 1 with up
  /// positive.
  using Shade = std::function<Color(double x, double y)>;

  explicit ColorWheel(double diameter = 96.0);

  void set_shade(Shade shade);

  /// Where the puck is, held to the disc. Sets without calling back.
  void set_position(double x, double y);
  [[nodiscard]] double x() const noexcept { return x_; }
  [[nodiscard]] double y() const noexcept { return y_; }

  /// Every movement of a drag, for a live picture.
  void set_on_change(std::function<void(double, double)> on_change) {
    on_change_ = std::move(on_change);
  }
  /// Once when a gesture ends, for the edit.
  void set_on_commit(std::function<void(double, double)> on_commit) {
    on_commit_ = std::move(on_commit);
  }

  /// The disc itself.
  [[nodiscard]] Rect disc() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return false; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

 private:
  void move_to(double px, double py);
  void regenerate();

  double diameter_;
  Shade shade_;
  double x_ = 0.0;
  double y_ = 0.0;
  bool dragging_ = false;
  std::vector<std::uint8_t> pixels_;
  int pixels_size_ = 0;
  std::function<void(double, double)> on_change_;
  std::function<void(double, double)> on_commit_;
};

/// A point a curve passes through, both ends 0 to 1.
struct CurvePointView {
  double x = 0.0;
  double y = 0.0;

  friend bool operator==(const CurvePointView&, const CurvePointView&) = default;
};

/// A curve to be bent by its points: Lumetri's RGB curves and its hue and
/// saturation curves.
///
/// The control holds the points and what the curve looks like between them,
/// but does not draw the curve through them itself: whoever owns it hands back
/// the samples the grade will actually use, so what is drawn is what is
/// applied.
class CurveGraph : public Widget {
 public:
  /// What runs along the bottom, which says what the horizontal axis means.
  enum class Backdrop { Tone, Hue, Saturation };

  /// `rest` is where a curve with no points lies: a diagonal when `diagonal`,
  /// a flat line at one half otherwise. A `periodic` curve goes round, as hue
  /// does.
  CurveGraph(Backdrop backdrop, Color line, bool diagonal, bool periodic, double height = 120.0);

  void set_points(std::vector<CurvePointView> points);
  [[nodiscard]] const std::vector<CurvePointView>& points() const noexcept { return points_; }
  /// The curve as the grade will apply it, evenly spaced from 0 to 1.
  void set_samples(std::vector<float> samples);
  void set_line(Color line) { line_ = line; }

  void set_on_change(std::function<void(const std::vector<CurvePointView>&)> on_change) {
    on_change_ = std::move(on_change);
  }
  void set_on_commit(std::function<void(const std::vector<CurvePointView>&)> on_commit) {
    on_commit_ = std::move(on_commit);
  }

  /// The square the curve is drawn in.
  [[nodiscard]] Rect plot() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return false; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

 private:
  [[nodiscard]] double sample_at(double x) const;
  [[nodiscard]] double screen_x(double x) const;
  [[nodiscard]] double screen_y(double y) const;
  /// The point within reach of a press, if any.
  [[nodiscard]] int point_near(double px, double py) const;
  void sort_keeping(int& dragged);
  void regenerate();

  Backdrop backdrop_;
  Color line_;
  bool diagonal_;
  bool periodic_;
  double height_;
  std::vector<CurvePointView> points_;
  std::vector<float> samples_;
  int dragging_ = -1;
  std::vector<std::uint8_t> strip_;
  int strip_width_ = 0;
  std::function<void(const std::vector<CurvePointView>&)> on_change_;
  std::function<void(const std::vector<CurvePointView>&)> on_commit_;
};

/// A range along a coloured track, with a softness either side: the HSL
/// secondary's hue, saturation and luma selectors. The handles above set the
/// range and the ones below how gradually it fades out; between them the
/// whole range can be slid along.
class RangeSlider : public Widget {
 public:
  /// The colour at a point along the track, 0 to 1.
  using Shade = std::function<Color(double at)>;

  /// A `wraps` range goes round, as hue does, so its low end may be above its
  /// high end.
  RangeSlider(Shade shade, bool wraps);

  /// Sets without calling back. Everything is 0 to 1.
  void set_range(double low, double high, double soft);
  [[nodiscard]] double low() const noexcept { return low_; }
  [[nodiscard]] double high() const noexcept { return high_; }
  [[nodiscard]] double soft() const noexcept { return soft_; }

  void set_on_change(std::function<void(double, double, double)> on_change) {
    on_change_ = std::move(on_change);
  }
  void set_on_commit(std::function<void(double, double, double)> on_commit) {
    on_commit_ = std::move(on_commit);
  }

  [[nodiscard]] Rect track() const;

  [[nodiscard]] Part part() const noexcept override { return Part::Panel; }
  [[nodiscard]] bool paints_surface() const noexcept override { return false; }
  [[nodiscard]] LayoutItem sizing(Axis axis, const LayoutContext& context) const override;
  void layout(const LayoutContext& context) override;
  void paint_content(Painter& painter, const Theme& theme) const override;

  bool on_mouse_down(const MouseEvent& event) override;
  bool on_mouse_move(const MouseEvent& event) override;
  bool on_mouse_up(const MouseEvent& event) override;

 private:
  enum class Grip { None, Low, High, SoftLow, SoftHigh, Whole };

  [[nodiscard]] double at(double px) const;
  [[nodiscard]] double screen(double at) const;
  [[nodiscard]] bool inside(double at) const;
  void regenerate();

  Shade shade_;
  bool wraps_;
  double low_ = 0.0;
  double high_ = 1.0;
  double soft_ = 0.0;
  Grip grip_ = Grip::None;
  double grabbed_at_ = 0.0;
  double grabbed_low_ = 0.0;
  double grabbed_high_ = 0.0;
  std::vector<std::uint8_t> strip_;
  int strip_width_ = 0;
  std::function<void(double, double, double)> on_change_;
  std::function<void(double, double, double)> on_commit_;
};

}  // namespace cutline::ui
