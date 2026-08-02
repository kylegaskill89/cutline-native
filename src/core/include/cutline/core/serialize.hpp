#pragma once

/// Project persistence: a versioned JSON document.
///
/// The document wraps the project rather than being it, so future non-project
/// metadata has somewhere to live without another format break:
///
///     { "version": 1, "project": { ... } }
///
/// Reading is deliberately forgiving about *absent* fields — anything missing
/// takes its default — and strict about malformed ones. A project file that
/// predates a field should still open; a corrupt one should say so.
///
/// Media are referenced by absolute path. A path that no longer resolves is
/// reported as a warning rather than dropping the clips that depend on it,
/// leaving the caller free to offer a relink.

#include "cutline/core/model.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::core {

/// Bumped whenever the on-disk shape changes incompatibly.
inline constexpr int kProjectSchemaVersion = 1;

/// Serialises a project. `indent` of -1 writes compactly.
[[nodiscard]] std::string to_json(const Project& p, int indent = 2);

struct LoadedProject {
  Project project;
  /// Non-fatal observations, such as media files that no longer exist.
  std::vector<std::string> warnings;
};

/// Parses a project document. Fails only on malformed JSON, a missing project
/// body, or a schema version this build does not understand.
[[nodiscard]] std::expected<LoadedProject, std::string> from_json(std::string_view text);

/// A clip's two effect stacks on their own.
struct EffectStacks {
  std::vector<ClipEffect> video;
  std::vector<AudioClipEffect> audio;

  friend bool operator==(const EffectStacks&, const EffectStacks&) = default;
};

/// Effects encoded exactly as they are inside a project.
///
/// Exposed so that anything saving effects *outside* a project — a preset, a
/// clipboard that outlives a session — writes the same shape a project does.
/// Two encoders for one structure is two things to keep in step, and the one
/// nobody edited is the one that would silently stop reading old files.
[[nodiscard]] std::string effects_to_json(const EffectStacks& stacks, int indent = 2);

[[nodiscard]] std::expected<EffectStacks, std::string> effects_from_json(std::string_view text);

}  // namespace cutline::core
