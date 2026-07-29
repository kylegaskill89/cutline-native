#pragma once

/// What effects exist, what they are called, and what they take.
///
/// `resolve_effect_params` knows how to *apply* an effect. Nothing until now
/// knew how to *offer* one: a panel that lets someone add a blur has to know
/// that blur exists, that it has one parameter called `amount`, that it runs
/// from 0 to 50 pixels and starts at 0 — and none of that was written anywhere
/// a program could read. It was in the spec table and in the shape of an
/// if-chain, which is not the same thing.
///
/// This lives beside the resolver rather than in the interface layer so the two
/// cannot drift, and because an effect's parameters are part of what the effect
/// *is* rather than of how it is presented.
///
/// **Default is not the same as neutral.** `fallback` is what a newly added
/// effect is given, which is generally a value you can see: adding a vignette
/// and having nothing happen is indistinguishable from the button not working.
/// What a *missing* parameter means to the resolver is a separate question, and
/// for vignette and flip the two deliberately differ.
///
/// Names, ranges and defaults come from the registry table in the spec, which
/// is the authoritative definition of each effect's behaviour.

#include <cstddef>
#include <span>
#include <string_view>

namespace cutline::render {

/// A numeric parameter of an effect, as a control needs it.
struct EffectParamSpec {
  /// As stored in `ClipEffect::params`.
  std::string_view key;
  /// As shown.
  std::string_view name;
  double minimum = 0.0;
  double maximum = 1.0;
  /// What a newly added effect is given, and what a reset returns to.
  double fallback = 0.0;
  /// Appended to the readout: "%", "px", "°".
  std::string_view suffix;
  /// Zero or one, and worth a checkbox rather than a slider.
  bool toggle = false;
};

/// A colour parameter, kept apart from the numeric ones because it is stored
/// separately in the model and cannot be keyframed.
struct EffectColorSpec {
  std::string_view key;
  std::string_view name;
  /// Hex, as written into `ClipEffect::colors`.
  std::string_view fallback;
};

/// What Premiere calls a bin: effects are grouped rather than listed as thirty
/// unrelated names.
enum class EffectCategory {
  Color,
  BlurAndSharpen,
  Keying,
  Transform,
  Stylize,
};

[[nodiscard]] std::string_view to_string(EffectCategory category) noexcept;

struct EffectSpec {
  /// Keys `ClipEffect::type`. The resolver dispatches on this exact string.
  std::string_view type;
  std::string_view name;
  EffectCategory category = EffectCategory::Color;
  std::span<const EffectParamSpec> params;
  std::span<const EffectColorSpec> colors;
};

/// Every effect the compositor can draw, in the order they should be offered.
[[nodiscard]] std::span<const EffectSpec> effect_catalog() noexcept;

/// The entry for a stored effect, or null for a type this build does not know —
/// which a project written by a newer version can legitimately contain.
[[nodiscard]] const EffectSpec* find_effect_spec(std::string_view type) noexcept;

}  // namespace cutline::render
