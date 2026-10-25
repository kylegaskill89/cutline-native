#pragma once

/// Keyboard shortcuts: every action a key can run, what each is bound to, and
/// where the bindings are kept.
///
/// The keys used to be two tables in the application and a dozen more tested
/// one by one in its window procedure, so a key could not be changed without
/// a build, a menu could only show the keys it happened to be told about, and
/// nothing could say whether two of them collided. Here every action has a
/// stable name, a label for the menus and the Keyboard Shortcuts window, and a
/// scope that says when it is offered; a keymap is which chords run which, and
/// it starts from a preset -- Premiere Pro's keys, or Avid Media Composer's --
/// with the person's own changes written over it.
///
/// What an action *does* is the application's business. This only answers
/// which action a chord means.

#include "cutline/editor/commands.hpp"
#include "cutline/ui/event.hpp"

#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutline::editor {

/// A key and the modifiers held with it.
struct Chord {
  ui::Key key = ui::Key::None;
  bool control = false;
  bool shift = false;
  bool alt = false;

  [[nodiscard]] bool empty() const noexcept { return key == ui::Key::None; }
  friend auto operator<=>(const Chord&, const Chord&) = default;
};

/// When an action is offered a key, which decides what it can be bound to
/// without stealing keys a control needs.
enum class Scope {
  /// Before anything else, wherever the keyboard is -- undo, save, the
  /// clipboard's chords. While somebody is typing in a field the field goes
  /// first, so Ctrl+A selects its text rather than every clip.
  Always,
  /// Before the focused control, but never while typing: the tools, J/K/L,
  /// Space. A bare letter no control wants, and one that worked only when
  /// nothing happened to be focused would be one nobody trusted.
  Bare,
  /// Only once the focused control has declined it: the arrows belong to a
  /// focused slider before they belong to the playhead.
  Fallback,
};

/// Something a key can run.
struct Action {
  std::string_view id;        ///< stable, and what keys.json records
  std::string_view label;     ///< as the menus and the shortcuts window say it
  std::string_view category;  ///< which heading it is listed under
  Scope scope = Scope::Fallback;
  /// The editing command it runs, when it is one; otherwise the application
  /// knows what it does by its id.
  std::optional<Command> command;
};

/// Every action there is, in the order the shortcuts window lists them.
[[nodiscard]] std::span<const Action> actions() noexcept;
[[nodiscard]] const Action* find_action(std::string_view id) noexcept;
/// The action that runs an editing command, if one does.
[[nodiscard]] const Action* action_for(Command command) noexcept;

/// The presets a keymap can start from, in the order they are offered.
[[nodiscard]] std::span<const std::string_view> presets() noexcept;
inline constexpr std::string_view kPremierePreset = "Premiere Pro";
inline constexpr std::string_view kAvidPreset = "Avid Media Composer";

/// Which chords run which action.
struct Keymap {
  /// The preset it starts from.
  std::string preset{kPremierePreset};
  /// Every action's chords, the preset's with the person's changes over them.
  /// An action with none has an empty list rather than no entry.
  std::map<std::string, std::vector<Chord>, std::less<>> bindings;

  [[nodiscard]] std::span<const Chord> chords(std::string_view action) const;
  /// The action a chord is bound to, of any scope, or null.
  [[nodiscard]] const Action* holder(const Chord& chord) const;
  /// The action a chord runs when offered in this scope, or null.
  [[nodiscard]] const Action* resolve(const Chord& chord, Scope scope) const;

  friend bool operator==(const Keymap&, const Keymap&) = default;
};

/// A preset's keys, with nothing changed. An unknown name is Premiere's.
[[nodiscard]] Keymap preset_keymap(std::string_view preset);

/// Adds a chord to an action. A chord runs one action only, so whatever held
/// it loses it -- which the shortcuts window says before it is done. Reports
/// the action that lost it, if one did.
const Action* assign(Keymap& keymap, std::string_view action, const Chord& chord);
/// Takes a chord off an action.
void unassign(Keymap& keymap, std::string_view action, const Chord& chord);
/// Whether the keymap differs from the preset it starts from -- which is when
/// the shortcuts window calls it custom.
[[nodiscard]] bool is_custom(const Keymap& keymap);

/// A chord as a menu writes it: "Ctrl+Shift+K", the character itself where
/// the key has one, as Windows writes "Ctrl+," rather than "Ctrl+Comma".
[[nodiscard]] std::string to_text(const Chord& chord);
/// Reads that back, or nothing when it is not a chord.
[[nodiscard]] std::optional<Chord> chord_from_text(std::string_view text);
/// How a key on its own is written: the character, or its name.
[[nodiscard]] std::string key_text(ui::Key key);

/// The keymap as text: the preset and only the actions the person changed,
/// so a preset that gains a key in a later version gives it to them too.
[[nodiscard]] std::string to_json(const Keymap& keymap, int indent = 2);
[[nodiscard]] std::expected<Keymap, std::string> keymap_from_json(std::string_view text);

/// Beside the other preferences: `%APPDATA%\Cutline\keys.json`.
[[nodiscard]] std::filesystem::path default_keys_path();
/// A missing file is Premiere's keys, not an error.
[[nodiscard]] std::expected<Keymap, std::string> read_keymap(const std::filesystem::path& path);
[[nodiscard]] std::expected<void, std::string> write_keymap(const std::filesystem::path& path,
                                                            const Keymap& keymap);

}  // namespace cutline::editor
