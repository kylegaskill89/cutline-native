#pragma once

#include <span>
#include <vector>

namespace cutline::core {

/// An edit lands on an existing keyframe when it is within this of its time.
inline constexpr double kKeyframeMatchEps = 1e-4;

/// A delete removes any keyframe within this of the requested time. The looser
/// tolerance is inherited from the reference, where clicking to delete needs
/// more slack than dragging to place.
inline constexpr double kKeyframeRemoveEps = 1e-3;

/// Interpolation applied *outgoing* from a keyframe, toward the next one.
///
/// The first three are fixed shapes. `Bezier` is the general case, shaped by
/// the handles on the two keyframes either side of the segment, and it is what
/// dragging a handle on the velocity graph switches a keyframe to. The other
/// three remain because they are what a chip with three positions can offer and
/// what every project written so far holds; they are also, exactly, three
/// points in the space `Bezier` spans — except Hold, which is not a curve at
/// all.
enum class Interp {
  Linear,
  Hold,
  Ease,
  Bezier,
};

/// Where a keyframe's handles sit when nothing has moved them.
///
/// A third of the way along, a third of the way up. Those are the control
/// points of the cubic that *is* a straight line, so a keyframe switched to
/// Bezier and not touched animates exactly as a linear one does — which is what
/// makes grabbing a handle feel like taking hold of the curve that was already
/// there rather than replacing it.
inline constexpr double kDefaultHandleX = 1.0 / 3.0;
inline constexpr double kDefaultHandleY = 1.0 / 3.0;

/// A single animation breakpoint. `t` is clip-local seconds.
///
/// The handles are in **normalised segment space**: x is a fraction of the
/// segment's duration and y a fraction of its value change, both measured from
/// this keyframe's own end of it. That is what makes a handle mean the same
/// shape whatever the segment's length or height, and it is why a keyframe
/// carries one handle for each direction rather than one per segment — the
/// handles belong to the keyframe, and the segment reads whichever end it needs.
struct Keyframe {
  double t = 0.0;
  double v = 0.0;
  Interp e = Interp::Linear;

  /// Toward the next keyframe.
  double out_x = kDefaultHandleX;
  double out_y = kDefaultHandleY;
  /// Back toward the previous one. Measured backwards, so the two sides of a
  /// keyframe are symmetric and a handle pulled "outward" is a larger number on
  /// either side.
  double in_x = kDefaultHandleX;
  double in_y = kDefaultHandleY;

  friend bool operator==(const Keyframe&, const Keyframe&) = default;
};

/// Maps a 0..1 progress fraction through a keyframe's outgoing interpolation.
///
/// Bezier has no answer here — its shape depends on the keyframe at the *other*
/// end of the segment as well — so it falls back to linear. Callers that have
/// both keyframes should use `segment_fraction`, which is all of them inside
/// this library; this stays for the ones that only have a mode.
[[nodiscard]] double ease_fraction(double f, Interp mode) noexcept;

/// Maps a 0..1 progress fraction across the segment from `a` to `b`.
///
/// The general form, and what the evaluator uses. Which shape applies is `a`'s
/// mode, because interpolation is stored on the keyframe a segment leaves —
/// and when that mode is Bezier, `a`'s outgoing handle and `b`'s incoming one
/// are the two control points of the cubic.
[[nodiscard]] double segment_fraction(const Keyframe& a, const Keyframe& b, double f) noexcept;

/// A cubic bezier through (0,0) and (1,1) with control points (x1,y1) and
/// (x2,y2), evaluated at `f` along **x** rather than along the curve.
///
/// The same function a browser applies to `cubic-bezier()`, and for the same
/// reason: what is wanted is "the value at this fraction of the time", which
/// means solving for the curve parameter first. Newton's method with a
/// bisection fallback, because Newton alone stalls where the curve is flat.
///
/// `x1` and `x2` are clamped to 0..1, which keeps the x component monotonic and
/// so keeps the solve well posed. A handle dragged backwards in time would
/// otherwise describe a curve that is at two values at once.
[[nodiscard]] double bezier_fraction(double f, double x1, double y1, double x2,
                                     double y2) noexcept;

/// Interpolates a t-sorted keyframe list at `local_t`, honouring each
/// keyframe's outgoing interpolation mode. Clamped to the end values outside
/// the range; an empty list evaluates to 0.
[[nodiscard]] double eval_keyframes(std::span<const Keyframe> kfs, double local_t) noexcept;

// ------------------------------------------------------------ list editing --

/// Inserts a keyframe at `t`, or updates the one already there, keeping the
/// list sorted by time. An updated keyframe keeps its own interpolation; a new
/// one inherits the list's mode, so a property animated with "ease" stays eased
/// as points are added.
void upsert_keyframe(std::vector<Keyframe>& kfs, double t, double v);

/// Removes any keyframe within `kKeyframeRemoveEps` of `t`.
void remove_keyframe_near(std::vector<Keyframe>& kfs, double t);

/// The list's interpolation mode, taken from its first keyframe.
[[nodiscard]] Interp keyframe_list_interp(std::span<const Keyframe> kfs) noexcept;

/// Applies one interpolation mode across the whole list.
void set_keyframe_list_interp(std::vector<Keyframe>& kfs, Interp mode) noexcept;

}  // namespace cutline::core
