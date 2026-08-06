#pragma once

/// The preferences that belong to the person rather than to the cut.
///
/// The fourth file under the user's application data, beside the workspaces,
/// the effect bins and the presets — and the one that should have been first.
/// Until this existed the theme was a plain member on `App`, written when a
/// button was pressed and read by nothing, so every launch started on the same
/// theme however many times somebody had chosen another. The same was true of
/// snapping, looping, the preview quality and how the pool was sorted: every
/// one of them a setting in every sense except that it was forgotten.
///
/// What belongs here is what outlives a project. The sequence size and the
/// frame rate do not — they travel with the cut and are saved in it — and the
/// rule that keeps the two apart is worth stating once: if opening somebody
/// else's project should change it, it is not a preference.
///
/// Every field has a default that is what the application did before this file
/// existed, so a fresh install behaves exactly as it always has and a missing
/// file is not a special case.

#include "cutline/editor/browser_binding.hpp"
#include "cutline/ui/browser.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace cutline::editor {

/// Bumped when the on-disk shape changes incompatibly. A file from the future
/// is refused rather than half-read, exactly as the workspaces are.
inline constexpr int kSettingsSchemaVersion = 1;

struct Settings {
  /// The theme, by name rather than by index.
  ///
  /// An index would quietly mean a different theme the first time one is
  /// inserted into the list, and the list is ordered for reading rather than
  /// for stability. A name that no longer exists falls back to the first, which
  /// is what a fresh install gets anyway.
  std::string theme;

  /// Whether an edge being dragged jumps to the things near it.
  bool snapping = true;
  /// Whether playback returns to the start of the marked range.
  bool looping = false;
  /// Whether scaling keeps a layer's shape. Premiere's "Uniform Scale".
  bool aspect_locked = false;

  /// How much of the sequence's resolution the preview renders at.
  ///
  /// Clamped on the way in rather than trusted: a hand-edited zero would
  /// divide the canvas to nothing and render a frame with no pixels in it.
  double preview_scale = 1.0;

  /// How the project panel is ordered and drawn.
  BrowserSort pool_sort = BrowserSort::Pool;
  bool pool_descending = false;
  ui::BrowserView pool_view = ui::BrowserView::List;

  friend bool operator==(const Settings&, const Settings&) = default;
};

/// The smallest and largest preview scales a file may ask for.
inline constexpr double kMinPreviewScale = 0.1;
inline constexpr double kMaxPreviewScale = 1.0;

[[nodiscard]] std::string to_json(const Settings& settings, int indent = 2);

/// Reads settings from text. Anything missing takes its default, and anything
/// unrecognised is ignored — a file written by a newer build should cost an
/// older one its new settings, not its ability to start.
[[nodiscard]] std::expected<Settings, std::string> settings_from_json(std::string_view text);

/// Where the file lives: beside the user's other application data.
[[nodiscard]] std::filesystem::path default_settings_path();

/// Reads the file. A missing one is not an error — it means nobody has changed
/// anything yet — and gives back the defaults.
[[nodiscard]] std::expected<Settings, std::string> read_settings(
    const std::filesystem::path& path);

/// Writes it, through a staging file so an interrupted save cannot leave a
/// half-written one behind. The same way projects and workspaces are written.
[[nodiscard]] std::expected<void, std::string> write_settings(const std::filesystem::path& path,
                                                              const Settings& settings);

}  // namespace cutline::editor
