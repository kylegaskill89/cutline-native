#pragma once

/// A painter that draws nothing and remembers everything.
///
/// Every question worth asking about how a widget looks — does a pressed XP
/// button invert its bevel, does an Aero panel blur what is behind it, does a
/// border land on top of the bevel rather than under it — is a question about
/// which calls were made and in what order. Recording them turns all of that
/// into ordinary unit tests, with no GPU, no window, and no comparing
/// screenshots to decide whether a change was intended.
///
/// It is not a mock. It is a real backend that happens to output a list.

#include "cutline/ui/painter.hpp"

#include <optional>
#include <vector>

namespace cutline::ui {

/// One recorded call. A variant would be tidier in principle; a tagged struct
/// reads far better in a failing assertion, which is where these are seen.
struct DrawCall {
  enum class Kind {
    PushClip,
    PopClip,
    Fill,
    Stroke,
    Bevel,
    Shadow,
    BackdropBlur,
    Text,
  };

  Kind kind = Kind::Fill;
  Rect bounds;
  double corner_radius = 0.0;

  Fill fill;                   ///< Fill
  Color color;                 ///< Stroke
  double width = 0.0;          ///< Stroke, and the blur radius for BackdropBlur
  std::optional<Bevel> bevel;  ///< Bevel
  std::optional<Shadow> shadow;///< Shadow
  std::optional<TextRun> run;  ///< Text
};

[[nodiscard]] std::string_view to_string(DrawCall::Kind kind) noexcept;

class RecordingPainter final : public Painter {
 public:
  void push_clip(const Rect& bounds, double corner_radius) override;
  void pop_clip() override;
  void fill(const Rect& bounds, double corner_radius, const Fill& fill) override;
  void stroke(const Rect& bounds, double corner_radius, const Color& color,
              double width) override;
  void bevel(const Rect& bounds, const Bevel& bevel) override;
  void shadow(const Rect& bounds, double corner_radius, const Shadow& shadow) override;
  void backdrop_blur(const Rect& bounds, double corner_radius, double radius) override;
  void text(const TextRun& run) override;

  /// A rough proportional-font estimate. There is no font here, and the only
  /// things layout tests need from a measurement are that it is deterministic
  /// and that it grows with both the string and the size.
  [[nodiscard]] double measure(std::string_view text, double size, bool bold) const override;

  [[nodiscard]] const std::vector<DrawCall>& calls() const noexcept { return calls_; }
  void clear() noexcept { calls_.clear(); }

  /// The kinds recorded, in order — the usual thing an assertion wants.
  [[nodiscard]] std::vector<DrawCall::Kind> kinds() const;

  /// How many calls of a kind were made.
  [[nodiscard]] std::size_t count(DrawCall::Kind kind) const noexcept;

  /// The first call of a kind, or null.
  [[nodiscard]] const DrawCall* first(DrawCall::Kind kind) const noexcept;

  /// Where a kind first appears in the order, or -1. Comparing two of these is
  /// how "the border goes on top of the bevel" is stated.
  [[nodiscard]] int index_of(DrawCall::Kind kind) const noexcept;

  /// True when every clip pushed was popped. A leaked clip silently swallows
  /// everything drawn afterwards, which looks like a missing widget.
  [[nodiscard]] bool clips_balanced() const noexcept;

 private:
  std::vector<DrawCall> calls_;
};

}  // namespace cutline::ui
