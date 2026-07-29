#pragma once

/// Named arrangements of panels, and remembering them between sessions.
///
/// A workspace is a name and a layout. The active one's layout *is* what is on
/// screen — dragging a panel changes it in place — so switching away and back
/// returns to the arrangement as it was left, and only an explicit reset puts a
/// built-in one back to how it was defined. That is what every editor does, and
/// the alternative, silently discarding a rearrangement on every switch, is the
/// behaviour people complain about.
///
/// The file is written beside the application's other settings rather than into
/// a project. Where the panels are is a fact about the person using it, not
/// about the edit: opening someone else's project should not rearrange the room
/// you work in.

#include "cutline/ui/dock.hpp"

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// Bumped when the on-disk shape changes incompatibly. A file from the future
/// is refused rather than half-read.
inline constexpr int kWorkspaceSchemaVersion = 1;

struct Workspace {
  std::string name;
  ui::DockLayout layout;

  friend bool operator==(const Workspace&, const Workspace&) = default;
};

/// Every arrangement the application offers, and which one is in use.
struct Workspaces {
  std::vector<Workspace> named;
  /// The name of the one on screen. Always one of `named` once settled.
  std::string active;

  /// The layout being shown, or null when there is nothing at all.
  [[nodiscard]] ui::DockLayout* current() noexcept;
  [[nodiscard]] const ui::DockLayout* current() const noexcept;

  friend bool operator==(const Workspaces&, const Workspaces&) = default;
};

/// The arrangements a fresh install comes with.
[[nodiscard]] std::vector<Workspace> built_in_workspaces();

/// The set a fresh install starts from, with the first one active.
[[nodiscard]] Workspaces default_workspaces();

/// Switches to a named arrangement. Reports whether it was there to switch to.
bool activate_workspace(Workspaces& workspaces, std::string_view name);

/// Puts a built-in arrangement back to how it was defined, discarding whatever
/// it has been dragged into since. Reports whether it did anything — a
/// workspace the user made up has no definition to go back to.
bool reset_workspace(Workspaces& workspaces, std::string_view name);

/// Adds an arrangement under a new name, copying whatever is on screen now, and
/// makes it the active one. A name already in use is overwritten, because that
/// is what saving over something means.
bool add_workspace(Workspaces& workspaces, std::string name);

/// Removes one. The last workspace cannot be removed — there would be nothing
/// to show — and neither can a built-in, which has no other way back.
bool remove_workspace(Workspaces& workspaces, std::string_view name);

/// Makes every arrangement show exactly the panels this build has, and makes
/// sure something is active. Called after reading a file, and safe to call on
/// anything.
void settle(Workspaces& workspaces, std::span<const ui::PanelId> known);

// ------------------------------------------------------------- persistence --

[[nodiscard]] std::string to_json(const Workspaces& workspaces, int indent = 2);
[[nodiscard]] std::expected<Workspaces, std::string> workspaces_from_json(std::string_view text);

/// Where the file lives: beside the user's other application data.
[[nodiscard]] std::filesystem::path default_workspace_path();

/// Reads the file. A missing one is not an error — it means a fresh install —
/// and gives back the defaults.
[[nodiscard]] std::expected<Workspaces, std::string> read_workspaces(
    const std::filesystem::path& path);

/// Writes it, through a staging file so an interrupted save cannot leave a
/// half-written one behind. Same reason projects are written that way.
[[nodiscard]] std::expected<void, std::string> write_workspaces(
    const std::filesystem::path& path, const Workspaces& workspaces);

}  // namespace cutline::editor
