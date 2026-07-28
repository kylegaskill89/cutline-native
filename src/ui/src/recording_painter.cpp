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
