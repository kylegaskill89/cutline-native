#include "cutline/ui/animator.hpp"

#include <algorithm>
#include <chrono>

namespace cutline::ui {

Animator::Animator()
    : clock_([] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
      }) {}

Animator::Animator(Clock clock) : clock_(std::move(clock)) {}

float Animator::toward(const void* owner, int channel, bool on, double in, double out) {
  const double now = clock_();
  const auto key = std::pair{owner, channel};
  auto found = entries_.find(key);
  if (found == entries_.end()) {
    found = entries_.emplace(key, Entry{.value = on ? 1.0f : 0.0f, .on = on, .when = now}).first;
  }
  Entry& entry = found->second;
  entry.touched = true;

  // A change of target starts the clock afresh. Time spent at rest before it
  // is not time spent fading: an idle control is not asked about between
  // paints, so measuring from the last time it was would count the whole
  // pause and finish the fade in a single frame.
  if (on != entry.on) entry.when = now;
  const double elapsed = std::max(0.0, now - entry.when);
  entry.when = now;
  entry.on = on;
  const double span = on ? in : out;
  if (span <= 0.0) {
    entry.value = on ? 1.0f : 0.0f;
  } else {
    const auto step = static_cast<float>(elapsed / span);
    entry.value = on ? std::min(1.0f, entry.value + step) : std::max(0.0f, entry.value - step);
  }

  // Smoothstep: starting and arriving gently is what makes a fade read as a
  // fade rather than as a slow cut.
  const float t = entry.value;
  return t * t * (3.0f - 2.0f * t);
}

bool Animator::running() const noexcept {
  return std::ranges::any_of(entries_, [](const auto& pair) {
    const Entry& entry = pair.second;
    return entry.value != (entry.on ? 1.0f : 0.0f);
  });
}

void Animator::begin_frame() {
  std::erase_if(entries_, [](const auto& pair) { return !pair.second.touched; });
  for (auto& [key, entry] : entries_) entry.touched = false;
}

}  // namespace cutline::ui
