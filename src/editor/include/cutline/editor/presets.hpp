#pragma once

/// Named effect stacks, saved once and applied everywhere.
///
/// Premiere calls these presets and keeps them in the Effects panel beside the
/// effects themselves, which is right: somebody reaching for "my usual look"
/// is reaching for the same thing they reach for when they want a blur, and
/// making them hunt in a different panel for it is a distinction only the
/// implementation cares about.
///
/// A preset is a **name and a stack**, not a name and a reference. It holds
/// copies of the effects with the values they had, keyframes included, so a
/// preset saved from one project applies whole to another — which is the only
/// reason to have them.
///
/// The file lives beside the application's other settings rather than inside a
/// project, for the same reason a workspace does: a look you have built up is a
/// fact about how you work, and opening somebody else's project should not
/// replace your presets with theirs.

#include "cutline/core/model.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// Bumped when the on-disk shape changes incompatibly. A file from the future
/// is refused rather than half-read.
inline constexpr int kPresetSchemaVersion = 1;

/// One saved stack.
struct EffectPreset {
  std::string name;
  /// Both stacks, because a preset saved from a clip should put back what was
  /// on it. A preset with only audio in it applies to an audio clip and passes
  /// over a picture, and the other way round — the same rule the effect
  /// clipboard follows, and for the same reason.
  std::vector<core::ClipEffect> video;
  std::vector<core::AudioClipEffect> audio;

  [[nodiscard]] bool empty() const noexcept { return video.empty() && audio.empty(); }

  friend bool operator==(const EffectPreset&, const EffectPreset&) = default;
};

/// Everything saved, in the order it should be offered.
struct Presets {
  std::vector<EffectPreset> named;

  friend bool operator==(const Presets&, const Presets&) = default;
};

/// Takes what is on a clip and names it.
///
/// A name already in use is overwritten, because that is what saving over
/// something means, and it keeps its place in the list rather than jumping to
/// the end — a preset you have just refined should not move.
///
/// Refuses an empty name, and refuses a clip with nothing on it: a preset that
/// applies nothing is indistinguishable from one that failed to save.
bool save_preset(Presets& presets, const core::Project& project, std::string_view clip_id,
                 std::string name);

/// Removes one by name. Reports whether it was there.
bool remove_preset(Presets& presets, std::string_view name);

[[nodiscard]] const EffectPreset* find_preset(const Presets& presets,
                                              std::string_view name) noexcept;

/// Applies a preset to a clip, **adding** to whatever is already there.
///
/// Adding rather than replacing, which is the opposite of what pasting a
/// clipboard does, and the difference is what each gesture means: pasting is
/// "make this clip look like that one", and a preset is a thing you reach for
/// and put on. Two presets on one clip is an ordinary thing to want; two pastes
/// is not.
///
/// Only the half that fits: video effects on a picture, audio effects on sound.
/// Returns the project unchanged when nothing would alter.
[[nodiscard]] core::Project apply_preset(core::Project project, std::string_view clip_id,
                                         const EffectPreset& preset);

// ------------------------------------------------------------- persistence --

[[nodiscard]] std::string to_json(const Presets& presets, int indent = 2);
[[nodiscard]] std::expected<Presets, std::string> presets_from_json(std::string_view text);

/// Where the file lives: beside the user's other application data.
[[nodiscard]] std::filesystem::path default_presets_path();

/// Reads the file. A missing one is not an error — it means nobody has saved
/// one yet — and gives back an empty set.
[[nodiscard]] std::expected<Presets, std::string> read_presets(
    const std::filesystem::path& path);

/// Writes it, through a staging file and a rename, so an interrupted save
/// cannot leave a half-written one behind.
[[nodiscard]] std::expected<void, std::string> write_presets(const std::filesystem::path& path,
                                                             const Presets& presets);

}  // namespace cutline::editor
