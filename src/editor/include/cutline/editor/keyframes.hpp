#pragma once

/// A clip's animation, as lanes on a time axis.
///
/// The other half of the inspector binding. `inspector.hpp` describes a
/// parameter's *value* — what a number shows and what setting it does — and
/// this describes its animation *in time*: which keyframes exist, where they
/// sit, and what moving one means.
///
/// The two are separate because they are read by different things and at
/// different rates. A parameter row is rebuilt on every edit; a lane view is
/// dragged, and asking it to carry a value range and a display scale it never
/// uses would be a struct built for the wrong reader.
///
/// A clip's animation lives in two places — the built-in transform, indexed by
/// `AnimProp`, and each effect's own map, keyed by name — and nothing above
/// here should have to know which. `ParamRef` is what hides that.
///
/// Times are **clip-local seconds** throughout, which is what the model stores
/// and what a lane's axis is. Timeline time is the caller's problem.

#include "cutline/core/keyframe.hpp"
#include "cutline/core/model.hpp"
#include "cutline/editor/inspector.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// Which animated property a lane, or an edit, is about.
struct ParamRef {
  /// The built-in transform, rather than an entry in the effect stack.
  static constexpr std::size_t kMotion = static_cast<std::size_t>(-1);

  std::size_t effect = kMotion;
  /// Which stack `effect` indexes: the audio one rather than the visual one.
  /// Meaningless when `motion()`, and the two stacks are numbered separately,
  /// so an index alone does not say which clip parameter is meant.
  bool audio = false;
  /// Which transform property. Meaningful when `effect` is `kMotion`.
  ClipParam param = ClipParam::Opacity;
  /// Which of the effect's parameters. Meaningful otherwise.
  std::string key;

  [[nodiscard]] bool motion() const noexcept { return effect == kMotion; }

  friend bool operator==(const ParamRef&, const ParamRef&) = default;
};

/// One animated property and its keyframes.
struct KeyframeLane {
  ParamRef ref;
  /// What the lane is called on screen. Qualified by the effect for an effect
  /// parameter, because "Amount" alone says nothing when three effects have
  /// one.
  std::string name;

  /// Sorted by time. `v` is in **model** units rather than display ones — the
  /// lane draws position in time and nothing else, and converting a value
  /// nobody reads would be a scale to keep in step for no benefit.
  std::vector<core::Keyframe> keys;

  friend bool operator==(const KeyframeLane&, const KeyframeLane&) = default;
};

/// Everything a lane view needs about a clip.
struct KeyframeModel {
  /// The clip's length in seconds, which is what the time axis spans. Zero
  /// when there is no clip, which is also what an empty selection produces.
  double duration = 0.0;
  /// Only the properties that are actually animated. A lane for a property
  /// with no keyframes would be a row of nothing, and Premiere does not show
  /// one either.
  std::vector<KeyframeLane> lanes;

  [[nodiscard]] bool empty() const noexcept { return lanes.empty(); }

  friend bool operator==(const KeyframeModel&, const KeyframeModel&) = default;
};

/// What is animated on a clip, in the order the inspector lists it: the
/// transform first, then each effect's parameters in stack order.
[[nodiscard]] KeyframeModel clip_keyframes(const core::Project& project,
                                           std::string_view clip_id);

/// Moves the keyframe nearest `from` to `to`.
///
/// Clamped inside the clip, because a keyframe outside it is one that can never
/// be reached again. Returns the project unchanged when there is no keyframe
/// near `from`, or when the move would land on another one — two keyframes at
/// the same instant have no meaningful order, and the one that survived would
/// be whichever the sort happened to keep.
[[nodiscard]] core::Project move_keyframe(core::Project project, std::string_view clip_id,
                                          const ParamRef& ref, double from, double to);

/// Sets the interpolation *out of* the keyframe nearest `at`, toward the next.
///
/// Per keyframe, unlike `set_clip_parameter_interp`, which writes one mode
/// across the whole property. The evaluator has always read the outgoing
/// keyframe's own mode — `ease_fraction(f, a.e)` — so a list with a different
/// curve out of each point already renders correctly. Only the setters ever
/// flattened it.
[[nodiscard]] core::Project set_keyframe_interp(core::Project project,
                                                std::string_view clip_id, const ParamRef& ref,
                                                double at, core::Interp mode);

/// Removes the keyframe nearest `at`.
///
/// Unchanged when it is the last one: a property with animation on and no
/// keyframes at all evaluates to zero, which is not what removing the last
/// point should mean. Turning animation off is the stopwatch's job.
[[nodiscard]] core::Project remove_keyframe(core::Project project, std::string_view clip_id,
                                            const ParamRef& ref, double at);

// -------------------------------------------------------- copy and paste --

/// One keyframe, named by where it lives and when.
struct KeyframeAddress {
  ParamRef ref;
  double t = 0.0;

  friend bool operator==(const KeyframeAddress&, const KeyframeAddress&) = default;
};

/// Keyframes lifted off a clip, ready to go back somewhere else.
///
/// Times are **relative to the earliest one copied**, not absolute, which is
/// what makes pasting at the playhead mean anything: the shape of the animation
/// is the spacing between its points, and an absolute copy would only ever go
/// back where it came from.
struct KeyframeClipboard {
  struct Lane {
    ParamRef ref;
    std::vector<core::Keyframe> keys;

    friend bool operator==(const Lane&, const Lane&) = default;
  };

  std::vector<Lane> lanes;

  [[nodiscard]] bool empty() const noexcept { return lanes.empty(); }

  friend bool operator==(const KeyframeClipboard&, const KeyframeClipboard&) = default;
};

/// Takes copies of the keyframes at `addresses`, grouped by property.
///
/// Empty when none of them names a keyframe that is there, which is also what
/// an empty selection produces — so a paste after a failed copy puts nothing
/// anywhere rather than putting back whatever was copied before.
[[nodiscard]] KeyframeClipboard copy_keyframes(const core::Project& project,
                                               std::string_view clip_id,
                                               std::span<const KeyframeAddress> addresses);

/// Puts them back, with the earliest landing at `at`.
///
/// Onto the same properties they came from, which is the only place they mean
/// anything: a rotation curve pasted onto opacity would be numbers in the wrong
/// units. A property that is no longer animated is skipped rather than being
/// switched on — turning animation on is the stopwatch's job, and a paste that
/// silently did it would be a paste that changed the picture in a way nobody
/// asked for.
///
/// Existing keyframes at the same instants are overwritten, the way placing one
/// by hand is. Anything that would land past either end of the clip is dropped,
/// not clamped: clamping would pile a whole curve onto the last frame.
[[nodiscard]] core::Project paste_keyframes(core::Project project, std::string_view clip_id,
                                            const KeyframeClipboard& clipboard, double at);

/// The keyframe time before `t`, if there is one. Strictly before, so pressing
/// the button repeatedly walks the list rather than sticking on one.
[[nodiscard]] std::optional<double> keyframe_before(std::span<const core::Keyframe> keys,
                                                    double t) noexcept;
/// And after.
[[nodiscard]] std::optional<double> keyframe_after(std::span<const core::Keyframe> keys,
                                                   double t) noexcept;

}  // namespace cutline::editor
