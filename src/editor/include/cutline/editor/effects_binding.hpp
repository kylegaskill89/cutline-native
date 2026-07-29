#pragma once

/// What the Effect Controls panel shows for a clip's effect stack.
///
/// The same shape as `inspector.hpp`, and for the same reason: the core has
/// stack operations and the interface has sliders, and neither should learn
/// about the other. This describes a clip's effects as data, so the panel is a
/// loop over a list rather than a form that has to be remembered whenever an
/// effect is added.
///
/// Only the *description* lives here. Removing, reordering, toggling and
/// setting a parameter are already core operations taking a clip id and an
/// index, and the panel calls those directly. Adding is the exception: "add a
/// blur" means "add a blur with the values a new blur should have", and that
/// needs the catalogue.
///
/// Values are in the units the effect stores, which the registry defines as the
/// units a person reads — percent, degrees, pixels. Unlike the transform, there
/// is no conversion to do.

#include "cutline/core/model.hpp"
#include "cutline/render/effect_catalog.hpp"
#include "cutline/ui/controls.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// One numeric parameter of one effect on one clip.
struct EffectParamRow {
  /// As stored, and as the core operations take it.
  std::string key;
  std::string name;
  ui::ValueRange range;
  double value = 0.0;
  /// What a reset returns to; the value a newly added effect was given.
  double fallback = 0.0;
  std::string suffix;
  /// Worth a checkbox rather than a slider.
  bool toggle = false;
  /// Animated, so the value shown is the static one the keyframes override.
  bool animated = false;

  friend bool operator==(const EffectParamRow&, const EffectParamRow&) = default;
};

struct EffectColorRow {
  std::string key;
  std::string name;
  /// Hex, as stored. The catalogue's default when the effect has none set.
  std::string value;

  friend bool operator==(const EffectColorRow&, const EffectColorRow&) = default;
};

/// One effect in the stack, in stack order.
struct EffectRow {
  /// Position in the clip's stack, which is what every core operation takes.
  /// Not stable across a removal or a move, so it is read again after either.
  std::size_t index = 0;
  std::string type;
  /// The catalogue's name, or the raw type for an effect this build does not
  /// know — a project written by a newer version should still open and still
  /// show that something is there.
  std::string name;
  bool enabled = true;
  /// True when the type is not in the catalogue, so the panel can say so
  /// rather than drawing an effect with no parameters and no explanation.
  bool unknown = false;
  std::vector<EffectParamRow> params;
  std::vector<EffectColorRow> colors;

  friend bool operator==(const EffectRow&, const EffectRow&) = default;
};

/// A clip's visual effect stack. Empty when the clip is not there, which is
/// also what an empty selection produces.
[[nodiscard]] std::vector<EffectRow> clip_effects(const core::Project& project,
                                                  std::string_view clip_id);

/// An effect that can be added, for the menu that offers them.
struct EffectChoice {
  std::string type;
  std::string name;
  std::string category;

  friend bool operator==(const EffectChoice&, const EffectChoice&) = default;
};

[[nodiscard]] std::vector<EffectChoice> addable_effects();

/// Adds an effect with the values a new one should have.
///
/// Returns the project unchanged when the type is not in the catalogue or the
/// clip is not there, like everything else that edits, so the session can skip
/// the undo entry.
[[nodiscard]] core::Project add_effect(core::Project project, std::string_view clip_id,
                                       std::string_view type);

}  // namespace cutline::editor
