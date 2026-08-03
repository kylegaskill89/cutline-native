#include "cutline/ui/cursor.hpp"

namespace cutline::ui {

std::string_view to_string(Cursor cursor) noexcept {
  switch (cursor) {
    case Cursor::Arrow: return "arrow";
    case Cursor::Text: return "text";
    case Cursor::ResizeWE: return "resize-we";
    case Cursor::ResizeNS: return "resize-ns";
    case Cursor::Move: return "move";
    case Cursor::Razor: return "razor";
    case Cursor::RateStretch: return "rate";
    case Cursor::Slip: return "slip";
    case Cursor::Slide: return "slide";
    case Cursor::Ripple: return "ripple";
    case Cursor::Roll: return "roll";
  }
  return "arrow";
}

}  // namespace cutline::ui
