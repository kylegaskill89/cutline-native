#include "cutline/editor/shortcuts.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <system_error>

namespace cutline::editor {
namespace {

using nlohmann::json;
using ui::Key;

/// Bumped when the file's shape changes incompatibly.
constexpr int kKeysSchemaVersion = 1;

constexpr std::array kActions{
    // -- File
    Action{"file.new", "New Project", "File", Scope::Always},
    Action{"file.open", "Open Project...", "File", Scope::Always},
    Action{"file.save", "Save", "File", Scope::Always},
    Action{"file.save_as", "Save As...", "File", Scope::Always},
    Action{"file.import", "Import...", "File", Scope::Always},
    Action{"file.export", "Export Media...", "File", Scope::Always},

    // -- Edit. The clipboard's chords are offered after the focused panel,
    // because the keyframe lanes have a clipboard of their own.
    Action{"edit.undo", "Undo", "Edit", Scope::Always, Command::Undo},
    Action{"edit.redo", "Redo", "Edit", Scope::Always, Command::Redo},
    Action{"edit.cut", "Cut", "Edit", Scope::Fallback, Command::Cut},
    Action{"edit.copy", "Copy", "Edit", Scope::Fallback, Command::Copy},
    Action{"edit.paste", "Paste", "Edit", Scope::Fallback, Command::Paste},
    Action{"edit.paste_insert", "Paste Insert", "Edit", Scope::Fallback, Command::PasteInsert},
    Action{"edit.paste_attributes", "Paste Attributes...", "Edit", Scope::Fallback,
           Command::PasteAttributes},
    Action{"edit.clear", "Clear", "Edit", Scope::Fallback, Command::Delete},
    Action{"edit.ripple_delete", "Ripple Delete", "Edit", Scope::Fallback, Command::RippleDelete},
    Action{"edit.select_all", "Select All", "Edit", Scope::Always, Command::SelectAll},
    Action{"edit.deselect_all", "Deselect All", "Edit", Scope::Fallback, Command::SelectNone},
    Action{"edit.keyboard_shortcuts", "Keyboard Shortcuts...", "Edit", Scope::Always},

    // -- Clip
    Action{"clip.link", "Link", "Clip", Scope::Fallback, Command::LinkClips},
    Action{"clip.unlink", "Unlink", "Clip", Scope::Fallback, Command::UnlinkClips},
    Action{"clip.nest", "Nest...", "Clip", Scope::Fallback, Command::Nest},

    // -- Sequence
    Action{"sequence.add_edit", "Add Edit", "Sequence", Scope::Always, Command::Split},
    Action{"sequence.add_edit_all", "Add Edit to All Tracks", "Sequence", Scope::Always,
           Command::SplitAllTracks},
    Action{"sequence.trim_previous", "Ripple Trim Previous Edit to Playhead", "Sequence",
           Scope::Fallback, Command::TrimPreviousToPlayhead},
    Action{"sequence.trim_next", "Ripple Trim Next Edit to Playhead", "Sequence",
           Scope::Fallback, Command::TrimNextToPlayhead},
    Action{"sequence.insert", "Insert", "Sequence", Scope::Fallback, Command::Insert},
    Action{"sequence.overwrite", "Overwrite", "Sequence", Scope::Fallback, Command::Overwrite},
    Action{"sequence.fit_to_fill", "Fit to Fill...", "Sequence", Scope::Fallback,
           Command::FitToFill},
    Action{"sequence.nudge_left", "Nudge Clip Left", "Sequence", Scope::Fallback,
           Command::NudgeLeft},
    Action{"sequence.nudge_right", "Nudge Clip Right", "Sequence", Scope::Fallback,
           Command::NudgeRight},
    Action{"sequence.add_video_track", "Add Video Track", "Sequence", Scope::Fallback,
           Command::AddVideoTrack},
    Action{"sequence.add_audio_track", "Add Audio Track", "Sequence", Scope::Fallback,
           Command::AddAudioTrack},
    Action{"sequence.remove_track", "Delete Track", "Sequence", Scope::Fallback,
           Command::RemoveTrack},
    Action{"sequence.apply_video_transition", "Apply Video Transition", "Sequence",
           Scope::Fallback, Command::ApplyVideoTransition},
    Action{"sequence.apply_audio_transition", "Apply Audio Transition", "Sequence",
           Scope::Fallback, Command::ApplyAudioTransition},
    Action{"sequence.apply_default_transitions", "Apply Default Transitions to Selection",
           "Sequence", Scope::Fallback, Command::ApplyDefaultTransitions},
    Action{"sequence.render_in_to_out", "Render In to Out", "Sequence", Scope::Fallback},
    Action{"sequence.trim_edit", "Trim Edit", "Sequence", Scope::Fallback},
    Action{"sequence.toggle_trim_type", "Toggle Trim Type", "Sequence", Scope::Fallback},
    Action{"sequence.trim_backward", "Trim Backward", "Sequence", Scope::Fallback},
    Action{"sequence.trim_forward", "Trim Forward", "Sequence", Scope::Fallback},
    Action{"sequence.trim_backward_many", "Trim Backward Many", "Sequence", Scope::Fallback},
    Action{"sequence.trim_forward_many", "Trim Forward Many", "Sequence", Scope::Fallback},
    Action{"sequence.snap", "Snap in Timeline", "Sequence", Scope::Bare},
    Action{"sequence.zoom_in", "Zoom In", "Sequence", Scope::Bare},
    Action{"sequence.zoom_out", "Zoom Out", "Sequence", Scope::Bare},
    Action{"sequence.zoom_fit", "Zoom to Sequence", "Sequence", Scope::Bare},

    // -- Markers
    Action{"markers.mark_in", "Mark In", "Markers", Scope::Fallback, Command::MarkIn},
    Action{"markers.mark_out", "Mark Out", "Markers", Scope::Fallback, Command::MarkOut},
    Action{"markers.clear_in_out", "Clear In and Out", "Markers", Scope::Fallback,
           Command::ClearMarks},
    Action{"markers.add", "Add Marker", "Markers", Scope::Fallback, Command::AddMarker},
    Action{"markers.next", "Go to Next Marker", "Markers", Scope::Fallback, Command::NextMarker},
    Action{"markers.previous", "Go to Previous Marker", "Markers", Scope::Fallback,
           Command::PreviousMarker},
    Action{"markers.clear_all", "Clear All Markers", "Markers", Scope::Fallback,
           Command::ClearMarkers},

    // -- Playback
    Action{"playback.play", "Play/Stop", "Playback", Scope::Bare},
    Action{"playback.shuttle_left", "Shuttle Left", "Playback", Scope::Bare},
    Action{"playback.stop", "Shuttle Stop", "Playback", Scope::Bare},
    Action{"playback.shuttle_right", "Shuttle Right", "Playback", Scope::Bare},
    Action{"playback.step_back", "Step Back One Frame", "Playback", Scope::Fallback,
           Command::PreviousFrame},
    Action{"playback.step_forward", "Step Forward One Frame", "Playback", Scope::Fallback,
           Command::NextFrame},
    Action{"playback.step_back_five", "Step Back Five Frames", "Playback", Scope::Fallback},
    Action{"playback.step_forward_five", "Step Forward Five Frames", "Playback", Scope::Fallback},
    Action{"playback.go_to_start", "Go to Sequence Start", "Playback", Scope::Fallback,
           Command::GoToStart},
    Action{"playback.go_to_end", "Go to Sequence End", "Playback", Scope::Fallback,
           Command::GoToEnd},

    // -- Tools
    Action{"tool.selection", "Selection Tool", "Tools", Scope::Bare},
    Action{"tool.ripple", "Ripple Edit Tool", "Tools", Scope::Bare},
    Action{"tool.roll", "Rolling Edit Tool", "Tools", Scope::Bare},
    Action{"tool.razor", "Razor Tool", "Tools", Scope::Bare},
    Action{"tool.rate_stretch", "Rate Stretch Tool", "Tools", Scope::Bare},
    Action{"tool.slip", "Slip Tool", "Tools", Scope::Bare},
    Action{"tool.slide", "Slide Tool", "Tools", Scope::Bare},
    Action{"tool.type", "Type Tool", "Tools", Scope::Bare},
    Action{"tool.rectangle", "Rectangle Tool", "Tools", Scope::Bare},
    Action{"tool.ellipse", "Ellipse Tool", "Tools", Scope::Bare},

    // -- Multi-Camera
    Action{"multicam.select_camera_1", "Select Camera 1", "Multi-Camera", Scope::Bare},
    Action{"multicam.select_camera_2", "Select Camera 2", "Multi-Camera", Scope::Bare},
    Action{"multicam.select_camera_3", "Select Camera 3", "Multi-Camera", Scope::Bare},
    Action{"multicam.select_camera_4", "Select Camera 4", "Multi-Camera", Scope::Bare},
    Action{"multicam.select_camera_5", "Select Camera 5", "Multi-Camera", Scope::Bare},
    Action{"multicam.select_camera_6", "Select Camera 6", "Multi-Camera", Scope::Bare},
    Action{"multicam.select_camera_7", "Select Camera 7", "Multi-Camera", Scope::Bare},
    Action{"multicam.select_camera_8", "Select Camera 8", "Multi-Camera", Scope::Bare},
    Action{"multicam.select_camera_9", "Select Camera 9", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_1", "Cut to Camera 1", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_2", "Cut to Camera 2", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_3", "Cut to Camera 3", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_4", "Cut to Camera 4", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_5", "Cut to Camera 5", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_6", "Cut to Camera 6", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_7", "Cut to Camera 7", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_8", "Cut to Camera 8", "Multi-Camera", Scope::Bare},
    Action{"multicam.cut_to_camera_9", "Cut to Camera 9", "Multi-Camera", Scope::Bare},

    // -- Window
    Action{"window.maximise_frame", "Maximize or Restore Frame", "Window", Scope::Bare},
    Action{"window.float_panel", "Undock Panel", "Window", Scope::Always},
    Action{"view.theme.slate", "Slate Theme", "Window", Scope::Bare},
    Action{"view.theme.luna", "Luna Theme", "Window", Scope::Bare},
    Action{"view.theme.aero", "Aero Theme", "Window", Scope::Bare},
    Action{"view.theme.phosphor", "Phosphor Theme", "Window", Scope::Bare},
};

constexpr std::array kPresets{kPremierePreset, kAvidPreset};

struct Default {
  std::string_view action;
  Chord chord;
};

[[nodiscard]] constexpr Chord bare(Key key) { return Chord{.key = key}; }
[[nodiscard]] constexpr Chord ctrl(Key key) { return Chord{.key = key, .control = true}; }
[[nodiscard]] constexpr Chord ctrl_shift(Key key) {
  return Chord{.key = key, .control = true, .shift = true};
}
[[nodiscard]] constexpr Chord ctrl_alt(Key key) {
  return Chord{.key = key, .control = true, .alt = true};
}
[[nodiscard]] constexpr Chord shift(Key key) { return Chord{.key = key, .shift = true}; }
[[nodiscard]] constexpr Chord alt(Key key) { return Chord{.key = key, .alt = true}; }

/// Premiere Pro's keys, which are what this application has always had.
constexpr std::array kPremiereKeys{
    Default{"file.new", ctrl(Key::N)},
    Default{"file.open", ctrl(Key::O)},
    Default{"file.save", ctrl(Key::S)},
    Default{"file.save_as", ctrl_shift(Key::S)},
    Default{"file.import", ctrl(Key::I)},
    Default{"file.export", ctrl(Key::M)},
    Default{"edit.undo", ctrl(Key::Z)},
    Default{"edit.redo", ctrl_shift(Key::Z)},
    Default{"edit.redo", ctrl(Key::Y)},
    Default{"edit.cut", ctrl(Key::X)},
    Default{"edit.copy", ctrl(Key::C)},
    Default{"edit.paste", ctrl(Key::V)},
    Default{"edit.paste_insert", ctrl_shift(Key::V)},
    Default{"edit.paste_attributes", ctrl_alt(Key::V)},
    Default{"edit.clear", bare(Key::Delete)},
    Default{"edit.ripple_delete", shift(Key::Delete)},
    Default{"edit.select_all", ctrl(Key::A)},
    Default{"edit.deselect_all", bare(Key::Escape)},
    Default{"edit.keyboard_shortcuts", ctrl_alt(Key::K)},
    Default{"clip.link", ctrl(Key::L)},
    Default{"clip.unlink", ctrl_shift(Key::L)},
    Default{"sequence.add_edit", ctrl(Key::K)},
    Default{"sequence.add_edit_all", ctrl_shift(Key::K)},
    Default{"sequence.trim_previous", bare(Key::Q)},
    Default{"sequence.trim_next", bare(Key::W)},
    Default{"sequence.insert", bare(Key::Comma)},
    Default{"sequence.overwrite", bare(Key::Period)},
    Default{"sequence.nudge_left", alt(Key::Left)},
    Default{"sequence.nudge_right", alt(Key::Right)},
    Default{"sequence.apply_video_transition", ctrl(Key::D)},
    Default{"sequence.apply_audio_transition", ctrl_shift(Key::D)},
    Default{"sequence.apply_default_transitions", shift(Key::D)},
    Default{"sequence.render_in_to_out", bare(Key::Enter)},
    Default{"sequence.trim_edit", shift(Key::T)},
    Default{"sequence.toggle_trim_type", ctrl(Key::T)},
    Default{"sequence.trim_backward", ctrl(Key::Left)},
    Default{"sequence.trim_forward", ctrl(Key::Right)},
    Default{"sequence.trim_backward_many", ctrl_shift(Key::Left)},
    Default{"sequence.trim_forward_many", ctrl_shift(Key::Right)},
    Default{"sequence.snap", bare(Key::S)},
    Default{"sequence.zoom_in", bare(Key::Equal)},
    Default{"sequence.zoom_out", bare(Key::Minus)},
    Default{"sequence.zoom_fit", bare(Key::Backslash)},
    Default{"markers.mark_in", bare(Key::I)},
    Default{"markers.mark_out", bare(Key::O)},
    Default{"markers.clear_in_out", ctrl_shift(Key::X)},
    Default{"markers.add", bare(Key::M)},
    Default{"markers.next", shift(Key::M)},
    Default{"markers.previous", ctrl_shift(Key::M)},
    Default{"markers.clear_all", ctrl_alt(Key::M)},
    Default{"playback.play", bare(Key::Space)},
    Default{"playback.shuttle_left", bare(Key::J)},
    Default{"playback.stop", bare(Key::K)},
    Default{"playback.shuttle_right", bare(Key::L)},
    Default{"playback.step_back", bare(Key::Left)},
    Default{"playback.step_forward", bare(Key::Right)},
    Default{"playback.step_back_five", shift(Key::Left)},
    Default{"playback.step_forward_five", shift(Key::Right)},
    Default{"playback.go_to_start", bare(Key::Home)},
    Default{"playback.go_to_end", bare(Key::End)},
    Default{"tool.selection", bare(Key::V)},
    Default{"tool.ripple", bare(Key::B)},
    Default{"tool.roll", bare(Key::N)},
    Default{"tool.razor", bare(Key::C)},
    Default{"tool.rate_stretch", bare(Key::R)},
    Default{"tool.slip", bare(Key::Y)},
    Default{"tool.slide", bare(Key::U)},
    Default{"tool.type", bare(Key::T)},
    Default{"window.maximise_frame", bare(Key::Backtick)},
    Default{"window.float_panel", ctrl_shift(Key::F)},
    // The number row is Premiere's for the cameras, so the themes, which
    // are this application's own, take Ctrl+Alt.
    Default{"view.theme.slate", ctrl_alt(Key::Digit1)},
    Default{"view.theme.luna", ctrl_alt(Key::Digit2)},
    Default{"view.theme.aero", ctrl_alt(Key::Digit3)},
    Default{"view.theme.phosphor", ctrl_alt(Key::Digit4)},
    Default{"multicam.select_camera_1", bare(Key::Digit1)},
    Default{"multicam.select_camera_2", bare(Key::Digit2)},
    Default{"multicam.select_camera_3", bare(Key::Digit3)},
    Default{"multicam.select_camera_4", bare(Key::Digit4)},
    Default{"multicam.select_camera_5", bare(Key::Digit5)},
    Default{"multicam.select_camera_6", bare(Key::Digit6)},
    Default{"multicam.select_camera_7", bare(Key::Digit7)},
    Default{"multicam.select_camera_8", bare(Key::Digit8)},
    Default{"multicam.select_camera_9", bare(Key::Digit9)},
    Default{"multicam.cut_to_camera_1", ctrl(Key::Digit1)},
    Default{"multicam.cut_to_camera_2", ctrl(Key::Digit2)},
    Default{"multicam.cut_to_camera_3", ctrl(Key::Digit3)},
    Default{"multicam.cut_to_camera_4", ctrl(Key::Digit4)},
    Default{"multicam.cut_to_camera_5", ctrl(Key::Digit5)},
    Default{"multicam.cut_to_camera_6", ctrl(Key::Digit6)},
    Default{"multicam.cut_to_camera_7", ctrl(Key::Digit7)},
    Default{"multicam.cut_to_camera_8", ctrl(Key::Digit8)},
    Default{"multicam.cut_to_camera_9", ctrl(Key::Digit9)},
};

/// Avid Media Composer's keys, where they differ: splice-in and overwrite on
/// V and B, lift and extract on Z and X, Add Edit on H, a second pair of
/// marks on E and R, and G to clear both. The tools that sat on those keys
/// give them up, as Media Composer has no tools on bare letters.
constexpr std::array kAvidKeys{
    Default{"sequence.insert", bare(Key::V)},
    Default{"sequence.overwrite", bare(Key::B)},
    Default{"edit.clear", bare(Key::Z)},
    Default{"edit.ripple_delete", bare(Key::X)},
    Default{"sequence.add_edit", bare(Key::H)},
    Default{"markers.clear_in_out", bare(Key::G)},
    Default{"markers.mark_in", bare(Key::I)},
    Default{"markers.mark_in", bare(Key::E)},
    Default{"markers.mark_out", bare(Key::O)},
    Default{"markers.mark_out", bare(Key::R)},
};

[[nodiscard]] std::string shown(const std::filesystem::path& path) { return path.string(); }

/// Every key a chord can be on, for reading one back from its name.
[[nodiscard]] const std::vector<Key>& every_key() {
  static const std::vector<Key> keys = [] {
    std::vector<Key> out{Key::Space,     Key::Comma,     Key::Minus,       Key::Period,
                         Key::Slash,     Key::Semicolon, Key::Equal,       Key::LeftBracket,
                         Key::Backslash, Key::RightBracket, Key::Backtick};
    for (int d = 0; d <= 9; ++d) out.push_back(static_cast<Key>('0' + d));
    for (int c = 'A'; c <= 'Z'; ++c) out.push_back(static_cast<Key>(c));
    for (auto v = static_cast<std::uint16_t>(Key::Escape); v <= static_cast<std::uint16_t>(Key::F12);
         ++v) {
      out.push_back(static_cast<Key>(v));
    }
    return out;
  }();
  return keys;
}

}  // namespace

std::span<const Action> actions() noexcept { return kActions; }

const Action* find_action(std::string_view id) noexcept {
  const auto found = std::ranges::find(kActions, id, &Action::id);
  return found == kActions.end() ? nullptr : &*found;
}

const Action* action_for(Command command) noexcept {
  const auto found = std::ranges::find(kActions, std::optional<Command>{command}, &Action::command);
  return found == kActions.end() ? nullptr : &*found;
}

std::span<const std::string_view> presets() noexcept { return kPresets; }

std::span<const Chord> Keymap::chords(std::string_view action) const {
  const auto found = bindings.find(action);
  if (found == bindings.end()) return {};
  return found->second;
}

const Action* Keymap::holder(const Chord& chord) const {
  for (const auto& [id, chords] : bindings) {
    if (std::ranges::contains(chords, chord)) return find_action(id);
  }
  return nullptr;
}

const Action* Keymap::resolve(const Chord& chord, Scope scope) const {
  const Action* action = holder(chord);
  return action != nullptr && action->scope == scope ? action : nullptr;
}

Keymap preset_keymap(std::string_view preset) {
  Keymap keymap;
  keymap.preset = std::ranges::contains(kPresets, preset) ? std::string(preset)
                                                          : std::string(kPremierePreset);
  for (const Action& action : kActions) keymap.bindings[std::string(action.id)];
  for (const Default& key : kPremiereKeys) keymap.bindings[std::string(key.action)].push_back(key.chord);

  if (keymap.preset == kAvidPreset) {
    // The preset's own keys replace Premiere's for the action, and take the
    // chord from whatever had it.
    for (const Default& key : kAvidKeys) keymap.bindings[std::string(key.action)].clear();
    for (const Default& key : kAvidKeys) assign(keymap, key.action, key.chord);
  }
  return keymap;
}

const Action* assign(Keymap& keymap, std::string_view action, const Chord& chord) {
  if (chord.empty() || find_action(action) == nullptr) return nullptr;
  const Action* lost = nullptr;
  for (auto& [id, chords] : keymap.bindings) {
    if (id == action) continue;
    if (const auto at = std::ranges::find(chords, chord); at != chords.end()) {
      chords.erase(at);
      lost = find_action(id);
    }
  }
  std::vector<Chord>& mine = keymap.bindings[std::string(action)];
  if (!std::ranges::contains(mine, chord)) mine.push_back(chord);
  return lost;
}

void unassign(Keymap& keymap, std::string_view action, const Chord& chord) {
  const auto found = keymap.bindings.find(action);
  if (found == keymap.bindings.end()) return;
  std::erase(found->second, chord);
}

bool is_custom(const Keymap& keymap) {
  return keymap.bindings != preset_keymap(keymap.preset).bindings;
}

std::string key_text(Key key) {
  const auto value = static_cast<std::uint16_t>(key);
  if (value > ' ' && value < 127) return std::string(1, static_cast<char>(value));
  if (key == Key::Space) return "Space";
  if (key == Key::Escape) return "Esc";
  if (key == Key::Delete) return "Del";
  return std::string(ui::to_string(key));
}

std::string to_text(const Chord& chord) {
  if (chord.empty()) return {};
  std::string text;
  // Windows' order, which is the order its own menus write them in.
  if (chord.control) text += "Ctrl+";
  if (chord.alt) text += "Alt+";
  if (chord.shift) text += "Shift+";
  return text + key_text(chord.key);
}

std::optional<Chord> chord_from_text(std::string_view text) {
  Chord chord;
  // The modifiers, then whatever is left is the key -- which may itself be a
  // '+' or a '-', so only a prefix that is a known modifier is taken off.
  for (;;) {
    if (text.starts_with("Ctrl+")) {
      chord.control = true;
      text.remove_prefix(5);
    } else if (text.starts_with("Alt+")) {
      chord.alt = true;
      text.remove_prefix(4);
    } else if (text.starts_with("Shift+")) {
      chord.shift = true;
      text.remove_prefix(6);
    } else {
      break;
    }
  }
  if (text.empty()) return std::nullopt;
  for (const Key key : every_key()) {
    // The short name a menu writes, or the long one -- "Esc" or "Escape".
    if (key_text(key) == text || (key >= Key::Escape && ui::to_string(key) == text)) {
      chord.key = key;
      return chord;
    }
  }
  return std::nullopt;
}

std::string to_json(const Keymap& keymap, int indent) {
  json out;
  out["version"] = kKeysSchemaVersion;
  out["preset"] = keymap.preset;
  json changed = json::object();
  const Keymap base = preset_keymap(keymap.preset);
  for (const auto& [id, chords] : keymap.bindings) {
    const auto preset_chords = base.chords(id);
    if (std::ranges::equal(chords, preset_chords)) continue;
    json list = json::array();
    for (const Chord& chord : chords) list.push_back(to_text(chord));
    changed[id] = std::move(list);
  }
  out["bindings"] = std::move(changed);
  return out.dump(indent);
}

std::expected<Keymap, std::string> keymap_from_json(std::string_view text) {
  const json document = json::parse(text, nullptr, false);
  if (document.is_discarded()) return std::unexpected("the keys file is not valid JSON");
  if (!document.is_object()) return std::unexpected("a keys file has to be an object");
  if (document.value("version", 0) > kKeysSchemaVersion) {
    return std::unexpected("those keys were written by a newer version of Cutline");
  }

  Keymap keymap = preset_keymap(document.value("preset", std::string(kPremierePreset)));
  const auto bindings = document.find("bindings");
  if (bindings == document.end() || !bindings->is_object()) return keymap;

  // Every changed action is cleared first and then given its chords, so the
  // file's choices win over the preset's wherever the two disagree. An action
  // this build does not have, or a chord it cannot read, is passed over: a
  // file from a newer version should cost an older one that key, not the rest.
  for (const auto& [id, list] : bindings->items()) {
    if (find_action(id) == nullptr) continue;
    keymap.bindings[id].clear();
  }
  for (const auto& [id, list] : bindings->items()) {
    if (find_action(id) == nullptr || !list.is_array()) continue;
    for (const json& entry : list) {
      if (!entry.is_string()) continue;
      if (const auto chord = chord_from_text(entry.get<std::string>()); chord.has_value()) {
        assign(keymap, id, *chord);
      }
    }
  }
  return keymap;
}

std::filesystem::path default_keys_path() {
  const char* roaming = std::getenv("APPDATA");
  const std::filesystem::path base =
      roaming == nullptr ? std::filesystem::path{"."} : std::filesystem::path{roaming};
  return base / "Cutline" / "keys.json";
}

std::expected<Keymap, std::string> read_keymap(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return preset_keymap(kPremierePreset);
  std::ostringstream text;
  text << file.rdbuf();
  if (file.bad()) return std::unexpected("could not read " + shown(path));
  return keymap_from_json(text.str());
}

std::expected<void, std::string> write_keymap(const std::filesystem::path& path,
                                              const Keymap& keymap) {
  std::error_code error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return std::unexpected("could not make " + shown(path.parent_path()) + ": " +
                             error.message());
    }
  }
  // Through a staging file beside it, as the preferences are written, so an
  // interrupted save leaves the old keys rather than half of the new ones.
  std::filesystem::path staging = path;
  staging += ".saving";
  {
    std::ofstream file(staging, std::ios::binary | std::ios::trunc);
    if (!file) return std::unexpected("could not write " + shown(staging));
    file << to_json(keymap);
    file.flush();
    if (!file) return std::unexpected("could not write " + shown(staging));
  }
  std::filesystem::rename(staging, path, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
    return std::unexpected("could not replace " + shown(path) + ": " + error.message());
  }
  return {};
}

}  // namespace cutline::editor
