/// Keyboard shortcuts: the actions, the presets, how a chord is resolved, and
/// the keys file.

#include "cutline/editor/shortcuts.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>

namespace cutline::editor {
namespace {

using ui::Key;

[[nodiscard]] Chord ctrl(Key key) { return Chord{.key = key, .control = true}; }
[[nodiscard]] Chord bare(Key key) { return Chord{.key = key}; }

TEST(Shortcuts, EveryActionHasItsOwnName) {
  std::set<std::string_view> seen;
  for (const Action& action : actions()) {
    EXPECT_TRUE(seen.insert(action.id).second) << action.id;
    EXPECT_FALSE(action.label.empty()) << action.id;
    EXPECT_FALSE(action.category.empty()) << action.id;
  }
}

TEST(Shortcuts, NoPresetGivesOneChordToTwoActions) {
  // A chord that ran two things would run whichever was found first, and
  // which that was would be an accident of the table's order.
  for (const std::string_view preset : presets()) {
    const Keymap keymap = preset_keymap(preset);
    std::set<Chord> seen;
    for (const auto& [id, chords] : keymap.bindings) {
      for (const Chord& chord : chords) {
        EXPECT_TRUE(seen.insert(chord).second) << preset << ": " << to_text(chord) << " twice";
      }
    }
  }
}

TEST(Shortcuts, PremieresKeysAreWhatTheApplicationAlwaysHad) {
  const Keymap keymap = preset_keymap(kPremierePreset);
  ASSERT_NE(keymap.resolve(ctrl(Key::Z), Scope::Always), nullptr);
  EXPECT_EQ(keymap.resolve(ctrl(Key::Z), Scope::Always)->id, "edit.undo");
  EXPECT_EQ(keymap.resolve(bare(Key::Space), Scope::Bare)->id, "playback.play");
  EXPECT_EQ(keymap.resolve(bare(Key::C), Scope::Bare)->id, "tool.razor");
  EXPECT_EQ(keymap.resolve(bare(Key::Left), Scope::Fallback)->id, "playback.step_back");
  EXPECT_EQ(keymap.resolve(Chord{.key = Key::M, .control = true, .alt = true}, Scope::Fallback)->id,
            "markers.clear_all");
}

TEST(Shortcuts, AChordIsOfferedOnlyInItsActionsScope) {
  // The arrows wait until a focused slider has declined them.
  const Keymap keymap = preset_keymap(kPremierePreset);
  EXPECT_EQ(keymap.resolve(bare(Key::Left), Scope::Always), nullptr);
  EXPECT_EQ(keymap.resolve(bare(Key::Left), Scope::Bare), nullptr);
}

TEST(Shortcuts, EditingCommandsAreFoundByTheirCommand) {
  ASSERT_NE(action_for(Command::Split), nullptr);
  EXPECT_EQ(action_for(Command::Split)->id, "sequence.add_edit");
  EXPECT_EQ(action_for(Command::Undo)->id, "edit.undo");
}

TEST(Shortcuts, AssigningAChordTakesItFromWhateverHadIt) {
  Keymap keymap = preset_keymap(kPremierePreset);
  const Action* lost = assign(keymap, "sequence.add_edit", bare(Key::K));
  ASSERT_NE(lost, nullptr);
  EXPECT_EQ(lost->id, "playback.stop");
  EXPECT_TRUE(keymap.chords("playback.stop").empty());
  EXPECT_EQ(keymap.resolve(bare(Key::K), Scope::Always)->id, "sequence.add_edit");
  // And keeps the one it had.
  EXPECT_EQ(keymap.chords("sequence.add_edit").size(), 2u);
  EXPECT_TRUE(is_custom(keymap));
}

TEST(Shortcuts, AvidPutsSpliceAndOverwriteOnVAndB) {
  const Keymap keymap = preset_keymap(kAvidPreset);
  EXPECT_EQ(keymap.holder(bare(Key::V))->id, "sequence.insert");
  EXPECT_EQ(keymap.holder(bare(Key::B))->id, "sequence.overwrite");
  EXPECT_EQ(keymap.holder(bare(Key::Z))->id, "edit.clear");
  EXPECT_EQ(keymap.holder(bare(Key::X))->id, "edit.ripple_delete");
  EXPECT_TRUE(keymap.chords("tool.selection").empty()) << "V went to splice-in";
  // Both marks: Premiere's I and O are still there beside E and R.
  EXPECT_EQ(keymap.chords("markers.mark_in").size(), 2u);
  EXPECT_EQ(keymap.holder(bare(Key::R))->id, "markers.mark_out") << "taken from Rate Stretch";
  EXPECT_FALSE(is_custom(keymap));
}

TEST(Shortcuts, AChordReadsBackAsItIsWritten) {
  for (const Chord chord : {ctrl(Key::Z), bare(Key::Space), bare(Key::Minus),
                            Chord{.key = Key::Equal, .control = true, .shift = true},
                            Chord{.key = Key::Delete, .shift = true}, bare(Key::F5),
                            Chord{.key = Key::Comma, .alt = true}, bare(Key::Backtick)}) {
    const std::string text = to_text(chord);
    const auto back = chord_from_text(text);
    ASSERT_TRUE(back.has_value()) << text;
    EXPECT_EQ(*back, chord) << text;
  }
  EXPECT_EQ(to_text(Chord{.key = Key::K, .control = true, .shift = true}), "Ctrl+Shift+K");
  EXPECT_EQ(chord_from_text("Ctrl+Escape"), ctrl(Key::Escape)) << "the long name too";
  EXPECT_FALSE(chord_from_text("Ctrl+").has_value());
  EXPECT_FALSE(chord_from_text("a").has_value()) << "keys are their capitals";
}

TEST(Shortcuts, TheFileHoldsOnlyWhatWasChanged) {
  Keymap keymap = preset_keymap(kPremierePreset);
  EXPECT_EQ(to_json(keymap).find("edit.undo"), std::string::npos);

  assign(keymap, "edit.undo", bare(Key::F1));
  const std::string text = to_json(keymap);
  EXPECT_NE(text.find("edit.undo"), std::string::npos);
  const auto back = keymap_from_json(text);
  ASSERT_TRUE(back.has_value()) << back.error();
  EXPECT_EQ(*back, keymap);
}

TEST(Shortcuts, AFileFromANewerBuildCostsOnlyWhatThisOneCannotRead) {
  const auto keymap = keymap_from_json(R"({"version": 1, "preset": "Avid Media Composer",
      "bindings": {"edit.undo": ["F2", "Hyper+Q"], "future.thing": ["F3"]}})");
  ASSERT_TRUE(keymap.has_value());
  EXPECT_EQ(keymap->preset, kAvidPreset);
  ASSERT_EQ(keymap->chords("edit.undo").size(), 1u);
  EXPECT_EQ(keymap->chords("edit.undo")[0], bare(Key::F2));
  EXPECT_EQ(keymap->holder(bare(Key::V))->id, "sequence.insert") << "the rest of the preset";
}

TEST(Shortcuts, NoFileIsPremieresKeys) {
  const auto keymap = read_keymap(std::filesystem::temp_directory_path() / "cutline-no-keys.json");
  ASSERT_TRUE(keymap.has_value());
  EXPECT_EQ(*keymap, preset_keymap(kPremierePreset));
}

TEST(Shortcuts, KeysSurviveBeingWrittenAndReadBack) {
  const auto path = std::filesystem::temp_directory_path() / "cutline-keys-test.json";
  Keymap keymap = preset_keymap(kAvidPreset);
  assign(keymap, "tool.razor", ctrl(Key::R));
  unassign(keymap, "playback.play", bare(Key::Space));
  ASSERT_TRUE(write_keymap(path, keymap).has_value());
  const auto back = read_keymap(path);
  std::filesystem::remove(path);
  ASSERT_TRUE(back.has_value()) << back.error();
  EXPECT_EQ(*back, keymap);
}

}  // namespace
}  // namespace cutline::editor
