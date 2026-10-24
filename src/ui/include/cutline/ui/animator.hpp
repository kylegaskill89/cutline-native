#pragma once

/// The interface's clock: how far each widget has got in fading between how it
/// looks at rest, under the pointer and pressed.
///
/// The widget layer had no notion of time, so every change of state was a
/// snap. That is right for XP, whose controls did exactly that, and wrong for
/// Vista, whose buttons glowed in and faded out slowly, and for any modern
/// interface, where a hover that arrives a tenth of a second late reads as
/// smooth rather than slow. The durations are the theme's; this only keeps
/// time.
///
/// One per widget host. A widget asks it once a paint for each thing it fades,
/// and the host keeps painting for as long as anything is still on its way.
/// Nothing runs between paints and nothing is scheduled: an interface at rest
/// does no work at all, which the frame loop depends on.

#include <functional>
#include <map>
#include <utility>

namespace cutline::ui {

class Animator {
 public:
  /// Seconds from any fixed point. Injected so a test can step time exactly
  /// rather than sleeping and hoping.
  using Clock = std::function<double()>;

  Animator();
  explicit Animator(Clock clock);

  void set_clock(Clock clock) { clock_ = std::move(clock); }

  /// How far `owner`'s `channel` has got towards `on`, from 0 to 1, eased.
  ///
  /// It moves at `in` seconds for the whole way up and `out` for the whole way
  /// down, so a glow can arrive quickly and leave slowly, as Vista's did. Zero
  /// snaps. The first time a channel is asked about it starts where it is
  /// going: nothing fades in merely by being drawn for the first time.
  [[nodiscard]] float toward(const void* owner, int channel, bool on, double in, double out);

  /// Whether anything asked about in the last paint had not yet arrived, which
  /// is what keeps the frame loop painting.
  [[nodiscard]] bool running() const noexcept;

  /// Called by the host before each paint. Forgets whatever was not asked
  /// about during the last one -- a widget that has gone, or is no longer
  /// drawn -- so nothing lingers and nothing keeps the loop awake for it.
  void begin_frame();

  /// How many channels it is keeping, for tests.
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

 private:
  struct Entry {
    float value = 0.0f;
    bool on = false;
    double when = 0.0;
    bool touched = true;
  };
  Clock clock_;
  std::map<std::pair<const void*, int>, Entry> entries_;
};

}  // namespace cutline::ui
