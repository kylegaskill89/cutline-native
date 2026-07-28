#pragma once

/// What the inspector shows for a clip, and what changing it does.
///
/// The same shape as the timeline binding, and for the same reason: the core
/// has property operations and the UI has sliders, and neither should learn
/// about the other. This describes a clip's editable properties as data, so the
/// panel is a loop over a list rather than a hand-built form that has to be
/// remembered whenever a property is added.
///
/// Values here are in **display** units, not model units — percent rather than
/// a fraction, degrees rather than radians. The conversion lives in one place
/// so a slider reading 100% and a clip storing 1.0 cannot drift apart.

#include "cutline/core/model.hpp"
#include "cutline/ui/controls.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// A clip property a control can edit.
enum class ClipParam {
  Opacity,
  X,
  Y,
  ScaleX,
  ScaleY,
  Rotation,
  Speed,
  Gain,
  FadeIn,
  FadeOut,
};

[[nodiscard]] std::string_view to_string(ClipParam param) noexcept;

/// One row of the inspector.
struct ParamSpec {
  ClipParam param = ClipParam::Opacity;
  /// What to call it on screen.
  std::string name;
  ui::ValueRange range;
  /// Current value, in display units.
  double value = 0.0;
  /// What a double-click returns to, also in display units.
  double fallback = 0.0;
  /// Appended to the readout: "%", "°", "x", "s".
  std::string suffix;

  friend bool operator==(const ParamSpec&, const ParamSpec&) = default;
};

/// The parameters worth showing for a clip, in the order they should appear.
///
/// A video clip gets its transform and opacity; an audio clip gets gain, and
/// none of the geometry that would mean nothing on it. Empty when the clip is
/// not there, which is also what an empty selection produces.
[[nodiscard]] std::vector<ParamSpec> clip_parameters(const core::Project& project,
                                                     std::string_view clip_id);

/// Applies one parameter, taking a value in display units.
///
/// Returns the project unchanged when it cannot apply, like everything else
/// that edits, so the session can skip the undo entry.
[[nodiscard]] core::Project set_clip_parameter(core::Project project, std::string_view clip_id,
                                               ClipParam param, double value);

}  // namespace cutline::editor
