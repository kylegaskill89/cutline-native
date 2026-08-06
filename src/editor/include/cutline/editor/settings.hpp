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

#include "cutline/editor/autosave.hpp"
#include "cutline/editor/browser_binding.hpp"
#include "cutline/editor/import.hpp"
#include "cutline/editor/timeline_binding.hpp"
#include "cutline/editor/transitions.hpp"
#include "cutline/ui/browser.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// Bumped when the on-disk shape changes incompatibly. A file from the future
/// is refused rather than half-read, exactly as the workspaces are.
inline constexpr int kSettingsSchemaVersion = 1;

/// How tall a proxy is written when nobody has said otherwise.
///
/// Spelled out here rather than taken from `media::kProxyHeight`, because this
/// layer is built and tested without FFmpeg anywhere near it — that is the
/// whole reason `editor` exists as a separate thing. The two are asserted equal
/// in the one translation unit that can see both.
inline constexpr int kDefaultProxyHeight = 540;

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

  // ------------------------------------------------------------- defaults --
  //
  // The three settings with no right answer, which is what makes them worth
  // exposing at all. The thread counts and cache budgets elsewhere in this
  // application were each chosen against a measurement written down beside
  // them, and a control offering somebody a worse answer than the measured one
  // only creates bad sessions.

  /// How long a still is placed for. Premiere's "Still image default duration".
  double still_length = kStillLength;
  /// How long a transition is when first added, before the join gets a say.
  double transition_length = kPreferredTransitionLength;
  /// How often a recovery copy is written, in seconds.
  int autosave_seconds = static_cast<int>(kAutosaveInterval.count());

  // -------------------------------------------------------------- labels --

  /// What each label is called, in `clip_labels()` order.
  ///
  /// Empty, or an empty entry, keeps the built-in name. A label is something
  /// people say out loud — "the violet ones are the interview" — and the whole
  /// value of renaming one is that the menu then says *Interview*. Premiere
  /// keeps these as a preference for the same reason.
  ///
  /// The colours stay where they are: they are the model's, they are written
  /// into every project that uses one, and a colour renamed on one machine
  /// would otherwise mean something different on another.
  std::vector<std::string> label_names;

  /// The label given to media of each kind as it is imported, by colour, in
  /// `ui::MediaKind` order. An empty entry means no label, which is the
  /// default for every kind.
  ///
  /// Premiere's "Label Defaults", and the half of this feature that does
  /// something on its own: a project where every still is already Rose and
  /// every title Mango was not coloured by anybody, and reads at a glance.
  std::vector<std::string> label_defaults;

  // ------------------------------------------------------------- proxies --

  /// How tall a proxy is written. Width follows the source's aspect.
  ///
  /// The one proxy setting with no right answer: what a machine can keep up
  /// with, and what a screen makes worth looking at, are facts about the desk
  /// this is running on. The *quality* stays fixed — it is a number on the
  /// encoder's own scale, and a control offering somebody a CRF to guess at
  /// makes worse sessions than a default chosen once with a reason beside it.
  int proxy_height = kDefaultProxyHeight;

  /// Where proxies are written, or empty for beside the footage.
  ///
  /// Premiere's Ingest Settings, and worth having because footage is so often
  /// on a drive that is slow, full, or somebody else's — a card mounted
  /// read-only has nowhere beside it to put anything.
  std::string proxy_folder;

  // --------------------------------------------------------------- audio --

  /// Which output to play through, by the system's own identifier, or empty for
  /// whichever the system prefers.
  ///
  /// Kept by id rather than by name because a name is neither unique nor
  /// stable: two identical interfaces are called the same thing, and one
  /// renamed would silently stop being the chosen one. The name is remembered
  /// beside it only so a device that is not plugged in can still be *named* in
  /// the list rather than appearing as a line of hex.
  std::string audio_device;
  std::string audio_device_name;

  friend bool operator==(const Settings&, const Settings&) = default;
};

/// What a label is called: the chosen name, or the built-in one.
[[nodiscard]] std::string_view label_name(const Settings& settings, std::size_t index);

/// The colour media of this kind should be labelled with, or empty for none.
[[nodiscard]] std::string_view label_default(const Settings& settings, ui::MediaKind kind);

/// The bounds each of those is clamped into, on the way in from a file and on
/// the way in from a field somebody typed.
///
/// Generous rather than tight: these exist to keep a typing mistake from
/// producing a length nothing can be done with, not to argue with somebody who
/// wants a ten-minute still. Zero is the one answer that has to be refused —
/// a still of no length is the bug this feature was born from.
inline constexpr double kMinStillLength = 0.1;
inline constexpr double kMaxStillLength = 600.0;
inline constexpr double kMinTransitionLength = 0.04;
inline constexpr double kMaxTransitionLength = 60.0;
inline constexpr int kMinAutosaveSeconds = 15;
inline constexpr int kMaxAutosaveSeconds = 3600;
inline constexpr int kMinProxyHeight = 180;
inline constexpr int kMaxProxyHeight = 2160;

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
