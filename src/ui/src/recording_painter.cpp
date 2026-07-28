#include "cutline/ui/recording_painter.hpp"

#include <algorithm>

namespace cutline::ui {

std::string_view to_string(DrawCall::Kind kind) noexcept {
  switch (kind) {
    case DrawCall::Kind::PushClip:
      return "push_clip";
    case DrawCall::Kind::PopClip:
      return "pop_clip";
    case DrawCall::Kind::Fill:
      return "fill";
    case DrawCall::Kind::Stroke:
      return "stroke";
    case DrawCall::Kind::Line:
      return "line";
    case DrawCall::Kind::Bevel:
      return "bevel";
    case DrawCall::Kind::Shadow:
      return "shadow";
    case DrawCall::Kind::BackdropBlur:
      return "backdrop_blur";
    case DrawCall::Kind::Text:
      return "text";
  }
  return "unknown";
}

void RecordingPainter::push_clip(const Rect& bounds, double corner_radius) {
  calls_.push_back({.kind = DrawCall::Kind::PushClip,
                    .bounds = bounds,
                    .corner_radius = corner_radius});
}

void RecordingPainter::pop_clip() { calls_.push_back({.kind = DrawCall::Kind::PopClip}); }

void RecordingPainter::fill(const Rect& bounds, double corner_radius, const Fill& value) {
  calls_.push_back({.kind = DrawCall::Kind::Fill,
                    .bounds = bounds,
                    .corner_radius = corner_radius,
                    .fill = value});
}

void RecordingPainter::stroke(const Rect& bounds, double corner_radius, const Color& color,
                              double width) {
  calls_.push_back({.kind = DrawCall::Kind::Stroke,
                    .bounds = bounds,
                    .corner_radius = corner_radius,
                    .color = color,
                    .width = width});
}

void RecordingPainter::line(double x1, double y1, double x2, double y2, const Color& color,
                            double width) {
  // Stored as a displacement rather than as a normalised rectangle, so a line
  // that runs up or to the left keeps its direction and an assertion can say
  // which way it went.
  calls_.push_back({.kind = DrawCall::Kind::Line,
                    .bounds = Rect{x1, y1, x2 - x1, y2 - y1},
                    .color = color,
                    .width = width});
}

void RecordingPainter::bevel(const Rect& bounds, const Bevel& value) {
  calls_.push_back({.kind = DrawCall::Kind::Bevel, .bounds = bounds, .bevel = value});
}

void RecordingPainter::shadow(const Rect& bounds, double corner_radius, const Shadow& value) {
  calls_.push_back({.kind = DrawCall::Kind::Shadow,
                    .bounds = bounds,
                    .corner_radius = corner_radius,
                    .shadow = value});
}

void RecordingPainter::backdrop_blur(const Rect& bounds, double corner_radius, double radius) {
  calls_.push_back({.kind = DrawCall::Kind::BackdropBlur,
                    .bounds = bounds,
                    .corner_radius = corner_radius,
                    .width = radius});
}

void RecordingPainter::text(const TextRun& run) {
  calls_.push_back({.kind = DrawCall::Kind::Text, .bounds = run.bounds, .run = run});
}

std::vector<DrawCall::Kind> RecordingPainter::kinds() const {
  std::vector<DrawCall::Kind> out;
  out.reserve(calls_.size());
  for (const DrawCall& call : calls_) out.push_back(call.kind);
  return out;
}

std::size_t RecordingPainter::count(DrawCall::Kind kind) const noexcept {
  return static_cast<std::size_t>(std::ranges::count(calls_, kind, &DrawCall::kind));
}

const DrawCall* RecordingPainter::first(DrawCall::Kind kind) const noexcept {
  const auto found = std::ranges::find(calls_, kind, &DrawCall::kind);
  return found == calls_.end() ? nullptr : &*found;
}

int RecordingPainter::index_of(DrawCall::Kind kind) const noexcept {
  const auto found = std::ranges::find(calls_, kind, &DrawCall::kind);
  if (found == calls_.end()) return -1;
  return static_cast<int>(std::distance(calls_.begin(), found));
}

double RecordingPainter::measure(std::string_view text, double size, bool bold) const {
  // Roughly the average advance of a proportional UI font as a fraction of its
  // size, with a little more for bold. This is not trying to be accurate; it
  // only has to be stable, so a test asserting that a button is wider than its
  // label gets the same answer every run.
  constexpr double kAverageAdvance = 0.55;
  constexpr double kBoldFactor = 1.05;
  return static_cast<double>(text.size()) * size * kAverageAdvance * (bold ? kBoldFactor : 1.0);
}

bool RecordingPainter::clips_balanced() const noexcept {
  int depth = 0;
  for (const DrawCall& call : calls_) {
    if (call.kind == DrawCall::Kind::PushClip) ++depth;
    if (call.kind == DrawCall::Kind::PopClip) {
      --depth;
      if (depth < 0) return false;  // popped more than were pushed
    }
  }
  return depth == 0;
}

}  // namespace cutline::ui
